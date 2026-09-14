---
status: done
created: 2026-09-14
tags:
  - c/adc
  - c/input-device
  - embedded/sensor
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/21_rocketpi_adc_joystick"
  - "[[20_rocketpi_adc_mcu_temperature]] 内部 ADC（对比外部 ADC 采样）"
---

# 21 ADC 双轴摇杆

## 一句话定性

双轴摇杆模块通过两个电位器输出 X/Y 轴的模拟电压，ADC 采样后得到摇杆位置，加上按键输入，用于嵌入式系统的人机交互（游戏手柄、菜单导航、机器人控制等）。

## 同类产品定位

- **双轴摇杆模块**：两个电位器 + 一个按键，模拟输出，成本 ~¥3
- **旋转编码器**：数字输出，无 ADC 需求，适合旋钮式输入
- **触摸屏**：坐标输入，成本高，适合复杂 UI
- **本例选型理由**：摇杆模块最简单直观，适合学习 ADC 外部信号采样

## 硬件连接

- X 轴：PC4 = ADC1_IN14（模拟输入）
- Y 轴：PC5 = ADC1_IN15（模拟输入）
- 按键：GPIO 输入（低电平有效）
- 供电：3.3V，GND

## 通信协议要点

- 无通信协议，纯 ADC 采样
- X/Y 轴：电位器分压，0~3.3V 对应 0~4095 ADC 值
- 中位：约 2048（电位器中间位置）
- 按键：GPIO 读取，低电平 = 按下

## 代码架构

```
┌─────────────────────────────────────────────────┐
│ main.c (应用层)                                 │
│  - 调用 adc_joystick_test_run(0, 200)          │
├─────────────────────────────────────────────────┤
│ driver_adc_joystick_test.c (驱动层)            │
│  - sample：ADC 采样 + 千分比缩放 + 按键读取    │
│  - run：循环采样打印                            │
│  - send_udlr_uart：摇杆→方向码 UART 输出       │
│  - map_direction：安装方向旋转映射              │
├─────────────────────────────────────────────────┤
│ adc.c (CubeMX 生成)                             │
│  - ADC1：12 位、双通道扫描、连续转换            │
│  - Rank 1 = CH14(X)，Rank 2 = CH15(Y)          │
│  - PC4/PC5 模拟输入                             │
└─────────────────────────────────────────────────┘
```

## 与 20 例（MCU 温度）的关键差异

| 维度 | 20 MCU 温度 | 21 摇杆 |
|------|-------------|---------|
| 信号源 | 内部温度传感器 | 外部电位器 |
| 通道 | TEMPSENSOR + VREFINT | CH14 + CH15 |
| 引脚 | 无（内部信号） | PC4 + PC5（模拟输入） |
| 校准 | VREFINT + 工厂常数 | 无需校准（直接读值） |
| 滤波 | 中值滤波（5 次） | 无滤波（单次采样） |
| 连续模式 | DISABLE（单次触发） | ENABLE（连续转换） |
| 附加功能 | 无 | 按键 + 方向映射 + 安装旋转 |

## 核心实现详解

### ADC 配置——双通道连续转换

```c
hadc1.Init.ScanConvMode = ENABLE;
hadc1.Init.ContinuousConvMode = ENABLE;   // 连续转换（与 20 例不同）
hadc1.Init.NbrOfConversion = 2;

sConfig.Channel = ADC_CHANNEL_14;   // PC4 = X 轴
sConfig.Rank = 1;
sConfig.SamplingTime = ADC_SAMPLETIME_112CYCLES;

sConfig.Channel = ADC_CHANNEL_15;   // PC5 = Y 轴
sConfig.Rank = 2;
```

**与 20 例对比**：
- 20 例：`ContinuousConvMode = DISABLE`，每次手动 `HAL_ADC_Start` 触发一轮
- 21 例：`ContinuousConvMode = ENABLE`，`HAL_ADC_Start` 后自动循环转换

**采样时间 112 周期**：外部电位器阻抗低（~10kΩ），比温度传感器（高阻抗）需要的采样时间短得多。112/21MHz ≈ 5.3µs。

**GPIO 模拟输入**：`GPIO_MODE_ANALOG` 模式下，GPIO 引脚直连 ADC 输入，数字部分断开（无上拉/下拉、无输出驱动）。

### adc_joystick_test_sample——单次采样

**作用**：触发一轮 ADC 扫描，读取 X/Y 轴原始值，转换为千分比，读取按键状态。

```c
uint8_t adc_joystick_test_sample(adc_joystick_sample_t *sample)
{
    HAL_ADC_Start(&hadc1);                           // 触发扫描
    HAL_ADC_PollForConversion(&hadc1, 10);           // 等 X 转换
    raw_x = (uint16_t)HAL_ADC_GetValue(&hadc1);      // 读 X
    HAL_ADC_PollForConversion(&hadc1, 10);           // 等 Y 转换
    raw_y = (uint16_t)HAL_ADC_GetValue(&hadc1);      // 读 Y
    HAL_ADC_Stop(&hadc1);

    sample->x.raw = raw_x;
    sample->x.permille = adc_joystick_test_scale_permille(raw_x);
    sample->y.raw = raw_y;
    sample->y.permille = adc_joystick_test_scale_permille(raw_y);
    sample->key_pressed = adc_joystick_test_read_key_internal();
}
```

**两次 PollForConversion**：扫描模式下，第一次返回 Rank 1（X），第二次返回 Rank 2（Y）。与 20 例完全相同的模式。

### adc_joystick_test_scale_permille——千分比缩放

**作用**：将 ADC 原始值（0~4095）映射到千分比（0~1000），分辨率为 0.1%。

```c
static uint16_t adc_joystick_test_scale_permille(uint16_t raw)
{
    uint32_t permille = ((uint32_t)raw * 1000U + (4095U / 2U)) / 4095U;
    if (permille > 1000U) permille = 1000U;
    return (uint16_t)permille;
}
```

- **`raw * 1000 + 2047`**：先乘后除 + 四舍五入（加除数的一半）
- **千分比 vs 百分比**：千分比提供 0.1% 分辨率，比百分比（1%）更精细，适合摇杆的微小位移检测

### adc_joystick_test_map_direction——方向映射

**作用**：将 ADC 原始值与阈值比较，判定上下左右方向，支持安装旋转角度补偿。

```c
#define ADC_X_LEFT_THR    1600U  // X < 1600 = 左
#define ADC_X_RIGHT_THR   2900U  // X > 2900 = 右
#define ADC_Y_DOWN_THR    2900U  // Y > 2900 = 下
#define ADC_Y_UP_THR      1600U  // Y < 1600 = 上
```

**阈值含义**：中位约 2048，偏移约 448（~11%）判定为方向。阈值基于实验数据，非计算得出。

**安装旋转映射**：

```c
#if ADC_JOYSTICK_DEFAULT_ORIENTATION == ADC_JOYSTICK_ORIENTATION_180_DEG
    const uint8_t logical_left  = phys_right;   // 物理右 = 逻辑左
    const uint8_t logical_right = phys_left;
    const uint8_t logical_up    = phys_down;
    const uint8_t logical_down  = phys_up;
#endif
```

摇杆模块可能以不同角度安装在电路板上，物理方向与逻辑方向不一致。通过宏配置旋转角度（0°/90°/180°/270°），自动映射物理方向到逻辑方向。本例默认 180°（摇杆倒装）。

### adc_joystick_send_udlr_uart——方向码 UART 输出

**作用**：持续采样摇杆，将方向转换为小键盘风格的 ASCII 码（4=左、6=右、8=上、5=下），通过 UART 发送。

```c
uint8_t adc_joystick_send_udlr_uart(uint32_t sample_count, uint32_t delay_ms, UART_HandleTypeDef *huart)
{
    while (1) {
        adc_joystick_test_sample(&sample);
        char out[4];
        uint32_t n = 0;
        adc_joystick_test_map_direction(&sample, out, &n);
        if (n > 0) {
            HAL_UART_Transmit(huart, (uint8_t *)out, n, 10);
        }
        HAL_Delay(delay);
    }
}
```

**设计意图**：将摇杆输入转换为标准方向码，上位机程序可直接解析。小键盘编码（4/6/8/5）比 UDLR 文本更紧凑。

### adc_joystick_test_run——调试打印

**作用**：循环采样并打印 X/Y 原始值、百分比、按键状态。

```c
printf("adc joystick: #%lu X=%4u (%3lu.%01lu%%) Y=%4u (%3lu.%01lu%%) KEY=%s\r\n",
       (unsigned long)(i + 1U),
       (unsigned int)sample.x.raw,
       (unsigned long)(sample.x.permille / 10U),
       (unsigned long)(sample.x.permille % 10U),
       // ...
       (sample.key_pressed != 0U) ? "DOWN" : "UP");
```

**定点打印百分比**：`permille / 10` 得整数部分，`permille % 10` 得小数部分，避免 `%f` 浮点库开销。如 permille=523 → `52.3%`。

## 设计问题与改进空间

1. **无滤波**：单次 ADC 采样可能有噪声抖动。可加滑动平均或中值滤波（如 20 例的 5 次中值），但摇杆本身有机械阻尼，抖动影响较小。

2. **阈值硬编码**：`ADC_X_LEFT_THR` 等 4 个阈值基于实验数据硬编码。不同摇杆模块的中位值可能不同，可改为自动校准（上电时读中位值）。

3. **`ContinuousConvMode = ENABLE` 但手动 Stop**：配置为连续模式，但代码中每次 `HAL_ADC_Start` 后都 `HAL_ADC_Stop`。实际上连续模式下 Start 后会自动循环转换，Stop 才停止。这里的行为等价于单次模式，配置可以改为 `DISABLE`。

4. **`send_udlr_uart` 的死循环**：`sample_count = 0` 时进入 `while(1)` 永不退出。可加退出条件（如按键长按退出）。

5. **与 20 例的 ADC 设计对比**：20 例用 VREFINT 校准 VDDA，21 例直接读原始值。外部电位器由 VDDA 供电，ADC 值 = `Vpin/VDDA × 4095`，VDDA 变化时原始值不变（分子分母同比例），所以不需要校准。

## 关联笔记

- [[20_rocketpi_adc_mcu_temperature|20 ADC MCU 温度]]：内部 ADC 采样，对比外部 ADC 采样
- [[08_rocketpi_uart_control_led|08 UART 命令控制 LED]]：另一种人机交互方式（串口命令 vs 摇杆）
- [[17_rocketpi_pwm_sg90|17 PWM 舵机]]：摇杆控制舵机的典型应用场景
