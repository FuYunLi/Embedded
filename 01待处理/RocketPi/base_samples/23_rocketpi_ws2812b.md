---
status: done
created: 2026-09-14
tags:
  - c/spi-like
  - c/dma
  - embedded/led
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/23_rocketpi_ws2812b"
  - "[[16_rocketpi_pwm_passive_buzzer]] PWM 输出（对比 WS2812B 的 PWM+DMA 编码）"
  - "[[07_rocketpi_uart_echo]] DMA 传输（对比 WS2812B 的 DMA+PWM）"
---

# 23 WS2812B 可编程 LED 灯带

## 一句话定性

WS2812B 是单线串行协议的 RGB LED，每个像素可独立控制颜色（24 位 RGB），通过精确时序的脉冲编码传输数据，MCU 用 PWM+DMA 实现纳秒级时序，用于嵌入式系统的彩色灯光显示。

## 同类产品定位

- **WS2812B**：最常用的可编程 LED，单线串行，800kHz 数据率，成本 ~¥0.1/颗
- **WS2811**：WS2812B 的前代，外置驱动 IC
- **SK6812**：兼容 WS2812B，增加白光通道（RGBW）
- **APA102**：SPI 接口，刷新率更高，但需要两根线（CLK+DATA）
- **本例选型理由**：WS2812B 最普及、成本最低、单线控制，适合学习 DMA+PWM 时序编码

## 硬件连接

- 数据引脚：PB1 = TIM3_CH4（PWM 输出）
- 驱动方式：PWM 占空比编码 0/1 位，DMA 批量传输
- 电源：5V（LED 供电），3.3V 数据电平可直接驱动（短距离）
- 级联：DOUT → 下一个 DIN，支持无限级联（理论）

## 通信协议要点

- **单线串行**：数据通过精确时序的高/低电平脉冲传输
- **位编码**：逻辑 1 = 高 700ns + 低 600ns；逻辑 0 = 高 350ns + 低 800ns
- **帧格式**：每个 LED 24 位（GRB 顺序，非 RGB），高位先发
- **复位信号**：低电平 >50µs 表示帧结束
- **数据率**：800kHz，每位 1.25µs

## 代码架构

```
┌─────────────────────────────────────────────────┐
│ main.c (应用层)                                 │
│  - 6 种灯效演示：blink/chase/rainbow/breathe/  │
│    theater_chase/gradient_wipe                  │
├─────────────────────────────────────────────────┤
│ driver_ws2812b_test.c (灯效层)                 │
│  - 6 种预设灯效模式                             │
├─────────────────────────────────────────────────┤
│ driver_ws2812b.c (驱动层)                      │
│  - init / set_pixel / fill / refresh           │
│  - ws_pack_byte：位→PWM 占空比编码              │
│  - ws_build_buffer：像素→DMA 缓冲区             │
│  - HAL_TIM_PWM_PulseFinishedCallback：DMA 完成  │
├─────────────────────────────────────────────────┤
│ tim.c + dma.c (CubeMX 生成)                    │
│  - TIM3 CH4 PWM：84MHz / 105 = 800kHz          │
│  - DMA1_Stream2：内存→外设，半字传输            │
│  - PB1 复用 AF2                                 │
└─────────────────────────────────────────────────┘
```

## 与 16 例（蜂鸣器）的关键差异

| 维度 | 16 蜂鸣器 | 23 WS2812B |
|------|----------|------------|
| PWM 语义 | 频率 = 音调 | 占空比 = 位值 |
| 频率 | 可变 | 固定 800kHz |
| 数据量 | 单个音符 | N×24 位（N 个 LED） |
| 传输方式 | CPU 逐个写 CCR | DMA 批量传输 |
| 阻塞 | 是（HAL_Delay） | 否（DMA 异步） |
| 定时器 | TIM3 CH3 | TIM3 CH4 |
| 精度 | 1µs | ~12ns（84MHz） |

## 核心实现详解

### TIM3 配置——800kHz PWM + DMA

```c
htim3.Init.Prescaler = 0;           // 不分频，84MHz
htim3.Init.Period = 105-1;          // 84MHz / 105 = 800kHz → 1.25µs/位
```

**每位 1.25µs**：WS2812B 协议要求每位持续 1.25µs。84MHz / 105 = 800kHz，精确匹配。

**DMA 配置**：

```c
hdma_tim3_ch4_up.Init.Direction = DMA_MEMORY_TO_PERIPH;
hdma_tim3_ch4_up.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;  // 16 位
hdma_tim3_ch4_up.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
hdma_tim3_ch4_up.Init.Mode = DMA_NORMAL;  // 单次传输
```

**双 DMA 链接**：`__HAL_LINKDMA(tim_pwmHandle, hdma[TIM_DMA_ID_CC4], ...)` 和 `__HAL_LINKDMA(tim_pwmHandle, hdma[TIM_DMA_ID_UPDATE], ...)`。TIM3 的 CC4 事件和 UPDATE 事件共用同一个 DMA 流，每次计数器到达 CCR 值时 DMA 自动送出下一个占空比值。

### ws_pack_byte——位编码

**作用**：将一个字节（MSB 先发）编码为 8 个 PWM 占空比值，写入 DMA 缓冲区。

```c
#define WS2812B_T0H_TICKS   32U   // 逻辑 0：高 32/105 ≈ 381ns
#define WS2812B_T1H_TICKS   70U   // 逻辑 1：高 70/105 ≈ 833ns

static void ws_pack_byte(uint8_t value, uint16_t **ptr)
{
    for (int bit = 7; bit >= 0; --bit) {
        **ptr = (value & (1U << bit)) ? WS2812B_T1H_TICKS : WS2812B_T0H_TICKS;
        (*ptr)++;
    }
}
```

**PWM 占空比编码**：WS2812B 不看绝对时间，看高电平占比。`T0H=32` 表示 32/105 ≈ 30.5% 占空比（逻辑 0），`T1H=70` 表示 70/105 ≈ 66.7% 占空比（逻辑 1）。低电平时间由剩余的 `105-32=73` 或 `105-70=35` ticks 决定。

**MSB 先发**：`bit = 7` 开始，与 I2C/WS2812B 协议一致。

### ws_build_buffer——像素→DMA 缓冲区

**作用**：将所有 LED 的 RGB 像素值编码为 DMA 缓冲区，末尾追加复位信号。

```c
static uint16_t ws_build_buffer(void)
{
    uint16_t *p = ws_dma_buf;
    for (uint16_t i = 0; i < ws_led_count; ++i) {
        uint8_t red = ws_pixels[i][0];
        uint8_t green = ws_pixels[i][1];
        uint8_t blue = ws_pixels[i][2];
        ws_pack_byte(green, &p);   // WS2812B 先发绿色！
        ws_pack_byte(red, &p);
        ws_pack_byte(blue, &p);
    }

    for (uint16_t i = 0; i < WS2812B_RESET_SLOTS; ++i) {
        *p++ = 0U;   // 复位信号：低电平 >50µs
    }

    return (uint16_t)(p - ws_dma_buf);
}
```

**GRB 顺序**：WS2812B 数据格式是 Green-Red-Blue，不是 RGB。这是硬件设计决定的，每颗芯片内部固定。

**复位信号**：80 个 0 值（低电平）= 80 × 1.25µs = 100µs > 50µs，满足复位要求。

**DMA 缓冲区大小**：`300 LEDs × 24 bits + 80 reset = 7280 个 uint16_t`，约 14.2KB RAM。

### ws2812b_refresh——DMA 发送

**作用**：将像素缓冲区编码为 DMA 缓冲区，启动 DMA 传输到 LED 灯带。

```c
bool ws2812b_refresh(void)
{
    if ((ws_tim == NULL) || (ws_led_count == 0U) || ws_dma_busy) {
        return false;
    }

    uint16_t payload = ws_build_buffer();
    ws_dma_busy = true;

    if (HAL_TIM_PWM_Start_DMA(ws_tim, WS2812B_TIMER_CHANNEL,
                              (uint32_t *)ws_dma_buf, payload) != HAL_OK) {
        ws_dma_busy = false;
        return false;
    }
    return true;
}
```

**非阻塞设计**：`ws_dma_busy` 标志防止重复发送。DMA 完成后在回调中清除标志。

### HAL_TIM_PWM_PulseFinishedCallback——DMA 完成回调

```c
void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim)
{
    if ((ws_tim != NULL) && (htim == ws_tim)) {
        HAL_TIM_PWM_Stop_DMA(ws_tim, WS2812B_TIMER_CHANNEL);
        __HAL_TIM_SET_COMPARE(ws_tim, WS2812B_TIMER_CHANNEL, 0U);  // 输出低电平
        ws_dma_busy = false;
    }
}
```

**停止后输出低电平**：DMA 传输完成后，TIM3 继续输出最后一个占空比值。手动将 CCR 设为 0 确保输出低电平，避免 LED 误触发。

### ws2812b_set_pixel / ws2812b_fill——像素操作

```c
bool ws2812b_set_pixel(uint16_t index, uint8_t red, uint8_t green, uint8_t blue)
{
    if ((index >= ws_led_count) || (ws_tim == NULL)) return false;
    ws_pixels[index][0] = red;
    ws_pixels[index][1] = green;
    ws_pixels[index][2] = blue;
    return true;
}

void ws2812b_fill(uint8_t red, uint8_t green, uint8_t blue)
{
    for (uint16_t i = 0; i < ws_led_count; ++i) {
        ws2812b_set_pixel(i, red, green, blue);
    }
}
```

**双缓冲设计**：`ws_pixels` 是像素缓冲区（staging buffer），`ws_dma_buf` 是 DMA 缓冲区。修改像素不影响正在 DMA 传输的数据，`refresh` 时才从 `ws_pixels` 编码到 `ws_dma_buf`。

### driver_ws2812b_test.c——6 种灯效

| 灯效 | 效果 | 关键参数 |
|------|------|----------|
| blink | 全带闪烁 | on/off 时间 |
| chase | 单点追逐 | 步进延时 |
| rainbow | 彩虹渐变 | 色相旋转速度 |
| breathe | 呼吸灯 | 亮度步进 |
| theater_chase | 间隔追逐 | 每 3 颗亮 1 颗 |
| gradient_wipe | 渐变擦除 | 起止颜色混合 |

## 设计问题与改进空间

1. **DMA 缓冲区 14.2KB**：300 LEDs × 24 × 2 bytes + 160 bytes = 14.5KB。STM32F401RE 有 96KB SRAM，可接受，但对小容量 MCU 是问题。可改为边编码边发送（双缓冲 DMA）。

2. **`ws_pack_byte` 的时序精度**：32 ticks = 381ns（规范 350ns ±150ns），70 ticks = 833ns（规范 700ns ±150ns）。381ns 偏离 350ns 约 9%，在容许范围内，但不同批次 WS2812B 可能容忍度不同。

3. **阻塞 vs 非阻塞灯效**：`ws2812b_test_*` 函数内部用 `HAL_Delay` 阻塞，整个灯效播放期间 CPU 不能做其他事。可改为状态机非阻塞，但复杂度大增。

4. **无 gamma 校正**：LED 亮度与 PWM 占空比是线性关系，但人眼对亮度的感知是指数的。加 gamma 校正（`output = (input/255)^2.2 × 255`）可让颜色渐变更自然。

5. **与 16 例的 PWM 设计对比**：16 例用 PWM 频率控制音调，23 例用 PWM 占空比编码数据。同一外设（TIM3），两种完全不同的应用。

## 关联笔记

- [[16_rocketpi_pwm_passive_buzzer|16 PWM 蜂鸣器]]：PWM 频率控制，对比占空比编码
- [[07_rocketpi_uart_echo|07 UART DMA]]：DMA 传输基础
- [[17_rocketpi_pwm_sg90|17 PWM 舵机]]：TIM3 的另一种用途
