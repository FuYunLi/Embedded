---
status: done
created: 2026-09-13
tags:
  - c/pwm
  - c/motor
  - embedded/actuator
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/18_rocketpi_pwm_motor"
  - "[[16_rocketpi_pwm_passive_buzzer]] PWM 蜂鸣器（频率控制）"
  - "[[17_rocketpi_pwm_sg90]] PWM 舵机（脉宽控制）"
---

# 18 PWM 直流电机 L9110

## 一句话定性

L9110 是双通道 H 桥电机驱动芯片，通过两路 PWM 控制直流电机的正转、反转和刹车，用于嵌入式系统的电机执行器（小车、风扇、水泵等）。

## 同类产品定位

- **L9110S**：双通道 H 桱，最大 800mA，SOP-8 封装，适合小型直流电机
- **L298N**：双通道 H 桱，最大 2A，模块化封装，适合中型电机
- **L293D**：四通道 H 桱，最大 600mA，DIP-16 封装
- **TB6612FNG**：双通道 H 桱，最大 1.2A，效率高于 L298N
- **本例选型理由**：L9110 体积小、成本低（~¥0.5）、单芯片双通道，适合学习 PWM 电机控制

## 硬件连接

- 引脚分配：PB6 = TIM4_CH1 → INB，PB7 = TIM4_CH2 → INA
- 驱动方式：GPIO 复用为定时器通道，推挽输出
- 电机接法：L9110 的 OUTA/OUTB 接电机两端
- 电源：L9110 VCC 接电机电源（3.3~12V），GND 共地

## 通信协议要点

- 无通信协议，纯 PWM 驱动
- 控制逻辑（刹车式调速）：
  - 正转：INB=0（低电平），INA=PWM
  - 反转：INA=0（低电平），INB=PWM
  - 刹车：INA=0，INB=0（双低）
- PWM 频率：10kHz（电机静音，效率高）

## 代码架构

```
┌─────────────────────────────────────────────────┐
│ main.c (应用层)                                 │
│  - 主循环调用 motor_l9110_test_task()           │
├─────────────────────────────────────────────────┤
│ driver_motor_l9110_test.c (测试状态机)          │
│  - 非阻塞 6 态 FSM：正转慢→加速→刹车→反转...   │
├─────────────────────────────────────────────────┤
│ driver_motor_l9110.c (驱动层)                   │
│  - init / drive / drive_signed / brake         │
│  - 占空比 → CCR 转换 + 双通道同步更新          │
├─────────────────────────────────────────────────┤
│ tim.c (CubeMX 生成)                             │
│  - TIM4 CH1/CH2 PWM：10kHz，双通道同步         │
│  - PB6(INB) + PB7(INA) 复用 AF2                │
└─────────────────────────────────────────────────┘
```

## 与 16/17 例的关键差异

| 维度 | 16 蜂鸣器 | 17 SG90 舵机 | 18 L9110 电机 |
|------|----------|--------------|--------------|
| PWM 语义 | 频率 = 音调 | 脉宽 = 角度 | 占空比 = 转速 |
| 通道数 | 1 | 1 | 2（INA + INB） |
| 方向控制 | 无 | 无（单向） | 有（正/反/刹车） |
| 频率 | 可变 | 固定 50Hz | 固定 10kHz |
| 周期 | 可变 | 固定 20ms | 固定 100µs |
| 阻塞模式 | 阻塞 | 阻塞/非阻塞 | 非阻塞状态机 |
| 反馈 | 无 | 无 | 无（开环） |

## 核心实现详解

### TIM4 配置——10kHz 双通道 PWM

```c
htim4.Init.Prescaler = 84-1;       // 84MHz / 84 = 1MHz → 1µs/tick
htim4.Init.Period = 50-1;          // 1MHz / 50 = 20kHz... 等等
```

**等等，这里有问题**：`Period = 50-1 = 49`，频率 = `1MHz / 50 = 20kHz`，不是注释说的 10kHz。可能是 CubeMX 配置时的计算误差，或者注释过时。实际 PWM 频率 = `84MHz / 84 / 50 = 20kHz`。

**PB6/PB7 复用配置**：

```c
GPIO_InitStruct.Pin = MOTOR_INB_Pin | MOTOR_INA_Pin;  // PB6 + PB7
GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
GPIO_InitStruct.Pull = GPIO_NOPULL;
GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
GPIO_InitStruct.Alternate = GPIO_AF2_TIM4;  // AF2 = TIM4
```

两路 PWM 共用同一个定时器，保证同步。

### motor_l9110_calc_pulse——占空比转 CCR

**作用**：将 0~100% 占空比换算为 CCR 脉宽值。

```c
static uint32_t motor_l9110_calc_pulse(uint8_t duty_percent)
{
    uint32_t period = MOTOR_L9110_TIMER->Init.Period;  // 49
    uint32_t pulse = (period + 1U) * duty_percent / 100U;  // 50 * duty / 100

    if (pulse > period) pulse = period;
    return pulse;
}
```

- **`(period + 1) * duty / 100`**：先乘后除，避免浮点。50% 占空比 → `50 * 50 / 100 = 25`
- **钳位**：`pulse > period` 时截断到 period（100% 占空比 = 全高）

### motor_l9110_drive——方向 + 占空比控制

**作用**：根据方向和占空比，同步更新两路 PWM 的 CCR 值。

```c
void motor_l9110_drive(motor_l9110_direction_t direction, uint8_t duty_percent)
{
    if (duty_percent > 100U) duty_percent = 100U;
    uint32_t pulse = motor_l9110_calc_pulse(duty_percent);

    switch (direction) {
    case MOTOR_L9110_DIR_FORWARD:
        motor_l9110_apply(pulse, 0U);     // INA=PWM, INB=0
        break;
    case MOTOR_L9110_DIR_REVERSE:
        motor_l9110_apply(0U, pulse);     // INA=0, INB=PWM
        break;
    case MOTOR_L9110_DIR_BRAKE:
    default:
        motor_l9110_apply(0U, 0U);        // 双低刹车
        break;
    }
}
```

**刹车式调速**：一个通道 PWM、另一个通道恒低。PWM 高电平时电流流过电机（驱动），低电平时电机两端短路（刹车）。这种模式比"两路互补 PWM"简单，但低速时扭矩脉动大。

### motor_l9110_apply——双通道同步更新

**作用**：同时写入两路 CCR，避免瞬间交叉导通。

```c
static void motor_l9110_apply(uint32_t ia_pulse, uint32_t ib_pulse)
{
    __HAL_TIM_SET_COMPARE(MOTOR_L9110_TIMER, MOTOR_L9110_CHANNEL_IA, ia_pulse);
    __HAL_TIM_SET_COMPARE(MOTOR_L9110_TIMER, MOTOR_L9110_CHANNEL_IB, ib_pulse);
}
```

**交叉导通风险**：如果先写 INA=高、再写 INB=高，在两行代码之间的一瞬间，两个通道都是高电平，H 桥上下管同时导通 → 短路。本例"刹车式调速"不会出现这种情况（只有一个通道是 PWM，另一个恒低），但代码保持了同步更新的习惯，为将来扩展留余地。

### motor_l9110_drive_signed——有符号占空比

**作用**：正数正转、负数反转，绝对值为占空比。简化上层调用（如 PID 控制器输出）。

```c
void motor_l9110_drive_signed(int8_t duty_percent)
{
    int16_t duty = duty_percent;
    if (duty > 100) duty = 100;
    else if (duty < -100) duty = -100;

    if (duty >= 0) {
        motor_l9110_drive(MOTOR_L9110_DIR_FORWARD, (uint8_t)duty);
    } else {
        motor_l9110_drive(MOTOR_L9110_DIR_REVERSE, (uint8_t)(-duty));
    }
}
```

**`int16_t duty = duty_percent`**：先提升到 16 位，再做范围检查。如果直接用 `int8_t` 比较，`-100` 到 `100` 的范围在 `int8_t`（-128~127）内不会溢出，但提升到 `int16_t` 更安全。

### motor_l9110_test_task——非阻塞状态机

**作用**：6 态 FSM 循环演示正转→加速→刹车→反转→加速→刹车，每 2s 切换状态。非阻塞设计，主循环可同时做其他事。

```c
typedef enum {
    MOTOR_L9110_TEST_FORWARD_SOFT = 0,  // 正转慢速 50%
    MOTOR_L9110_TEST_FORWARD_FAST,      // 正转加速 100%
    MOTOR_L9110_TEST_BRAKE_FROM_FWD,    // 正转后刹车
    MOTOR_L9110_TEST_REVERSE_SOFT,      // 反转慢速 50%
    MOTOR_L9110_TEST_REVERSE_FAST,      // 反转加速 100%
    MOTOR_L9110_TEST_BRAKE_FROM_REV     // 反转后刹车
} motor_l9110_test_state_t;

void motor_l9110_test_task(void)
{
    uint32_t now = HAL_GetTick();
    if ((now - s_state_tick) < s_state_hold_ms) return;  // 未到驻留时间

    switch (s_state) {
    case MOTOR_L9110_TEST_FORWARD_SOFT:
        motor_l9110_test_enter(MOTOR_L9110_TEST_FORWARD_FAST, 2000);
        break;
    // ... 6 个状态轮转
    }
}
```

**非阻塞 vs 阻塞**：
- 16/17 例用 `HAL_Delay` 阻塞，播放/扫描期间 MCU 不能做其他事
- 18 例用 `HAL_GetTick()` 时间戳检查，每个状态驻留指定时间后自动切换
- 非阻塞是嵌入式系统的标准模式：主循环轮询，状态机驱动

### motor_l9110_test_enter——状态切换

**作用**：进入新状态时，记录起始时间、设置驻留时长、执行对应的电机动作。

```c
static void motor_l9110_test_enter(motor_l9110_test_state_t next_state, uint32_t hold_ms)
{
    s_state = next_state;
    s_state_hold_ms = hold_ms;
    s_state_tick = HAL_GetTick();

    switch (next_state) {
    case MOTOR_L9110_TEST_FORWARD_SOFT:
        motor_l9110_drive(MOTOR_L9110_DIR_FORWARD, 50);
        break;
    case MOTOR_L9110_TEST_FORWARD_FAST:
        motor_l9110_drive(MOTOR_L9110_DIR_FORWARD, 100);
        break;
    case MOTOR_L9110_TEST_BRAKE_FROM_FWD:
        motor_l9110_brake();
        break;
    // ...
    }
}
```

**状态机的典型三要素**：（1）状态枚举；（2）状态切换函数（enter）；（3）轮询函数（task）。三者配合实现非阻塞时序控制。

## 设计问题与改进空间

1. **PWM 频率不匹配**：CubeMX 配置 `Period = 50-1`，实际频率 20kHz，注释说 10kHz。20kHz 超出人耳范围（静音），对电机效率影响不大，但可能增加开关损耗。若需 10kHz，应改为 `Period = 100-1`。

2. **`motor_l9110_apply` 的原子性**：两行 `__HAL_TIM_SET_COMPARE` 之间理论上可能被中断打断。在本例中不影响（刹车式调速只有一个通道是 PWM），但若将来改为互补 PWM，需要关中断保护。

3. **`int8_t` 范围提升**：`motor_l9110_drive_signed` 的形参是 `int8_t`（-128~127），但函数内部 clamp 到 -100~100。如果调用方传入 -128，会被 clamp 到 -100，语义上合理但可能让调用方困惑。

4. **测试状态机的硬编码**：6 个状态的时序和占空比全部硬编码。可改为配置表驱动（类似 16 例的曲谱数组），但当前规模下硬编码更直观。

5. **无电流检测**：L9110 最大 800mA，堵转时电流可能超限。工程代码应加过流保护（ADC 检测或硬件限流）。

## 关联笔记

- [[16_rocketpi_pwm_passive_buzzer|16 PWM 蜂鸣器]]：PWM 频率控制
- [[17_rocketpi_pwm_sg90|17 PWM 舵机]]：PWM 脉宽控制
- [[19_rocketpi_hcsr04|19 超声波测距]]：GPIO 输入捕获（对比 PWM 输出）
