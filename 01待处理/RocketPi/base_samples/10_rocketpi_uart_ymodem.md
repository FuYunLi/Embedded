---
status: done
created: 2026-09-12
tags:
  - stm32/uart
  - embedded/ymodem
  - embedded/protocol
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/10_rocketpi_uart_ymodem/ymodem.h"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/10_rocketpi_uart_ymodem/ymodem.c"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/10_rocketpi_uart_ymodem/ymodem_port.c"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/10_rocketpi_uart_ymodem/main.c"
---

# 10_rocketpi_uart_ymodem

> 代码：`Embedded_Code/01待处理/rocketpi/base_samples/10_rocketpi_uart_ymodem/`（ymodem.h + ymodem.c + ymodem_port.c + main.c，共约 38KB）
> 硬件：STM32F401RE（RocketPi），USART2 115200，片内 Flash Sector 6-7（256KB，0x08040000~0x08080000）
> 目标：通过串口用 Ymodem 协议传输文件——接收模式把 PC 发来的文件写入 Flash，发送模式把 MCU 里的数据发给 PC

## 交互效果

用串口助手（SecureCRT / Tera Term / minicom）的内置 Ymodem 功能：

**接收模式（默认）**：MCU 上电后发 `C` 握手 → 串口助手选文件发送 → MCU 逐块收、ACK 应答 → 传输完成，文件写入 Flash
```
YMODEM receive mode\r\n
[RX] firmware.bin  1024 / 1024  100%  avg 11.5 KB/s\r\n
[YMODEM] recv rc=0\r\n
```

**发送模式**（改宏 `#define YMODEM_MODE_SEND`）：MCU 上电后等 `C` → 把内存中的假文件按 Ymodem 协议发出 → 串口助手自动保存为文件
```
YMODEM transmit mode\r\n
[TX] rocketpi_demo.txt  1024 / 1024  100%  avg 11.5 KB/s\r\n
[YMODEM] send rc=0\r\n
```

## 在协议谱系中的位置

[[08_rocketpi_uart_control_led_4|08 的协议谱系]] 把 42 例分为文本/二进制两大门派。10 是第一个**二进制协议**实现，也是从"裸字节流"到"可靠文件传输"这条演变链的终点：

```
裸字节收发（06/07）
  ↓ 无帧边界、无校验
文本命令交互（08/09）
  ↓ 换行分帧，无校验
二进制帧协议（13 雷达帧：帧头+CRC）
  ↓ 有校验，无应答
可靠文件传输（10 Ymodem：帧头+CRC+ACK/NAK+重传+文件语义）
```

每一步都是解决上一步暴露的问题：无帧边界 → 加帧头；无校验 → 加 CRC；无应答 → 加 ACK/NAK；无文件语义 → 加 Block 0 传文件名和大小。

## 架构：四个文件的分工

| 文件 | 角色 | 与硬件的关系 |
|---|---|---|
| `ymodem.h` | 协议常量（SOH/STX/EOT/ACK/NAK/CAN/CHC）+ 五组抽象接口定义 + YContext 上下文 | 零依赖 |
| `ymodem.c` | **协议引擎**：CRC16、发送状态机（ymd_send_multi）、接收状态机（ymd_recv_multi） | **完全不碰 HAL**，只通过函数指针调用 YPort/YTimer/YStore |
| `ymodem_port.c` | **平台适配层**：STM32 UART 读写 + Flash 写入 + 组装 YContext | **唯一碰 HAL 的文件** |
| `main.c` | 极简入口：宏切发送/接收模式，调一次就进死循环 | 初始化外设 |

这个分离是 10 最值得学习的软件设计——见 [[10_rocketpi_uart_ymodem_1|依赖注入详解]]。

## Ymodem 协议帧结构

Ymodem 是 1982 年 Ward Christensen 提出的 XMODEM 的增强版。帧是二进制格式（非文本）：

```
SOH(1B) | 序号(1B) | ~序号(1B) | 数据128B | CRC16(2B)    ← 128 字节包
STX(1B) | 序号(1B) | ~序号(1B) | 数据1024B | CRC16(2B)   ← 1K 包
```

**两级校验**：序号取反（`seq + inv == 0xFF`，快速粗筛）→ CRC16-CCITT（多项式 0x1021，精确校验）。比 08/09 的文本协议（零校验）多了完整的错误检测能力。

控制字节定义在 ymodem.h：

| 常量 | 值 | 含义 | 谁发 |
|---|---|---|---|
| `YMD_SOH` | 0x01 | 128 字节数据包头 | 发送方 |
| `YMD_STX` | 0x02 | 1K 数据包头 | 发送方 |
| `YMD_EOT` | 0x04 | 传输结束（发两次） | 发送方 |
| `YMD_ACK` | 0x06 | 确认 | 接收方 |
| `YMD_NAK` | 0x15 | 否定/请求重发 | 接收方 |
| `YMD_CAN` | 0x18 | 取消传输 | 任一方 |
| `YMD_CHC` | 0x43 | `'C'`，握手就绪+请求 CRC16 | 接收方 |

## 完整传输流程

以接收模式为例（MCU 收文件）：

```
发送方（PC）                           接收方（MCU）
  │                                      │
  │    ← 'C'（我准备好了）                │  ymd_recv_multi 发 'C'
  │                                      │
  │── Block 0（文件名\0大小\0）────→     │  提取文件名+大小，ACK+'C'
  │                                      │
  │── Block 1（数据，seq=1）──────→      │  CRC 校验通过 → ACK
  │── Block 2（数据，seq=2）──────→      │  CRC 校验通过 → ACK
  │── ...                                │
  │                                      │
  │── EOT ───────────────────────→       │  第一个 EOT → NAK
  │── EOT ───────────────────────→       │  第二个 EOT → ACK+'C'
  │                                      │
  │── Block 0（空名=会话结束）───→       │  空名 → ACK，会话结束
```

三个值得观察的设计：

- **EOT 双次发送**：第一个 EOT 后回 NAK（确认意图），第二个 EOT 后回 ACK（确认结束）。防误触 EOT 导致传输中断
- **尾包填充 0x1A**：`memset(blk+r, 0x1A, plen-r)`——Ctrl+Z，来自 CP/M 时代的文件填充约定
- **空 Block 0 结束会话**：文件名为空字符串 = "没有更多文件了"。这是 Ymodem-Y batch 模式的多文件边界标记

## CRC16-CCITT 实现

```c
static uint16_t crc16_ccitt(const uint8_t* q, int len) {
    uint16_t crc = 0;
    while (len-- > 0) {
        crc ^= (uint16_t)(*q++) << 8;
        for (int i=0;i<8;i++)
            crc = (crc & 0x8000) ? (crc<<1) ^ 0x1021 : (crc<<1);
    }
    return crc;
}
```

逐字节逐位异或：对每个字节，左移 8 位与 CRC 异或，然后逐位检查最高位——最高位为 1 则左移后异或多项式 0x1021，否则只左移。这是 CRC 的最朴素实现（无查表），教学代码首选。工程级优化会用 256 项查找表把逐位循环消除，速度提升 8 倍。与 [[08_rocketpi_uart_control_led|08]] 对比：08 的文本协议没有校验——**CRC 是二进制协议的标配，文本协议的可选**。

## 平台适配层（ymodem_port.c）

### 串口适配（STM32Serial）

```c
static int stm32_putc(YPort* p, uint8_t ch){
    STM32Serial* s=(STM32Serial*)p->self;   // 从接口取私有数据
    HAL_UART_Transmit(s->huart, &ch, 1, 1000);
}
```

`p->self` 指向 `STM32Serial` 结构体（存 `UART_HandleTypeDef*`），这是 C 里模拟面向对象私有成员的标准手法。`stm32_read_exact` 实现"N 字节精确读取+总超时"：循环调 HAL_UART_Receive 逐字节收，HAL_GetTick 跟踪总耗时——二进制协议的典型读取方式，不是行缓冲。

### Flash 写入

接收模式写入 STM32 片内 Flash Sector 6-7（256KB）：

- `flash_open_write`：首次调用擦除 Sector 6+7，后续从上次结束位置继续写
- `flash_write_bytes`：逐字节 `HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, ...)`
- `flash_close_write`：`ok=1` 推进游标；`ok=0` 丢弃（传输失败不写入）

**发送侧的"假文件"**：`dummy_open_read` 把编译时写死的字符串 `g_dummy_payload`（约 1KB 测试文本）当文件发送——没有文件系统时，内存字符串就是最小"文件"。

## 当前实现的水平

| 维度 | 当前状态 | 工程级应有 |
|---|---|---|
| 协议正确性 | 完整（握手/CRC/ACK-NAK/EOT 双次/多文件会话） | 同 |
| 阻塞/非阻塞 | 阻塞（所有 I/O 走 HAL 阻塞接口） | 非阻塞（状态机 + 中断/DMA） |
| 存储 | 片内 Flash 256KB | SD 卡 / LittleFS / 外部 Flash |
| 发送侧文件来源 | 内存假数据 | 文件系统读取 |
| 文件名处理 | 接收时忽略、发送时硬编码 | 动态传入 |
| 平台抽象 | 优秀（函数指针依赖注入） | 同 |

**协议级完整、应用级最小**——协议引擎本身是生产级分离设计，但存储和文件来源是最小教学实现。

## 文件的概念：MCU 不关心后缀

一个文件 = 文件名 + 大小 + 内容字节。操作系统负责"取名字存磁盘"，MCU 没有文件系统——Flash 里就是一坨连续字节。Ymodem 传输时，**文件名和大小只是 Block 0 里的两个字符串**（如 `firmware.bin\012345\0`），MCU 可以用也可以忽略。当前例程的 `flash_open_write` 就忽略了文件名——对这台 MCU 来说，文件就是 Flash 里从 0x08040000 开始的一坨字节。

txt 和 bin 在传输时都是字节流，Ymodem 不会因后缀不同而区别处理。区别在"使用方"：PC 端收到后根据 Block 0 里的文件名保存为对应文件；MCU 端存进 Flash 后，应用层决定当字符串打印还是当固件执行。**MCU 不需要"解码器"——解码是应用程序的事，不是传输协议的事。**

详细讨论见 [[10_rocketpi_uart_ymodem_2|Ymodem 与文件传输概念]]。

## 与前例连线

- [[07_rocketpi_uart_echo]] — DMA 接收，10 的接收侧可替换为 DMA+Idle 版
- [[08_rocketpi_uart_control_led_4|串口文本协议谱系]] — 10 在谱系中"二进制可靠传输"位置的定位
- [[08_rocketpi_uart_control_led|08 主笔记]] — 文本协议对照基线（零校验 vs CRC16）
- [[09_rocketpi_uart_control_led_cjson_1|cJSON 背景与 API]] — 另一种"协议引擎与平台分离"的设计参照

## 值得记住的三个设计词

- **依赖注入**：协议引擎通过函数指针调用平台能力（YPort/YTimer/YStore），换平台只需重写 port 文件
- **两级校验**：序号取反快速粗筛 + CRC16 精确校验，先廉价后昂贵的错误检测策略
- **文件语义层**：Block 0 传文件名和大小——传输协议在"字节流"之上叠加了一层"文件"抽象
