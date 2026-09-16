---
status: done
created: 2026-09-16
tags:
  - c/spi
  - c/display
  - embedded/graphics
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/25_rocketpi_spi_lcd_bitmap"
  - "[[24_rocketpi_spi_lcd_speedtest]] SPI LCD 帧率测试（25 例的基础）"
  - "[[23_rocketpi_ws2812b]] DMA 传输（对比 SPI DMA）"
---

# 25 SPI LCD 位图显示 ST7789

## 一句话定性

ST7789 240×240 TFT LCD 的位图显示驱动，在 24 例帧率测试基础上新增位图绘制、字符渲染、字符串显示能力，通过 SPI+DMA 将 RGB565 位图数据逐行发送到 LCD GRAM。

## 同类产品定位

- **ST7789**：SPI 接口 TFT LCD 驱动 IC，240×240，RGB565，内置 162KB GRAM
- **ILI9341**：同类 SPI LCD，320×240，更早更普及
- **ST7735**：低分辨率版（128×160），成本更低
- **本例选型理由**：ST7789 分辨率适中、SPI 接口简单、适合学习 LCD 驱动和位图渲染

## 硬件连接

- SPI：SCK + MOSI + CS + DC（数据/命令选择）
- 控制：RST（复位）+ BL（背光）
- 引脚分配：PA5=SCK, PA7=MOSI, PA4=CS, PA1=DC, PA0=RST, PA2=BL

## 通信协议要点

- 4 线 SPI：标准 SPI + DC 引脚区分命令/数据
- 像素格式：RGB565（16bit/像素），高字节先出
- 命令协议：1 字节命令码 + N 字节参数
- 关键命令：CASET(0x2A) 列地址、RASET(0x2B) 行地址、RAMWR(0x2C) 写像素

## 代码架构

```
┌─────────────────────────────────────────────────┐
│ main.c (应用层)                                 │
│  - Init + Clear(WHITE) + DrawBitmap(240×240)    │
├─────────────────────────────────────────────────┤
│ test_rgb565_240x240.h (位图数据)                │
│  - 240×240 RGB565 位图，115,200 字节            │
├─────────────────────────────────────────────────┤
│ st7789.c (驱动层，手写)                        │
│  - Init / Clear / DrawPixel / DrawHLine /      │
│    DrawVLine / FillRect / DrawBitmap /          │
│    DrawBitLine16BPP / ShowChar / ShowString /   │
│    TestFrameRate                                │
├─────────────────────────────────────────────────┤
│ spi.c + dma.c (CubeMX 生成)                    │
│  - SPI1 + DMA1_Stream3/TX                      │
└─────────────────────────────────────────────────┘
```

## 与 24 例（帧率测试）的关键差异

| 维度 | 24 帧率测试 | 25 位图显示 |
|------|------------|------------|
| 核心功能 | FillRect 全屏换色 | DrawBitmap 精确像素 |
| 新增函数 | TestFrameRate | DrawBitmap + DrawBitLine16BPP + ShowChar + ShowString |
| 位图数据 | 无 | test_rgb565_240x240（115KB Flash） |
| 字符渲染 | 无 | Font16x24 位图字体 |
| 字节序处理 | FillRect 内联 | DrawBitLine16BPP 独立函数 |
| 应用场景 | 性能测试 | 图片显示 + 文字叠加 |

## 核心实现详解

### ST7789_DrawBitmap——矩形位图显示

**作用**：将 RGB565 格式的位图数据逐行发送到 LCD 指定矩形区域。

**形参**：`xs, ys` — 左上角坐标；`xsize, ysize` — 宽高；`p` — 位图数据指针（RGB565，小端存储）。

**输入/输出**：输入 = 坐标 + 尺寸 + 位图数据；输出 = LCD 对应区域显示位图内容。

**调用者**：`main`，上电后显示一张 240×240 图片。

```c
void ST7789_DrawBitmap(uint16_t xs, uint16_t ys, uint16_t xsize, uint16_t ysize, const uint8_t *p)
{
    if (xs >= ST7789_X_RES || ys >= ST7789_Y_RES) return;
    if (xsize == 0 || ysize == 0) return;

    // ① 裁剪到屏幕范围
    uint16_t xmax = (xs + xsize > ST7789_X_RES) ? (ST7789_X_RES - xs) : xsize;
    uint16_t ymax = (ys + ysize > ST7789_Y_RES) ? (ST7789_Y_RES - ys) : ysize;

    // ② 逐行发送
    for (uint16_t i = 0; i < ymax; i++) {
        ST7789_DrawBitLine16BPP(xs, ys + i, p + (uint32_t)i * xsize * 2, xmax);
    }
}
```

- **① 裁剪**：位图可能超出屏幕边界，`xmax`/`ymax` 取屏幕剩余空间和位图尺寸的较小值
- **② 逐行发送**：每行调用 `DrawBitLine16BPP`，指针偏移 `i * xsize * 2`（每像素 2 字节）
- **行优先存储**：位图数据按行排列，与 LCD 的行扫描方向一致

### ST7789_DrawBitLine16BPP——单行位图 + 小端转大端

**作用**：将一行 RGB565 数据从小端存储转换为大端（高字节先出），发送到 LCD。

```c
void ST7789_DrawBitLine16BPP(uint16_t xs, uint16_t y, const uint8_t *p, uint16_t xsize)
{
    uint32_t bytes = (uint32_t)xsize * 2;

    // ③ 小端 → 大端转换
    for (uint32_t i = 0; i < bytes; i += 2) {
        uint8_t lo = p[i];
        uint8_t hi = p[i+1];
        ST7789_Buf[i]   = hi;   // 高字节先出
        ST7789_Buf[i+1] = lo;
    }

    ST7789_SetWindow(xs, y, xs + xsize - 1, y);
    ST7789_WriteCmd(0x2C);      // RAMWR
    ST7789_WriteDataDMA(ST7789_Buf, bytes);
}
```

- **③ 字节序交换**：C 数组在小端 MCU 上存储为 `{低字节, 高字节}`，ST7789 要求高字节先出。交换后写入 `ST7789_Buf`
- **SetWindow + RAMWR**：设置单行窗口，然后连续写入像素数据
- **缓冲区限制**：`ST7789_Buf` 大小由 `ST7789_BUF_SIZE` 定义，一行 240 像素 = 480 字节，需要缓冲区 ≥ 480 字节

### ST7789_ShowChar——字符渲染

**作用**：将 ASCII 字符的位图字体渲染到 LCD 指定位置，支持前景色和背景色。

```c
void ST7789_ShowChar(uint16_t x, uint16_t y, uint8_t ch, FontDef font, uint16_t color, uint16_t bgcolor)
{
    // ④ 设置字符窗口
    ST7789_SetWindow(x, y, x + w - 1, y + h - 1);
    ST7789_WriteCmd(0x2C);

    uint32_t wrote = 0;
    for (uint16_t row = 0; row < h; row++) {
        // ⑤ 读取字体位图行
        uint16_t bits = font.data[((uint32_t)(ch - 32) * h) + row];

        for (uint16_t col = 0; col < w; col++) {
            // ⑥ 位判断：1=前景色，0=背景色
            uint16_t c = (bits & (1U << (w - 1 - col))) ? color : bgcolor;

            // ⑦ 写入缓冲区
            ST7789_Buf[idx + 0] = (uint8_t)(c >> 8);
            ST7789_Buf[idx + 1] = (uint8_t)(c & 0xFF);
            wrote += 2;

            // ⑧ 缓冲区满则发送
            if ((wrote % ST7789_BUF_SIZE) == 0) {
                ST7789_WriteDataDMA(ST7789_Buf, ST7789_BUF_SIZE);
            }
        }
    }

    // ⑨ 发送剩余数据
    uint32_t remain = wrote % ST7789_BUF_SIZE;
    if (remain) ST7789_WriteDataDMA(ST7789_Buf, remain);
}
```

- **⑤ 字体位图**：`FontDef.data` 是 `uint16_t` 数组，每个元素代表一行的位图（16 位，MSB 左对齐）。`(ch - 32)` 是 ASCII 偏移（空格=32 是第一个可打印字符）
- **⑥ 位判断**：`(w - 1 - col)` 从高位到低位遍历，MSB 对应最左边像素
- **⑦⑧⑨ 分块发送**：像素数据逐个写入 `ST7789_Buf`，满 `ST7789_BUF_SIZE` 就发一次 DMA，最后发剩余

### ST7789_ShowString——字符串渲染

**作用**：逐字符渲染字符串，支持自动换行。

```c
void ST7789_ShowString(uint16_t x, uint16_t y, const char *str, FontDef font, uint16_t color, uint16_t bgcolor)
{
    uint16_t cx = x, cy = y;
    while (*str) {
        // ⑩ 自动换行
        if (cx + fw > ST7789_X_RES) {
            cx = 0;
            cy += fh;
            if (cy + fh > ST7789_Y_RES) break;  // 屏幕用完
            if (*str == ' ') { str++; continue; } // 空格跳过
        }

        ST7789_ShowChar(cx, cy, (uint8_t)*str, font, color, bgcolor);
        cx += fw;
        str++;
    }
}
```

- **⑩ 自动换行**：当前 X 坐标 + 字符宽度 > 屏幕宽度时，换到下一行开头
- **空格跳过**：换行后如果当前字符是空格（单词分隔），跳过避免行首出现空格

### ST7789_TestFrameRate——帧率测试

**作用**：60 帧全屏换色计时，计算 FPS 并居中显示在屏幕上。

```c
void ST7789_TestFrameRate(void)
{
    static const uint16_t test_colors[] = { 0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000, 0x07FF, 0xF81F, 0xFFE0 };
    const uint32_t frame_count = 60U;

    tick_start = HAL_GetTick();
    for (uint32_t f = 0; f < frame_count; f++) {
        uint16_t color = test_colors[f % 8];
        ST7789_FillRect(0, 0, 239, 239, color);
    }
    elapsed = HAL_GetTick() - tick_start;

    double fps = (double)frame_count * 1000.0 / (double)elapsed;
    // ... 居中显示 FPS 文字 ...
}
```

### main.c——应用层

```c
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init(); MX_DMA_Init(); MX_USART2_UART_Init(); MX_SPI1_Init(); MX_ADC1_Init();

    ST7789_Init();
    ST7789_Clear(WHITE);
    ST7789_DrawBitmap(0, 0, 240, 240, (uint8_t *)test_rgb565_240x240);  // 显示位图

    while (1) {}
}
```

`test_rgb565_240x240` 是一个 115,200 字节的 `const uint8_t` 数组（240×240×2），存储在 Flash 中。对 STM32F401RE（512KB Flash）来说占 22.5%。

## 设计问题与改进空间

1. **115KB 位图数据占 Flash 22.5%**：对 512KB Flash 的 F401RE 来说可接受，但对小容量 MCU 是问题。可改为 SD 卡读取或压缩格式（如 RLE）。

2. **DrawBitLine16BPP 的字节序交换**：每行都做一次 `lo↔hi` 交换，CPU 开销约 `240 × 3 = 720` 条指令/行。可在生成位图时就按大端存储，省去运行时交换。

3. **ShowChar 的 clipping 粗糙**：字符超出屏幕时直接 `return` 不绘制，而不是裁剪到可见部分。可改为部分绘制。

4. **TestFrameRate 用 `%f` 浮点库**：增加 5~10KB Flash。可改为定点整数打印（如 20 例的 `%d.%01d`）。

5. **ST7789_Buf 复用冲突**：`DrawBitmap`、`ShowChar`、`FillRect` 等函数共用同一个静态缓冲区，不能在 DMA 传输中调用另一个绘图函数。当前代码是阻塞式 DMA（轮询等待），所以不会冲突，但改为异步 DMA 后需要双缓冲。

6. **与 24 例的架构演进**：24 例只有 Init/Clear/FillRect/TestFrameRate，25 例新增了 DrawBitmap/DrawBitLine16BPP/ShowChar/ShowString，从"性能测试"升级为"图形显示"。

## 关联笔记

- [[24_rocketpi_spi_lcd_speedtest|24 SPI LCD 帧率测试]]：25 例的基础，ST7789 初始化和基本绘图
- [[23_rocketpi_ws2812b|23 WS2812B LED 灯带]]：另一种 DMA 传输方式（PWM+DMA vs SPI+DMA）
- [[06_rocketpi_uart_printf|06 printf 调试]]：stdio 重定向，25 例用于打印 FPS
