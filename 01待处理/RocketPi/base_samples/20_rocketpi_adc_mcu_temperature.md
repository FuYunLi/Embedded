---
status: done
created: 2026-09-13
tags:
  - c/adc
  - c/signal-processing
  - embedded/sensor
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/20_rocketpi_adc_mcu_temperature"
  - "[[14_rocketpi_i2c_aht30]] 外部温度传感器（对比 MCU 内部温度传感器）"
---

# 20 ADC MCU 内部温度传感器

## 一句话定性

STM32F401RE 内置温度传感器，通过 ADC 采样得到芯片结温，用于监控 MCU 发热、辅助温度补偿，精度 ±1.5°C（校准后），无需外部器件。

## 同类产品定位

- **MCU 内部温度传感器**：零成本、零引脚、精度一般（±1.5°C），适合监控芯片自身温度
- **AHT30/SHT30**：外部 I2C 温湿度传感器，精度更高（±0.3°C），适合测量环境温度
- **热敏电阻（NTC）**：外部 ADC 采样，成本低但需要分压电路和查表/拟合
- **本例选型理由**：无需外部器件，学习 ADC 采样、校准常数、中值滤波

## 硬件连接

- 无外部引脚：温度传感器和 VREFINT 都是 MCU 内部信号
- ADC 通道：ADC_CHANNEL_TEMPSENSOR（Rank 1）+ ADC_CHANNEL_VREFINT（Rank 2）
- 采样时间：480 ADC 周期（温度传感器需要较长采样时间稳定）

## 通信协议要点

- 无通信协议，纯 ADC 采样
- ADC 分辨率：12 位（0~4095）
- 参考电压：VDDA（通常 3.3V），通过 VREFINT 校准实际值
- 温度计算：利用工厂校准常数 TS_CAL1/TS_CAL2 进行线性插值

## 代码架构

```
┌─────────────────────────────────────────────────┐
│ main.c (应用层 + 驱动层，全部手写)              │
│  - read_mcu_temperature：ADC 采样+中值滤波+校准 │
│  - print_temperature：串口输出                   │
│  - 主循环每 1s 读取并打印                        │
├─────────────────────────────────────────────────┤
│ adc.c (CubeMX 生成)                             │
│  - ADC1 初始化：12 位、双通道扫描、软件触发     │
│  - Rank 1 = 温度传感器，Rank 2 = VREFINT        │
└─────────────────────────────────────────────────┘
```

**与前几例不同**：本例没有独立的驱动层文件，所有逻辑都在 main.c 中。MCU 内部温度传感器不需要复杂的协议驱动，直接读 ADC 寄存器即可。

## 核心实现详解

### ADC 配置——双通道扫描

```c
hadc1.Init.Resolution = ADC_RESOLUTION_12B;      // 12 位（0~4095）
hadc1.Init.ScanConvMode = ENABLE;                 // 扫描模式（多通道顺序转换）
hadc1.Init.ContinuousConvMode = DISABLE;          // 单次转换（软件触发）
hadc1.Init.NbrOfConversion = 2;                   // 2 个通道

// Rank 1: 温度传感器
sConfig.Channel = ADC_CHANNEL_TEMPSENSOR;
sConfig.Rank = 1;
sConfig.SamplingTime = ADC_SAMPLETIME_480CYCLES;  // 480 周期采样

// Rank 2: 内部参考电压
sConfig.Channel = ADC_CHANNEL_VREFINT;
sConfig.Rank = 2;
sConfig.SamplingTime = ADC_SAMPLETIME_480CYCLES;
```

**扫描模式**：一次 `HAL_ADC_Start` + `HAL_ADC_PollForConversion` 依次转换 Rank 1 和 Rank 2。第一次 PollForConversion 得到温度，第二次得到 VREFINT。

**480 周期采样时间**：温度传感器内部阻抗较高，需要更长的采样时间让采样电容充满。480 周期 @ ADC 时钟 = 84MHz/4 = 21MHz → 480/21MHz ≈ 22.8µs。

### read_mcu_temperature——ADC 采样 + 中值滤波 + 校准

**作用**：多次采样温度和 VREFINT，中值滤波去噪，用工厂校准常数计算校准后的摄氏温度。

**形参**：`temperature_c` — 输出温度指针。

**输入/输出**：输入 = 无；输出 = 校准后的温度写入 *temperature_c，返回 HAL_OK/HAL_ERROR。

**调用者**：`main` 主循环，每 1s 调一次。

```c
static HAL_StatusTypeDef read_mcu_temperature(float *temperature_c)
{
    // ① 多次采样
    uint32_t temp_samples[5];
    uint32_t vref_samples[5];
    for (uint32_t i = 0; i < 5; ++i) {
        HAL_ADC_Start(&hadc1);
        HAL_ADC_PollForConversion(&hadc1, 10);  // 等温度转换
        temp_samples[i] = HAL_ADC_GetValue(&hadc1);
        HAL_ADC_PollForConversion(&hadc1, 10);  // 等 VREFINT 转换
        vref_samples[i] = HAL_ADC_GetValue(&hadc1);
    }
    HAL_ADC_Stop(&hadc1);

    // ② 中值滤波
    insertion_sort(temp_samples, 5);
    insertion_sort(vref_samples, 5);
    uint32_t raw_temp = temp_samples[2];      // 中值（索引 2 = 5/2）
    uint32_t raw_vrefint = vref_samples[2];

    // ③ 读取工厂校准常数
    uint16_t vrefint_cal = *(volatile uint16_t *)0x1FFF7A2A;  // VREFINT 校准值
    uint16_t ts_cal1 = *(volatile uint16_t *)0x1FFF7A2C;      // 30°C 时的 ADC 值
    uint16_t ts_cal2 = *(volatile uint16_t *)0x1FFF7A2E;      // 110°C 时的 ADC 值

    // ④ 计算实际 VDDA
    float vdda = 3.3f * ((float)vrefint_cal / (float)raw_vrefint);

    // ⑤ 校准温度计算
    if ((ts_cal1 != 0) && (ts_cal2 != 0) && (ts_cal2 != ts_cal1)) {
        float temp_at_cal_vdda = ((float)raw_temp * vdda) / 3.3f;
        *temperature_c = ((temp_at_cal_vdda - (float)ts_cal1) *
                          (110.0f - 30.0f) /
                          ((float)ts_cal2 - (float)ts_cal1)) + 30.0f;
    } else {
        // ⑥ 退化方案：使用典型参数
        float vsense = ((float)raw_temp / 4095.0f) * vdda;
        *temperature_c = ((0.76f - vsense) / 0.0025f) + 25.0f;
    }
}
```

### ① 多次采样

每次 `HAL_ADC_Start` 触发一轮扫描（Rank 1 + Rank 2），两次 `PollForConversion` 分别读取温度和 VREFINT。5 次循环得到 5 组数据。

### ② 中值滤波

```c
static void insertion_sort(uint32_t *data, uint32_t length)
{
    for (uint32_t i = 1; i < length; ++i) {
        uint32_t key = data[i];
        int32_t j = (int32_t)i - 1;
        while ((j >= 0) && (data[j] > key)) {
            data[j + 1] = data[j];
            --j;
        }
        data[j + 1] = key;
    }
}
```

**为什么用中值而不是均值**：ADC 噪声通常是高斯分布，均值滤波对高斯噪声有效。但实际环境中可能有脉冲干扰（如电机启动瞬间的电源波动），中值滤波对脉冲干扰更鲁棒——5 个样本中只要有 3 个是正常的，中值就是正确的。

**插入排序 vs 快速排序**：5 个元素用插入排序（O(n²)）比快排（O(n log n)）更简单，且对小数组更快（无递归开销、缓存友好）。

### ③ 工厂校准常数

```
0x1FFF7A2A: VREFINT 校准值（在 3.3V VDDA、30°C 下测得的 ADC 值）
0x1FFF7A2C: TS_CAL1（30°C 时温度传感器的 ADC 值，在 3.3V VDDA 下测得）
0x1FFF7A2E: TS_CAL2（110°C 时温度传感器的 ADC 值，在 3.3V VDDA 下测得）
```

这些地址在 STM32F401 的系统存储区（OTP），出厂时由 ST 写入，每个芯片不同。

### ④ 计算实际 VDDA

```
VDDA = 3.3V × (VREFINT_CAL / ADC_VREFINT_RAW)
```

VREFINT 是内部 1.2V 参考电压。工厂在校准条件下（VDDA=3.3V）测得 ADC 值为 `vrefint_cal`。实际运行时 VDDA 可能偏离 3.3V（如电池供电时），通过 VREFINT 的实际 ADC 值反推 VDDA。

### ⑤ 校准温度计算

```
温度 = ((ADC_RAW × VDDA / 3.3V - TS_CAL1) × (110 - 30) / (TS_CAL2 - TS_CAL1)) + 30
```

**原理**：TS_CAL1/TS_CAL2 是在 VDDA=3.3V 条件下测得的 ADC 值。如果实际 VDDA 偏离 3.3V，ADC 值会同比例变化，所以先用 `raw_temp × VDDA / 3.3V` 换算到"等效 3.3V 条件下的 ADC 值"，再做线性插值。

**线性插值**：温度传感器的输出电压与温度近似线性关系。两个校准点（30°C/110°C）定义一条直线，任意 ADC 值可映射到温度。

### ⑥ 退化方案

如果校准常数缺失（ts_cal1=0 或 ts_cal2=0），使用数据手册的典型参数：

```
温度 = (0.76V - Vsense) / 2.5mV/°C + 25°C
```

- V25 = 0.76V：25°C 时传感器输出电压的典型值
- Avg_Slope = 2.5mV/°C：温度每升高 1°C，输出电压下降 2.5mV
- **精度较差**：典型值 vs 实际值可能差 ±5°C

### print_temperature——定点打印

```c
static void print_temperature(float temperature_c)
{
    char buffer[64];
    int length = snprintf(buffer, sizeof(buffer), "MCU Temp: %.2f C\r\n", temperature_c);
    HAL_UART_Transmit(&huart2, (uint8_t *)buffer, bytes_to_send, HAL_MAX_DELAY);
}
```

本例用 `%f` 打印（链接浮点库），与 14 例的定点整数打印不同。MCU 温度监控场景下 Flash 开销不是关键考虑。

## 设计问题与改进空间

1. **`%f` 打印浮点**：链接浮点库增加 5~10KB Flash。可改为定点整数打印（如 `temp10/10, abs(temp10%10)`），但本例代码量小，影响不大。

2. **HAL_MAX_DELAY 阻塞发送**：与 17 例的 DMA 非阻塞发送对比，本例用阻塞发送，每秒一次，影响可忽略。

3. **5 次采样的开销**：每次采样 2 通道 × 5 次 = 10 次 ADC 转换，约 10 × 22.8µs ≈ 228µs。加上排序和浮点计算，总耗时 < 1ms，对 1s 周期无影响。

4. **中值滤波的样本数**：5 个样本取中值，可抵抗 2 个脉冲干扰。如果环境噪声更大，可增加到 7 或 9 个样本，但会增加采样时间。

5. **温度传感器的自热**：ADC 采样时温度传感器会轻微发热，连续高频采样可能导致读数偏高。1s 周期采样影响可忽略。

6. **与 14 例（AHT30）对比**：AHT30 测量环境温度（±0.3°C），MCU 温度传感器测量芯片结温（±1.5°C）。结温通常比环境温度高 5~20°C（取决于 CPU 负载）。

## 关联笔记

- [[14_rocketpi_i2c_aht30|14 AHT30 温湿度]]：外部 I2C 温度传感器，对比 MCU 内部温度传感器
- [[21_rocketpi_adc_joystick|21 ADC 摇杆]]：外部 ADC 采样（电位器），对比内部信号采样
- [[05_rocketpi_delay_us|05 微秒延时]]：定时器做延时，对比 ADC 做信号采集
