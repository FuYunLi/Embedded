---
status: done
created: 2026-09-13
tags:
  - c/pwm
  - c/timer
  - embedded/actuator
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/16_rocketpi_pwm_passive_buzzer"
---

# 16 PWM 无源蜂鸣器

## 一句话定性

无源蜂鸣器需要外部提供方波信号才能发声，通过 PWM 控制频率实现不同音调，用于嵌入式系统的音频提示和简单音乐播放。

## 同类产品定位

- **有源蜂鸣器**：内部集成振荡电路，通电即响，频率固定，只能做开关式提示音
- **无源蜂鸣器**：需要外部方波驱动，频率可调，能播放不同音调的音乐
- **本例选型理由**：无源蜂鸣器成本与有源相当（~¥0.3），但功能更强（可播放音乐），适合学习 PWM 应用

## 硬件连接

- 引脚分配：PB0 = TIM3_CH3（PWM 输出）
- 驱动方式：GPIO 复用为定时器通道，推挽输出，无上拉
- 音调控制：PWM 频率 = 音调频率（如 262Hz = C4 中音 do）
- 音量控制：PWM 占空比（50% 时振幅最大，过高/过低会失真）

## 通信协议要点

- 无通信协议，纯硬件 PWM 驱动
- 频率范围：人耳可听 20Hz~20kHz，常用音调 262Hz(C4)~1047Hz(C6)
- 占空比：推荐 50%（方波），过高/过低会导致音色失真

## 代码架构

```
┌─────────────────────────────────────────────────┐
│ main.c (应用层)                                 │
│  - beep 两声 + 播放《欢乐颂》                   │
├─────────────────────────────────────────────────┤
│ driver_buzzer_songs.h (曲谱数据)                │
│  - 音符频率宏 + 静态音符数组                    │
├─────────────────────────────────────────────────┤
│ driver_buzzer_test.c (驱动层)                   │
│  - start/stop/beep/play_sequence               │
│  - PWM 频率+占空比 → Prescaler/ARR/CCR 计算    │
├─────────────────────────────────────────────────┤
│ tim.c (CubeMX 生成)                             │
│  - TIM3 CH3 PWM 初始化 + PB0 复用配置           │
└─────────────────────────────────────────────────┘
```

## 核心实现详解

### buzzer_test_apply——PWM 参数计算与应用

**作用**：根据目标频率和占空比，计算定时器的 Prescaler、ARR（自动重装载值）、CCR（比较值），停止当前 PWM，写入新参数，重新启动。

**设计定位**：这是一个「频率+占空比联动切换」的便捷接口。正常 PWM 项目通常会封装两个独立函数：
- `pwm_set_frequency(freq)` — 只改 ARR
- `pwm_set_duty(duty)` — 只改 CCR

蜂鸣器场景特殊：频率变时占空比通常也要跟着调（保持 50% 方波），放一起更方便。通用 PWM 马达/LED 场景确实应该分开封装。

**形参**：`frequency_hz` — 目标频率（Hz）；`duty_percent` — 占空比（0~100）。

**输入/输出**：输入 = 频率 + 占空比；输出 = TIM3 的 PSC/ARR/CCR 寄存器被更新，PWM 输出新波形，返回 0/1。

**调用者**：`buzzer_test_start`、`buzzer_test_beep`、`buzzer_test_play_sequence`。

```c
static uint8_t buzzer_test_apply(uint32_t frequency_hz, uint8_t duty_percent)
{
    // ① 默认值处理
    if (frequency_hz == 0U) frequency_hz = BUZZER_TEST_DEFAULT_FREQUENCY_HZ;  // 2000Hz
    if (duty_percent == 0U) duty_percent = BUZZER_TEST_DEFAULT_DUTY_PERCENT;  // 50%
    if (duty_percent > 100U) duty_percent = 100U;

    // ② 计算每周期所需时钟 ticks
    uint64_t ticks_per_period = buzzer_test_get_timer_clock();  // 84MHz
    ticks_per_period = (ticks_per_period + frequency_hz / 2) / frequency_hz;  // 四舍五入

    // ③ 分频：ticks 超过 ARR 最大值时增大 Prescaler
    uint32_t prescaler = 1U;
    if (ticks_per_period > 0x10000UL) {
        prescaler = (ticks_per_period + 0x10000UL - 1) / 0x10000UL;
        if (prescaler > 0x10000UL) prescaler = 0x10000UL;
        ticks_per_period = (ticks_per_period + prescaler - 1) / prescaler;
    }

    // ④ 计算 ARR 和 CCR
    uint32_t auto_reload = (uint32_t)ticks_per_period;
    uint32_t compare = (uint32_t)(((uint64_t)auto_reload * duty_percent) / 100ULL);

    // ⑤ 停止 → 写寄存器 → 启动
    // 注意：HAL 要求修改 ARR/CCR 前先停止 PWM，否则可能产生毛刺
    // __HAL_TIM_SET_* 宏直接写寄存器，不经过 HAL 中间层
    HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_3);
    __HAL_TIM_SET_PRESCALER(&htim3, prescaler - 1U);
    __HAL_TIM_SET_AUTORELOAD(&htim3, auto_reload - 1U);
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_3, compare);
    __HAL_TIM_SET_COUNTER(&htim3, 0U);
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_3);
}
```

**频率计算公式**：

```
PWM 频率 = 定时器时钟 / (Prescaler × ARR)
         = 84MHz / (PSC+1) / (ARR+1)

目标：frequency_hz = 262Hz (C4)
ticks_per_period = 84000000 / 262 = 320610
ARR = 320610 - 1 = 320609
PSC = 0 (未分频)

目标：frequency_hz = 2000Hz
ticks_per_period = 84000000 / 2000 = 42000
ARR = 42000 - 1 = 41999
PSC = 0
```

**③ 分频逻辑**：ARR 是 16 位寄存器（最大 65535）。当 `ticks_per_period > 65536` 时，必须增大 Prescaler 来缩小 ticks。例如 262Hz 需要 320610 ticks，`320610/65536 ≈ 5`，所以 PSC=4，ARR=320610/5=64122。

**⑤ 先停后改**：HAL 要求修改 ARR/CCR 前先停止 PWM，否则可能产生毛刺。`__HAL_TIM_SET_*` 宏直接写寄存器，不经过 HAL 中间层。

### buzzer_test_get_timer_clock——获取定时器时钟频率

**作用**：读取 APB1 总线时钟频率，考虑 APB1 预分频器对定时器时钟的影响。

```c
static uint32_t buzzer_test_get_timer_clock(void)
{
    RCC_ClkInitTypeDef clk_config;
    uint32_t flash_latency;
    HAL_RCC_GetClockConfig(&clk_config, &flash_latency);
    uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();

    if (clk_config.APB1CLKDivider == RCC_HCLK_DIV1) {
        return pclk1;           // APB1 不分频 → 定时器时钟 = PCLK1
    }
    return pclk1 * 2U;          // APB1 分频 → 定时器时钟 = PCLK1 × 2
}
```

**STM32 定时器时钟规则**：当 APB1 预分频系数 = 1 时，定时器时钟 = PCLK1；否则定时器时钟 = PCLK1 × 2。本例 `APB1CLKDivider = RCC_HCLK_DIV2`（84MHz/2 = 42MHz），所以定时器时钟 = 42MHz × 2 = 84MHz。

### buzzer_test_beep——单音播放

**作用**：播放指定频率和时长的单个音符，播完自动停止。

**关于 HAL_Delay 与 PWM 的关系**：`HAL_Delay()` 不会让 PWM 输出暂停。PWM 由硬件定时器自动生成，一旦启动就持续输出，不依赖 CPU 干预。CPU 执行 `HAL_Delay()` 时，定时器继续在后台产生 PWM 波形。即使 CPU 死循环或进入 `__WFI()` 待机，只要定时器时钟未被关闭，PWM 就不会停止。

```c
uint8_t buzzer_test_beep(uint32_t frequency_hz, uint8_t duty_percent, uint32_t duration_ms)
{
    const uint32_t duration = (duration_ms == 0U) ? BUZZER_TEST_DEFAULT_DURATION_MS : duration_ms;
    buzzer_test_apply(frequency_hz, duty_percent);  // 启动 PWM（硬件开始输出方波）
    HAL_Delay(duration);                              // CPU 空等，期间 PWM 硬件持续输出
    buzzer_test_stop();                               // 停止 PWM
}
```

### buzzer_test_play_sequence——序列播放

**作用**：按顺序播放一组音符，支持静音（rest）和音符间间隔。

**`gap_ms` 的含义**：不是「每个音最少 20ms」，而是两个音符之间的「静音间隔」。默认 20ms 是防粘连 — 如果 `gap_ms=0`，两个音符会无缝衔接，蜂鸣器振膜来不及停振，听起来会糊成一片。

```
音符1 ──▶ 静音20ms ──▶ 音符2 ──▶ 静音20ms ──▶ 音符3
         ↑ gap         ↑ gap
```

```c
uint8_t buzzer_test_play_sequence(const buzzer_test_note_t *notes, uint32_t length, uint32_t gap_ms)
{
    const uint32_t gap = (gap_ms == 0U) ? 20U : gap_ms;

    for (uint32_t i = 0U; i < length; ++i) {
        if (notes[i].frequency_hz == 0U) {
            buzzer_test_stop();                           // 静音（停止 PWM 输出）
            HAL_Delay(notes[i].duration_ms);              // 静音时长
        } else {
            buzzer_test_apply(notes[i].frequency_hz, notes[i].duty_percent);
            HAL_Delay(notes[i].duration_ms);              // 播放时长（PWM 硬件持续输出）
            buzzer_test_stop();                           // 停止当前音符
        }
        if (gap > 0U && i + 1U < length) {
            HAL_Delay(gap);                               // 音符间间隔（静音）
        }
    }
}
```

**`buzzer_test_note_t` 结构体**：

```c
typedef struct {
    uint32_t frequency_hz;  // 音调频率（0 = 静音）
    uint8_t duty_percent;   // 占空比（0 = 默认 50%）
    uint32_t duration_ms;   // 持续时长（0 = 默认 200ms）
} buzzer_test_note_t;
```

每个字段的 0 值都有默认兜底，简化曲谱定义。

### driver_buzzer_songs.h——曲谱数据

**音符频率宏**：

```c
#define BUZZER_NOTE_C4     262U   // 中音 do
#define BUZZER_NOTE_D4     294U   // 中音 re
#define BUZZER_NOTE_E4     330U   // 中音 mi
// ... 十二平均律，四舍五入到整数
```

**曲谱示例（《欢乐颂》片段）**：

```c
static const buzzer_test_note_t buzzer_song_ode_to_joy[] = {
    { BUZZER_NOTE_E4, 0U, ODE_TO_JOY_DURATION(25U) },
    { BUZZER_NOTE_E4, 0U, ODE_TO_JOY_DURATION(25U) },
    { BUZZER_NOTE_F4, 0U, ODE_TO_JOY_DURATION(25U) },
    // ... 54 个音符
};
```

**`ODE_TO_JOY_DURATION` 宏**：`units * 8ms`，通过调整系数控制播放速度。25 单位 = 200ms，36 单位 = 288ms，模拟 4/4 拍的长短音。

### buzzer_test_stop——停止 PWM 输出

**作用**：停止定时器 PWM 输出，引脚回到空闲电平，定时器停止计数。

**意义**：不只是打印汇报，是真关停定时器降低功耗：
- `HAL_TIM_PWM_Stop()` — 引脚不再输出 PWM，回到空闲电平
- `HAL_TIM_Base_Stop()` — 定时器停止计数，不再消耗时钟
- `__HAL_TIM_SET_COMPARE(..., 0U)` — 清零比较值，确保引脚输出低电平

```
运行状态：  TIM3 时钟开启 → 计数器跑 → PWM 输出 → 蜂鸣器响 → 功耗↑
停止状态：  TIM3 时钟关闭 → 计数器停 → 引脚静止 → 蜂鸣器哑 → 功耗↓
```

**对电池供电产品**：播放完一段后彻底关停，而不是让定时器空转，这个细节很重要。

```c
uint8_t buzzer_test_stop(void)
{
    HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_3);   // 停止 PWM 输出
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_3, 0U);  // 清零 CCR，确保低电平
    s_buzzer_running = 0U;                      // 更新状态标志
    printf("buzzer: stop\r\n");                 // 打印汇报
    return 0U;
}
```

### main.c——应用层

```c
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART2_UART_Init();
    MX_TIM3_Init();

    buzzer_test_beep(2700, 50, 200);   // 第一声
    HAL_Delay(100);
    buzzer_test_beep(2700, 50, 200);   // 第二声
    HAL_Delay(500);
    buzzer_test_play_sequence(buzzer_song_ode_to_joy, 300, 300);  // 播放《欢乐颂》

    while (1) {}
}
```

- **beep 两声**：2700Hz 超出常用音调范围，属于高频提示音，类似"嘀嘀"
- **play_sequence 第二个参数 300**：实际数组只有 54 个元素，传 300 会读越界。应传 `BUZZER_SONG_ODE_TO_JOY_LENGTH`（54）。**这是一个 bug**

### tim.c——CubeMX 生成的 PWM 初始化

```c
htim3.Instance = TIM3;
htim3.Init.Prescaler = 84-1;         // 84 分频 → 1MHz
htim3.Init.Period = 369;              // 1MHz / 370 ≈ 2702Hz
htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

sConfigOC.OCMode = TIM_OCMODE_PWM1;
sConfigOC.Pulse = 185;                // 50% 占空比 (185/370)
sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_3);
```

**PB0 复用配置**：

```c
GPIO_InitStruct.Pin = GPIO_PIN_0;
GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;   // 复用推挽
GPIO_InitStruct.Pull = GPIO_NOPULL;
GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
GPIO_InitStruct.Alternate = GPIO_AF2_TIM3;  // AF2 = TIM3
HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
```

## 设计问题与改进空间

1. **`play_sequence` 越界 bug**：main.c 传 `buzzer_song_ode_to_joy` 数组长度 300，实际只有 54 个元素。应传 `BUZZER_SONG_ODE_TO_JOY_LENGTH`。会导致读取栈上随机数据，可能产生不可预测的音调或 HardFault。

2. **`play_sequence` 阻塞式播放**：整个播放过程用 `HAL_Delay` 阻塞，播放期间 MCU 无法做其他事。可改为非阻塞式（状态机 + 定时器中断），但复杂度显著增加。

3. **PWM 停止/启动的毛刺**：每次 `buzzer_test_apply` 都先 Stop 再 Start，切换瞬间可能产生毛刺。可改为只更新 ARR/CCR 不停止（需 `AutoReloadPreload = ENABLE`）。

4. **`volatile s_buzzer_running` 未使用**：声明了状态标志但未在任何地方读取，可能是为非阻塞模式预留的，当前版本未实现。

5. **频率精度**：整数除法有截断误差。如 262Hz → `84000000/262 = 320610`，实际输出 `84000000/320610 = 262.0003Hz`，误差 < 0.01%，人耳不可分辨。

## 补充说明：PWM 与 HAL_Delay 的关系

**核心原理**：PWM 由硬件定时器自动生成，一旦启动就持续输出，不依赖 CPU 干预。

```
┌─────────────────────────────────────────────┐
│              TIMx 定时器 (硬件)               │
│                                             │
│   ARR (自动重装载) ──┐                        │
│   CCRx (比较值)   ──┤──▶ 比较器 ──▶ PWM 输出引脚 │
│   计数器 (CNT)   ──┘                        │
│                                             │
│   硬件每个时钟周期自动：CNT++ → 与CCR比较 → 输出翻转  │
└─────────────────────────────────────────────┘
```

- `HAL_TIM_PWM_Start()` 启动后，**硬件定时器自己跑**
- CPU 执行 `HAL_Delay()` 时，定时器继续在后台产生 PWM 波形
- **PWM 输出不会暂停**

**对比：什么会暂停？**

| 情况 | PWM 输出 | 原因 |
|------|---------|------|
| `HAL_Delay()` | ✅ 继续 | 硬件定时器独立运行 |
| 关中断 | ✅ 继续 | 定时器不依赖中断产生 PWM |
| CPU 死循环 | ✅ 继续 | 同上 |
| `__WFI()` 待机 | ✅ 继续 | 定时器时钟仍在 |
| `__WFI()` + 关定时器时钟 | ❌ 停止 | 时钟被关了 |

**有无 DMA 的区别**：

```
无 DMA（本例）：
  ┌──────────┐
  │ CPU 轮询  │──▶ 手动改 CCR 值 ──▶ PWM 变频/变占空比
  └──────────┘
  
  每个音符：CPU 设置 CCR → HAL_Delay → CPU 改下一个 CCR → ...
  （音符切换时有微小间隙，人耳几乎听不出）

有 DMA：
  ┌──────────┐     ┌──────────┐
  │ DMA 控制器 │────▶│ 自动改 CCR │──▶ PWM 平滑变化
  └──────────┘     └──────────┘
  
  CPU 只需启动 DMA，后续自动搬运数据
  （适合音频流、连续波形等无缝切换场景）
```

蜂鸣器是「音符级」控制，不是「采样级」控制，不需要 DMA。

## 关联笔记

- [[17_rocketpi_pwm_sg90|17 PWM 舵机]]：同为 PWM 应用，舵机控制角度 vs 蜂鸣器控制频率
- [[18_rocketpi_pwm_motor|18 PWM 电机]]：PWM 控制电机转速
- [[05_rocketpi_delay_us|05 微秒延时]]：TIM11 做微秒延时，对比 TIM3 做 PWM
