---
status: done
created: 2026-09-16
tags:
  - c/spi
  - c/lvgl
  - embedded/gui
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/26_rocketpi_spi_lcd_240x240_lvgl"
  - "[[25_rocketpi_spi_lcd_bitmap]] SPI LCD 位图显示（26 例的基础）"
  - "[[21_rocketpi_adc_joystick]] ADC 摇杆（26 例的输入源）"
---

# 26 SPI LCD LVGL GUI 框架

## 一句话定性

在 ST7789 240×240 LCD 上移植 LVGL 9.2 图形库，通过摇杆输入驱动 GUI 控件（按键/编码器演示），实现嵌入式系统的完整 GUI 框架——从底层像素驱动到上层控件交互。

## 同类产品定位

- **LVGL**：开源嵌入式 GUI 库，MIT 许可，支持 30+ 平台，代码量 ~100KB（精简后）
- **emWin**：Segger 商业 GUI 库，STM32 免费授权
- **TouchGFX**：ST 官方 GUI 框架，专为 STM32 优化
- **μGFX**：轻量级 GUI，适合资源受限平台
- **本例选型理由**：LVGL 开源免费、社区活跃、文档完善、适合学习嵌入式 GUI 开发

## 硬件连接

- LCD：PA5=SCK, PA7=MOSI, PA4=CS, PA1=DC, PA0=RST, PA2=BL（同 24/25 例）
- 摇杆：PC4=X(ADC1_IN14), PC5=Y(ADC1_IN15), 按键=GPIO 输入（同 21 例）
- 定时器：TIM3（LVGL 心跳 + 可选帧率测试）

## 通信协议要点

- LVGL 不直接碰硬件，通过 `lv_port_disp`（显示驱动）和 `lv_port_indev`（输入驱动）适配
- 显示驱动：调用 ST7789 的 `SetWindow` + `WriteDataDMA` 将帧缓冲区刷新到 LCD
- 输入驱动：读取 ADC 摇杆位置，转换为 LVGL 的方向键事件

## 代码架构

```
┌─────────────────────────────────────────────────┐
│ main.c (应用层)                                 │
│  - lv_init → lv_port_disp_init →               │
│    lv_port_indev_init → lv_demo_keypad_encoder  │
│  - 主循环：lv_timer_handler + HAL_Delay(1)      │
├─────────────────────────────────────────────────┤
│ lv_demos (LVGL 官方演示)                       │
│  - lv_demo_keypad_encoder：按键/编码器控件演示  │
├─────────────────────────────────────────────────┤
│ lv_port_disp / lv_port_indev (LVGL 移植层)     │
│  - 显示：帧缓冲区 → ST7789 WriteDataDMA        │
│  - 输入：ADC 摇杆 → LVGL 方向键事件            │
├─────────────────────────────────────────────────┤
│ st7789.c (LCD 驱动，复用 24/25 例)             │
├─────────────────────────────────────────────────┤
│ driver_adc_joystick_lcd.c (摇杆 LCD 可视化)    │
│  - 独立于 LVGL 的摇杆光点绘制                   │
├─────────────────────────────────────────────────┤
│ driver_adc_joystick_test.c (摇杆采样，复用 21) │
├─────────────────────────────────────────────────┤
│ lvgl/ (LVGL 9.2 库源码)                        │
└─────────────────────────────────────────────────┘
```

## 与 24/25 例的关键差异

| 维度 | 24 帧率测试 | 25 位图显示 | 26 LVGL GUI |
|------|------------|------------|-------------|
| 显示层 | FillRect 全屏 | DrawBitmap 像素 | LVGL 帧缓冲 |
| 交互 | 无 | 无 | 摇杆→GUI 控件 |
| 输入处理 | 无 | 无 | ADC→方向键→LVGL 事件 |
| 复杂度 | 低（单函数） | 中（5 个绘图函数） | 高（GUI 框架 + 移植层） |
| Flash 占用 | ~30KB | ~145KB（含位图） | ~200KB+（含 LVGL 库） |
| RAM 占用 | ~1KB | ~1KB | ~20KB+（帧缓冲 + 控件树） |
| 代码来源 | 手写 | 手写 | LVGL 官方库 + 移植层 |

## 核心实现详解

### main.c——LVGL 初始化流程

```c
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init(); MX_DMA_Init(); MX_USART2_UART_Init();
    MX_SPI1_Init(); MX_ADC1_Init(); MX_TIM3_Init();

#if ENABLE_ST7789_FPS_TEST
    ST7789_Init();
    ST7789_TestFrameRate();  // 可选：先跑帧率测试
#endif

    lv_init();                // ① LVGL 核心初始化
    lv_port_disp_init();      // ② 显示驱动移植
    lv_port_indev_init();     // ③ 输入驱动移植

    lv_demo_keypad_encoder(); // ④ 加载官方演示

    // ⑤ 将摇杆绑定到 LVGL 输入设备
    lv_indev_t *keypad = lv_port_indev_get_keypad();
    lv_group_t *group = lv_group_get_default();
    if ((keypad != NULL) && (group != NULL)) {
        lv_indev_set_group(keypad, group);
    }

    while (1) {
        lv_timer_handler();   // ⑥ LVGL 事件循环
        HAL_Delay(1);         // ⑦ 1ms 周期
    }
}
```

- **① lv_init**：初始化 LVGL 内核（内存管理、定时器、事件系统）
- **② lv_port_disp_init**：注册显示驱动，告诉 LVGL 如何将像素写到 LCD（调用 ST7789 的 API）
- **③ lv_port_indev_init**：注册输入设备，告诉 LVGL 如何读取摇杆状态
- **④ lv_demo_keypad_encoder**：加载 LVGL 官方的按键/编码器演示，包含按钮、滑块、下拉菜单等控件
- **⑤ 输入绑定**：将摇杆输入设备绑定到默认控件组，摇杆方向键可以在控件间切换焦点
- **⑥ lv_timer_handler**：LVGL 的主循环驱动函数，处理动画、重绘、输入事件。必须定期调用
- **⑦ HAL_Delay(1)**：1ms 周期，LVGL 内部定时器精度为 1ms

### lv_port_disp——显示驱动移植

LVGL 的显示驱动需要实现一个 `flush` 回调：

```c
// 伪代码，实际在 lv_port_disp.c 中
static void disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint16_t x0 = area->x1;
    uint16_t y0 = area->y1;
    uint16_t x1 = area->x2;
    uint16_t y1 = area->y2;

    ST7789_SetWindow(x0, y0, x1, y1);
    ST7789_WriteCmd(0x2C);  // RAMWR
    ST7789_WriteDataDMA(px_map, (x1 - x0 + 1) * (y1 - y0 + 1) * 2);

    lv_display_flush_ready(disp);  // 通知 LVGL 刷新完成
}
```

LVGL 维护一个帧缓冲区（frame buffer），当控件内容变化时，LVGL 计算脏区域（dirty area），调用 `flush` 将该区域的像素数据发送到 LCD。

### lv_port_indev——输入驱动移植

LVGL 的输入设备需要实现一个 `read` 回调：

```c
// 伪代码，实际在 lv_port_indev.c 中
static void indev_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    adc_joystick_sample_t sample;
    adc_joystick_test_sample(&sample);

    // 将摇杆位置转换为方向键事件
    if (sample.x.raw < 1600) data->key = LV_KEY_LEFT;
    else if (sample.x.raw > 2900) data->key = LV_KEY_RIGHT;
    else if (sample.y.raw < 1600) data->key = LV_KEY_UP;
    else if (sample.y.raw > 2900) data->key = LV_KEY_DOWN;

    data->state = (sample.key_pressed) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
```

**与 21 例的复用**：摇杆采样直接复用 `driver_adc_joystick_test.c` 的 `adc_joystick_test_sample` 函数，LVGL 移植层只做坐标→方向键的映射。

### driver_adc_joystick_lcd.c——独立的摇杆可视化

**作用**：不依赖 LVGL，直接在 LCD 上绘制摇杆位置的光点。可与 LVGL 共存或独立使用。

```c
void adc_joystick_lcd_draw(const adc_joystick_sample_t *sample)
{
    // ⑧ ADC 原始值 → 屏幕坐标（带边距）
    x = adc_joystick_lcd_map_axis(sample->x.raw, ST7789_WIDTH);
    y = adc_joystick_lcd_map_axis(sample->y.raw, ST7789_HIGHT);

    // ⑨ 擦除旧光点（用背景色画圆）
    if (s_has_dot) {
        adc_joystick_lcd_draw_filled_circle(s_prev_x, s_prev_y, 8, BLACK);
    }

    // ⑩ 绘制新光点
    adc_joystick_lcd_draw_filled_circle(x, y, 8, CYAN);

    s_prev_x = x;
    s_prev_y = y;
    s_has_dot = 1;
}
```

- **⑧ map_axis**：将 ADC 0~4095 映射到屏幕坐标，留 8 像素边距防止光点超出屏幕
- **⑨⑩ 先擦后画**：用背景色（BLACK）画旧位置的圆擦除，再用前景色（CYAN）画新位置。简单的"脏矩形"动画

### adc_joystick_lcd_draw_filled_circle——填充圆

```c
static void adc_joystick_lcd_draw_filled_circle(uint16_t cx, uint16_t cy, uint16_t radius, uint16_t color)
{
    const int32_t r_sq = (int32_t)radius * radius;
    for (int32_t dy = -r; dy <= r; ++dy) {
        for (int32_t dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy * dy > r_sq) continue;  // 圆外跳过
            ST7789_DrawPixel(cx + dx, cy + dy, color);
        }
    }
}
```

**逐像素绘制**：对每个像素检查是否在圆内（`dx²+dy² ≤ r²`），是则画点。效率低（8 半径圆约 201 次 DrawPixel），但代码简单。可优化为逐行扫描 + 整行 DMA 发送。

## 设计问题与改进空间

1. **LVGL 帧缓冲区 RAM 开销**：240×240×2 = 115KB 全帧缓冲超出 F401RE 的 96KB SRAM。实际使用部分缓冲（如 240×40 = 19KB）或多缓冲策略。

2. **`lv_timer_handler` 的调用频率**：1ms 周期调用，但 LVGL 内部定时器精度也是 1ms，可能导致动画不够流畅。可改为在 SysTick 中断中调用 `lv_tick_inc(1)`，主循环不加延时。

3. **`adc_joystick_lcd_draw_filled_circle` 效率**：逐像素 DrawPixel，每个像素都走 SetWindow+RAMWR+WriteData。可改为整行批量发送。

4. **LVGL 库代码量**：LVGL 9.2 精简后约 100KB Flash + 20KB RAM，对 F401RE（512KB Flash/96KB RAM）来说可接受但紧张。可通过 `lv_conf.h` 裁剪不需要的控件和功能。

5. **摇杆→方向键的映射精度**：硬编码阈值（1600/2900），与 21 例相同。可改为自动校准。

## 关联笔记

- [[25_rocketpi_spi_lcd_bitmap|25 SPI LCD 位图显示]]：ST7789 驱动层，26 例复用
- [[21_rocketpi_adc_joystick|21 ADC 摇杆]]：摇杆采样层，26 例复用
- [[24_rocketpi_spi_lcd_speedtest|24 SPI LCD 帧率测试]]：ST7789 初始化，26 例可选复用
