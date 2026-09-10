---
status: todo
created: 2026-09-10
tags:
  - stm32/gpio
  - rocketpi/base_samples
  - todo
references:
  - "[[05_rocketpi_delay_us]]"
---

# 05_rocketpi_delay_us_1

> 进阶补充：由 05 补齐的 gpio.c 引出——GPIO 速度档位（Speed）到底控制什么。
> 前置笔记：[[05_rocketpi_delay_us]]

## GPIO_SPEED_FREQ 控制的是压摆率

```c
GPIO_InitStruct.Pin = GPIO_PIN_10;
GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;   // 本例的关键配置
```

GPIO 速度档位控制的是**输出驱动器的压摆率（slew rate）**——引脚电平翻转时上升/下降沿的陡峭程度，不是"软件访问速度"。

| 档位 | 边沿量级 | 适用 |
|---|---|---|
| LOW | 几百 ns 级爬升 | 点灯、秒级人眼信号（02~04 的 LED） |
| MEDIUM / HIGH | 中间档 | 中速数字信号 |
| VERY HIGH | 几十 ns 级 | 高速信号：本例 500kHz 方波、SDIO、SPI 高速时钟 |

## 本例为什么必须 VERY HIGH

PC10 以 1µs 间隔翻转（500kHz 方波）。若用默认 LOW 档：

- 单是边沿爬升就吃掉几百 ns
- 加上 HAL 版翻转函数 1~2µs 的调用开销
- 示波器波形严重变形：高电平时间被边沿压窄，占空比失真

这和 main.c 用 `GPIOC->ODR` 直写是同一逻辑的两端：**µs 级波形上，驱动器速度和软件开销都得抠**。

## 速度档按信号频率选，不是越快越好

VERY HIGH 功耗更大、EMI 更强，慢信号用快驱动纯浪费。02~04 里 LED 配 `GPIO_SPEED_FREQ_LOW` 就够——翻转间隔是秒级，边沿再缓也无所谓。

判断方法：**信号周期（含边沿）与驱动器边沿时间同一量级时必须提速；信号周期比边沿时间大两个量级以上，选最低档即可。**
