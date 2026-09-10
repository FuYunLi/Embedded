---
status: todo
created: 2026-09-10
tags:
  - c/stdio
  - stm32/uart
  - rocketpi/base_samples
  - todo
references:
  - "[[06_rocketpi_uart_printf]]"
---

# 06_rocketpi_uart_printf_1

> 进阶补充 1：printf 从 C 标准库到串口的完整链路。解释标准 C、GCC/newlib、Keil 的关系与区别。
> 前置笔记：[[06_rocketpi_uart_printf]] §stdio 重定向

## 先分清三个概念：标准 C、GCC、Keil

初学嵌入式最容易被这三个词绕晕，先捋清关系：

**标准 C（C 标准）**：一份"语言说明书"，规定 `printf`、`int` 多大、指针怎么运算等。但它只规定**行为**，不提供**实现**——`printf` 内部怎么写屏幕？标准不管，留给了"实现者"。

**编译器套件（实现者）**：把 C 代码翻译成机器码，并附带一套**C 标准库的实现**（printf 的真身就在这里）：

| 编译器 | 厂商 | 附带的 C 库 | 常见 IDE |
|---|---|---|---|
| **GCC (arm-none-eabi-gcc)** | 开源 | **newlib**（专为嵌入式裁剪的 C 库） | STM32CubeIDE、PlatformIO |
| **ARMCC (armclang)** | ARM 官方 | **Arm C 库**（标准版或 MicroLIB 精简版） | Keil MDK |

同一份标准 C 代码，两个编译器都能编过（行为一致）；但 printf **底层落地的钩子不一样**——这就是 06 代码里 `#ifdef __GNUC__` 分叉的原因。

**另一个易混点：交叉编译**。PC 上编 PC 程序，编译器产出 x86 机器码；嵌入式开发是在 PC（x86）上编出 STM32（ARM）机器码，这叫交叉编译，工具链前缀 `arm-none-eabi-` 就是"目标 ARM 平台、无操作系统（bare-metal）"的意思。C 库也必须是给 ARM 用的（newlib），不是 PC 上那套 glibc。

## printf 的下落之旅（GCC/newlib 路线）

```
printf("hi %d", 42)
    │
    ▼
vfprintf（C 库内部：解析 %d，把 42 变字符 '4''2'，拼成 "hi 42"）
    │
    ▼
putchar / fwrite 等流操作（C 库内部）
    │
    ▼
_write(fd, buf, len)   ←—— 弱符号桩，默认实现是"什么也不做/触发半主机"
    │                      你重写它，printf 的输出就改道了
    ▼
HAL_UART_Transmit(&huart2, ...)  →  USART2 TX 引脚 → PC 串口助手
```

**`_write` 是 newlib 预留的系统调用出口**。newlib 本想跑在带操作系统的环境里（Linux 上 `_write` 由内核实现），裸机上没有内核，ST 在启动文件里把它做成弱符号（`__attribute__((weak))`）——用户不写就是空操作，**用户一写，同名强符号覆盖它**。printf 全家（printf/puts/fwrite/perror……）最终都汇到 `_write`，所以重定向一个函数，整个 stdio 都通。

### 系统调用桩全家桶

`_write` 不是孤例。newlib 在裸机上预留了一整组桩，默认全空，用到才需要补：

| 桩函数 | 被谁调用 | 作用 |
|---|---|---|
| `_write` | printf/puts | 输出重定向（本例主角） |
| `_read` | scanf/getchar | 输入重定向（06 里同步实现了） |
| `_sbrk` | malloc | 堆内存伸缩，malloc/free 必须有它 |
| `_exit` | exit/abort | 程序退出（裸机上一般是死循环） |
| `_kill`/`_getpid` | 信号相关 | 裸机通常给空壳应付链接 |

**调试建议**：MCU 上跑 `malloc` 报链接错误或卡死，十有八九是没写 `_sbrk`。看到 `undefined reference to _sbrk` 就是 newlib 在找桩没找到。

## Keil/ARMCC 路线：为什么是 fputc

Arm C 库不用 `_write` 这套 POSIX 风格接口，而是老式的单字符桩：

```
printf("hi %d", 42) → vfprintf 内部 → 每格式化出一个字符调一次 fputc(ch, f)
```

- **粒度不同**：GCC 一次给整段缓冲，Keil 一次给一个字符。输出 100 字符，Keil 要调 100 次 `HAL_UART_Transmit`，每次带函数开销——这就是 Keil 下 printf 更慢的原因
- **semihosting（半主机）陷阱**：Arm C 库默认假定"调试器在旁边"，未重定向的 printf 会通过调试器把输出送到 PC 窗口。**不接调试器直接跑板子，printf 触发 BKPT 指令直接停机**。所以 06 代码里必须：

```c
#pragma import(__use_no_semihosting)   // 声明不用半主机
struct __FILE { int handle; };          // 自备 FILE 结构（库不再提供）
FILE __stdout;                          // 自备标准输出对象
void _sys_exit(int x) { (void)x; }      // 接住库对退出的引用
```

这四件套是 Keil 重定向的固定仪式，初学者照抄即可，原理等踩到"拔了调试器 printf 就死机"自然懂。

- **MicroLIB**：Keil 勾选的精简 C 库（代码小、无半主机依赖），勾了它连上面的仪式都简化。06 代码 `#ifndef __MICROLIB` 就是区分"标准库要仪式 / MicroLIB 免仪式"两种情形。

## 两条路线对照总结

| | GCC + newlib | Keil + Arm C 库 |
|---|---|---|
| 重定向入口 | `_write`（整段） | `fputc`（单字符） |
| 输入 | `_read` | `fgetc` |
| 半主机问题 | 无（默认不依赖调试器） | 有，需 `__use_no_semihosting` 仪式 |
| C 库体积 | newlib 可裁剪（nano 版更小） | 标准库大，MicroLIB 小但功能少 |
| 效率 | 高（缓冲聚合） | 低（逐字符） |

**为什么 06 的 main 不用 printf 而自研 uart_printf**：重定向路线把格式化引擎（newlib 的 vfprintf，数 KB~十几 KB Flash）整个拖进固件，且超时行为不受控（`HAL_MAX_DELAY` 永等）。自研 `uart_printf` 只用 `vsnprintf`（仍在 C 库里但可控）+ 自己的发送与超时策略，代价小、行为明确——调试组件"自己造轮子"是嵌入式常态。

## 关联笔记

- [[06_rocketpi_uart_printf]] — 总览与本篇的母体，§stdio 重定向是本篇的入口
- [[06_rocketpi_uart_printf_2|变参函数原理]] — printf 家族如何实现"参数个数不定"
