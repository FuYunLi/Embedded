---
status: done
created: 2026-09-16
tags:
  - c/pwm
  - c/timer
  - embedded/actuator
  - rocketpi/base_samples
  - c/data-structure
  - c/macro
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/16_rocketpi_pwm_passive_buzzer"
  - "[[16_rocketpi_pwm_passive_buzzer]]"
---

# 16b PWM 无源蜂鸣器——序列播放详解

## 一句话定性

基于 `driver_buzzer_test.h` 的序列播放机制，通过音符数组 + 状态机实现音乐播放，是 PWM 蜂鸣器驱动的核心应用。

## 数据结构设计

### buzzer_test_note_t——音符结构体

```c
typedef struct {
    uint32_t frequency_hz;  // 音调频率（0 = 静音）
    uint8_t duty_percent;   // 占空比（0 = 默认 50%）
    uint32_t duration_ms;   // 持续时长（0 = 默认 200ms）
} buzzer_test_note_t;
```

**设计思想**：每个字段的 0 值都有默认兜底，简化曲谱定义。

```
┌─────────────────────────────────────────────────────────┐
│                buzzer_test_note_t 内存布局               │
├─────────────────────────────────────────────────────────┤
│ frequency_hz (4B) │ duty_percent (1B) │ padding (3B) │ duration_ms (4B) │
├─────────────────────────────────────────────────────────┤
│ 262 (C4)          │ 50               │              │ 200              │
│ 0 (静音)           │ 0 (默认50%)       │              │ 300              │
└─────────────────────────────────────────────────────────┘
```

**字段语义**：
- `frequency_hz = 0`：表示静音（rest），停止 PWM 输出
- `duty_percent = 0`：使用默认值 50%（方波，振幅最大）
- `duration_ms = 0`：使用默认值 200ms

### 宏定义——音符频率常量

```c
// 十二平均律频率定义（四舍五入到整数）
#define BUZZER_NOTE_C4     262U   // 中音 do (261.63Hz)
#define BUZZER_NOTE_D4     294U   // 中音 re (293.66Hz)
#define BUZZER_NOTE_E4     330U   // 中音 mi (329.63Hz)
#define BUZZER_NOTE_F4     349U   // 中音 fa (349.23Hz)
#define BUZZER_NOTE_G4     392U   // 中音 sol (392.00Hz)
#define BUZZER_NOTE_A4     440U   // 中音 la (440.00Hz)
#define BUZZER_NOTE_B4     494U   // 中音 si (493.88Hz)
#define BUZZER_NOTE_C5     523U   // 高音 do (523.25Hz)

// 十二平均律公式：f = 440 × 2^((n-69)/12)
// 其中 n 是 MIDI 音符号（A4 = 69）
```

**声学原理**：十二平均律将八度等分为 12 个半音，频率比为 2^(1/12) ≈ 1.05946。

```
音阶频率关系（以 A4=440Hz 为基准）：
C4    D4    E4    F4    G4    A4    B4    C5
262   294   330   349   392   440   494   523
  ↑     ↑     ↑     ↑     ↑     ↑     ↑     ↑
  do    re    mi    fa    sol   la    si    do'
```

### 宏定义——时间控制

```c
#define BUZZER_TEST_DEFAULT_FREQUENCY_HZ    2000U   // 默认频率 2kHz
#define BUZZER_TEST_DEFAULT_DUTY_PERCENT    50U     // 默认占空比 50%
#define BUZZER_TEST_DEFAULT_DURATION_MS     200U    // 默认播放时长 200ms
#define BUZZER_TEST_DEFAULT_NOTE_GAP_MS     20U     // 默认音符间隔 20ms
#define BUZZER_TEST_MAX_DUTY_PERCENT        100U    // 最大占空比 100%
```

**设计考量**：
- **20ms 间隔**：蜂鸣器振膜停振时间，防止音符粘连
- **200ms 时长**：适合快速音符，可通过 `ODE_TO_JOY_DURATION` 宏调整
- **50% 占空比**：方波，振幅最大，音色最纯净

## 声学原理

### 蜂鸣器发声机制

```
┌─────────────────────────────────────────────────────────┐
│              无源蜂鸣器内部结构                           │
├─────────────────────────────────────────────────────────┤
│                                                         │
│    ┌─────────────┐                                      │
│    │  金属振膜    │ ←── 电磁线圈驱动                      │
│    └─────────────┘                                      │
│          ↑                                              │
│    ┌─────────────┐                                      │
│    │  电磁线圈    │ ←── PWM 方波驱动                      │
│    └─────────────┘                                      │
│          ↑                                              │
│    ┌─────────────┐                                      │
│    │  永磁体      │                                      │
│    └─────────────┘                                      │
│                                                         │
│  PWM 方波 → 线圈电流变化 → 电磁力变化 → 振膜振动 → 空气振动 → 声波 │
└─────────────────────────────────────────────────────────┘
```

**频率与音调**：
- PWM 频率 = 蜂鸣器振动频率 = 音调频率
- 262Hz → C4（中音 do）
- 440Hz → A4（标准音 la）

**占空比与音量**：
- 50% 占空比：方波，振幅最大，音色最纯净
- 过高/过低：波形不对称，产生谐波失真，音色变差

### 音符时值与节拍

```
4/4 拍示例（《欢乐颂》）：
┌─────┬─────┬─────┬─────┐
│ E4  │ E4  │ F4  │ G4  │  ← 第1小节
│ 1拍 │ 1拍 │ 1拍 │ 1拍 │
└─────┴─────┴─────┴─────┘

时值控制：
- 25 单位 × 8ms = 200ms（四分音符）
- 36 单位 × 8ms = 288ms（附点四分音符）
- 50 单位 × 8ms = 400ms（二分音符）
```

**ODE_TO_JOY_DURATION 宏**：
```c
#define ODE_TO_JOY_DURATION(units)  ((units) * 8U)
// 25 单位 = 200ms（四分音符）
// 36 单位 = 288ms（附点四分音符）
// 50 单位 = 400ms（二分音符）
```

## 驱动层实现

### 核心函数调用链

```
应用层调用：
buzzer_test_play_sequence(notes, length, gap_ms)
    │
    ├─▶ buzzer_test_apply(frequency, duty)  // 设置 PWM 参数
    │       │
    │       ├─▶ buzzer_test_get_timer_clock()  // 获取定时器时钟
    │       │
    │       ├─▶ 计算 PSC/ARR/CCR
    │       │
    │       └─▶ HAL_TIM_PWM_Stop() → 写寄存器 → HAL_TIM_PWM_Start()
    │
    ├─▶ HAL_Delay(duration)  // 播放时长（PWM 硬件持续输出）
    │
    └─▶ buzzer_test_stop()  // 停止 PWM
            │
            ├─▶ HAL_TIM_PWM_Stop()
            │
            └─▶ __HAL_TIM_SET_COMPARE(..., 0U)  // 清零 CCR
```

### 时序控制逻辑

```c
for (uint32_t i = 0U; i < length; ++i) {
    if (notes[i].frequency_hz == 0U) {
        // 静音处理
        buzzer_test_stop();                           // 停止 PWM 输出
        HAL_Delay(notes[i].duration_ms);              // 静音时长
    } else {
        // 发音处理
        buzzer_test_apply(notes[i].frequency_hz, notes[i].duty_percent);
        HAL_Delay(notes[i].duration_ms);              // 播放时长（PWM 硬件持续输出）
        buzzer_test_stop();                           // 停止当前音符
    }
    if (gap > 0U && i + 1U < length) {
        HAL_Delay(gap);                               // 音符间间隔（静音）
    }
}
```

**时序图**：

```
时间轴 ─────────────────────────────────────────────────────▶

音符1 (262Hz, 200ms)          音符2 (330Hz, 200ms)
┌─────────────────────┐       ┌─────────────────────┐
│  PWM 输出 262Hz     │       │  PWM 输出 330Hz     │
└─────────────────────┘       └─────────────────────┘
                            ↑
                            │ 20ms 静音间隔
                            ↓
                    ┌───────────────┐
                    │  无 PWM 输出   │
                    └───────────────┘

状态变化：
[停止] → apply(262) → Delay(200) → stop → Delay(20) → apply(330) → ...
```

## 应用层实现

### 曲谱定义示例

```c
// 《欢乐颂》片段
static const buzzer_test_note_t buzzer_song_ode_to_joy[] = {
    { BUZZER_NOTE_E4, 0U, ODE_TO_JOY_DURATION(25U) },  // mi, 200ms
    { BUZZER_NOTE_E4, 0U, ODE_TO_JOY_DURATION(25U) },  // mi, 200ms
    { BUZZER_NOTE_F4, 0U, ODE_TO_JOY_DURATION(25U) },  // fa, 200ms
    { BUZZER_NOTE_G4, 0U, ODE_TO_JOY_DURATION(25U) },  // sol, 200ms
    { BUZZER_NOTE_G4, 0U, ODE_TO_JOY_DURATION(25U) },  // sol, 200ms
    { BUZZER_NOTE_F4, 0U, ODE_TO_JOY_DURATION(25U) },  // fa, 200ms
    { BUZZER_NOTE_E4, 0U, ODE_TO_JOY_DURATION(25U) },  // mi, 200ms
    { BUZZER_NOTE_D4, 0U, ODE_TO_JOY_DURATION(25U) },  // re, 200ms
    { BUZZER_NOTE_C4, 0U, ODE_TO_JOY_DURATION(25U) },  // do, 200ms
    { BUZZER_NOTE_C4, 0U, ODE_TO_JOY_DURATION(25U) },  // do, 200ms
    { BUZZER_NOTE_D4, 0U, ODE_TO_JOY_DURATION(25U) },  // re, 200ms
    { BUZZER_NOTE_E4, 0U, ODE_TO_JOY_DURATION(25U) },  // mi, 200ms
    { BUZZER_NOTE_E4, 0U, ODE_TO_JOY_DURATION(36U) },  // mi, 288ms（附点）
    { BUZZER_NOTE_D4, 0U, ODE_TO_JOY_DURATION(12U) },  // re, 96ms（八分）
    { BUZZER_NOTE_D4, 0U, ODE_TO_JOY_DURATION(50U) },  // re, 400ms（二分）
    // ... 更多音符
};

#define BUZZER_SONG_ODE_TO_JOY_LENGTH  (sizeof(buzzer_song_ode_to_joy) / sizeof(buzzer_song_ode_to_joy[0]))
```

### main.c 调用示例

```c
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART2_UART_Init();
    MX_TIM3_Init();

    // 提示音：两声高频 beep
    buzzer_test_beep(2700, 50, 200);   // 第一声
    HAL_Delay(100);
    buzzer_test_beep(2700, 50, 200);   // 第二声
    HAL_Delay(500);

    // 播放《欢乐颂》
    buzzer_test_play_sequence(
        buzzer_song_ode_to_joy,
        BUZZER_SONG_ODE_TO_JOY_LENGTH,  // 正确：54 个音符
        300                              // 音符间隔 300ms
    );

    while (1) {}
}
```

**注意**：原代码传 300 作为长度是 bug，应传 `BUZZER_SONG_ODE_TO_JOY_LENGTH`（54）。

## 关键问题解答

### Q1: HAL_Delay 会让 PWM 暂停吗？

**不会**。PWM 由硬件定时器自动生成，一旦启动就持续输出，不依赖 CPU 干预。

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
- 即使 CPU 死循环或进入 `__WFI()` 待机，只要定时器时钟未被关闭，PWM 就不会停止

### Q2: 为什么需要 DMA？

**有无 DMA 影响的是「改变 PWM 参数」的方式**：

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

**蜂鸣器是「音符级」控制，不是「采样级」控制，不需要 DMA。**

### Q3: gap_ms 的含义是什么？

**是音符之间的「静音间隔」，不是「每个音最少 20ms」**。

```
音符1 ──▶ 静音20ms ──▶ 音符2 ──▶ 静音20ms ──▶ 音符3
         ↑ gap         ↑ gap
```

默认 20ms 是防粘连 — 如果 `gap_ms=0`，两个音符会无缝衔接，蜂鸣器振膜来不及停振，听起来会糊成一片。

### Q4: buzzer_test_stop 只是打印吗？

**不只是打印，是真关停定时器降低功耗**：

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

```
运行状态：  TIM3 时钟开启 → 计数器跑 → PWM 输出 → 蜂鸣器响 → 功耗↑
停止状态：  TIM3 时钟关闭 → 计数器停 → 引脚静止 → 蜂鸣器哑 → 功耗↓
```

**对电池供电产品**：播放完一段后彻底关停，而不是让定时器空转，这个细节很重要。

## 与 SkyStar 项目对比

**SkyStar 的 `dev_buzzer` 封装风格**：

```c
// 分离的接口设计
bsp_status_t dev_buzzer_set_freq(uint16_t freq);     // 只改频率
bsp_status_t dev_buzzer_set_volume(uint8_t volume);   // 只改音量
bsp_status_t dev_buzzer_tone(uint16_t freq, uint8_t volume);  // 联动切换
```

**RocketPi 的 `driver_buzzer_test` 封装风格**：

```c
// 一体化接口设计
uint8_t buzzer_test_apply(uint32_t frequency_hz, uint8_t duty_percent);  // 频率+占空比联动
uint8_t buzzer_test_beep(uint32_t frequency_hz, uint8_t duty_percent, uint32_t duration_ms);  // 单音播放
uint8_t buzzer_test_play_sequence(const buzzer_test_note_t *notes, uint32_t length, uint32_t gap_ms);  // 序列播放
```

**对比**：
- **SkyStar**：接口更灵活，适合复杂控制（如实时调节音量）
- **RocketPi**：接口更简洁，适合音乐播放（序列化控制）

## 关联笔记

- [[16_rocketpi_pwm_passive_buzzer|16 PWM 无源蜂鸣器]]：主笔记，PWM 基础与单音播放
- [[17_rocketpi_pwm_sg90|17 PWM 舵机]]：同为 PWM 应用，舵机控制角度 vs 蜂鸣器控制频率
- [[18_rocketpi_pwm_motor|18 PWM 电机]]：PWM 控制电机转速
