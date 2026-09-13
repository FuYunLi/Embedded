---
status: done
created: 2026-09-13
tags:
  - c/gpio-input
  - c/timer
  - embedded/sensor
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/19_rocketpi_hcsr04"
  - "[[05_rocketpi_delay_us]] TIM11 微秒延时（对比 TIM1 微秒时间戳）"
---

# 19 超声波测距 HC-SR04

## 一句话定性

HC-SR04 是超声波测距模块，通过发射超声波脉冲并测量回波时间计算距离，量程 2~400cm，精度 ±3mm，用于嵌入式系统的非接触测距（避障、液位检测等）。

## 同类产品定位

- **HC-SR04**：最常用的超声波模块，5V 供电，量程 2~400cm，成本 ~¥3
- **HC-SR04P**：3.3V 版本，可直接接 STM32
- **US-100**：带温度补偿，精度更高
- **VL53L0X**：激光测距，精度更高（±3%），量程更短（1.2m），成本更高
- **本例选型理由**：HC-SR04 价格最低、使用最广泛，适合学习超声波测距原理

## 硬件连接

- TRIG（触发引脚）：PC 输出，推挽，无上拉
- ECHO（回波引脚）：PC 输入，无上拉
- 驱动方式：TRIG 发 10µs 高脉冲触发测量，ECHO 返回高电平持续时间 = 超声波往返时间

## 通信协议要点

- **触发**：TRIG 引脚输出 ≥10µs 高脉冲
- **回波**：模块自动发射 8 个 40kHz 超声波脉冲，ECHO 引脚变高，收到回波后变低
- **距离公式**：`距离 = ECHO 高电平时间 × 声速 / 2`。声速 340m/s = 0.034cm/µs，所以 `距离(cm) = 时间(µs) × 0.034 / 2 = 时间(µs) / 58`
- **测量范围**：2cm（最短，太近回波混叠）~ 400cm（最远，回波太弱）
- **测量周期**：建议 ≥60ms，避免余波干扰

## 代码架构

```
┌─────────────────────────────────────────────────┐
│ main.c (应用层)                                 │
│  - 调用 hcsr04_read_test(102400)               │
├─────────────────────────────────────────────────┤
│ driver_hcsr04_read_test.c (测试层)              │
│  - 循环读取距离并打印                           │
├─────────────────────────────────────────────────┤
│ driver_hcsr04.c (驱动层，LibDriver)            │
│  - hcsr04_read：触发→等回波→计算距离            │
│  - 函数指针注入（同 15 例风格）                 │
├─────────────────────────────────────────────────┤
│ driver_hcsr04_interface.c (平台适配层)          │
│  - TRIG GPIO 输出 + ECHO GPIO 输入              │
│  - TIM1 微秒时间戳 + 溢出计数                   │
├─────────────────────────────────────────────────┤
│ tim.c (CubeMX 生成)                             │
│  - TIM1：84 分频 → 1µs 分辨率，65535 溢出      │
│  - 溢出中断扩展为 64 位时间戳                   │
└─────────────────────────────────────────────────┘
```

## 与 05 例（微秒延时）的关键差异

| 维度 | 05 TIM11 微秒延时 | 19 TIM1 微秒时间戳 |
|------|-------------------|-------------------|
| 用途 | 阻塞延时 | 时间戳读取 |
| 模式 | 忙等计数器 | 中断溢出 + 读计数器 |
| 精度 | 1µs | 1µs |
| 位宽 | 16 位（回绕减法） | 64 位（溢出计数扩展） |
| 阻塞 | 是 | 否（读时间戳不阻塞） |
| 定时器 | TIM11（基本定时器） | TIM1（高级定时器） |

## 核心实现详解

### TIM1 配置——1µs 分辨率 + 溢出中断

```c
htim1.Init.Prescaler = 84-1;       // 84MHz / 84 = 1MHz → 1µs/tick
htim1.Init.Period = 65535;         // 16 位最大值，65.535ms 溢出
htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
```

**溢出中断**：

```c
static volatile uint32_t s_tim1_overflow_count = 0;

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM1) {
        s_tim1_overflow_count++;
    }
}
```

每次 TIM1 计数到 65535 溢出时，中断回调将 `s_tim1_overflow_count` 加 1。配合当前计数器值，可计算出 64 位微秒时间戳。

### hcsr04_interface_get_time_us——64 位微秒时间戳

**作用**：读取 TIM1 当前计数器值和溢出次数，合成 64 位微秒时间戳。处理读取过程中溢出可能发生的竞态。

```c
static uint64_t hcsr04_interface_get_time_us(void)
{
    uint32_t overflow_before;
    uint32_t counter;

    do {
        overflow_before = s_tim1_overflow_count;
        counter = __HAL_TIM_GET_COUNTER(&htim1);
    } while (overflow_before != s_tim1_overflow_count);

    return ((uint64_t)overflow_before * 0x10000UL) + counter;
}
```

**竞态处理**：如果在读 `overflow_before` 和读 `counter` 之间发生了溢出中断，`overflow_before` 是旧值但 `counter` 已经回绕到小值，计算结果会少 65535µs。`do-while` 循环检测 `overflow_before` 是否变化，变化则重读。

**64 位时间戳**：`overflow * 65536 + counter`。理论最大时间 = 2^64 µs ≈ 58 万年，实际不会溢出。

### hcsr04_interface_delay_us——微秒延时

**作用**：基于 TIM1 时间戳的忙等延时，精度 1µs。

```c
void hcsr04_interface_delay_us(uint32_t us)
{
    if (us == 0U) return;
    uint64_t start = hcsr04_interface_get_time_us();
    while ((hcsr04_interface_get_time_us() - start) < us) {
        /* busy wait */
    }
}
```

**与 05 例对比**：05 例用 TIM11 计数器直接比较（16 位回绕减法），本例用 64 位时间戳差值。64 位无回绕问题，但每次读时间戳需要两次内存读 + 一次中断检查，开销略大。

### hcsr04_interface_trig_write——触发引脚控制

```c
uint8_t hcsr04_interface_trig_write(uint8_t value)
{
    HAL_GPIO_WritePin(TRIGGER_GPIO_Port, TRIGGER_Pin,
                      (value != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return 0;
}
```

**与 15 例对比**：15 例的 GPIO 回调通过 `void *ctx` 传入端口/引脚，支持多实例。本例直接用 CubeMX 定义的宏（`TRIGGER_GPIO_Port`/`TRIGGER_Pin`），不支持多实例，但代码更简洁。

### hcsr04_interface_echo_read——回波引脚读取

```c
uint8_t hcsr04_interface_echo_read(uint8_t *value)
{
    if (value == NULL) return 1;
    *value = (uint8_t)HAL_GPIO_ReadPin(ECHO_GPIO_Port, ECHO_Pin);
    return 0;
}
```

ECHO 引脚配置为 `GPIO_MODE_INPUT`，无上拉（模块内部有上拉）。

### hcsr04_read_test——循环测距

```c
uint8_t hcsr04_read_test(uint32_t times)
{
    // ... 初始化 handle、注入回调 ...

    for (i = 0; i < times; i++) {
        uint32_t time_us;
        float m;
        res = hcsr04_read(&gs_handle, &time_us, &m);
        if (res != 0) return 1;

        m *= 100.0f;  // 米 → 厘米
        printf("hcsr04: distance is %fcm.\n", m);
        hcsr04_interface_delay_ms(1000);  // 每秒测一次
    }
}
```

- **`m *= 100.0f`**：驱动层返回米，测试层转厘米打印
- **`hcsr04_read` 返回两个值**：`time_us`（原始回波时间）和 `m`（计算后的距离）
- **102400 次测量**：约 28 小时连续测距，测试长时间稳定性

### hcsr04_handle_t——函数指针注入

```c
typedef struct {
    uint8_t (*trig_init)(void);        // TRIG GPIO 初始化
    uint8_t (*trig_deinit)(void);
    uint8_t (*trig_write)(uint8_t);    // TRIG 输出高/低
    uint8_t (*echo_init)(void);        // ECHO GPIO 初始化
    uint8_t (*echo_deinit)(void);
    uint8_t (*echo_read)(uint8_t *);   // ECHO 读取电平
    uint8_t (*timestamp_read)(hcsr04_time_t *);  // 微秒时间戳
    void (*delay_us)(uint32_t);        // 微秒延时
    void (*delay_ms)(uint32_t);        // 毫秒延时
    void (*debug_print)(const char *, ...);
    uint8_t inited;
} hcsr04_handle_t;
```

**与 15 例（AT24CXX）对比**：同为 LibDriver 风格的函数指针注入。HC-SR04 需要 `trig_write`（GPIO 输出）+ `echo_read`（GPIO 输入）+ `timestamp_read`（定时器），比 EEPROM 的 `iic_read/write` 多了时间维度。

### hcsr04_time_t——时间结构体

```c
typedef struct {
    uint64_t microsecond;  // 微秒（0~999）
    uint32_t millisecond;  // 毫秒
} hcsr04_time_t;
```

**为什么拆成毫秒+微秒**：避免 64 位除法开销。`timestamp_read` 内部将 64 位微秒时间戳拆分为 `ms = now_us / 1000` 和 `us = now_us % 1000`。驱动层只需要微秒差值来计算距离。

## 设计问题与改进空间

1. **`hcsr04_interface_echo_read` 的函数名错误**：头文件中宏名为 `DRIVER_HCSR04_LINK_ECHO_WRITE`，但实际链接的是 `echo_read`。宏名误导，应为 `DRIVER_HCSR04_LINK_ECHO_READ`。

2. **TIM1 溢出中断的优先级**：`HAL_NVIC_SetPriority(TIM1_UP_TIM10_IRQn, 1, 0)` 在 `start_timer_if_needed` 中设置，但 `MX_TIM1_Init` 中 MspInit 已经设置了优先级 0,0。两次设置不一致，后者覆盖前者。

3. **`hcsr04_read` 的阻塞等待**：驱动层内部用忙等轮询 ECHO 引脚电平变化（等回波），最长可能等 ~25ms（400cm 对应的往返时间）。期间 CPU 无法做其他事。可改为输入捕获中断，但复杂度大增。

4. **`%f` 打印浮点**：测试代码用 `printf("%fcm", m)`，链接浮点库增加 5~10KB Flash。可改为定点整数打印（如 16 例的 `%d.%01d`）。

5. **温度补偿未实现**：声速受温度影响（`331.3 + 0.606 × T` m/s），当前固定 340m/s。US-100 模块内置温度传感器，HC-SR04 需要外部补偿。

## 关联笔记

- [[05_rocketpi_delay_us|05 微秒延时]]：TIM11 微秒延时，对比 TIM1 微秒时间戳
- [[14_rocketpi_i2c_aht30|14 AHT30 温湿度]]：温度补偿声速需要温度传感器
- [[13_rocketpi_uart_radar|13 UART 雷达]]：另一种测距方案（24GHz 毫米波 vs 超声波）
