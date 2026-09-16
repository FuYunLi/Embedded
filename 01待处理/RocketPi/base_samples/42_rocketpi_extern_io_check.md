---
status: done
created: 2026-09-16
tags:
  - c/gpio
  - c/hardware-test
  - embedded/verification
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/42_rocketpi_extern_io_check"
  - "[[01_rocketpi_led]] LED GPIO 输出（42 例的扩展）"
---

# 42 外部 IO 引脚检查

## 一句话定性

Rocket-Pi 开发板的外部 IO 引脚全面翻转测试，同时切换 18 个 GPIO 引脚（PA/PB/PC），用于验证硬件连接和引脚功能是否正常，是开发板出厂检测或故障排查的基础工具。

## 同类产品定位

- **本例**：批量 GPIO 翻转，肉眼/示波器观察，简单直接
- **01 例**：单个 LED 控制，GPIO 输出基础
- **万用表/示波器**：逐引脚测量，精确但费时
- **本例定位**：42 例的压轴，将前面学到的 GPIO 知识应用于硬件验证

## 硬件连接

- PA6, PA8, PA9, PA10
- PB1, PB2, PB4, PB6, PB7, PB12, PB13, PB15
- PC2, PC6, PC7, PC9, PC10, PC11

共 18 个引脚，覆盖 PA/PB/PC 三个端口，每 500ms 翻转一次。

## 代码架构

```
main.c (5.3KB，极简)
└── 主循环：HAL_GPIO_TogglePin × 3 组 + HAL_Delay(500)
```

## 核心实现

### main——批量 GPIO 翻转

```c
while (1)
{
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_2|GPIO_PIN_6|GPIO_PIN_7|GPIO_PIN_9
                             |GPIO_PIN_10|GPIO_PIN_11);

    HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_6|GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10);

    HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_12|GPIO_PIN_13
                             |GPIO_PIN_15|GPIO_PIN_4|GPIO_PIN_6|GPIO_PIN_7);

    HAL_Delay(500);
}
```

- **三组 TogglePin**：按端口分组，每组用位掩码同时切换多个引脚
- **500ms 周期**：1Hz 方波，肉眼可见闪烁，示波器可测量
- **所有引脚同相**：同时高→同时低→同时高，无相位差

### 引脚用途说明

这些引脚覆盖了 Rocket-Pi 开发板上大部分外设接口：

| 引脚 | 典型用途 |
|------|---------|
| PA6, PA8~10 | SPI1、USART1、TIM |
| PB1, PB2, PB4 | ADC、TIM3、SPI1 |
| PB6, PB7 | I2C1、TIM4 |
| PB12~13, PB15 | SPI2/I2S2、TIM1 |
| PC2, PC6~7, PC9~11 | ADC、TIM3/8、SDIO |

## 设计问题与改进空间

1. **无输出验证**：代码只翻转引脚，不读回验证。可加 `HAL_GPIO_ReadPin` 检查输出是否生效（需外部回环连接）。

2. **所有引脚同相**：无法区分相邻引脚。可改为交替相位（奇数引脚先高，偶数引脚先低），示波器上更易辨认。

3. **无引脚标注输出**：串口不打印当前引脚状态。可加 `printf` 输出每个引脚的电平，方便无示波器时调试。

4. **与 01 例对比**：01 例是单个 LED 的 GPIO 输出基础，42 例是 18 个引脚的批量验证。从单引脚到多引脚的扩展，体现了 GPIO 的通用性。

## 关联笔记

- [[01_rocketpi_led|01 LED GPIO 输出]]：GPIO 输出基础，42 例的扩展
- [[02_rocketpi_key_scan|02 按键轮询]]：GPIO 输入基础，对比输出
