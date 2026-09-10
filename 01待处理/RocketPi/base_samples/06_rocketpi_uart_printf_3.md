---
status: todo
created: 2026-09-10
tags:
  - c/debugging
  - stm32/uart
  - rocketpi/base_samples
  - todo
references:
  - "[[06_rocketpi_uart_printf]]"
---

# 06_rocketpi_uart_printf_3

> 进阶补充 3：uart_hexdump 详解——先看它解决什么问题、长什么样，再拆代码实现。
> 前置笔记：[[06_rocketpi_uart_printf]] §uart_hexdump

## 它解决什么问题：printf 看不见二进制

调试文本没问题，但嵌入式调试大量面对**二进制数据**：串口收到的协议帧、传感器原始报文、Flash 里的内容、DMA 缓冲区。这些字节里大量是不可打印字符（0x00、0xFF、控制符），直接 printf 出来是乱码或屏幕乱跳，人眼无法分析。

hexdump 的思路：**每个字节展开成两位十六进制文本**（0x00~0xFF → "00"~"FF"），任何字节都变成可见字符，一行对齐排列，肉眼就能对照协议文档逐字段核对。

## 使用场景（什么时候会用到它）

| 场景 | 例子 | hexdump 帮你看清 |
|---|---|---|
| 串口协议调试 | 雷达/蓝牙/4G 模组返回帧 | 帧头、长度、校验各在哪个字节 |
| DMA/接收缓冲区 | RX buffer 收了什么 | 有没有收到、收到几个字节、内容对不对 |
| Flash/EEPROM 内容 | AT24C02 读回一段 | 写入的数据是否完整、擦除是否为 0xFF |
| 传感器原始数据 | AHT30 六字节报文 | 温湿度位域在报文里的排布 |

一句话：**凡是"这段内存里到底是什么"的疑问，都丢给 hexdump**。

## 效果展示

main.c 里那行调用与串口助手上实际看到：

```c
static uint8_t rxbuf[32] = {0x12,0x34,0x56,0x78, 'A','B','C'};
uart_hexdump(rxbuf, sizeof(rxbuf), "RX BUF");
```

```
RX BUF (len=32):
00000000  12 34 56 78 41 42 43 00 00 00 00 00 00 00 00 00  |.4VxB,BC........|
00000010  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  |................|
```

一行三个区域，逐字段解读第一行：

```
00000000   12 34 56 78 41 42 43 00 00 00 00 00 00 00 00 00   |.4VxB,BC........|
└──┬──┘   └──────────────────┬──────────────────────────┘   └────────┬────────┘
 偏移地址                  hex 区（16 字节）                      ASCII 栏
 从 0x10 开始              第 8 字节后多一个空格，                 可打印字符原样显示，
 是第二行的偏移            把 16 字节分成 4+4 / 4+4 两半，         不可打印（<0x20 或 >0x7E）
                          方便肉眼按 4 字节对齐                    显示为 '.'
```

读法示范：`41 42 43` 是 `'A' 'B' 'C'` 的 ASCII，所以右侧 ASCII 栏对应位置显示 `ABC`；开头 `12 34 56 78` 不可打印，显示 4 个 `.`。**hex 区和 ASCII 栏是同一份数据的两种视图**——hex 看精确值，ASCII 碰运气认字符串（看到 "ABC" 就知道是文本内容）。

这套"偏移 + hex + ASCII"三段式不是本例发明，是 PC 上 `hexdump`/`xxd` 命令的通用格式——熟悉一个等于在 PC 和 MCU 两边通用。

## 代码实现原理：一次拼一行的工程

```c
void uart_hexdump(const void *data, size_t len, const char *title)
{
    const uint8_t *p = (const uint8_t*)data;
    if (title) uart_printf("%s (len=%u):\n", title, (unsigned)len);

    char line[80];                                   // 一行文本的拼接缓冲
    for (size_t i = 0; i < len; i += 16) {           // 外层：一次处理 16 字节
        int pos = 0;
        /* 区域1：偏移地址，8 位十六进制 */
        pos += snprintf(line + pos, sizeof(line) - pos, "%08X  ", (unsigned)i);

        /* 区域2：hex 区，逐字节拼，第 8 字节后加空格 */
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < len) pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", p[i + j]);
            else             pos += snprintf(line + pos, sizeof(line) - pos, "   ");  // 不足 16 补空格对齐
            if (j == 7) pos += snprintf(line + pos, sizeof(line) - pos, " ");
        }

        /* 区域3：ASCII 栏 */
        pos += snprintf(line + pos, sizeof(line) - pos, " |");
        for (size_t j = 0; j < 16 && i + j < len; ++j) {
            uint8_t c = p[i + j];
            pos += snprintf(line + pos, sizeof(line) - pos, "%c",
                            (c >= 32 && c <= 126) ? c : '.');    // 可打印原样，否则 '.'
        }
        pos += snprintf(line + pos, sizeof(line) - pos, "|\n");

        uart_puts(line);                              // 整行发出
    }
}
```

### 三个值得学的写法

**1. `snprintf` 链式拼接 + 游标 pos。** 每段拼接都写 `snprintf(line + pos, sizeof(line) - pos, ...)`——从"当前位置"写起、限"剩余空间"，写完 pos 前移。这样无论多少段、单段多长，**永远不会写出缓冲区**。这是 C 里拼接不定长文本的标准安全套路，比 `strcat`（先strlen再拼、两遍扫描还易越界）好得多。

**2. 不足 16 字节的行补空格对齐。** len=32 整除 16 看不出来，但如果 len=5：hex 区只打 5 个字节后，用 `"   "`（3 空格，与 "XX " 等宽）占位补满 16 个位置，ASCII 栏则只打实际字节——**hex 区对齐是死的，ASCII 栏跟着数据走**。没有补齐的话最后一行的 ASCII 栏会左移，和上行错位，肉眼对不上。

**3. 可打印判断 `c >= 32 && c <= 126`。** 0x20 是空格、0x7E 是 `~`，这是 ASCII 可打印区间的教科书边界。命中原样输出，否则 `.`。注意 80 字节行缓冲的构成：8(偏移)+3×16(hex)+1+1+16(ASCII)+2 ≈ 76，留了余量。

### 为什么"一次拼一行"而不是"逐字节直发"

逐字节直发（每字节一次 `snprintf + uart_puts`）会产生 3 字节、1 字节的碎片化阻塞发送，UART 每次发送都有启动开销，一个 32 字节 dump 会产生上百次小发送。拼满一整行（76 字节）一次发出，阻塞次数减到 2 次（32 字节 = 2 行）——**发送粒度合并**，是低速串口上的实用优化。

## 快速上手：把 hexdump 当调试按钮

往任何可疑位置插一行：

```c
uart_hexdump(&something, sizeof(something), "checkpoint");
```

看输出、对照数据手册、定位问题，再删掉。它和断点调试互补：断点看"那一刻的状态"，hexdump 看"一段时间内数据怎么变"（连打多次就能看到变化过程）。

## 关联笔记

- [[06_rocketpi_uart_printf]] — 总览，§uart_hexdump 概览与效果示例
- [[06_rocketpi_uart_printf_2|变参函数原理]] — hexdump 内部的 uart_printf/snprintf 都基于 C 库格式化
