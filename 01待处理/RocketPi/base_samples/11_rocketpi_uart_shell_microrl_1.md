---
status: done
created: 2026-09-12
tags:
  - stm32/uart
  - c/ring-buffer
  - embedded/interrupt
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/11_rocketpi_uart_shell_microrl/main.c"
---

# 中断接收与环形缓冲：从阻塞到非阻塞的接收层升级

> [[11_rocketpi_uart_shell_microrl|11 主笔记]] 说"11 是 08 的成熟形态"——这篇拆解接收层的升级。08 用阻塞 HAL_UART_Receive 死等字节，11 改成中断+环形缓冲，主循环从"被串口独占"变成"空闲时处理"。

## 08 的问题：主循环被阻塞

```c
// 08 的主循环
while (1) {
    uint8_t byte = 0U;
    if (HAL_UART_Receive(&huart2, &byte, 1U, HAL_MAX_DELAY) == HAL_OK) {
        console_handle_input_byte(byte);
    }
}
```

`HAL_MAX_DELAY` 意味着"没字节就永远等"。主循环在等待期间什么都不做——如果有 LED 闪烁、传感器采集、电机控制等任务，全部被阻塞。07 用 DMA+Idle 解决了这个问题（硬件自动收，主循环空闲），但 07 是"一帧一收"。11 要的是"逐字节处理"（shell 交互），所以选了中断方案。

## 11 的方案：中断存字节 + 主循环处理

```c
// 中断回调
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart){
    if (huart->Instance == USART2) {
        shell_queue_char(s_uart2_rx);   // 字节进环形缓冲
        shell_start_rx();               // 重启下一次中断接收
    }
}

// 主循环
while (1) {
    shell_process_input();   // 从缓冲取字节喂给 microrl
}
```

**中断里只做两件事**：存字节（`shell_queue_char`）、重启接收（`shell_start_rx`）。行编辑、命令解析、LED 控制全在主循环——**ISR 尽量短，耗时操作挪主循环**。如果 ISR 里做行编辑（调 microrl_insert_char），ISR 耗时会变长，可能导致后续字节丢失（下一个字节到了但 ISR 还没退出）。

## shell_start_rx：中断接收的启动

```c
static void shell_start_rx(void){
    HAL_UART_Receive_IT(&huart2, &s_uart2_rx, 1);
}
```

**作用**：告诉 HAL"准备接收 1 个字节，收到后调 RxCpltCallback"。每次中断回调后必须重新调用——HAL 的中断接收是一次性的（收完一次就停），不像 DMA 可以连续收。

**`s_uart2_rx`**：单字节缓冲，HAL 收到字节后写入这个变量。中断回调里读它然后存进环形缓冲。**这个变量的生命周期贯穿整个程序**——必须是 static 全局，不能是局部变量（ISR 返回后局部变量就没了）。

## 环形缓冲区（Ring Buffer）

```c
static volatile uint8_t s_shell_rx_buffer[128];
static volatile uint16_t s_shell_rx_head;   // 写指针（中断写）
static volatile uint16_t s_shell_rx_tail;   // 读指针（主循环读）
```

环形缓冲区是嵌入式最常用的数据结构之一——**生产者（中断）写、消费者（主循环）读、满丢弃、空等待**。

### volatile 的必要性

`head` 在 ISR 里更新、`tail` 在主循环里更新。没有 `volatile`，编译器在优化时可能把主循环里的 `while (head != tail)` 优化成"只读一次 head，以后不再读"——因为编器认为循环体内没有修改 head。volatile 告诉编译器"这个变量可能被硬件/中断修改，每次都重新读"。

### shell_queue_char：生产者（中断写入）

```c
static void shell_queue_char(uint8_t value){
    uint16_t next_head = (uint16_t)((s_shell_rx_head + 1U) % SHELL_RX_BUFFER_SIZE);
    if (next_head != s_shell_rx_tail) {   // 满了就丢弃
        s_shell_rx_buffer[s_shell_rx_head] = value;
        s_shell_rx_head = next_head;
    }
}
```

**形参**：`value` — 从 HAL 收到的字节。

**满丢弃策略**：`next_head == tail` 表示缓冲满，新字节直接丢掉。中断不能阻塞（不能等主循环腾出空间），丢字节是唯一安全选择。对 shell 场景来说，偶尔丢一个字符用户会注意到（少了一个字母），重新敲就行。对高速数据传输场景，需要更大的缓冲或流控机制。

**取模 `% 128`**：当 head 到达 128 时回绕到 0，形成"环形"。128 是 2 的幂时取模可以优化为位与 `& 0x7F`，但编译器通常会自动做这个优化。

### shell_process_input：消费者（主循环读取）

```c
static void shell_process_input(void){
    while (s_shell_rx_head != s_shell_rx_tail) {        // 缓冲非空
        uint8_t ch = s_shell_rx_buffer[s_shell_rx_tail]; // 取一个字节
        s_shell_rx_tail = (uint16_t)((s_shell_rx_tail + 1U) % SHELL_RX_BUFFER_SIZE);
        if (ch == '\r') {
            ch = '\n';                    // \r → \n（microrl 配置 _ENDL_LF）
            if (s_shell_rx_head != s_shell_rx_tail) {
                uint8_t peek = s_shell_rx_buffer[s_shell_rx_tail];
                if (peek == '\n') {
                    s_shell_rx_tail = (uint16_t)((s_shell_rx_tail + 1U) % SHELL_RX_BUFFER_SIZE);
                }
            }
        }
        microrl_insert_char(&s_shell, ch);
    }
}
```

**\r → \n 转换 + 吞 \r\n**：PC 端串口助手发 `\r\n` 两连发。microrl 配置了 `_ENDL_LF` 只认 `\n` 触发执行，所以把 `\r` 转成 `\n`，再吞掉紧跟的 `\n`。**peek 检查**：只在 `\r` 后面紧跟 `\n` 时才吞——如果 `\r` 后面是其他字符（理论上不会，但防御），不吞。

### 单生产者单消费者的安全性

环形缓冲区在"一个中断写、一个主循环读"的场景下天然线程安全——**不需要锁**。原因是：

1. `head` 只被 ISR 写，`tail` 只被主循环读——没有同时写同一变量的情况
2. `head` 和 `tail` 是 `uint16_t`，在 32 位 MCU 上是原子读写（一条指令完成）
3. 主循环读 `head` 时看到的要么是旧值（ISR 还没更新）要么是新值——旧值时 `head==tail` 等下一轮循环，新值时正常读取。两种情况都不会出错

**如果反过来**（多生产者或多消费者），就需要互斥保护——这是 RTOS 信号量/Mutex 的典型应用场景。

## UART 错误回调

```c
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart){
    if (huart->Instance == USART2) {
        shell_start_rx();   // 出错也重启接收
    }
}
```

**作用**：UART 收到错误字节（帧错误、溢出、噪声）时 HAL 调这个回调。如果不重启接收，shell 就卡死了——**错误恢复是长跑韧性的关键**。07 的 ErrorCallback 也是同样处理。

## 中断接收 vs 阻塞接收 vs DMA 接收

| 方式 | 主循环占用 | 字节粒度 | 适用场景 | 例程 |
|---|---|---|---|---|
| 阻塞 HAL_UART_Receive | 100%（死等） | 逐字节 | 教学/一次性操作 | 08 |
| 中断 HAL_UART_Receive_IT | 0%（ISR 存字节） | 逐字节 | Shell/交互/低速 | 11 |
| DMA+Idle ReceiveToIdle_DMA | 0%（硬件收） | 一帧 | 高速/定长/变长帧 | 07 |

11 选中断而不是 DMA 的原因：shell 是逐字节交互（人敲键盘），没有"一帧多少字节"的概念。DMA 适合"一次收一批"的场景，中断适合"随时来一个"的场景。

## 环形缓冲区的通用模式

环形缓冲区不只用于串口接收——在嵌入式中无处不在：

- **UART 接收缓冲**（本例）
- **UART 发送缓冲**（ISR 取字节发，主循环塞字节）
- **日志缓冲**（生产者写日志，消费者输出到串口/文件）
- **音频缓冲**（I2S DMA 填充，解码器消费）
- **FreeRTOS 队列**（内部实现就是环形缓冲+信号量）

掌握了"head/tail + 取模 + 满丢弃/空等待 + volatile"这套模式，所有环形缓冲场景都能复用。

## 关联笔记

- [[11_rocketpi_uart_shell_microrl]] — 主笔记：架构全景与命令系统
- [[11_rocketpi_uart_shell_microrl_2|microrl 库核心解析]] — 库内部怎么处理字节
- [[07_rocketpi_uart_echo]] — DMA 接收的另一种非阻塞方案
- [[08_rocketpi_uart_control_led|08 主笔记]] — 阻塞接收的对照基线
