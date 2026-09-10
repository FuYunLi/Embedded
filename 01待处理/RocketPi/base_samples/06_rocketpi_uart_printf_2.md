---
status: todo
created: 2026-09-10
tags:
  - c/language
  - stm32/uart
  - rocketpi/base_samples
  - todo
references:
  - "[[06_rocketpi_uart_printf]]"
---

# 06_rocketpi_uart_printf_2

> 进阶补充 2：变参函数（可变参数）原理——`uart_printf` 如何做到"参数个数不定"。
> 前置笔记：[[06_rocketpi_uart_printf]] §uart_printf；C 语言基础概念配合本篇食用

## 从一个问题开始

```c
uart_printf("value=%d, hex=0x%X\n", val, val);   // 2 个参数
uart_printf("a=%d b=%d c=%d\n", a, b, c);        // 3 个参数
```

同一个函数，调用时参数个数可以不同——普通函数做得到吗？做不到。普通函数声明 `void f(int a, int b)`，就必须传两个 int，个数和类型编译期就锁死。

C 语言为此提供了一套**变参机制**：参数列表最后放 `...`，表示"后面还有若干个参数，个数类型不定"。

```c
int uart_printf(const char *fmt, ...);
//                              ^^^ 变参部分
```

**规则：`...` 前必须有至少一个具名参数**。这个具名参数（`fmt`）是"路标"——告诉函数从哪开始找变参、一共几个、都是什么类型。printf 靠解析 `fmt` 里的 `%d`、`%X` 才知道后面跟着几个参数、各是什么类型。这也是为什么 `printf("%s", 42)` 会跑飞：路标说"这里是个字符串指针"，你却塞了个整数，C 无法替你检查（类型安全由程序员自己负责）。

## va_list 三件套逐个拆

```c
#include <stdarg.h>

int uart_printf(const char *fmt, ...)
{
    va_list ap;                    // ① 声明"参数游标"
    va_start(ap, fmt);             // ② 游标定位到 fmt 之后的第一个变参
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);  // ③ 交给 vsnprintf 逐个消费
    va_end(ap);                    // ④ 收尾（某些平台上恢复现场）
    ...
}
```

| 宏 | 名字含义 | 干的事 |
|---|---|---|
| `va_list` | variable-argument list | 一个游标变量，指向参数区当前位置 |
| `va_start(ap, last_named)` | start | 把游标拨到最后一个具名参数（fmt）的后面 |
| `va_arg(ap, type)` | argument | 取出当前参数并按 type 解读，游标后移（本例没直接用，vsnprintf 内部在用） |
| `va_end(ap)` | end | 结束使用，清理游标 |

`va_arg` 的使用样子（uart_printf 里没用，但自己写变参函数会用到）：

```c
void demo(int count, ...)          // 用具名参数 count 告知后面有几个
{
    va_list ap;
    va_start(ap, count);
    for (int i = 0; i < count; i++) {
        int x = va_arg(ap, int);   // 按 int 取，一个一个来
        uart_printf("arg%d=%d\n", i, x);
    }
    va_end(ap);
}
// 调用：demo(3, 10, 20, 30);
```

**关键认知：变参函数必须自己知道"怎么读"**。参数个数和类型的信息不会自动传递——要么像 printf 靠 fmt 字符串描述，要么像 demo 非参数 count 显式告知。没有路标的变参函数是读不回来的。

## 底层真相：参数在栈（或寄存器）上连续排布

ARM 平台调用约定（AAPCS）下，参数按寄存器 r0~r3 传递，传不下的压栈。变参场景里编译器把"..."部分的参数**在内存里连续排列**，`va_list` 本质是一个指针：

```
demo(3, 10, 20, 30) 到达函数内部时（简化示意）：

栈/参数区：  [ 3 ][ 10 ][ 20 ][ 30 ]
              ↑            ↑
            count        va_start 后 ap 指向这里
```

- `va_start(ap, count)`：ap = &count + sizeof(count)（示意），对准第一个变参
- `va_arg(ap, int)`：从 ap 读 4 字节当 int，ap += 4
- 不同类型宽度不同（double 8 字节、char 4 字节——**变参里 char 会被提升为 int**）

**默认参数提升（default argument promotions）**是初学者大坑：变参位置的 `char`/`short` 自动升为 `int`，`float` 自动升为 `double`。所以 `va_arg(ap, char)` 是**未定义行为**——必须按 `int` 取。这也解释了为什么 `printf("%f", 1.0f)` 能对：float 早就升成 double 了。

> 各编译器对 va_list 的具体实现不同（GCC/newlib 在 ARM 上可能是结构体，含堆栈回溯信息），但**宏的用法是标准 C 规定的**，代码不用关心底层差异——这和 [[06_rocketpi_uart_printf_1]] 里"标准规定行为、实现者落地"是同一哲学。

## 三个使用守则

1. **变参不能为空**：`f("hi")` 合法（fmt 是具名参数），但路标必须存在；格式符和实参要一一对应
2. **没有类型检查**：格式串写 `%s` 却传 int，编译器默认不报错（GCC 的 `-Wformat` 警告能查一部分），运行时直接读错内存
3. **vsnprintf 而非 vsprintf**：带 n 的版本把缓冲区上限传入，永不越界。06 代码 `vsnprintf(buf, sizeof(buf), fmt, ap)` 就是标准安全姿势；裸 `vsprintf` 是缓冲区溢出重灾区

## 回看 uart_printf 全貌

```c
int uart_printf(const char *fmt, ...)
{
    char buf[UART_LOG_BUF_SIZE];        // 栈上 256 字节格式化区
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);   // 变参 → 字符串
    va_end(ap);
    if (n < 0) return n;                // 格式化出错
    size_t out_len = (n < (int)sizeof(buf)) ? n : sizeof(buf) - 1;
    uart_write_with_crlf(buf, out_len); // 发送（CRLF 规范化）
    return n;                           // 返回"本应输出"的长度，与 printf 行为一致
}
```

三步走：**接住变参（va_ 三件套）→ 拼成字符串（vsnprintf 安全版）→ 发出去（阻塞 UART + CRLF）**。所有 printf 风格函数都是这个套路，学会一个等于学会一类。

## 关联笔记

- [[06_rocketpi_uart_printf]] — 总览，§uart_printf 变参三板斧是本篇入口
- [[06_rocketpi_uart_printf_1|stdio 重定向]] — vsnprintf 属于 C 库，格式化引擎的来龙去脉在隔壁
