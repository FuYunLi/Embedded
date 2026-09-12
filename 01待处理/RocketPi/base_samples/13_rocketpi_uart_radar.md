---
status: done
created: 2026-09-12
tags:
  - stm32/uart
  - embedded/driver
  - embedded/radar
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/13_rocketpi_uart_radar/main.c"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/13_rocketpi_uart_radar/driver_mg58f18_radar.c"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/13_rocketpi_uart_radar/driver_mg58f18_radar_interface.c"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/13_rocketpi_uart_radar/driver_mg58f18_radar_test.c"
---

# 13_rocketpi_uart_radar

> 代码：`Embedded_Code/01待处理/rocketpi/base_samples/13_rocketpi_uart_radar/`（driver_mg58f18_radar.c + interface.c + test.c + main.c + debug_driver.c，共约 60KB）
> 硬件：STM32F401RE（RocketPi），USART1 9600（雷达），USART2 115200（调试），MG58F18 24GHz 毫米波雷达模组
> 目标：通过串口控制 MG58F18 雷达模组——配置参数、查询状态、检测触发。是 42 例中第一个**串口外设驱动**案例。

## 交互效果

上电后自动执行全量参数读写测试，调试串口输出：

```
=== MG58F18 radar test ===
[RADAR][init] ok
Distance threshold: 3000
[RADAR][set_distance_threshold] ok
Output delay: 1000 ms
[RADAR][set_delay_ms] ok
...
=== MG58F18 radar smoke test done ===

[RADAR] cmd=0x87 data2=0x00 data3=0x00 data4=0x01 checksum=0x86
        trigger state: TRIGGERED
[RADAR][IO] state changed: LOW -> HIGH
```

运行时主循环每 20ms 轮询：打印雷达发来的帧 + 监视 OUT 引脚跳变。人体靠近时雷达检测到触发，OUT 引脚跳高，同时串口发触发帧。

## MG58F18 雷达硬件

MG58F18 是 24GHz 毫米波雷达传感器模组（宁波迈阶电子），用于人体存在/运动检测。

| 维度 | 传统 PIR（被动红外） | 24GHz 毫米波雷达（MG58F18） |
|---|---|---|
| 检测原理 | 检测红外热辐射变化 | 发射 24GHz 微波、接收反射信号分析 |
| 静止检测 | 不能（必须有温度变化） | 能（微弱呼吸/心跳也能检测） |
| 穿透性 | 不能穿透玻璃/薄墙 | 能穿透塑料外壳、薄墙 |
| 距离 | 近（几米） | 可调（100~65000） |
| 抗干扰 | 易受热源干扰 | 不受温度影响 |

**与 MCU 的接口**：两路——GPIO（OUT 引脚，高/低电平表示触发状态）和 UART（配置参数、查询状态）。13 用 USART1 连雷达（9600bps），USART2 连调试串口（115200bps），**两个串口各司其职**，雷达数据和调试信息不混。

## 串口通信协议

MG58F18 的 UART 协议是**定长 7 字节二进制帧**，控制帧和回复帧格式相同：

```
Byte0   Byte1   Byte2   Byte3   Byte4   Byte5   Byte6
0x5A    命令码   高字节   中字节   低字节   校验     0xFE
帧头    Data1   Data2   Data3   Data4   XOR      帧尾
```

**校验**：`Data1 ^ Data2 ^ Data3 ^ Data4`（四个数据字节异或），比 [[10_rocketpi_uart_ymodem|10 的 Ymodem CRC16]] 简单得多。

**命令码规律**：设置命令 0x01~0x20，查询命令 = 设置命令 | 0x80（如 0x01 设置距离 / 0x81 查询距离）。查询时 Data2~Data4 全填 0x00。

**关键时序约束**（来自协议文档）：
1. 上电后等 700ms 才能操作 UART
2. 发控制帧后必须等回复帧才能再发下一帧（停等式）
3. 100ms 内没收到回复 = 通信失败，可重发
4. 参数写入立即生效，但掉电丢失——需发"保存设置"命令（0x20）才持久化

**与 [[08_rocketpi_uart_control_led|08 JSON]] 的对比**：08 是文本协议（人可读、换行分帧），13 是二进制协议（定长、帧头帧尾、校验）。13 的帧更紧凑（7 字节 vs 08 的几十字节），但人看不懂（需要 hexdump），调试依赖 [[06_rocketpi_uart_printf_3|06_3 的 hexdump 能力]]。

## 架构：三层分离

| 文件 | 行数 | 层次 | 与硬件的关系 |
|---|---|---|---|
| `driver_mg58f18_radar_interface.c` | ~120 | 硬件适配层 | **唯一碰 HAL 的文件**：UART DMA 收发、GPIO 读取 |
| `driver_mg58f18_radar.c` | ~850 | 协议引擎+命令层 | **完全不碰 HAL**，只调 interface 的函数 |
| `driver_mg58f18_radar_test.c` | ~250 | 测试层 | 调 driver 的 API，打印结果 |
| `main.c` | ~130 | 入口 | 初始化外设，调 test_run + test_poll |

**和 [[10_rocketpi_uart_ymodem|10 的 Ymodem]] 同模式**：interface.c = ymodem_port.c，driver.c = ymodem.c，test.c = 应用层。但 13 的分离更彻底——driver 是一个**独立可移植的设备驱动**，换 MCU 只改 interface.c，driver.c 一字不改。

## interface.c：硬件适配层（~120 行）

### 接收：DMA + Idle

```c
static bool mg58f18_radar_interface_start_rx(void) {
    HAL_StatusTypeDef status =
        HAL_UARTEx_ReceiveToIdle_DMA(s_hal.uart, s_hal.rx_buffer, sizeof(s_hal.rx_buffer));
    if (status == HAL_OK) {
        __HAL_DMA_DISABLE_IT(s_hal.uart->hdmarx, DMA_IT_HT);
        return true;
    } else if (status == HAL_BUSY) {
        HAL_UART_AbortReceive(s_hal.uart);
        // 重启...
    }
}
```

和 [[07_rocketpi_uart_echo|07 的 DMA+Idle]] 同方案，多了 HAL_BUSY 时的 Abort+重启——**错误恢复**。`HAL_UARTEx_RxEventCallback` 回调里调 `mg58f18_radar_receive_bytes` 把字节灌入协议引擎，然后重启 DMA。

### 发送：DMA + 忙等

```c
mg58f18_radar_interface_hw_send(data, length, timeout_ms) {
    memcpy(s_hal.tx_buffer, data, length);      // 拷贝到发送缓冲
    s_hal.tx_busy = true;
    HAL_UART_Transmit_DMA(s_hal.uart, s_hal.tx_buffer, length);
    // 忙等 tx_busy 被 TxCpltCallback 清零，或超时
}
```

**memcpy 拷贝到 tx_buffer**：DMA 发送需要缓冲区在发送期间保持有效——如果直接发调用方的栈上数据，函数返回后栈回收，DMA 还在发就踩内存。tx_buffer 是 static 全局，生命周期覆盖整个程序。

### GPIO 读取

```c
bool mg58f18_radar_interface_hw_read_io(void) {
    return (HAL_GPIO_ReadPin(RADAR_IO_GPIO_Port, RADAR_IO_Pin) == GPIO_PIN_SET);
}
```

雷达 OUT 引脚直接反映触发状态——比发串口查询命令更快、更实时。test_poll 里每 20ms 读一次，检测跳变。

## driver.c：协议引擎+命令层（~850 行）

### 协议解析状态机（~200 行）

```c
static mg58f18_radar_status_t mg58f18_radar_core_process_byte(
    mg58f18_radar_core_t *ctx, uint8_t byte)
{
    if (ctx->parser.index == 0U) {
        if (byte != 0x5A) return ctx->last_error;  // 等帧头
        ctx->parser.buffer[0] = byte;
        ctx->parser.index = 1U;
        return ctx->last_error;
    }
    ctx->parser.buffer[ctx->parser.index++] = byte;
    if (ctx->parser.index < 7) return ctx->last_error;  // 还没收满

    // 收满 7 字节 → 校验
    uint8_t checksum = buffer[1] ^ buffer[2] ^ buffer[3] ^ buffer[4];
    bool checksum_ok = (checksum == buffer[5]);
    bool tail_ok = (buffer[6] == 0xFE);

    if (checksum_ok && tail_ok) {
        store_frame(ctx, buffer);     // 存储有效帧
        if (ctx->awaiting_reply && frame.command == ctx->pending_command)
            ctx->awaiting_reply = false;   // 匹配到等待的应答
    }

    // 帧尾后紧跟帧头 → 重用为下一帧起点
    bool restart = (buffer[6] == 0x5A);
    reset_parser(ctx);
    if (restart) { buffer[0] = 0x5A; index = 1; }
}
```

**"帧尾后紧跟帧头"的处理**是亮点：如果第 7 字节恰好是 0x5A（下一帧的帧头），不清零而是保留为下一帧的起始——**不丢同步**。在连续数据流中（模组主动上报触发帧时可能连续发），这个处理保证不会因为帧边界重叠而丢帧。

### 事务机制：发送-等应答

```c
static mg58f18_radar_status_t mg58f18_radar_send_command_raw(
    uint8_t command, uint8_t data2, uint8_t data3, uint8_t data4,
    mg58f18_radar_frame_t *response)
{
    uint8_t frame[7] = {0x5A, command, data2, data3, data4, checksum, 0xFE};

    core_begin_transaction(&s_core, command);   // 标记"正在等这个命令码的应答"
    interface_hw_send(frame, 7, timeout);        // DMA 发送
    mg58f18_radar_wait_for_reply(timeout);       // 轮询等待 awaiting_reply 清零
    // 解析状态机在 DMA 回调里自动运行，匹配到命令码后清除标志
}
```

**发送和接收是异步的**：发送后不直接等字节，而是设 `awaiting_reply` 标志 + `pending_command` 命令码，DMA 回调里收到帧后解析状态机自动匹配。`wait_for_reply` 只是轮询标志——**比 10 的 Ymodem 的纯阻塞等待更灵活**。

### echo 校验：写后验证

```c
static mg58f18_radar_status_t mg58f18_radar_expect_echo(
    uint8_t command, uint8_t data2, uint8_t data3, uint8_t data4)
{
    mg58f18_radar_frame_t frame;
    mg58f18_radar_send_command_raw(command, data2, data3, data4, &frame);
    if (frame.data2 != data2 || frame.data3 != data3 || frame.data4 != data4)
        return FRAME_ERROR;
    return OK;
}
```

所有 set 命令都用 `expect_echo`：发完命令后，模组回显相同参数，驱动对比确认写入成功。**写后验证**是串口外设驱动的常见模式——协议规定了回显语义，驱动就利用它做校验。

### 命令 API（~500 行）

15+ 个参数，每个参数一个 set + get 函数对。全部走同一个底层流程：参数检查 → 组帧 → 发送 → 等应答 → 提取返回值。

```c
// set 示例
mg58f18_radar_set_distance_threshold(uint16_t threshold) {
    if (threshold < 100 || threshold > 65000) return INVALID_ARGUMENT;
    data3 = (threshold >> 8) & 0xFF;
    data4 = threshold & 0xFF;
    return expect_echo(0x01, 0x00, data3, data4);
}

// get 示例
mg58f18_radar_get_distance_threshold(uint16_t *threshold) {
    send_command_raw(0x81, 0x00, 0x00, 0x00, &frame);
    *threshold = (frame.data3 << 8) | frame.data4;
}
```

**500 行来自参数多，不是逻辑复杂**——每个函数 10~20 行，模式完全相同。

## test.c：联调用例（~250 行）

### mg58f18_radar_test_run：全量读写测试

上电后依次读取所有参数、写回（验证 echo）、打印结果。每个命令间等 120ms。**硬件联调的标准做法**——先全量读写一遍验证通信正常，再做业务逻辑。

### mg58f18_radar_test_poll：运行时监控

主循环每 20ms 调一次：打印收到的帧 + 监视 OUT 引脚跳变。**两种触发检测方式并行**：串口帧（详细信息）和 GPIO 电平（实时性更高）。

## main.c：双串口初始化

```c
MX_USART2_UART_Init();   // 调试串口（printf 走这里）
MX_USART1_UART_Init();   // 雷达串口（driver 走这里）
mg58f18_radar_test_run(); // 上电全量测试

while (1) {
    mg58f18_radar_test_poll();
    HAL_Delay(20);
}
```

两个串口完全独立：USART2 是调试输出，USART1 是雷达通信。**雷达数据不会干扰调试输出**。

## 与前例的对比

| 维度 | 08 JSON 命令 | 10 Ymodem | 13 雷达驱动 |
|---|---|---|---|
| 协议类型 | 文本（人可读） | 二进制 | 二进制 |
| 帧长度 | 可变（换行分帧） | 可变（128/1024） | 固定 7 字节 |
| 校验 | 无 | CRC16 | XOR |
| 应答 | JSON 回复 | ACK/NAK | echo 回显 |
| 复杂度来源 | 解析器设计 | 状态机+错误恢复 | 参数数量多 |
| 分层 | 单文件 | 协议+port | 协议+port+test |

13 的协议比 10 简单（定长帧、XOR 校验、无重传），但**它是第一个"写给真实外设"的驱动**——不是协议演示，而是真正控制一个传感器模组。

## 与前例连线

- [[10_rocketpi_uart_ymodem|10 主笔记]] — 同为"协议引擎+port 层"分离
- [[10_rocketpi_uart_ymodem_1|10 依赖注入]] — 13 的 interface 层是另一种平台抽象
- [[07_rocketpi_uart_echo]] — 13 的 DMA+Idle 接收与 07 同方案
- [[08_rocketpi_uart_control_led_4|串口文本协议谱系]] — 13 在谱系中"二进制定长帧协议"位置
- [[06_rocketpi_uart_printf_3|06_3 hexdump]] — 调试二进制协议必备

## 值得记住的三个设计词

- **echo 校验**：利用模组的回显语义做写后验证——协议规定了行为，驱动利用它做质量保证
- **帧尾即帧头重用**：解析状态机在帧尾后检查是否紧跟下一帧头，是则保留为起始——不丢同步的连续流处理
- **三层分离**：interface（硬件适配）→ driver（协议+命令）→ test（联调）——换 MCU 只改 interface，driver 一字不改
