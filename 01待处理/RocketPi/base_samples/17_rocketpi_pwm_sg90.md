---
status: done
created: 2026-09-13
tags:
  - c/pwm
  - c/timer
  - embedded/actuator
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/17_rocketpi_pwm_sg90"
  - "[[16_rocketpi_pwm_passive_buzzer]] PWM 蜂鸣器（频率控制）"
---

# 17 PWM 舵机 SG90

## 一句话定性

SG90 是 PWM 控制的舵机（伺服电机），通过脉冲宽度控制转轴角度（0~180°），用于嵌入式系统的角度执行器（机械臂、云台、阀门等）。

## 同类产品定位

- **SG90**：9g 微型舵机，扭矩 1.2kg·cm，角度 0~180°，成本 ~¥5
- **MG90S**：金属齿轮版 SG90，扭矩更大（1.8kg·cm），更耐用
- **MG996R**：大扭矩舵机（10kg·cm），适合机械臂
- **本例选型理由**：SG90 体积小、成本低、PWM 控制简单，适合学习舵机原理

## 硬件连接

- 引脚分配：PC9 = TIM3_CH4（PWM 输出）
- 驱动方式：GPIO 复用为定时器通道，推挽输出，无上拉
- 控制信号：50Hz PWM（20ms 周期），脉宽 500~2500µs 对应 0~180°

## 通信协议要点

- **PWM 频率**：50Hz（20ms 周期），舵机内部控制电路以此为基准
- **脉宽范围**：500µs = 0°，1500µs = 90°，2500µs = 180°（SG90 标称范围）
- **角度分辨率**：2000µs / 180° ≈ 11.1µs/°，TIM3 1µs 分辨率可实现 0.09° 精度
- **控制方式**：持续发送 PWM 信号，舵机实时跟踪角度；停止 PWM 后舵机保持最后位置

## 代码架构

```
┌─────────────────────────────────────────────────┐
│ main.c (应用层)                                 │
│  - Init + Scan（连续扫描 0~180°）               │
├─────────────────────────────────────────────────┤
│ driver_sg90_test.c (驱动层)                     │
│  - SetPulse / SetAngle / Sweep / Scan          │
│  - 脉宽 ↔ 角度转换 + DMA 日志                   │
├─────────────────────────────────────────────────┤
│ tim.c (CubeMX 生成)                             │
│  - TIM3 CH4 PWM：84 分频 → 1µs 分辨率          │
│  - ARR=19999 → 20ms 周期（50Hz）                │
│  - PC9 复用 AF2                                 │
└─────────────────────────────────────────────────┘
```

## 与 16 例（蜂鸣器）的关键差异

| 维度 | 16 蜂鸣器 | 17 SG90 舵机 |
|------|----------|--------------|
| PWM 语义 | 频率 = 音调 | 脉宽 = 角度 |
| 周期 | 可变（262Hz~2kHz） | 固定 50Hz（20ms） |
| 占空比 | 可变（音量） | 固定 50Hz，脉宽可变 |
| 分辨率 | 16 位 ARR | 1µs（20000 ticks） |
| 控制对象 | 频率 | 脉宽 |
| 反馈 | 无（开环） | 无（开环，但舵机内部闭环） |

## 核心实现详解

### TIM3 配置——1µs 分辨率 + 20ms 周期

```c
htim3.Init.Prescaler = 84-1;       // 84MHz / 84 = 1MHz → 1µs/tick
htim3.Init.Period = 19999;         // 1MHz / 20000 = 50Hz → 20ms 周期
htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;  // 允许运行时改 ARR

sConfigOC.OCMode = TIM_OCMODE_PWM1;
sConfigOC.Pulse = 999;             // 初始脉宽 999µs ≈ 中位偏左
```

**与 16 例对比**：
- 16 例：PSC=84-1（1µs 分辨率），ARR 动态计算（频率可变）
- 17 例：PSC=84-1（1µs 分辨率），ARR 固定 19999（50Hz 锁定）

**`AutoReloadPreload = ENABLE`**：允许运行时修改 ARR 而不产生毛刺。16 例是 DISABLE（每次改 ARR 都要停启 PWM）。

### SG90_Test_SetAngle——角度转脉宽

**作用**：将角度（0~180°）线性映射到脉宽（500~2500µs），写入 CCR 寄存器。

**形参**：`angle_deg` — 目标角度（0~180°）。

**输入/输出**：输入 = 角度；输出 = TIM3 CH4 的 CCR 被更新，舵机转到目标角度，返回 HAL_OK/HAL_ERROR。

**调用者**：`SG90_Test_Init`（初始 90°）、`SG90_Test_Sweep`（定点扫描）、`SG90_Test_Scan`（连续扫描）。

```c
HAL_StatusTypeDef SG90_Test_SetAngle(float angle_deg)
{
    if (angle_deg != angle_deg) return HAL_ERROR;  // ① NaN 检测

    if (angle_deg < 0.0f) angle_deg = 0.0f;        // ② 钳位
    else if (angle_deg > 180.0f) angle_deg = 180.0f;

    float pulse = (float)SG90_MIN_PULSE_US +
                  (angle_deg / 180.0f) * (float)(SG90_MAX_PULSE_US - SG90_MIN_PULSE_US);
    // ③ 线性映射：500 + (angle/180) * 2000

    return SG90_Test_SetPulse((uint16_t)(pulse + 0.5f));  // ④ 四舍五入
}
```

- **① NaN 检测**：`angle_deg != angle_deg` 是 IEEE 754 NaN 的标准检测方式。NaN 参与比较永远返回 false，所以 `NaN != NaN` 为 true
- **② 钳位**：超出 0~180° 范围时截断到边界，返回 HAL_OK（不是 HAL_ERROR）——钳位后的值是合法的
- **③ 线性映射公式**：`pulse = 500 + (angle/180) * 2000`。0° → 500µs，90° → 1500µs，180° → 2500µs
- **④ `+ 0.5f` 四舍五入**：`(uint16_t)` 强转是截断，加 0.5 实现四舍五入。如 1500.6 → 1501.1 → 1501

### SG90_Test_SetPulse——直接设置脉宽

**作用**：钳位脉宽到 500~2500µs 范围，写入 CCR 寄存器。返回 HAL_ERROR 表示输入被钳位。

```c
HAL_StatusTypeDef SG90_Test_SetPulse(uint16_t pulse_width_us)
{
    uint16_t clamped = clamp_pulse(pulse_width_us);
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_4, clamped);
    return (clamped == pulse_width_us) ? HAL_OK : HAL_ERROR;
}
```

**CCR 直接 = 脉宽（µs）**：因为 TIM3 被配置为 1µs/tick，所以 CCR 值直接就是高电平持续的微秒数。这是 17 例最优雅的设计——不需要任何换算。

### SG90_Test_Scan——连续扫描

**作用**：以 0.25° 步进连续扫描 0→180→0，同时通过 DMA UART 输出当前角度日志。

```c
void SG90_Test_Scan(uint32_t step_delay_ms)
{
    const uint32_t steps_per_degree = 4U;  // 0.25° 步进
    const uint32_t max_index = 180U * steps_per_degree;  // 720 步
    const float inv_steps = 1.0f / (float)steps_per_degree;
    uint32_t base_delay = step_delay_ms / steps_per_degree;
    uint32_t remainder = step_delay_ms % steps_per_degree;

    while (1) {
        // 正向扫描 0 → 180
        for (uint32_t idx = 0; idx <= max_index; ++idx) {
            float angle = (float)idx * inv_steps;  // 0.0, 0.25, 0.5, ...
            SG90_Test_SetAngle(angle);
            if ((idx % 20) == 0 || idx == max_index) {
                log_angle(angle);  // 每 5° 输出一次日志
            }
            HAL_Delay(base_delay + (idx % steps_per_degree < remainder ? 1U : 0U));
        }

        // 反向扫描 179.75 → 0.25（避免端点重复停留）
        for (uint32_t idx = max_index - 1; idx > 0; --idx) {
            // ...
        }
    }
}
```

- **0.25° 步进**：`steps_per_degree = 4`，每个角度分 4 步到达，运动更平滑
- **日志节流**：每 20 步（5°）输出一次，避免 DMA 发送过于频繁影响运动流畅度
- **反向扫描跳过端点**：`idx = max_index - 1` 到 `idx > 0`，避免在 0° 和 180° 处重复停留
- **余数分配延时**：`step_delay_ms = 2` 时，`base_delay = 0`，`remainder = 2`。每 4 步中有 2 步多延时 1ms，平均 0.5ms/步

### log_angle——DMA 双缓冲日志

**作用**：通过 DMA 发送角度日志，不阻塞主循环。

```c
static char uart_log_buffer[2][32];
static uint8_t uart_active_buffer;

static void log_angle(float angle_deg)
{
    if (huart2.gState != HAL_UART_STATE_READY) return;  // ① 上次 DMA 未完成则跳过

    uint8_t next_buffer = uart_active_buffer ^ 1U;       // ② 切换缓冲区
    int len = snprintf(uart_log_buffer[next_buffer], 32,
                       "SG90 angle: %.2f\r\n", angle_deg);

    if (HAL_UART_Transmit_DMA(&huart2, uart_log_buffer[next_buffer], len) == HAL_OK) {
        uart_active_buffer = next_buffer;                // ③ 成功则切换
    }
}
```

- **① 非阻塞检查**：`gState != HAL_UART_STATE_READY` 表示上次 DMA 发送还在进行，直接跳过不输出。**日志可以丢，运动不能停**
- **② 双缓冲**：两个 32 字节缓冲区交替使用。正在 DMA 发送的是 `uart_active_buffer`，新数据写入另一个
- **③ XOR 切换**：`^ 1U` 在 0/1 之间切换，比 `if/else` 更简洁

**为什么不用 printf**：printf 是阻塞的，115200bps 发送 20 字节需要 ~1.7ms，会显著影响舵机运动的平滑度。DMA 发送几乎不占 CPU 时间。

### SG90_Test_Sweep——定点扫描

**作用**：依次移动到 0°、45°、90°、135°、180° 五个位置，每个位置停留指定时间。

```c
void SG90_Test_Sweep(uint32_t dwell_ms)
{
    static const float positions[] = {0.0f, 45.0f, 90.0f, 135.0f, 180.0f};
    for (size_t i = 0; i < 5; ++i) {
        SG90_Test_SetAngle(positions[i]);
        HAL_Delay(dwell_ms);
    }
}
```

比 Scan 简单得多，适合演示和调试。

## 设计问题与改进空间

1. **SG90 脉宽范围可能不精确**：标称 500~2500µs，实际舵机个体差异较大（有些 600~2400µs）。可加校准功能（存储 min/max 到 EEPROM）。

2. **Scan 是死循环**：`while(1)` 内的扫描永远不会退出，main 的 while(1) 永远不会执行。可改为单次扫描或加退出条件。

3. **DMA 双缓冲的竞态**：`log_angle` 检查 `gState` 后、调用 `Transmit_DMA` 前，如果 DMA 恰好完成（中断触发），`gState` 变为 READY，但 `uart_active_buffer` 还没切换。实际影响不大（最多丢一次日志），但严格来说不是原子操作。

4. **角度精度**：float 精度约 7 位有效数字，180.0f / 720 = 0.25f 精确表示没问题。但如果改成更细步进（如 0.1°），float 精度可能不够，应改用定点整数。

5. **与 16 例的 PWM 设计对比**：16 例动态计算 PSC/ARR（频率可变），17 例固定 PSC/ARR（脉宽可变）。两种 PWM 应用模式：频率控制 vs 脉宽控制。

## 关联笔记

- [[16_rocketpi_pwm_passive_buzzer|16 PWM 蜂鸣器]]：同为 PWM 应用，频率控制 vs 脉宽控制
- [[18_rocketpi_pwm_motor|18 PWM 电机]]：PWM 控制电机转速（占空比控制）
- [[05_rocketpi_delay_us|05 微秒延时]]：TIM11 做微秒延时，对比 TIM3 做 PWM
