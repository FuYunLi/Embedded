---
status: todo
created: 2026-09-11
tags:
  - c/library
  - stm32/uart
  - rocketpi/base_samples
  - todo
references:
  - "[[06_rocketpi_uart_printf]]"
---

# 06_rocketpi_uart_printf_4

> 进阶补充 4：借 06 的代码补一课 C 标准库——嵌入式开发常用但教材/教程很少系统讲的函数，重点是 printf 家族的完整版图。
> 前置笔记：[[06_rocketpi_uart_printf]]、[[06_rocketpi_uart_printf_2|变参函数原理]]

## 先把最大的误会解开：printf 家族不是"打印函数"

初学时都以为 `printf` = 打印到屏幕。**这个理解在 PC 上勉强对，在嵌入式上全错**。

printf 家族的真身是**"格式化"函数**：把"模板字符串 + 一堆参数"加工成一个字节序列。至于这个序列去哪——屏幕、文件、串口、内存——**不归 printf 管，归它调用的底层出口函数管**（见 [[06_rocketpi_uart_printf_1]] 的重定向链路）。C 标准把家族分成三大出口，每个出口再配 v 版本：

### 第一维：格式化结果写到哪

| 后缀 | 全称 | 结果去哪 | 嵌入式用途 |
|---|---|---|---|
| （无） | print formatted | **标准输出** stdout | 经重定向到串口（06 的备用路线） |
| `f` | file | **指定文件流** FILE* | SD 卡日志（fatfs f_open 得到的 FILE 不通用，见后文） |
| `s` | string | **内存缓冲区** char[] | ★ 嵌入式主力：先拼进内存，再自己决定发去哪 |

`s` 系列才是嵌入式的核心：`sprintf(buf, "temp=%d", t)` 把结果写进 buf，**不碰任何外设**——06 的 uart_printf 内部就是 `vsnprintf` 先拼内存，再交给 uart_puts 发串口。**格式化（纯内存操作）和传输（外设操作）分离**，这个分离是嵌入式日志设计的根基。

### 第二维：参数怎么来——v 家族

`v` = va_list（可变参数列表）。普通 printf 的参数写在调用处；**v 系列的参数已经打包在 va_list 里**：

```c
int  printf(const char *fmt, ...);            // 参数写在调用处
int vprintf(const char *fmt, va_list ap);     // 参数已在 ap 里，接过来了
```

v 家族的存在只有一个理由：**让你能写自己的 printf 包装函数**。回想 [[06_rocketpi_uart_printf_2]] 的三板斧：`uart_printf` 自己用 `va_start` 收集了参数，但它不想自己解析 `%d %X`（那要重写整个格式化引擎），于是把装满参数的 `ap` **原封不动转交**给 `vsnprintf`：

```c
int uart_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);  // ← 转交！不是 vsprintf(fmt, args...)
    va_end(ap);
    ...
}
```

**结论**：`s` + `v` 组合（vsnprintf）= "把变参拼进内存缓冲区"——嵌入式里 99% 的自研打印函数都是这一个套路的变体。注意家族里没有 `vsprintf` 的"安全限宽"变体时才用 vsprintf，**永远优先带 n 的**：`sprintf` 不知缓冲多大，是溢出重灾区；`snprintf(buf, sizeof(buf), ...)` 永不越界。

### printf 家族全景表（嵌入式视角）

| 函数 | 去哪 | 参数 | 嵌入式评价 |
|---|---|---|---|
| printf | stdout | 调用处 | 需重定向，newlib 格式化引擎占 Flash |
| sprintf | char[] | 调用处 | 有溢出风险，少用 |
| snprintf | char[] + 限宽 | 调用处 | ★ 单次拼接首选 |
| vprintf | stdout | va_list | 少用 |
| vsprintf | char[] | va_list | 溢出风险，别用 |
| vsnprintf | char[] + 限宽 | va_list | ★★ 自研 printf 包装的唯一核心，06 在用 |
| fprintf | FILE* | 调用处 | 裸机少用（文件流依赖重） |

记法：**前缀 v = "参数已打包"；后缀 n = "有长度保险"；s = "写内存"**。

## 一个常见坑： FatFs 的 f_printf 不是这个家族

用 FatFs 时会遇到 `f_printf`、`f_puts`——名字长得像 stdio，但它是 FatFs 自己实现的**独立迷你格式化器**，操作的是 `FIL`（FatFs 文件对象），不是 C 库的 `FILE*`。两者不能混用（不能把 `f_open` 的 FIL* 传给 fprintf）。原理上倒是同套路：格式化 + 写入目标分离，只是"写入目标"换成了 FatFs 的磁盘层。认清"格式化引擎可以有很多套，出口由实现者定"这个本质就不会晕。

## 借 06 代码看其他常用标准库函数

hexdump/uart_printf 一套代码里其实埋了一堆初学者不熟的标准库成员，逐个过：

### strlen —— 数长度，数到 '\0' 为止

```c
uart_write_with_crlf(s, strlen(s));   // uart_puts 里
```

`strlen` 从头扫描直到遇到 `'\0'`（字符串终止符），返回**不含 '\0'** 的字符数。要点：

- **它是 O(n) 扫描，不是"属性查询"**——每调一次都从头数一遍。循环里反复 strlen 是常见性能坑
- `sizeof` vs `strlen`：`uint8_t rxbuf[32]` 的 sizeof 是 32（编译期，含未用空间），strlen 只有当它真是字符串时才有意义（遇到第一个 0x00）。二进制缓冲区用 sizeof/显式 len，**永远别用 strlen**
- 0x00 是合法数据 → 二进制里用 strlen 会提前截断。这就是 hexdump 必须显式传 len 的原因（呼应 [[06_rocketpi_uart_printf_3]] 形参 2）

### memset —— 一口气填内存

```c
memset(handle, 0, sizeof(Button));    // multi_button.c button_init 里，顺手看 04 的
```

把一段内存的每个字节都设为同一值（常用 0 清零）。三个参数：目标、值（按**字节**填）、字节数。要点：

- 填 0 之外要小心：`memset(buf, 1, 100)` 每字节变 0x01，**不是**把 int 数组每个元素填成 1（int 4 字节，会变成 0x01010101）
- 结构体清零标配：`memset(&obj, 0, sizeof(obj))`——但含指针成员的已初始化结构体慎用（会丢掉已有分配）

### memcpy / memmove —— 搬内存

一对孪生：都是"从 src 拷 n 字节到 dst"，区别在**重叠区**行为：

- `memcpy`：不保证重叠安全（快，靠"假设不重叠"优化）
- `memmove`：重叠也正确（慢一点，内部判断拷贝方向）

串口收到一帧数据要挪进环形缓冲、协议解析要抽字段，都是它们的活。**长度必须自己算对**——源和目标哪边算小了都是踩内存。

### memcmp —— 比内存，不是比字符串

```c
memcmp(frame_header, "\xA5\x5A", 2) == 0   // 常见于协议帧头比对
```

逐字节比较 n 个字节，相等返回 0。协议解析里比帧头/命令码用它，而不是 strcmp（strcmp 会停在 0x00，二进制帧里可能提前"相等"）。

### snprintf 的返回值——前面用过的再点一次

06 的 uart_printf 里 `out_len = (n < sizeof(buf)) ? n : sizeof(buf) - 1` 用到了 snprintf 返回值的完整语义：**返回"本应输出的字符数"**，缓冲不够时这个数 > 实际写入数。所以"返回值 >= sizeof(buf)"就是"被截断了"的信号——内核式代码会拿这个打 "...[truncated]" 警告（06 里被注释掉的那段就是）。

### isprint / isascii —— 字符分类函数

06_3 提过内核用 `isascii(ch) && isprint(ch)` 替代本例的手写 `c >= 32 && c <= 126`。C 标准的 `<ctype.h>` 提供一整族字符分类/转换函数：

| 函数 | 判断/转换 |
|---|---|
| `isdigit` / `isxdigit` | 十进制/十六进制数字（解析协议数值字段） |
| `isalpha` / `isalnum` | 字母/字母数字（解析命令行） |
| `isspace` | 空白符（分词、跳过空格） |
| `isprint` / `isgraph` | 可打印（hexdump/日志过滤） |
| `toupper` / `tolower` | 大小写转换（命令大小写不敏感） |

手写区间判断完全等价，但用标准函数**自文档**（`isprint(c)` 一眼懂，`c >= 32 && c <= 126` 要愣一下）。注意它们的入参约定是 int（传 EOF 或 unsigned char 值），传 char 前先转 unsigned char 是严谨写法。

## 嵌入式 C 标准库的"能用/慎用/禁用"速查

| 级别 | 函数 | 理由 |
|---|---|---|
| 放心用 | memset/memcpy/memmove/memcmp、snprintf、ctype 族 | 纯内存操作，无依赖 |
| 慎用 | sprintf/strcat/strcpy（无 n 版）、strlen 在循环里 | 溢出/性能 |
| 看配置 | printf/fopen 系、malloc/free | 依赖重定向桩/_sbrk/文件系统，见 [[06_rocketpi_uart_printf_1]] |
| 一般禁 | exit/abort、信号系、宽字符系 | 裸机没有"退出"概念，库未移植 |

malloc/free 单说一句：能用但必须配好 `_sbrk` 桩（[[06_rocketpi_uart_printf_1]] 的桩全家桶表），且长期运行的系统慎用——碎片化会让堆慢慢耗尽，嵌入式更常直接用静态分配/内存池。

## 关联笔记

- [[06_rocketpi_uart_printf]] — 总览：uart_printf/hexdump 代码出处
- [[06_rocketpi_uart_printf_1|stdio 重定向]] — printf 家族的"出口"如何落到串口
- [[06_rocketpi_uart_printf_2|变参函数原理]] — v 家族存在的理由（va_list 转交）
- [[06_rocketpi_uart_printf_3|hexdump 详解]] — strlen/sizeof 分野、snprintf 返回值语义的实战场景
