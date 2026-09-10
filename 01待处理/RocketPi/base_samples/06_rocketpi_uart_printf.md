---
status: todo
created: 2026-09-10
tags:
  - stm32/uart
  - rocketpi/base_samples
  - todo
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/06_rocketpi_uart_printf/main.c"
---

# 06_rocketpi_uart_printf

> 代码：`Embedded_Code/01待处理/rocketpi/base_samples/06_rocketpi_uart_printf/`（main.c、debug_driver.c）
> 硬件：STM32F401RE（RocketPi），USART2 接串口调试器（USB 转串口），PC 端串口助手 115200-8-N-1 查看
> 目标：让单片机像 PC 程序一样能 printf，并建立一套自己的调试输出工具

## 一图看懂本例在做什么

```
main.c 调用                       debug_driver.c                    硬件
──────────                       ──────────────                    ────
uart_printf("value=%d", val)
        │
        ├─ vsnprintf 把参数拼成字符串 ──→ 格式化引擎（C 库）
        │
        └─ uart_write_with_crlf ────→ HAL_UART_Transmit ───→ USART2 TX 引脚
                                                                     │
PC 串口助手 ←──────────────────────────── USB 转串口 ←──────────────┘
```

本例三层结构：**应用层调用（main）→ 调试驱动层（debug_driver）→ HAL/硬件（USART2）**。这个分层是嵌入式调试组件的标准形态。

## 按代码顺序走一遍

### 1. main.c：初始化与调用点

```c
MX_GPIO_Init();
MX_USART2_UART_Init();     // CubeMX 生成的串口初始化（在 usart.c，本目录缺失）

uart_puts("Hello, UART!\n");                        // ① 纯字符串
uart_printf("value=%d, hex=0x%X\n", val, val);      // ② 格式化
uart_hexdump(rxbuf, sizeof(rxbuf), "RX BUF");       // ③ 十六进制转储
HAL_Delay(1000);                                    // 每秒一轮
```

三个函数就是三种调试输出形态：字符串、格式化文本、二进制内容可视化。`usart.c/h`（波特率等配置）本目录缺失，在原仓库。

### 2. debug_driver.c 顶部：stdio 重定向（可选能力）

```c
#ifdef __GNUC__                    // GCC：重定向 _write，一次拿整段缓冲
int _write(int file, char *ptr, int len) {
    HAL_UART_Transmit(&huart2, (uint8_t*)ptr, len, HAL_MAX_DELAY);
    return len;
}
#elif defined(__ARMCC_VERSION)     // Keil：重定向 fputc，一次一个字符
int fputc(int ch, FILE *f) { ... }
#endif
```

这段让 C 库的 `printf`/`puts` 等函数"知道"往 USART2 输出。**本例 main 里其实没启用 printf**（注释掉了），改用自研 uart_printf——重定向是备用能力，两条路都看懂才算掌握：

- GCC/newlib：printf → ... → `_write(fd, buf, len)`，整段缓冲一次发出，效率高
- Keil/ARMCC：printf → ... → `fputc(ch, f)`，逐字符调用，开销大；还需 `#pragma import(__use_no_semihosting)` 断开"半主机"依赖（否则不接调试器时 printf 死机）

### 3. 配置区：三根"可调旋钮"

```c
#define UART_LOG_INSTANCE  huart2    // 换串口只改这里
#define UART_LOG_TIMEOUT   1000      // 单次发送最长等 1s，防止硬件坏了卡死程序
#define UART_LOG_BUF_SIZE  256       // 格式化缓冲大小，决定单条日志最长
```

用 `#ifndef` 包裹是为了允许外部覆盖（编译选项里 -D 重定义）。

### 4. 底层发送：阻塞式一字节流

```c
static inline void uart_write_blocking(const uint8_t *data, size_t len)
{
    HAL_UART_Transmit(&UART_LOG_INSTANCE, (uint8_t*)data, (uint16_t)len, UART_LOG_TIMEOUT);
}
```

所有输出最终都汇到这里。**阻塞式**：CPU 守着 UART 每次一字节发完才返回。简单可靠，代价是发送期间 CPU 空等——日志只能在主循环里用，不能进中断。

### 5. CRLF 规范化：串口终端的显示问题

```c
static void uart_write_with_crlf(const char *s, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        if (s[i] == '\n')  uart_write_blocking("\r\n", 2);   // \n 补成 \r\n
        else               uart_write_blocking(&s[i], 1);
    }
}
```

C 代码里习惯只写 `\n`，但 PC 串口助手大多要求"回车+换行"（`\r\n`）才换行——只发 `\n` 会看到下一行接在上一行屁股后头。这个函数逐字节扫一遍，把 `\n` 自动补成 `\r\n`，调用方就不用操心格式了。

### 6. uart_printf：变参函数三板斧

```c
int uart_printf(const char *fmt, ...)
{
    char buf[UART_LOG_BUF_SIZE];
    va_list ap;
    va_start(ap, fmt);                              // ① 从 fmt 之后取参数
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);   // ② 格式化进栈缓冲，带长度保护
    va_end(ap);                                     // ③ 清理
    if (n < 0) return n;
    size_t out_len = (n < (int)sizeof(buf)) ? n : sizeof(buf) - 1;  // 防截断
    uart_write_with_crlf(buf, out_len);
    return n;
}
```

`...` 可变参数 + `va_list` 是 C 语言处理"参数个数不定"的标准机制，printf 家族全靠它。核心一步是 `vsnprintf`：把 `value=%d` 这样的模板和后面的参数**在栈上拼成最终字符串**，`sizeof(buf)` 保证永不写越界。返回值 `n` 是"本应需要的长度"——超过 buffer 说明被截断了。

### 7. uart_hexdump：二进制内容可视化

```c
void uart_hexdump(const void *data, size_t len, const char *title)
```

每 16 字节一行：偏移地址 + hex 值（第 8 字节后多空格对半分）+ 右侧 ASCII 栏。内部用 80 字节栈缓冲逐段 `snprintf` 拼接，每次都传"当前位置 + 剩余空间"，零溢出。调试串口帧、协议包、Flash 内容的利器——细节丰富，值得单独一篇细讲（见补充笔记，待写）。

## 三种输出的实际效果

main 循环里那段代码在 PC 串口助手上每秒打印：

```
Hello, UART!
value=123, hex=0x7B
RX BUF (len=32):
00000000  12 34 56 78 41 42 43 00 00 00 00 00 00 00 00 00  |.4VxB,BC........|
```

（hexdump 的 ASCII 栏里 `0x12/0x34/0x56/0x78` 不可打印显示为 `.`，`'A','B','C'` 原样显示。）

## 记住这几个坑

1. **调试函数绝不能在中断里调**——阻塞发送最坏等 1s，中断里等 1ms 都是灾难
2. **uart_printf 栈上 256 字节缓冲**——调用它之前要确认栈空间够（默认主栈一般 1~4KB，够用但别在深递归里用）
3. **HAL_MAX_DELAY vs UART_LOG_TIMEOUT**——重定向的 `_write` 用了永等超时，UART 硬件出问题会卡死整个程序；自研驱动有 1s 封顶，更安全。这两者并存是代码的小瑕疵，读懂即可
4. **`\n` vs `\r\n`**——C 代码写 `\n`，串口助手要 `\r\n`，规范化已自动处理，但绕过驱动直发 HAL 时要记得自己补 `\r`
