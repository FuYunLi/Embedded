---
status: done
created: 2026-09-12
tags:
  - c/architecture
  - embedded/ymodem
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/10_rocketpi_uart_ymodem/ymodem.h"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/10_rocketpi_uart_ymodem/ymodem_port.c"
---

# 依赖注入：ymodem.h 的五组接口与 ymodem_port.c 的适配实现

> [[10_rocketpi_uart_ymodem|10 主笔记]] 提到"协议引擎完全不碰 HAL"——这篇拆解这层隔离是怎么实现的。ymodem.h 定义了五组函数指针接口，ymodem_port.c 填入 STM32 的具体实现，ymodem.c 通过接口间接调用。这就是 C 语言的依赖注入。

## 为什么需要分离

把协议引擎和硬件绑在一起（如 ymodem.c 直接调 HAL_UART_Transmit）的问题：

1. **换平台要改协议代码**——换到 Linux 串口、换到 FreeRTOS、换到另一个 MCU，ymodem.c 要逐行改 HAL 调用
2. **无法独立测试**——协议逻辑的单元测试必须连硬件，无法在 PC 上跑
3. **多人协作冲突**——改 HAL 配置的人可能误碰协议逻辑

分离后：**ymodem.c 是纯算法，不含任何平台代码**。换平台只写一个新的 port 文件，协议引擎一字不改。这就是依赖注入（Dependency Injection）的核心思想——被调用方不主动创建依赖，而是由外部注入。

## 五组接口的设计

ymodem.h 用 C 结构体+函数指针模拟面向对象的接口（纯虚函数）。每个接口有一个 `void* self` 成员，指向平台私有数据——相当于面向对象的 `this` 指针。

### YPort——串口读写

```c
typedef struct YPort {
    void*  self;                            // 平台私有数据（STM32Serial*）
    int  (*open)(struct YPort*, const char* name, uint32_t baud);
    void (*close)(struct YPort*);
    int  (*putc)(struct YPort*, uint8_t ch);     // 发 1 字节
    int  (*getc)(struct YPort*, uint8_t* ch, int ms); // 收 1 字节（带超时）
    int  (*write)(struct YPort*, const void* buf, int len); // 发 N 字节
    int  (*read_exact)(struct YPort*, void* buf, int len, int ms); // 精确收 N 字节
} YPort;
```

**作用**：协议引擎所有串口 I/O 都通过这个接口。`putc`/`getc` 是单字节操作（用于发送控制字节、接收 ACK/NAK），`write`/`read_exact` 是批量操作（用于发送数据块、接收数据块）。

**`getc` 的超时参数 `ms`**：这是二进制协议的关键——不是无限等（08 的 HAL_MAX_DELAY），而是"最多等 ms 毫秒，没收到就返回 0"。超时是协议错误恢复的前提：等不到 ACK 就重发，等不到数据就放弃。

### YTimer——时间抽象

```c
typedef struct YTimer {
    void* self;
    uint32_t (*now_ms)(void);       // 当前时间戳（毫秒）
    void     (*sleep_ms)(int ms);   // 延时
} YTimer;
```

**作用**：进度计算、超时判断、握手等待都依赖时间。裸机用 `HAL_GetTick`，RTOS 用 `xTaskGetTickCount()`，PC 测试用 `gettimeofday`。

### YStore——存储抽象

```c
typedef struct YStore {
    void* self;
    // 发送侧（读文件）
    void*   (*open_read)(const char* path);
    int     (*read)(void* fh, void* buf, int len);
    int     (*seek)(void* fh, int64_t off, int whence);   // 可选
    int64_t (*tell)(void* fh);                            // 可选
    int64_t (*size)(void* fh);                            // 可选
    void    (*close_read)(void* fh);
    // 接收侧（写文件）
    void*   (*open_write)(const char* out_dir, const char* name);
    int     (*write)(void* fh, const void* buf, int len);
    void    (*close_write)(void* fh, int ok);   // ok=1 正常，ok=0 异常
} YStore;
```

**作用**：协议引擎不关心数据从哪来、存到哪去。发送侧的 `open_read` / `read` / `close_read` 被 `ymd_send_multi` 调用；接收侧的 `open_write` / `write` / `close_write` 被 `ymd_recv_multi` 调用。

**`close_write` 的 `ok` 参数**：传输成功时 `ok=1`（Flash 版推进游标），失败时 `ok=0`（Flash 版丢弃数据）。**关闭语义由调用方控制**，存储层不需要知道传输是否成功。

**可选函数（seek/tell/size）**：用 NULL 表示不支持。`ymd_send_multi` 里有 `if (ctx->store->size) fsz=ctx->store->size(f);` 的可选调用——协议引擎适应存储层的能力，不强求。

### YHooks——回调钩子

```c
typedef struct YHooks {
    void (*on_log)(const char* s);
    void (*on_progress)(const char* tag, const char* name, uint64_t done, int64_t total);
} YHooks;
```

**作用**：日志输出和进度显示。ymemd.c 里 `ylog(ctx, "[TX] ...")` 和 `yprog(ctx, ...)` 都通过钩子输出——不直接调 printf，因为不同平台的日志方式不同（串口、RTT、文件、UI）。当前 port 文件的 `cli_log` 直接调 `printf`，`cli_prog` 是空实现（进度打印在 ymodem.c 内部的 prog_tick 里做）。

### YConfig——配置参数

```c
typedef struct YConfig {
    int rx_timeout_ms;      // 单步读超时，默认 3000
    int hs_total_ms;        // 握手总时长，默认 20000
    int retry_max;          // 数据块重试次数，默认 10
    int packet_prefer_1k;   // 1=1K 包，0=128B 包
} YConfig;
```

**作用**：把可调参数集中管理，不硬编码在协议引擎里。不同场景（高速本地传输 vs 低速无线链路）需要不同的超时和重试策略。

## ymodem_port.c 的组装：ymodem_make_ctx

```c
static void ymodem_make_ctx(YContext* out_ctx, UART_HandleTypeDef* huart){
    static STM32Serial ss;
    static YPort  port;
    static YTimer timer;
    static YStore store;

    ss.huart = huart;

    port.self = &ss;
    port.open = stm32_open;
    port.close= stm32_close;
    port.putc = stm32_putc;
    port.getc = stm32_getc;
    port.write= stm32_write;
    port.read_exact = stm32_read_exact;

    timer.self = NULL;
    timer.now_ms = stm32_now_ms;
    timer.sleep_ms = stm32_sleep_ms;

    store.self = NULL;
    store.open_read  = dummy_open_read;     // 发送侧：内存假数据
    store.read       = dummy_read_bytes;
    store.seek       = dummy_seek_any;
    store.tell       = dummy_tell_any;
    store.size       = dummy_size_any;
    store.close_read = dummy_close_read;

    store.open_write = flash_open_write;    // 接收侧：Flash 写入
    store.write      = flash_write_bytes;
    store.close_write= flash_close_write;

    out_ctx->port  = &port;
    out_ctx->timer = &timer;
    out_ctx->store = &store;
    out_ctx->hooks.on_log = cli_log;
    out_ctx->hooks.on_progress = cli_prog;
    out_ctx->cfg.rx_timeout_ms    = 3000;
    out_ctx->cfg.hs_total_ms      = 20000;
    out_ctx->cfg.retry_max        = 10;
    out_ctx->cfg.packet_prefer_1k = 1;
}
```

**这是依赖注入的"组装点"**——所有平台特定的实现在这里绑定到接口上，然后 YContext 传给协议引擎。`static` 变量保证生命周期覆盖整个传输过程（栈上变量会随函数返回消失）。

**发送侧和接收侧用不同的 store 实现**：发送时读内存（dummy），接收时写 Flash——协议引擎不关心这个差异，它只调 `store->open_read` / `store->write` 等统一接口。

## C 语言的"接口"模式

C 没有 `interface` 关键字，但结构体+函数指针可以精确模拟：

| 面向对象概念 | C 实现 |
|---|---|
| 接口（interface） | 结构体里全是函数指针（YPort / YTimer / YStore） |
| 实现（implementation） | 具体的 static 函数（stm32_putc / stm32_getc） |
| 注入（injection） | 组装函数把实现赋给接口（ymodem_make_ctx） |
| this 指针 | `void* self` 成员 |
| 依赖反转 | 被调用方（ymodem.c）只依赖接口，不依赖实现 |

这个模式在嵌入式 C 项目中极其常见：HAL 本身就是这个模式（`UART_HandleTypeDef` 里的回调函数）、FreeRTOS 的 hooks、驱动框架的 vtable。10 是初学者第一次见到它被系统使用。

## 这个分离带来的实际好处

**可测试性**：ymodem.c 的 CRC16、帧组装、状态机可以在 PC 上用 mock 的 port/timer/store 做单元测试——写一个 PC 的 port 实现（读写文件描述符），不需要 STM32 硬件。

**可移植性**：换到 Linux 只需写一个新 port（open/read/write 走 POSIX 文件描述符，putc/getc 走 termios），ymodem.c 一字不改。

**可替换存储**：当前用 Flash，换成 SD 卡只改 store 的三个函数（open_write/write/close_write），不影响协议引擎。LittleFS、FATFS、外部 SPI Flash 都是同理。

## 关联笔记

- [[10_rocketpi_uart_ymodem]] — 主笔记：协议帧结构与传输流程
- [[10_rocketpi_uart_ymodem_2|Ymodem 与文件传输概念]] — 文件语义层的讨论
- [[09_rocketpi_uart_control_led_cjson_1|cJSON 背景与 API]] — 另一种"引擎与平台分离"的参照（cJSON 的 Hooks 机制）
- [[08_rocketpi_uart_control_led_2|08 数据结构与 API 设计]] — 依赖单向流动的设计原则
