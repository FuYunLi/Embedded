---
status: todo
created: 2026-09-11
tags:
  - stm32/uart
  - stm32/dma
  - rocketpi/base_samples
  - todo
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/07_rocketpi_uart_echo/main.c"
---

# 07_rocketpi_uart_echo

> 代码：`Embedded_Code/01待处理/rocketpi/base_samples/07_rocketpi_uart_echo/main.c`（usart.c/dma.c 及头文件在原仓库，配置要点已并入本笔记）
> 硬件：STM32F401RE（RocketPi），USART2 PA2=TX/PA3=RX，115200-8-N-1；RX 挂 DMA1_Stream5，TX 挂 DMA1_Stream6
> 目标：串口回显——PC 发什么，板子原样发回什么。串口接收的主流方案样板

## 一图看懂数据流

```
PC 串口助手 "hello\n"
      │
      ▼ RX 引脚
┌─────────────────────────────────────────────────┐
│ DMA1_Stream5 硬件搬运：字节自动写入 g_uart_rx_buffer │  ← CPU 零参与
└─────────────────────────────────────────────────┘
      │
      │ 线路空闲超过 1 字节时间 → IDLE 事件
      ▼
HAL_UARTEx_RxEventCallback(Size=6)          ← 唯一一次中断
      ① memcpy → g_uart_tx_buffer
      ② uart_restart_rx_dma()  立刻重启接收
      ③ HAL_UART_Transmit 阻塞发出 "hello\n"
      │
      ▼
PC 串口助手看到 "hello\n"（回显）
```

主循环 `while(1){ HAL_Delay(1); }` 空转——**所有业务都在中断回调里完成**。这是 42 例里第一个"主循环完全失业"的例程。

## 先解决一个经典难题：一次收多少字节算一帧？

串口是字节流，没有"消息边界"。PC 发来 `hello\n`，MCU 怎么知道"6 个字节是一帧、收完了"？传统两种方案都有硬伤：

| 方案 | 做法 | 硬伤 |
|---|---|---|
| 定长接收 | `HAL_UART_Receive` 死等收满 N 字节 | 消息比 N 短就永远卡住 |
| 逐字节中断 | 每收 1 字节进一次 ISR | 115200 下每 87µs 进一次中断，CPU 负担重 |

07 用第三种：**DMA 搬运 + 空闲（Idle）事件定帧**。

### Idle 事件：硬件天然的帧边界

UART 线路空闲时保持高电平，每个字节的起始位拉低线路。**RX 线持续空闲超过 1 个字节的时间**（10 个位周期，115200 下约 87µs），硬件置 IDLE 标志 → 触发一次事件：

```
h    e    l    l    o    \n
┌──┐ ┌──┐ ┌──┐ ┌──┐ ┌──┐ ┌──┐ ←────── 线路保持高电平
┘  └─┘  └─┘  └─┘  └─┘  └─┘  └─────────────────
                                     ↑
                          空闲 > 1 字节时间 = IDLE 事件
                          回调 Size = 6（本帧 6 字节）
```

语义就是"发完一小段、停顿一下"——和人对"一句话说完"的直觉一致。这是 AT 指令、Modbus、GPS NMEA 解析的事实标准方案；`HAL_UARTEx_ReceiveToIdle_DMA` 2019 年前后才进 HAL，现在是新项目接收设计的默认起点。

## 逐段精读 main.c

### 1. 缓冲区：一对 128 字节兄弟

```c
#define UART_DMA_BUFFER_SIZE 128U
static uint8_t g_uart_rx_buffer[UART_DMA_BUFFER_SIZE];
static uint8_t g_uart_tx_buffer[UART_DMA_BUFFER_SIZE];
```

**为什么必须是两个，不能收发共用一个？** 回调时序里藏着答案：重启 DMA 后，新到的字节立刻往 `g_uart_rx_buffer` 里写；如果发送用的也是它，**发送出去的内容和 DMA 新写入互相踩踏**，回显错乱。数据先 `memcpy` 到 tx_buffer，rx_buffer 交给 DMA 随便写，互不干扰——最小型的双缓冲分离。

128 的取值：覆盖一次终端粘贴的常见短文本。收超长消息会触发 DMA 全满事件（见下文"半满中断"），也算一帧，不至于死。

### 2. uart_restart_rx_dma：把接收通道"重新上膛"

```c
static HAL_StatusTypeDef uart_restart_rx_dma(void)
{
  HAL_StatusTypeDef status = HAL_UARTEx_ReceiveToIdle_DMA(&huart2,
                                                          g_uart_rx_buffer,
                                                          UART_DMA_BUFFER_SIZE);
  if (status == HAL_OK)
  {
    if (huart2.hdmarx != NULL)
    {
      __HAL_DMA_DISABLE_IT(huart2.hdmarx, DMA_IT_HT);   // 关半满中断
    }
  }
  return status;
}
```

**为什么要专门的"重启"函数**：usart.c 里 DMA 配的是 `DMA_NORMAL` 模式——一帧收完 DMA 停止，**接收通道此时是死的**。不重启，之后来的所有字节全部丢失。每次回调都要重新上膛，所以抽成函数，三处复用（启动、回调、错误恢复）。

**`__HAL_DMA_DISABLE_IT(..., DMA_IT_HT)` 那行的作用**：`ReceiveToIdle_DMA` 有三种触发回调的情况——

| 事件 | 触发条件 | 本例要不要 |
|---|---|---|
| IDLE | 线路空闲 > 1 字节时间 | 要（帧边界） |
| 全满 TC | 缓冲 128 字节收满 | 要（超长消息保护） |
| **半满 HT** | 缓冲收到 64 字节 | **不要，代码里关掉** |

半满本是"DMA 没停、提前通知 CPU 来处理"的保护机制，适合大缓冲连续流（如音频）。回显场景它只会把一帧拆成两次回调（64 字节时来一次"假帧"），事件流变脏。**ST 论坛被问烂的细节**：不关 HT，短消息偶尔莫名分叉；关掉后短消息只有 Idle 触发。

### 3. 回调：三步的顺序就是正确性

```c
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
  if ((huart != &huart2) || (Size == 0U))    // 判句柄（多串口共用回调）+ 判空帧
  {
    return;
  }

  const uint16_t copy_size = (Size <= UART_DMA_BUFFER_SIZE) ? Size : UART_DMA_BUFFER_SIZE;
  memcpy(g_uart_tx_buffer, g_uart_rx_buffer, copy_size);   // ① 数据搬出

  if (uart_restart_rx_dma() != HAL_OK)                     // ② 先重启接收！
  {
    Error_Handler();
  }

  if (HAL_UART_Transmit(&huart2, g_uart_tx_buffer, copy_size, HAL_MAX_DELAY) != HAL_OK)
  {
    Error_Handler();                                       // ③ 后阻塞发送
  }
}
```

**①→②→③ 的顺序是本例最精髓的一处设计**。假设把 ②③ 对调（先发送再重启）：发送 "hello\n" 6 字节耗时约 520µs（115200，10 位/字节），更长的消息按毫秒算——**这段时间 DMA 是停的，对方此时发来的任何字节全部丢失**。先重启再发送，接收窗口的关闭时间压缩到 memcpy+重启的几微秒。实时系统的通用思维：**尽快解除"不能再接收"的状态，耗时处理往后放**。

**③ 的阻塞发送在 ISR 上下文**：06_4 说"调试函数不能进中断"，这里 `HAL_UART_Transmit` 进了——矛盾吗？不矛盾，看耗时：6 字节回显 520µs，对按键/回显类业务可接受；但如果回调里是"发一条 200 字节日志"（约 17ms），中断延迟就灾难了。**判断标准永远是"在 ISR 里停了多久"，不是"能不能调"**。业务变复杂后的正确演进是：回调只置标志/入队，主循环消费（退回 03 的模式），或用 DMA 发送 `HAL_UART_Transmit_DMA` 完全异步。

`copy_size` 的 clamp（min）是冗余防御——HAL 保证 Size 不超过传入缓冲大小，写上无害，模板代码求稳。

### 4. 错误回调：让程序"打不死"

```c
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart != &huart2) return;
  if (uart_restart_rx_dma() != HAL_OK) Error_Handler();
}
```

串口错误（ORE 溢出、FE 帧错误、NE 噪声）发生后，接收链路可能瘫掉。这里不区分错误类型，一律重启接收——**长跑程序的基本韧性**：错误发生了，恢复现场继续跑，而不是死在半路。调试这类"跑一天忽然没响应"的程序，第一件事就是查错误回调有没有恢复逻辑。

### 5. main 与初始化顺序

```c
MX_GPIO_Init();
MX_DMA_Init();          // DMA 的时钟和 NVIC 先配
MX_USART2_UART_Init();  // 再配 UART（会把 DMA 挂上来）
if (uart_restart_rx_dma() != HAL_OK) Error_Handler();
uart_send_banner();
while (1) { HAL_Delay(1); }
```

**`MX_DMA_Init()` 必须在 UART 之前**（CubeMX 生成的固定顺序）：DMA 的 NVIC 中断使能要先就位，UART 初始化过程中才不会出现"DMA 已工作但中断没使能"的标志竞态。用户代码区只有两件事：启动接收、打横幅，然后主循环退役。

## 配套配置速览（原仓库 usart.c/dma.c）

- **USART2**：115200-8-N-1，PA2/PA3 复用推挽 `GPIO_AF7_USART2`（串口引脚必须配 AF 复用模式，不是普通输出——引脚的控制权交给外设）
- **DMA RX**：`DMA1_Stream5`、Channel 4、`PERIPH_TO_MEMORY`（外设→内存）、`MINC_ENABLE`（内存地址递增——逐字节写进数组的不同位置，不加会全写进第一个字节）、`DMA_NORMAL`（收完即停，故需重启）
- **DMA TX**：`DMA1_Stream6`、`MEMORY_TO_PERIPH`。注意本例发送实际用的阻塞 API，TX 的 DMA 配了但没在用户代码中使用（CubeMX 模板常态，留给 08 用 `Transmit_DMA` 时启用）
- **NVIC**：USART2_IRQn、DMA1_Stream5/6_IRQn 三个中断都使能——IDLE 事件走 UART 中断，DMA 完成/半满走 DMA 中断，两路汇入 HAL 统一分发到回调

## 与前例的连线

- **03**：中断置标志、主循环干活。07 激进版——回调里干完所有活（memcpy+重启+发送），主循环失业。能这么干是因为回显业务极短；业务一复杂就要退回标志位模式
- **04**：SysTick 心跳驱动状态机 vs 07 硬件事件驱动回调——事件驱动架构的两个样板
- **06**：管"往外发"（格式化+阻塞发送）；07 管从外收（DMA+帧边界）。一收一发合起来是 08（命令控 LED）的地基
- **串口框架**主题（知识库另有目录）的演进方向：本例双缓冲 → 环形缓冲 + 队列 + DMA 循环模式

## 值得延伸的对比：Normal vs Circular DMA

本例 Normal 模式 + 手动重启，是最易懂的形态。生产代码更常用 `DMA_CIRCULAR` 模式：DMA 收满自动回到缓冲头部继续写，永不停止，配合"读指针/写指针"的环形缓冲消费数据——接收窗口永不关闭，连重启的几微秒都省了。代价是需要自己管理读写指针的竞态。13（雷达帧解析）会给出实际用例，此处先立个坐标。

## 关联笔记

- [[06_rocketpi_uart_printf]] — 发送侧基建（uart_send_string 就是简化版 uart_puts）
- [[03_rocketpi_key_irq]] — 中断+标志位模式的起点，07 的对照面
- [[06_rocketpi_uart_printf_4|C 标准库补课]] — memcpy/strlen 在本例的直接应用
