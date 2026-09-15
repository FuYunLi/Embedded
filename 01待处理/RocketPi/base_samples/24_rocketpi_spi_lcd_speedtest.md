---
status: done
created: 2026-09-15
tags:
  - c/spi
  - c/dma
  - embedded/display
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/24_rocketpi_spi_lcd_speedtest"
  - "[[23_rocketpi_ws2812b]] DMA+PWM 时序编码（对比 SPI+DMA 像素传输）"
  - "[[16_rocketpi_pwm_passive_buzzer]] PWM 频率计算（对比 SPI 时钟分频）"
---

# 24 SPI LCD 帧率测试 ST7789

## 一句话定性

ST7789 是 SPI 接口的 240x240 TFT LCD 驱动芯片，通过 SPI 发送命令/像素数据控制显示内容，本例用 DMA 批量传输像素实现全屏帧率测试，是第一个 SPI 外设和第一个显示输出外设。

## 同类产品定位

- **ST7789V**：240x240/240x320 TFT LCD 驱动，SPI 接口，内置 162KB GRAM，成本 ~¥5
- **ILI9341**：240x320 TFT LCD 驱动，SPI/并口，市场占有率高，与 ST7789 指令集兼容度高
- **ST7735**：128x160/160x80 低分辨率版本，引脚兼容 ST7789
- **SSD1306**：OLED 驱动，I2C/SPI，分辨率 128x64，无背光，对比度高但尺寸小
- **本例选型理由**：ST7789 是当前最主流的 SPI LCD 驱动，240x240 分辨率适合嵌入式 GUI 学习

## 硬件连接

- SPI 接口：SPI1（PA5=SCK, PA7=MOSI）
- 控制引脚：
  - PA8 = LCD_CS（片选，低有效）
  - PB10 = LCD_DC（数据/命令选择，0=命令，1=数据）
  - PB4 = LCD_RST（复位，低有效）
  - PB5 = LCD_BL（背光，高有效）
- 分辨率：240x240，像素格式 RGB565（16bit/像素）
- 偏移：X_SHIFT=0, Y_SHIFT=0（若模组 IC 窗口为 240x320，需 Y_SHIFT=80）

## 通信协议要点

- **SPI 模式 0**：CPOL=0, CPHA=0，时钟空闲低电平，第一个边沿采样
- **DC 引脚区分命令/数据**：DC=0 时 MOSI 上是命令字节，DC=1 时是像素数据
- **CS 控制传输边界**：每条命令/数据块前后拉低/拉高 CS
- **命令格式**：1 字节命令码 + N 字节参数（具体长度由命令定义）
- **像素格式**：RGB565，高字节先出（大端），16bit/像素
- **关键命令**：
  - 0x11：Sleep Out（退出睡眠，需等 120ms）
  - 0x3A：像素格式（0x55 = 16bit RGB565）
  - 0x2A：列地址设置（CASET），4 字节参数
  - 0x2B：行地址设置（RASET），4 字节参数
  - 0x2C：写显存（RAMWR），后跟像素数据流
  - 0x21：反色显示
  - 0x36：MADCTL（显示方向 + RGB/BGR）
  - 0x29：显示开

## 代码架构

```
main.c (应用层)
  - ST7789_Init() / ST7789_Clear(GRED) / ST7789_TestFrameRate()
      ↓
st7789.c 内部分层：
  ┌─────────────────────────────────────────────┐
  │ 上层 API                                    │
  │  Clear / FillRect / DrawPixel / DrawHLine / │
  │  DrawVLine / DrawBitmap / DrawBitLine16BPP / │
  │  ShowChar / ShowString / TestFrameRate      │
  ├─────────────────────────────────────────────┤
  │ 中层：ST7789_SetWindow                      │
  │  CASET(0x2A) + RASET(0x2B) + RAMWR(0x2C)   │
  │  三条命令一次性拉低 CS，减少切换开销         │
  ├─────────────────────────────────────────────┤
  │ 底层 I/O                                    │
  │  st7789_tx_blocking：阻塞发送（命令/小数据） │
  │  ST7789_WriteDataDMA：DMA+轮询等待（大块像素）│
  │  ST7789_WriteCmd / ST7789_WriteData         │
  ├─────────────────────────────────────────────┤
  │ HAL 层                                      │
  │  HAL_SPI_Transmit（阻塞）                   │
  │  HAL_SPI_Transmit_DMA（DMA 启动）            │
  │  HAL_SPI_GetState（轮询 DMA 完成）           │
  └─────────────────────────────────────────────┘

头文件：
  st7789.h       — 配置宏 + 控制引脚宏 + 颜色常量 + API 声明
  fonts.h        — FontDef 结构体定义
  font16x24_ascii.h — 16×24 ASCII 位图字体数据（95 个字符 × 24 行 uint16_t）
```

## 与 23 例（WS2812B）的关键差异

| 维度 | 23 WS2812B | 24 ST7789 LCD |
|------|------------|---------------|
| 接口 | 单线自定义时序 | 4 线 SPI（SCK+MOSI+CS+DC） |
| DMA 用途 | PWM 占空比编码 | SPI 像素数据流 |
| 数据格式 | GRB 24bit/LED | RGB565 16bit/像素 |
| 时序要求 | 纳秒级精确 | 标准 SPI 时钟，宽松 |
| 复位信号 | >50µs 低电平 | 10ms 低电平 + 120ms 延时 |
| 数据量 | N×24 位 | 240×240×16 = 921,600 位 |
| 传输模式 | DMA 异步 + 回调 | DMA + 轮询等待 |

## 核心实现详解

### SPI 与 DMA 的关系

SPI 是同步串行协议，SCK 每个周期移出 1bit。STM32 的 SPI 外设有自己的 TX/RX 数据寄存器（SPI_DR），CPU 或 DMA 在每个字节发送完成后向 SPI_DR 写入下一个字节。

DMA 的作用：不用 CPU 逐字节搬运，DMA 控制器自动从内存读取像素数据写入 SPI_DR。本例用 `HAL_SPI_Transmit_DMA` 启动传输，然后轮询 `HAL_SPI_GetState` 等待完成——虽然不是真正异步，但比 CPU 逐字节搬运快得多（DMA 总线访问是硬件级别的）。

### st7789_tx_blocking 与 ST7789_WriteDataDMA

```c
static inline void st7789_tx_blocking(const uint8_t *buf, size_t len) {
    HAL_SPI_Transmit(ST7789_SPI, (uint8_t*)buf, len, HAL_MAX_DELAY);
}

static inline void ST7789_WriteDataDMA(const uint8_t *data, size_t len) {
    if (len == 0) return;
    ST7789_CS_LOW();
    ST7789_DC_HIGH();
    HAL_SPI_Transmit_DMA(ST7789_SPI, (uint8_t*)data, len);
    while (HAL_SPI_GetState(ST7789_SPI) == HAL_SPI_STATE_BUSY_TX) { }
    ST7789_CS_HIGH();
}
```

**为什么命令用阻塞、数据用 DMA**：命令只有 1~5 字节，DMA 启动开销（配置寄存器 + 中断）反而比直接阻塞发送慢。像素数据动辄几百到几万字节，DMA 搬运效率远超 CPU。

**DMA+轮询等待的设计**：`HAL_SPI_Transmit_DMA` 启动后 DMA 控制器自动搬运，CPU 通过轮询 SPI 状态寄存器等待完成。比阻塞发送的优势在于：DMA 搬运期间 CPU 不需要逐字节参与，总线利用率更高。代价是 CPU 在等待循环中做不了其他事——若要真正异步，需要改用 DMA 完成中断回调。

### ST7789_SetWindow——窗口设置

```c
static void ST7789_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint16_t xs = x0 + X_SHIFT;
    uint16_t xe = x1 + X_SHIFT;
    uint16_t ys = y0 + Y_SHIFT;
    uint16_t ye = y1 + Y_SHIFT;

    uint8_t caset[4] = { xs >> 8, xs & 0xFF, xe >> 8, xe & 0xFF };
    uint8_t raset[4] = { ys >> 8, ys & 0xFF, ye >> 8, ye & 0xFF };

    ST7789_CS_LOW();

    /* CASET(0x2A) */
    ST7789_DC_LOW();  st7789_tx_blocking(&c, 1);   // 命令
    ST7789_DC_HIGH(); st7789_tx_blocking(caset, 4); // 4 字节参数

    /* RASET(0x2B) */
    ST7789_DC_LOW();  st7789_tx_blocking(&c, 1);
    ST7789_DC_HIGH(); st7789_tx_blocking(raset, 4);

    /* RAMWR(0x2C) — 命令已发出，DC 保持低，后续数据由 WriteData 接管 */
    ST7789_DC_LOW();  st7789_tx_blocking(&c, 1);

    ST7789_CS_HIGH();
}
```

**CASET+RASET+RAMWR 三合一**：三条命令在同一次 CS 拉低期间发出，避免 CS 切换的额外开销。RAMWR 命令发出后，DC 保持低电平状态——后续的像素数据写入会重新设置 DC=HIGH。

**坐标偏移 X_SHIFT/Y_SHIFT**：部分 240x240 模组的玻璃面板实际连接的是 240x320 的 ST7789 IC，显示窗口在 GRAM 中有偏移。Y_SHIFT=80 表示从第 80 行开始写，跳过上面不可见的 80 行。

### ST7789_Init——初始化序列

```c
void ST7789_Init(void)
{
    ST7789_BL_HIGH();   // 背光先亮
    ST7789_CS_HIGH();

    // 复位：>10ms 低电平
    ST7789_RST_HIGH();  HAL_Delay(1);
    ST7789_RST_LOW();   HAL_Delay(10);
    ST7789_RST_HIGH();  HAL_Delay(120);

    ST7789_WriteCmd(0x11);  HAL_Delay(120);  // Sleep Out（必须等 120ms）

    // 像素格式 16bpp
    ST7789_WriteCmd(0x3A);  fmt=0x55; ST7789_WriteData(&fmt, 1);

    // 电源/时序/伽玛参数（厂家推荐值，一般不改）
    ST7789_WriteCmd(0xB2);  // Porch control
    ST7789_WriteCmd(0xB7);  // Gate control
    ST7789_WriteCmd(0xBB);  // VCOM = 1.35V
    ST7789_WriteCmd(0xC0);  // LCM control
    ST7789_WriteCmd(0xC2);  // VDV/VRH
    ST7789_WriteCmd(0xC3);  // GVDD = 4.8V
    ST7789_WriteCmd(0xC4);  // VDV = 0V
    ST7789_WriteCmd(0xC6);  // Frame rate = 60Hz
    ST7789_WriteCmd(0xD0);  // Power control
    ST7789_WriteCmd(0xE0);  // 正伽玛
    ST7789_WriteCmd(0xE1);  // 负伽玛

    ST7789_WriteCmd(0x21);  // 反色（Normal → Inverted，因 ST7789 默认极性）
    ST7789_WriteCmd(0x36);  // MADCTL：方向 + RGB/BGR
    ST7789_WriteCmd(0x29);  // Display On
}
```

**复位时序要求**：ST7789 数据手册要求 RST 低电平至少 10ms，释放后至少等 120ms 再发命令。Sleep Out 命令后也需要 120ms 等待内部稳压器稳定。

**0x21 反色命令**：ST7789 默认极性是"反色"（0x20 是 Normal），所以需要发 0x21（Inversion On）才能得到正常显示效果。不同厂家模组可能不同，若颜色反了就去掉这条。

**MADCTL（0x36）**：控制显示方向（0°/90°/180°/270°）和颜色顺序（RGB/BGR）。`madctl = 0x00` 表示默认方向 + RGB 顺序；若屏幕颜色偏蓝偏红，可能需要设置 bit3（0x08）切换为 BGR。

### ST7789_Clear——全屏填充

```c
void ST7789_Clear(uint16_t color)
{
    uint32_t total_bytes = 240 * 240 * 2;  // 115,200 字节

    // 预填充缓冲区（480 字节 = 一行像素）
    for (uint32_t i = 0; i < ST7789_BUF_SIZE; i += 2) {
        ST7789_Buf[i]   = color >> 8;
        ST7789_Buf[i+1] = color & 0xFF;
    }

    ST7789_SetWindow(0, 0, 239, 239);

    // RAMWR 命令已由 SetWindow 发出
    // 按块写入（每块 480 字节 = 一行）
    uint32_t remain = total_bytes;
    while (remain) {
        uint32_t chunk = (remain > ST7789_BUF_SIZE) ? ST7789_BUF_SIZE : remain;
        ST7789_WriteDataDMA(ST7789_Buf, chunk);
        remain -= chunk;
    }
}
```

**为什么分块写入**：整屏 115,200 字节，如果用一个大缓冲区会占用 112.5KB RAM（超过 STM32F401RE 的 96KB SRAM）。所以用 480 字节缓冲区（刚好一行 240 像素 × 2 字节），反复发送同一块缓冲区内容。ST7789 的 GRAM 指针在写入后自动递增，不需要重新设置窗口。

**颜色字节序**：RGB565 在 C 代码中是 16bit 整数（小端存储），但 ST7789 需要高字节先出。所以先写 `color >> 8`（高 5 位 R），再写 `color & 0xFF`（低 3 位 G + 5 位 B）。

### ST7789_DrawBitLine16BPP——小端转大端

```c
void ST7789_DrawBitLine16BPP(uint16_t xs, uint16_t y, const uint8_t *p, uint16_t xsize)
{
    uint32_t bytes = (uint32_t)xsize * 2;

    for (uint32_t i = 0; i < bytes; i += 2) {
        uint8_t lo = p[i];      // 数组中低字节在前（小端）
        uint8_t hi = p[i+1];
        ST7789_Buf[i]   = hi;   // LCD 需要高字节先出
        ST7789_Buf[i+1] = lo;
    }

    ST7789_SetWindow(xs, y, xs + xsize - 1, y);
    ST7789_WriteCmd(0x2C);
    ST7789_WriteDataDMA(ST7789_Buf, bytes);
}
```

**小端 vs 大端**：C 编译器在 ARM 上用小端存储（低字节在低地址），但 ST7789 SPI 接口要求高字节先出（大端）。所以 `p[0]` 是低字节、`p[1]` 是高字节，需要交换后写入发送缓冲区。这个字节交换是所有 SPI LCD 驱动的通用操作。

### ST7789_ShowChar——字体渲染

```c
void ST7789_ShowChar(uint16_t x, uint16_t y, uint8_t ch, FontDef font, uint16_t color, uint16_t bgcolor)
{
    ST7789_SetWindow(x, y, x + w - 1, y + h - 1);
    ST7789_WriteCmd(0x2C);

    uint32_t wrote = 0;
    for (uint16_t row = 0; row < h; row++) {
        uint16_t bits = font.data[((uint32_t)(ch - 32) * h) + row];
        for (uint16_t col = 0; col < w; col++) {
            uint16_t c = (bits & (1U << (w - 1 - col))) ? color : bgcolor;

            ST7789_Buf[idx + 0] = (uint8_t)(c >> 8);
            ST7789_Buf[idx + 1] = (uint8_t)(c & 0xFF);
            wrote += 2;

            if ((wrote % ST7789_BUF_SIZE) == 0) {
                ST7789_WriteDataDMA(ST7789_Buf, ST7789_BUF_SIZE);
            }
        }
    }
    // 发送剩余不满一缓冲区的数据
    if (remain) ST7789_WriteDataDMA(ST7789_Buf, remain);
}
```

**字体数据格式**：`Font16x24_ASCII[]` 数组，每个字符占 24 个 `uint16_t`（24 行），每行 16bit，MSB = 最左像素。`ch - 32` 是因为空格（0x20）是第一个可打印字符。

**位取法**：`(bits >> (w - 1 - col)) & 1`，从高位到低位逐位取。bit=1 显示前景色，bit=0 显示背景色。

**分块 DMA 发送**：一个 16×24 字符 = 768 字节，超过 480 字节缓冲区，所以需要分两次 DMA 发送。`wrote` 计数器在满 480 字节时触发一次 DMA。

### ST7789_TestFrameRate——帧率测试

```c
void ST7789_TestFrameRate(void)
{
    static const uint16_t test_colors[] = { RED, GREEN, BLUE, WHITE, BLACK, CYAN, MAGENTA, YELLOW };
    const uint32_t frame_count = 60U;

    tick_start = HAL_GetTick();
    for (uint32_t f = 0; f < frame_count; f++) {
        uint16_t color = test_colors[f % 8];
        ST7789_FillRect(0, 0, 239, 239, color);
    }
    elapsed = HAL_GetTick() - tick_start;

    double fps = (double)frame_count * 1000.0 / (double)elapsed;
    // 居中显示 FPS 数字
    ST7789_Clear(BLACK);
    ST7789_ShowString(x, y, fps_text, Font16x24, WHITE, BLACK);
}
```

**测试方法**：60 帧全屏填充不同颜色，计时计算 FPS。`HAL_GetTick()` 返回毫秒级时间戳。

**FPS 计算**：`fps = frame_count * 1000 / elapsed_ms`。例如 60 帧耗时 500ms → 120 FPS。

**最后显示 FPS**：测试结束后清屏，在屏幕中央用 16×24 字体显示 FPS 数值。`snprintf` 格式化浮点数到字符串，然后计算居中坐标。

### fonts.h 与 font16x24_ascii.h

```c
// fonts.h
typedef struct {
    const uint8_t width;
    uint8_t height;
    const uint16_t *data;
} FontDef;

extern FontDef Font16x24;

// font16x24_ascii.h
uint16_t Font16x24_ASCII[] = {
    /* 0x20 ' ' */ 0x0000, 0x0000, ... (24 个 uint16_t)
    /* 0x21 '!' */ 0x07FF, 0x07FF, ...
    // ... 共 95 个字符（0x20~0x7E）
};
FontDef Font16x24 = { 16, 24, Font16x24_ASCII };
```

**位图字体原理**：每个字符是一个 16×24 的位图，每行 16bit（uint16_t），bit=1 画前景色，bit=0 画背景色。共 95 个 ASCII 可打印字符，数据量 = 95 × 24 × 2 = 4,560 字节。

**FontDef 结构体**：`width` 是每行位数（最大 16，`ShowChar` 里有裁剪），`height` 是行数，`data` 指向位图数组。设计上支持不同尺寸的字体（只要换数据数组），但本例只有 16×24 一种。

### main.c——应用层

```c
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init(); MX_DMA_Init();
    MX_USART2_UART_Init(); MX_SPI1_Init(); MX_ADC1_Init();

    ST7789_Init();
    ST7789_Clear(GRED);
    ST7789_TestFrameRate();

    while (1) {}
}
```

**CubeMX 生成的外设初始化顺序**：GPIO → DMA → USART2 → SPI1 → ADC1。DMA 必须在 SPI1 之前初始化（SPI1 的 DMA 传输依赖 DMA 控制器）。

**ADC1 初始化**：本例代码中未使用 ADC，可能是 CubeMX 工程残留配置。

## 设计问题与改进空间

1. **DMA 轮询等待**：`ST7789_WriteDataDMA` 在 DMA 发送期间 CPU 空转等待，没有利用 DMA 异步特性。可改为 DMA 完成中断 + 双缓冲：CPU 准备下一帧数据的同时 DMA 发送当前帧。

2. **阻塞式 I/O**：命令和小数据用 `HAL_SPI_Transmit` 阻塞发送，每条命令都有 HAL 超时检查开销。对于初始化序列（只执行一次）可接受，但高频调用场景（如动画）应优化。

3. **缓冲区仅一行**：`ST7789_BUF_SIZE = 240*2 = 480` 字节，整屏需要 240 次 DMA 调用。若 RAM 允许，增大到 240*240*2（112.5KB）可一次 DMA 发完整屏，帧率会显著提升。

4. **无 clipping 优化**：`DrawHLine`/`DrawVLine`/`FillRect` 都有越界检查和裁剪，但 `ShowChar` 的裁剪较粗糙（字符部分超出屏幕就直接 return，不画部分可见的像素）。

5. **TestFrameRate 用 %f**：`printf("...%.2f FPS...", fps)` 和 `snprintf(fps_text, ..., "%.2f FPS", fps)` 使用浮点格式化，链接浮点库增加 5~10KB Flash。可改为定点打印（与 14/15 例同策略）。

6. **与 25/26 例的关系**：25 例在同一驱动基础上增加位图显示功能，26 例集成 LVGL 图形库。24 例是最小可运行的 LCD 驱动，后续例程复用此驱动。

## 关联笔记

- [[23_rocketpi_ws2812b|23 WS2812B]]：DMA+PWM 时序编码，对比 SPI+DMA 像素传输
- [[16_rocketpi_pwm_passive_buzzer|16 PWM 蜂鸣器]]：PWM 频率计算，对比 SPI 时钟分频
- [[07_rocketpi_uart_echo|07 UART DMA]]：DMA 传输基础（UART 版）
