---
status: done
created: 2026-09-16
tags:
  - c/i2s
  - c/audio
  - embedded/peripheral
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/27_rocketpi_i2s"
  - "[[16_rocketpi_pwm_passive_buzzer]] PWM 蜂鸣器（对比 I2S 音频输出）"
  - "[[23_rocketpi_ws2812b]] DMA 传输（对比 I2S DMA）"
---

# 27 I2S 音频播放 MAX98357

## 一句话定性

I2S（Inter-IC Sound）是数字音频传输标准，MCU 通过 I2S 接口将 PCM 音频数据流式发送到 MAX98357 D 类功放芯片，驱动扬声器播放语音，用于嵌入式系统的音频输出（TTS 语音、提示音、音乐播放等）。

## 同类产品定位

- **MAX98357**：I2S 输入 D 类功放，3W 单声道，内置 DAC，成本 ~¥3
- **MAX98356**：同类芯片，不同封装
- **PCM5102A**：I2S DAC，音质更高但需外接功放
- **CS4344**：I2S DAC，低功耗
- **本例选型理由**：MAX98357 最简单（I2S 直接输入、内置 DAC+功放），适合学习 I2S 音频输出

## 硬件连接

- PB13 = I2S2_CK → BCLK（比特时钟）
- PB12 = I2S2_WS → LRC（左右声道时钟）
- PB15 = I2S2_SD → DIN（串行音频数据）
- GAIN/SD：悬空（默认增益，常开）

## 通信协议要点

- **I2S 标准**：Philips 标准，WS 高=左声道，WS 低=右声道
- **数据格式**：16 位扩展（16B_EXTENDED），每个采样 16 位
- **采样率**：16kHz（语音够用，音乐需 44.1kHz+）
- **PCM 数据**：预录制的语音数据，存储在 `audio.h` 的 `const uint16_t audio_track[]` 数组中
- **DMA 传输**：分块发送，每块最大 65535 采样，中断回调续发下一块

## 代码架构

```
┌─────────────────────────────────────────────────┐
│ main.c (应用层 + 播放控制)                      │
│  - Audio_BeginPlayback：启动播放                │
│  - Audio_StartNextChunk：分块 DMA 发送          │
│  - HAL_I2S_TxCpltCallback：DMA 完成续发         │
├─────────────────────────────────────────────────┤
│ audio.h (音频数据)                              │
│  - const uint16_t audio_track[]：PCM 采样数组   │
│  - AUDIO_TRACK_SAMPLE_COUNT：采样总数           │
├─────────────────────────────────────────────────┤
│ i2s.c (CubeMX 生成)                            │
│  - I2S2 配置：Philips 标准、16kHz、16 位        │
│  - DMA1_Stream4：内存→外设，半字传输            │
│  - PB12(WS)/PB13(CK)/PB15(SD) 复用 AF5         │
└─────────────────────────────────────────────────┘
```

## 核心实现详解

### I2S 配置——16kHz 16 位 Philips 标准

```c
hi2s2.Instance = SPI2;
hi2s2.Init.Mode = I2S_MODE_MASTER_TX;            // 主机发送
hi2s2.Init.Standard = I2S_STANDARD_PHILIPS;       // Philips 标准
hi2s2.Init.DataFormat = I2S_DATAFORMAT_16B_EXTENDED;  // 16 位扩展
hi2s2.Init.MCLKOutput = I2S_MCLKOUTPUT_DISABLE;  // 不输出 MCLK
hi2s2.Init.AudioFreq = I2S_AUDIOFREQ_16K;         // 16kHz 采样率
hi2s2.Init.CPOL = I2S_CPOL_LOW;                   // 时钟空闲低
hi2s2.Init.ClockSource = I2S_CLOCK_PLL;           // PLL 时钟源
```

**I2S 时钟计算**：`PLLI2SN=50, PLLI2SR=4` → PLLI2S 时钟 = `HSE/PLLM × PLLI2SN / PLLI2SR = 8MHz/4 × 50 / 4 = 25MHz`。16kHz × 16bit × 2ch = 512kHz，25MHz / 512 ≈ 49，精度足够。

**16B_EXTENDED**：每个采样 16 位，WS 信号在一个时钟周期前变化（I2S 标准要求）。

### Audio_BeginPlayback——启动播放

**作用**：初始化播放控制结构体，发送第一个 DMA 块。

```c
static void Audio_BeginPlayback(void)
{
    audio_ctrl.next_index = 0U;
    audio_ctrl.samples_remaining = (uint32_t)AUDIO_TRACK_SAMPLE_COUNT;
    audio_ctrl.state = AUDIO_PLAYBACK_STATE_IDLE;

    if (audio_ctrl.samples_remaining == 0U) {
        audio_ctrl.state = AUDIO_PLAYBACK_STATE_DONE;
        return;
    }

    if (Audio_StartNextChunk() != HAL_OK) {
        audio_ctrl.state = AUDIO_PLAYBACK_STATE_ERROR;
        Error_Handler();
    }
}
```

### Audio_StartNextChunk——分块 DMA 发送

**作用**：将音频数据分成 ≤65535 采样的块，通过 DMA 发送到 I2S 外设。

```c
static HAL_StatusTypeDef Audio_StartNextChunk(void)
{
    if (audio_ctrl.samples_remaining == 0U) {
        audio_ctrl.state = AUDIO_PLAYBACK_STATE_DONE;
        return HAL_OK;
    }

    // ① 计算本次块大小
    uint32_t chunk = (audio_ctrl.samples_remaining > AUDIO_DMA_MAX_TRANSFER_SAMPLES) ?
                     AUDIO_DMA_MAX_TRANSFER_SAMPLES :
                     audio_ctrl.samples_remaining;

    // ② DMA 发送
    const uint16_t *chunk_ptr = &audio_track[audio_ctrl.next_index];
    HAL_StatusTypeDef status = HAL_I2S_Transmit_DMA(&hi2s2, (uint16_t *)chunk_ptr, (uint16_t)chunk);

    if (status == HAL_OK) {
        audio_ctrl.next_index += chunk;
        audio_ctrl.samples_remaining -= chunk;
        audio_ctrl.state = AUDIO_PLAYBACK_STATE_RUNNING;
    }

    return status;
}
```

- **① 分块原因**：`HAL_I2S_Transmit_DMA` 的长度参数是 `uint16_t`，最大 65535。音频数据可能远超此值，需要分块发送
- **② DMA 发送**：非阻塞，函数立即返回。DMA 完成后触发 `HAL_I2S_TxCpltCallback` 回调

### HAL_I2S_TxCpltCallback——DMA 完成续发

**作用**：DMA 传输完成中断回调，检查是否还有剩余数据，有则续发下一块。

```c
void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance != hi2s2.Instance) return;

    if (audio_ctrl.samples_remaining == 0U) {
        audio_ctrl.state = AUDIO_PLAYBACK_STATE_DONE;
        return;
    }

    if (Audio_StartNextChunk() != HAL_OK) {
        audio_ctrl.state = AUDIO_PLAYBACK_STATE_ERROR;
        Error_Handler();
    }
}
```

**链式 DMA**：每块 DMA 完成后，在回调中启动下一块。直到所有采样发送完毕。这是长音频播放的标准模式——单次 DMA 无法传输超过 65535 个半字。

### audio_playback_ctrl_t——播放控制状态机

```c
typedef enum {
    AUDIO_PLAYBACK_STATE_IDLE = 0,
    AUDIO_PLAYBACK_STATE_RUNNING,
    AUDIO_PLAYBACK_STATE_DONE,
    AUDIO_PLAYBACK_STATE_ERROR
} audio_playback_state_t;

typedef struct {
    uint32_t next_index;           // 下一块起始索引
    uint32_t samples_remaining;    // 剩余采样数
    audio_playback_state_t state;  // 播放状态
} audio_playback_ctrl_t;
```

**状态机**：IDLE → RUNNING（DMA 发送中）→ DONE（全部完成）或 ERROR（DMA 失败）。主循环可通过检查 `state` 知道播放进度。

### main.c——应用层

```c
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init(); MX_DMA_Init(); MX_I2S2_Init();

    Audio_BeginPlayback();  // 启动播放

    while (1) {}  // 播放在 DMA 中断中自动续发
}
```

启动后主循环为空——音频播放完全由 DMA 中断驱动，CPU 几乎不参与。

## 设计问题与改进空间

1. **音频数据占 Flash**：16kHz × 16bit = 32KB/s，10 秒语音 = 320KB。对 512KB Flash 的 F401RE 来说占 62.5%。可改为从 SD 卡读取（28/29 例）。

2. **16kHz 采样率**：语音够用，但音乐需要 44.1kHz 或 48kHz。提高采样率会增加 I2S 时钟配置复杂度和数据量。

3. **单声道**：MAX98357 是单声道功放，I2S 的 WS 信号区分左右声道但只用一个声道。可通过 GAIN 引脚选择左/右/混合。

4. **无音量控制**：PCM 数据是固定的，运行时无法调节音量。可在发送前对采样值做缩放（`sample * volume / 100`），或用 MAX98357 的 GAIN 引脚。

5. **`audio.h` 数据来源**：readme 提到用 `edge-tts` 工具将文字转语音生成 PCM 数据头文件。这是离线预处理，运行时直接播放。

6. **与 16 例（蜂鸣器）对比**：16 例用 PWM 方波驱动无源蜂鸣器（单频音调），27 例用 I2S PCM 数据驱动 MAX98357（任意音频）。PWM 适合简单提示音，I2S 适合语音/音乐。

## 关联笔记

- [[16_rocketpi_pwm_passive_buzzer|16 PWM 蜂鸣器]]：简单音频输出（方波），对比 I2S PCM 音频
- [[23_rocketpi_ws2812b|23 WS2812B LED]]：另一种 DMA 传输模式（PWM+DMA vs I2S+DMA）
- [[30_rocketpi_sd_audio_to_i2s|30 SD 卡音频]]：从 SD 卡读取音频到 I2S 播放
