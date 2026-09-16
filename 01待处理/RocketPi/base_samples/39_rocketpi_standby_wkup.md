---
status: done
created: 2026-09-16
tags:
  - c/power
  - c/standby
  - embedded/low-power
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/39_rocketpi_standby_wkup"
---

# 39 Standby 待机唤醒

## 一句话定性

STM32F401 的 Standby 模式是最低功耗睡眠状态（~2µA），仅保留 WKUP 引脚唤醒能力，唤醒后相当于复位重启，用于嵌入式系统的电池供电场景（传感器节点、遥控器等）。

## STM32 低功耗模式对比

| 模式 | 功耗 | 唤醒时间 | 保留内容 | 唤醒源 |
|------|------|----------|----------|--------|
| Sleep | ~mA | 即时 | 全部 | 任意中断 |
| Stop | ~µA | ~µs | SRAM + 寄存器 | EXTI |
| Standby | ~2µA | ~ms | 仅 WKUP 标志 | WKUP 引脚 / RTC / IWDG |

## 硬件连接

- WKUP 引脚：PA0（PWR_WAKEUP_PIN1），上升沿唤醒
- LED：PB1/B/P/R（三色 LED，用于指示唤醒状态）

## 代码架构

```
main.c (9.6KB，全部逻辑)
├── main：判断唤醒来源 → LED 指示 → 延时 3s → 进入 Standby
├── EnterStandbyMode：清除标志 → 使能 WKUP → 进入 Standby
├── SetAllLeds：三色 LED 统一控制
└── EnableAllPeripheralClocks：打开所有外设时钟（模拟高功耗）
```

## 核心实现

### main——唤醒判断 + Standby 循环

```c
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    EnableAllPeripheralClocks();  // ① 打开所有外设时钟

    // ② 判断是否从 Standby 唤醒
    uint8_t woke_from_standby = (__HAL_PWR_GET_FLAG(PWR_FLAG_SB) != RESET);

    if (woke_from_standby) {
        __HAL_PWR_CLEAR_FLAG(PWR_FLAG_SB);  // 清除 Standby 标志
        __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WU);  // 清除唤醒标志
        SetAllLeds(GPIO_PIN_RESET);          // LED 亮（唤醒成功）
    } else {
        SetAllLeds(GPIO_PIN_SET);            // LED 灭（首次上电）
    }

    HAL_Delay(3000);  // ③ 延时 3 秒观察
    EnterStandbyMode();  // ④ 进入 Standby

    while (1) {}  // 永远不会到这里
}
```

- **① EnableAllPeripheralClocks**：打开所有外设时钟，让工作模式功耗尽可能大，方便对比 Standby 的低功耗效果。实际产品中不需要
- **② PWR_FLAG_SB**：Standby 标志位，唤醒后为 SET。用它区分"首次上电"和"从 Standby 唤醒"
- **③ 3 秒延时**：让 LED 亮 3 秒，方便观察。实际产品中可缩短或去掉
- **④ EnterStandbyMode**：进入 Standby，MCU 停止执行，等待 WKUP 引脚上升沿

### EnterStandbyMode——进入 Standby

```c
static void EnterStandbyMode(void)
{
    HAL_PWR_DisableWakeUpPin(PWR_WAKEUP_PIN1);  // ⑤ 先禁用
    __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WU);          // ⑥ 清除旧唤醒标志
    HAL_PWR_EnableWakeUpPin(PWR_WAKEUP_PIN1);   // ⑦ 重新使能
    HAL_PWR_EnterSTANDBYMode();                  // ⑧ 进入 Standby
}
```

- **⑤⑥⑦ 先禁用再使能**：清除可能残留的旧唤醒标志，避免立即唤醒。这是 Standby 进入的标准流程
- **⑧ EnterSTANDBYMode**：执行 WFI（Wait For Interrupt），MCU 进入 Standby。唤醒后从 Reset_Handler 重新开始执行

### WKUP 引脚唤醒

```
PA0 上升沿 → PWR 唤醒逻辑 → MCU 复位 → 从 Reset_Handler 开始
                                      → PWR_FLAG_SB = SET（Standby 标志）
                                      → 程序判断 SB 标志 → 执行唤醒后逻辑
```

**唤醒 = 复位**：Standby 唤醒后，MCU 状态全部丢失（SRAM 清零、寄存器复位），只有 PWR_FLAG_SB 和备份寄存器保留。相当于按了一次复位按钮。

### EnableAllPeripheralClocks——功耗对比

```c
static void EnableAllPeripheralClocks(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    // ... GPIOA~H, DMA1/2, CRC, USB, TIM2~11, SPI1~4, I2C1~3,
    //     USART1~6, ADC1, SDIO, DAC, SYSCFG ...
}
```

**设计意图**：打开所有外设时钟，让工作模式功耗尽可能大（~mA 级）。Standby 模式下这些时钟全部关闭（~2µA），对比效果明显。实际产品中，不需要的外设不应打开时钟。

## 设计问题与改进空间

1. **唤醒 = 复位**：Standby 唤醒后所有状态丢失，需要重新初始化所有外设。如果应用需要保持状态（如传感器数据），应改用 Stop 模式（保留 SRAM）。

2. **仅 WKUP 引脚唤醒**：本例只用 PA0 上升沿唤醒。STM32F4 还支持 RTC 闹钟唤醒和 IWDG 看门狗唤醒，可组合使用。

3. **无 RTC 唤醒**：电池供电场景通常需要定时唤醒（如每 5 分钟采集一次数据）。可加 RTC 闹钟作为唤醒源。

4. **LED 指示逻辑**：唤醒后 LED 亮 3 秒再进 Standby。实际产品中，唤醒后采集数据、发送、再进 Standby，LED 可省略以节省功耗。

5. **EnableAllPeripheralClocks 的 #if 穷举**：与 15 例的 `at24cxx_enable_gpio_clock` 相同问题——每个外设一个 `#if defined` 分支。可改为宏数组。

## 关联笔记

- [[20_rocketpi_adc_mcu_temperature|20 ADC 温度传感器]]：低功耗场景的典型应用（定时唤醒采集）
- [[37_rocketpi_esp8266|37 ESP8266 WiFi]]：WiFi 模块功耗大，Standby 可关闭 WiFi 节省电量
