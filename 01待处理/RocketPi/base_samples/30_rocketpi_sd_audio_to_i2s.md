---
status: done
created: 2026-09-16
tags:
  - c/audio
  - c/fatfs
  - c/i2s
  - embedded/multimedia
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/30_rocketpi_sd_audio_to_i2s"
  - "[[27_rocketpi_i2s]] I2S 音频播放（30 例的基础）"
  - "[[29_rocketpi_sdio_card_fatfs]] FATFS 文件系统（30 例的存储层）"
  - "[[25_rocketpi_spi_lcd_bitmap]] ST7789 LCD 驱动（30 例的频谱显示）"
---

# 30 SD 卡音频到 I2S 播放

## 一句话定性

从 SD 卡读取 PCM 音频文件，通过 FATFS 文件系统解析，经 DMA 双缓冲流式传输到 I2S 外设驱动 MAX98357 功放播放，同时在 LCD 上显示实时频谱，是嵌入式系统的完整音频播放器。

## 代码架构

```
main.c (29KB，全部业务逻辑)
├── audio_sd_start_playback：挂载+打开+预读双缓冲+启动DMA
├── audio_sd_process_playback：主循环播放状态机（双缓冲交换+续发+预读）
├── audio_apply_volume_to_buffer：Q15 定点音量缩放
├── spectrum_*：Goertzel 频谱分析 + LCD 频谱柱绘制
└── fatfs_speed_test：SD 卡读写速度测试
```

## 核心设计

### 双缓冲流式播放

buf0/buf1 交替：一个 DMA 播放时，CPU 从 SD 卡预读另一个。SD 卡读取时间 < DMA 播放时间则无缝播放。

启动流程：挂载 SD 卡 → 打开 `audio/audio.bin` → 预读两块到 buf0/buf1 → DMA 播放 buf0。主循环中 DMA 完成回调置标志，`audio_sd_process_playback` 交换缓冲区、续发 DMA、从 SD 卡预读下一块。

### Q15 数字音量

`gain_q15 = percent * 32767 / 100`，采样值乘以增益后右移 15 位，含四舍五入（`+ (1<<14)`）和饱和处理（±32768）。100% → 32767（约1.0），50% → 16383（约0.5）。

### Goertzel 频谱

比 FFT 高效——只算 40 个频点，每个频点一个二阶 IIR 滤波器。能量公式 `power = q1² + q2² - coeff*q1*q2`。指数平滑（α=0.30）+ 峰值衰减（α=0.90）。LCD 上 40 条频谱柱，蓝→青→绿→黄→红渐变。

### I2S 静音

`audio_output_force_idle` 停止 DMA 后将 I2S 引脚（PB12/PB13/PB15）重新配置为推挽输出低电平，消除浮空噪声。

## 设计问题

1. **29KB 单文件**：音频播放+音量+频谱+LCD+FATFS 测试全在一个 main.c，应拆分模块
2. **音量原地修改缓冲**：`audio_apply_volume_to_buffer` 在预读时修改原始数据，动态调音量需双份数据
3. **无播放列表**：只播固定文件 `audio/audio.bin`，可扩展为目录扫描

## 关联笔记

- [[27_rocketpi_i2s|27 I2S 音频播放]]：内置音频 + I2S，30 例的基础
- [[29_rocketpi_sdio_card_fatfs|29 FATFS 文件系统]]：SD 卡文件读写，30 例的存储层
- [[25_rocketpi_spi_lcd_bitmap|25 SPI LCD 位图]]：ST7789 LCD 驱动，30 例的显示层
- [[16_rocketpi_pwm_passive_buzzer|16 PWM 蜂鸣器]]：简单音频输出，对比完整音频播放器
