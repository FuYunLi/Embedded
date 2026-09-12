---
status: done
created: 2026-09-12
tags:
  - stm32/uart
  - embedded/logging
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/12_rocketpi_uart_easylogger/main.c"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/12_rocketpi_uart_easylogger/elog_port.c"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/12_rocketpi_uart_easylogger/elog.c"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/12_rocketpi_uart_easylogger/elog.h"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/12_rocketpi_uart_easylogger/elog_cfg.h"
---

# 12_rocketpi_uart_easylogger

> 代码：`Embedded_Code/01待处理/rocketpi/base_samples/12_rocketpi_uart_easylogger/`（elog.h + elog_cfg.h + elog.c + elog_async.c + elog_buf.c + elog_utils.c + elog_port.c + main.c，共约 60KB）
> 硬件：STM32F401RE（RocketPi），USART2 115200，DMA 发送
> 目标：移植 EasyLogger 日志框架，通过 DMA UART 输出带级别/标签/时间戳/颜色的格式化日志

## 交互效果

```
EasyLogger V2.2.99 is initialize success.
EasyLogger raw log output demo.
A/main [2] Assert level demo log.
E/main [3] Error level demo log.
W/main [4] Warn level demo log.
I/main [5] Info level demo log.
D/main  [6] (main.c:89 elog_demo_all_levels) Debug level demo log.
V/main  [7] (main.c:90 elog_demo_all_levels) Verbose level demo log.
I/main [1007] Heartbeat 0
I/main [2008] Heartbeat 1
I/main [3009] Heartbeat 2
```

每个级别独立配置输出格式——ERROR/WARN/INFO 只显示级别+标签+时间（精简），DEBUG/VERBOSE 还显示文件名+行号+函数名（详细）。终端下有 ANSI 颜色：Assert=品红、Error=红、Warn=黄、Info=青、Debug=绿、Verbose=蓝。

## 在日志演进中的位置

```
06 手写 uart_printf / hexdump
  ↓ 无级别、无过滤、无颜色、阻塞发送
12 EasyLogger
  ✓ 6 级独立格式配置
  ✓ 级别/标签/关键词/标签级别 四级过滤
  ✓ ANSI 颜色输出
  ✓ DMA 发送+降级阻塞
  ✓ 线程安全（自旋锁/RTOS 互斥）
  ✓ hexdump（带日志前缀+过滤）
  ✓ 异步输出模式（可选，需 RTOS）
```

06 教你怎么手写 printf 重定向和 hexdump，12 教你怎么用一个完整的日志框架。两者互补：理解 06 的底层原理后，用 12 的框架提高开发效率。

## 架构：文件分工

| 文件 | 角色 | 需要改吗 |
|---|---|---|
| `elog_cfg.h` | 配置宏（缓冲大小/级别/颜色/格式/异步模式） | **按需改** |
| `elog.h` | 库接口定义 + 级别常量 + 格式位掩码 + 日志宏 | 不改 |
| `elog.c` | 核心（~800 行）：格式化/过滤/锁/hexdump | 不改 |
| `elog_async.c` | 异步输出：环形缓冲 + 线程/轮询输出 | 不改 |
| `elog_buf.c` | 缓冲输出模式 | 不改 |
| `elog_utils.c` | 工具函数（strcpy/cpyln） | 不改 |
| `elog_port.c` | **平台适配层**（~120 行）：输出/锁/时间/进程信息 | **要写** |
| `main.c` | 应用层：初始化配置 + demo | 要写 |

和 [[11_rocketpi_uart_shell_microrl|11 的 microrl]]、[[10_rocketpi_uart_ymodem|10 的 Ymodem]] 同模式：**库引擎不动，port 文件适配硬件，main.c 注册配置**。

## 初始化流程

```c
elog_init();                    // ① 初始化（内部调 elog_port_init）
elog_set_fmt(ELOG_LVL_ASSERT, ELOG_FMT_ALL & ~ELOG_FMT_P_INFO);   // ② 按级别配格式
elog_set_fmt(ELOG_LVL_ERROR,  ELOG_FMT_LVL | ELOG_FMT_TAG | ELOG_FMT_TIME);
elog_set_fmt(ELOG_LVL_DEBUG,  ELOG_FMT_ALL & ~(ELOG_FMT_FUNC | ELOG_FMT_P_INFO));
...
elog_start();                   // ③ 启用输出
elog_i("main", "EasyLogger initialized. DMA UART logging ready.");  // ④ 第一条日志
```

三步走：`elog_init`（初始化 port + 异步模块）→ `elog_set_fmt`（按级别配格式）→ `elog_start`（启用输出）。`elog_start` 之前的所有日志不会输出——防止初始化阶段的日志干扰。

## 日志 API

| 宏 | 级别 | 缩写 | 输出前缀 |
|---|---|---|---|
| `elog_a(tag, fmt, ...)` | Assert | `A/` | 品红 |
| `elog_e(tag, fmt, ...)` | Error | `E/` | 红 |
| `elog_w(tag, fmt, ...)` | Warn | `W/` | 黄 |
| `elog_i(tag, fmt, ...)` | Info | `I/` | 青 |
| `elog_d(tag, fmt, ...)` | Debug | `D/` | 绿 |
| `elog_v(tag, fmt, ...)` | Verbose | `V/` | 蓝 |
| `elog_raw(fmt, ...)` | Raw | 无前缀 | 无颜色 |

还有短写法：在文件顶部定义 `#define LOG_TAG "sensor"` 和 `#define LOG_LVL ELOG_LVL_DEBUG`，之后可以直接用 `log_i("temp: %d", val)` ——省掉每次写 tag。

## 过滤系统（四级）

```c
elog_set_filter_lvl(ELOG_LVL_INFO);              // 级别过滤：DEBUG/VERBOSE 不输出
elog_set_filter_tag("sensor");                    // 标签过滤：只输出含 "sensor" 的标签
elog_set_filter_kw("error");                      // 关键词过滤：只输出含 "error" 的行
elog_set_filter_tag_lvl("driver", ELOG_LVL_ERROR);// 标签级别过滤：driver 标签只输出 ERROR 以上
```

四级过滤在 `elog_output` 里依次检查：级别 → 标签级别 → 标签子串 → 关键词。任一级不通过直接 return，不走格式化和输出——**过滤在格式化之前，省 CPU**。

## 格式位掩码

```c
ELOG_FMT_LVL     // 级别标志（A/E/W/I/D/V）
ELOG_FMT_TAG     // 标签名
ELOG_FMT_TIME    // 时间戳
ELOG_FMT_P_INFO  // 进程信息（RTOS 下有意义）
ELOG_FMT_T_INFO  // 线程信息（RTOS 下有意义）
ELOG_FMT_DIR     // 文件路径
ELOG_FMT_FUNC    // 函数名
ELOG_FMT_LINE    // 行号
```

每个级别独立配置：`elog_set_fmt(ELOG_LVL_INFO, ELOG_FMT_LVL | ELOG_FMT_TAG | ELOG_FMT_TIME)` 表示 INFO 级别只显示级别+标签+时间。用位掩码组合，`ELOG_FMT_ALL` 是全部开启，`ELOG_FMT_ALL & ~ELOG_FMT_FUNC` 是全部开启但去掉函数名。

## elog_port.c：六个 port 函数

EasyLogger 要求用户实现六个 port 函数。12 给出了 STM32 HAL 版本：

### elog_port_output：DMA 发送 + 降级

```c
void elog_port_output(const char *log, size_t size) {
    while (size > 0U) {
        uint16_t chunk = (size > 0xFFFFU) ? 0xFFFFU : (uint16_t)size;
        wait_uart_idle();                  // 等上一次 DMA 发完
        if (uart_tx_dma(cur, chunk)) {
            wait_uart_idle();              // 等本次 DMA 发完
        } else {
            HAL_UART_Transmit(&huart2, cur, chunk, HAL_MAX_DELAY);  // DMA 失败降级阻塞
        }
        cur += chunk;
        size -= chunk;
    }
}
```

**分块发送**：DMA 长度参数是 uint16_t，超长时分块。**DMA 失败降级**：宁可慢也不丢日志——日志系统的核心需求。`wait_uart_idle` 是忙等 `uart_dma_idle` 标志（TxCpltCallback 里置 true），当前实现是阻塞式的。

### elog_port_output_lock / unlock：裸机自旋锁

```c
void elog_port_output_lock(void) {
    while (1) {
        uint32_t primask = __get_PRIMASK();
        __disable_irq();
        if (!elog_lock_flag) {
            elog_lock_flag = 1;
            if (!primask) __enable_irq();
            break;
        }
        if (!primask) __enable_irq();
    }
}
```

关中断检查标志位，没锁住就抢占，锁住了就开中断继续等。`__get_PRIMASK` / `__enable_irq` 的配对保证：调用前中断已关则解锁时不开——**尊重调用方的中断状态**。RTOS 环境下换成 `osMutexAcquire` / `osMutexRelease`。

### elog_port_get_time：HAL_GetTick 时间戳

```c
const char *elog_port_get_time(void) {
    static char cur_time[16];
    snprintf(cur_time, sizeof(cur_time), "%lu", HAL_GetTick());
    return cur_time;
}
```

返回毫秒时间戳字符串。`static` 缓冲保证返回值有效。

### elog_port_get_p_info / get_t_info：RTOS 钩子

裸机下返回空字符串。RTOS 下可返回当前任务名。

## elog_cfg.h 关键配置

| 配置项 | 当前值 | 含义 |
|---|---|---|
| `ELOG_OUTPUT_LVL` | `ELOG_LVL_VERBOSE` | 静态编译级别（低于此级别的日志代码不编译） |
| `ELOG_LINE_BUF_SIZE` | 1024 | 每行日志最大长度 |
| `ELOG_FILTER_TAG_MAX_LEN` | 30 | 标签最大长度 |
| `ELOG_NEWLINE_SIGN` | `"\r\n"` | 换行符（串口终端需要 \r\n） |
| `ELOG_COLOR_ENABLE` | 定义 | 启用 ANSI 颜色 |
| `ELOG_FMT_USING_FUNC/LINE/DIR` | 定义 | 启用函数名/行号/文件路径 |
| `ELOG_ASYNC_OUTPUT_ENABLE` | 注释 | 异步模式未启用（需 RTOS 线程） |

**静态编译级别**：`ELOG_OUTPUT_LVL` 控制哪些日志宏会被编译。设为 `ELOG_LVL_INFO` 时，`elog_d` / `elog_v` 展开为空——DEBUG/VERBOSE 日志不占 Flash。发布时改这一行就能去掉所有调试日志。

## EasyLogger 核心能力（elog.c 概览）

elog.c 约 800 行，核心是 `elog_output` 函数——所有日志宏最终都调它：

```
elog_i("main", "temp: %d", val)
  → elog_output(ELOG_LVL_INFO, "main", __FILE__, __FUNCTION__, __LINE__, "temp: %d", val)
    → 检查 output_enabled
    → 检查级别过滤（global + tag_level）
    → 检查标签过滤（strstr）
    → 加锁
    → 拼装前缀：[颜色] + 级别 + 标签 + [时间 进程 线程] + (文件 行号 函数) + 用户格式
    → vsnprintf 格式化用户内容
    → 检查关键词过滤
    → 加颜色结尾 + 换行
    → 输出（同步/异步/缓冲三选一）
    → 解锁
```

**hexdump**：`elog_hexdump("sensor", 16, buf, len)` 输出和 [[06_rocketpi_uart_printf_3|06_3 hexdump]] 相同格式，但自动带 `D/HEX` 前缀、标签过滤、颜色。

## 与 06 uart_printf 的对比

| 维度 | 06 uart_printf | 12 EasyLogger |
|---|---|---|
| 输出方式 | 阻塞 HAL_UART_Transmit | DMA 发送+降级阻塞 |
| 格式化 | 手写 vsnprintf | 库内部 vsnprintf + 前缀拼装 |
| 级别 | 无 | 6 级独立配置 |
| 过滤 | 无 | 级别/标签/关键词/标签级别 |
| 颜色 | 无 | ANSI 颜色（可选） |
| 线程安全 | 无 | 自旋锁/RTOS 互斥 |
| hexdump | 手写 uart_hexdump | elog_hexdump（带过滤） |
| 异步 | 无 | 环形缓冲+线程/轮询 |
| port 函数 | 无（直接调 HAL） | 6 个 port 函数适配平台 |

## 移植到新工程

移植 EasyLogger 只需三步：

1. **拷贝文件**：elog.h + elog_cfg.h + elog.c + elog_async.c + elog_buf.c + elog_utils.c 到工程
2. **改 elog_cfg.h**：调缓冲大小、级别、颜色、换行符
3. **写 elog_port.c**：实现六个 port 函数（输出/锁/时间/进程/线程）

elog_port.c 的输出函数可以对接任何传输方式：UART DMA（当前）、UART 阻塞、RTT（Segger）、SWO（ITM）、文件系统、网络 socket——**EasyLogger 不关心日志去哪，只管格式化和过滤**。

## 缺失文件已补齐

`elog.h` 和 `elog_cfg.h` 已从原仓库迁移到 Embedded_Code。现在 12 目录有完整八个文件。

## 与前例连线

- [[06_rocketpi_uart_printf|06 主笔记]] — 手写 printf 重定向和 hexdump，12 的前置知识
- [[06_rocketpi_uart_printf_3|06_3 hexdump 详解]] — elog_hexdump 的格式与原理出处
- [[11_rocketpi_uart_shell_microrl|11 主笔记]] — 同为"库移植+port 文件"模式
- [[10_rocketpi_uart_ymodem_1|10 依赖注入]] — elog_port 的六个函数是另一种"平台抽象"

## 值得记住的三个设计词

- **port 函数模式**：库定义接口（elog_port_output 等），用户填实现——和 10 的 YPort/YStore 同思想，是嵌入式库的标准移植方式
- **静态编译过滤**：`ELOG_OUTPUT_LVL` 让低于某级别的日志代码不进二进制——零运行时开销，比运行时过滤更彻底
- **DMA 发送+降级阻塞**：正常走 DMA（快），DMA 失败走阻塞（稳）——"宁可慢也不丢"的日志系统设计原则
