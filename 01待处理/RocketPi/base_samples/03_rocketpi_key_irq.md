---
status: todo
created: 2026-09-09
tags:
  - stm32/gpio
  - stm32/exti
  - rocketpi/base_samples
  - todo
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/03_rocketpi_key_irq/main.c"
---

# 03_rocketpi_key_irq

> 代码：`Embedded_Code/01待处理/rocketpi/base_samples/03_rocketpi_key_irq/`（main.c、gpio.c、stm32f4xx_it.c）
> 硬件：STM32F401RE（RocketPi），PA0 按键接 EXTI0，LED_R/G/B 三个 LED

## 实验目标

用 EXTI 外部中断响应按键，代替 02 的主循环轮询：中断里只置标志位，LED 翻转放在主循环执行。

## 代码结构：三处分工

### 1. gpio.c — 中断通路配置

```c
GPIO_InitStruct.Pin = KEY_PA0_Pin;
GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;   // 上升沿触发中断
GPIO_InitStruct.Pull = GPIO_NOPULL;
HAL_GPIO_Init(KEY_PA0_GPIO_Port, &GPIO_InitStruct);

/* EXTI interrupt init*/
HAL_NVIC_SetPriority(EXTI0_IRQn, 0, 0);       // NVIC 使能，抢占优先级 0
HAL_NVIC_EnableIRQ(EXTI0_IRQn);
```

- `GPIO_MODE_IT_RISING`：与 02 的普通输入 `GPIO_MODE_INPUT` 的本质区别——引脚电平跳变会触发 EXTI 硬件事件，而不是等 CPU 来读
- 按下产生上升沿 → EXTI line0 挂起 → NVIC 跳到 `EXTI0_IRQHandler`
- 注意上拉配置为 `GPIO_NOPULL`：消抖不再依赖轮询延时，而是由回调里的时间戳判断完成

### 2. stm32f4xx_it.c — 中断到回调的传递链

```c
void EXTI0_IRQHandler(void)
{
  HAL_GPIO_EXTI_IRQHandler(KEY_PA0_Pin);      // HAL 公共入口：清标志位 + 分发回调
}

/* USER CODE BEGIN 1 */
volatile uint8_t key_flag = 0;

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == KEY_PA0_Pin)
  {
    static uint32_t last_press_tick = 0;
    uint32_t now = HAL_GetTick();

    if ((now - last_press_tick) >= 20U)       // 20ms 时间戳消抖
    {
      last_press_tick = now;
      key_flag = 1;
    }
  }
}
```

中断链路：**EXTI0_IRQHandler → HAL_GPIO_EXTI_IRQHandler（清除挂起位）→ HAL_GPIO_EXTI_Callback（弱函数，用户重写）**。

回调里做三件事：

1. **判引脚**：多个 EXTI 共用此回调，靠 `GPIO_Pin` 参数区分来源
2. **消抖**：不用 02 的"延时后再读"（中断里不能死等），改用 `HAL_GetTick()` 时间戳——两次触发间隔不足 20ms 直接丢弃。这是中断消抖的标准手法
3. **置标志**：`key_flag = 1`，立刻退出，把耗时工作留给主循环

`volatile` 是关键：`key_flag` 在中断和主循环两个执行流之间共享，不加的话主循环可能读到寄存器缓存的旧值，永远看不到中断里的修改。

### 3. main.c — 主循环消费标志位

```c
extern volatile uint8_t key_flag;   // 声明在 USER CODE 0 区

while (1)
{
  if (key_flag)
  {
    HAL_GPIO_TogglePin(LED_R_GPIO_Port, LED_R_Pin);
    HAL_GPIO_TogglePin(LED_G_GPIO_Port, LED_G_Pin);
    HAL_GPIO_TogglePin(LED_B_GPIO_Port, LED_B_Pin);
    key_flag = 0;                   // 消费后清零，等待下次中断
  }
}
```

主循环不再有任何延时和轮询按键——空闲时就是空转，随时可以被中断打断。翻转 RGB 三色后把标志清零，一次中断对应一次翻转。

## 与 02 的对照

| 维度 | 02 轮询 | 03 中断 |
|---|---|---|
| 响应时机 | 主循环扫到才处理 | 电平跳变立即响应 |
| 消抖 | 延时 20ms 后再读 | 时间戳判断，不阻塞 |
| 松手处理 | `while` 死等 | 不需要（上升沿天然一次一触发） |
| CPU 空闲时 | 仍在轮询 | 空转，功耗场景可配合休眠 |

标志位 + `volatile` 是中断与主循环通信的最简模式。局限也明显：标志只能记"发生过"，连续快速按键会被合并。更复杂的按键语义（长按/双击）交给 04 的 MultiButton 状态机。

## 中断安全小提醒

`key_flag` 读-清不是原子操作（`if (key_flag) ... key_flag = 0;` 之间若来了新中断，标志会被覆盖丢失）。本例只做 LED 翻转无伤大雅，但生产代码中多字节共享数据需要 `__disable_irq()` 临界区或关中断保护。
