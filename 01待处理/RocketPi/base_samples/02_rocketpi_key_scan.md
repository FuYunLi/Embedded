---
status: todo
created: 2026-09-09
tags:
  - stm32/gpio
  - rocketpi/base_samples
  - todo
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/02_rocketpi_key_scan/main.c"
---

# 02_rocketpi_key_scan

> 代码：`Embedded_Code/01待处理/rocketpi/base_samples/02_rocketpi_key_scan/main.c`
> 硬件：STM32F401RE（RocketPi），PA0 按键，LED_B/LED_G/LED_P 三个 LED

## 实验目标

主循环轮询按键电平，`#if 0 / #else` 切换两种响应模型：

| 分支 | 模型 | 行为 |
|---|---|---|
| `#if 0`（禁用） | 电平跟随 | 按住亮、松开灭，LED 实时跟随按键 |
| `#else`（生效） | 边沿触发 | 每按一次翻转一次 LED 状态 |

## 硬件电平约定

从代码读写逻辑可反推板级设计：

- **按键 PA0**：按下读到 **高电平**（`GPIO_PIN_SET`）→ 外部配有**下拉**，悬空时稳定为低
- **LED**：写 `GPIO_PIN_RESET` 点亮 → **低电平点亮**（灌电流接法）

## 分支一：电平跟随（`#if 0`）

```c
if(HAL_GPIO_ReadPin(GPIOA,GPIO_PIN_0))   // 检测到按下（高电平）
{
    HAL_GPIO_WritePin(LED_B_GPIO_Port, LED_B_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, LED_G_Pin|LED_P_Pin, GPIO_PIN_RESET);
}else{
    HAL_GPIO_WritePin(LED_B_GPIO_Port, LED_B_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, LED_G_Pin|LED_P_Pin, GPIO_PIN_SET);
}
HAL_Delay(20);
```

要点：

- LED 状态**没有记忆**，完全由当前电平决定，逻辑与硬件直连
- `HAL_Delay(20)` 仅为降低扫描频率，并未做消抖——LED 可能出现肉眼可见的微抖，但因亮灭是持续刷新，实际影响小

## 分支二：按下翻转一次（`#else`，阻塞式模板）

```c
if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_0) == GPIO_PIN_SET) {   // 检测到按下
    HAL_Delay(20);                                      // ① 去抖
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_0) == GPIO_PIN_SET) { // ② 二次确认
        HAL_GPIO_TogglePin(LED_B_GPIO_Port, LED_B_Pin); // ③ 翻转 LED
        HAL_GPIO_TogglePin(GPIOB, LED_G_Pin|LED_P_Pin);

        // ④ 等待松手，防止一次长按触发多次
        while (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_0) == GPIO_PIN_SET) {
            HAL_Delay(5);
        }
    }
}
```

四步逻辑是经典"**阻塞式按键检测**"模板：

1. **消抖**：机械触点闭合瞬间会抖动几毫秒，延时 20ms 后再读
2. **二次确认**：排除尖峰毛刺误判
3. **状态翻转**：LED 获得记忆，亮 ↔ 灭切换
4. **等待松手**：等价于软件实现"边沿检测"——一次按压只产生一次触发，长按不连发

## 关键局限与演进方向

等待松手的 `while` 是**死等**：期间 CPU 完全被占用，无法响应其他任务。这正是后续例程要解决的问题：

- **03 中断方式**：按键接 EXTI，按下时异步触发，主循环解放
- **04 MultiButton**：状态机 + 周期扫描，支持单击/双击/长按等复合事件
