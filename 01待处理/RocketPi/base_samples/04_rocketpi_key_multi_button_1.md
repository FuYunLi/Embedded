---
status: todo
created: 2026-09-10
tags:
  - c/language
  - stm32/timer
  - rocketpi/base_samples
  - todo
references:
  - "[[04_rocketpi_key_multi_button]]"
---

# 04_rocketpi_key_multi_button_1

> 进阶补充：由 04 的心跳代码 `if (++s_button_tick_divider >= TICKS_INTERVAL)` 引出的两个问题。
> 前置笔记：[[04_rocketpi_key_multi_button]]

## 1. `++i` vs `i++`：不只是风格

**语义**：前置先自增再取值（新值参与运算）；后置先取旧值参与运算，再自增。

放到 04 的 `if` 里对比：

```c
if (++cnt >= 5)   // 先加 1 再比：cnt 依次是 1,2,3,4,5 → 第 5 次命中
if (cnt++ >= 5)   // 先比再加：cnt 依次是 0,1,2,3,4 → 第 6 次才命中
```

后置版本**多计一次**（0 也占一个 tick），周期从 5ms 变 6ms——不是风格问题，是实际的定时误差。这里用前置是正确性选择。

### volatile 下的提醒

若被计数的变量是 `volatile` 且被中断/主循环共享，`cnt++` 是读-改-写三步，前置后置都不原子。与 Button 结构体位域竞态同根源：单一上下文访问才有安全保证。

### 纠正流传说法："前置比后置快"

对 C++ 类对象（迭代器拷贝开销）成立；对 int 基本类型，编译器优化后机器码通常相同。MCU 上循环计数写 `i++` 没有性能损失。

## 2. 5ms 心跳的其他实现方式

| 方案 | 原理 | 优点 | 代价/注意 |
|---|---|---|---|
| SysTick 分频（04 现用） | 借 HAL 1ms tick 计数分频 | 零额外定时器；SysTick 本来就要跑 | ISR 里做活，回调要短；与 HAL_Delay 同源 |
| 独立硬件定时器（TIM6/7 等） | 专用 TIM 产生 5ms 中断，`HAL_TIM_PeriodElapsedCallback` 里调 `button_ticks()` | 周期独立精确，不受 HAL tick 影响 | 多占一个定时器外设；[[MultiButton状态机与按键逻辑分析]] 案例用 TIM6 |
| 主循环时间片轮询 | `while(1)` 里 `HAL_GetTick()` 判断距上次是否满 5ms | 不占中断资源；与主循环同上下文，无竞态，可省 volatile | 精度受主循环最长任务拖累；按键场景够用 |
| LPTIM / DWT->CYCCNT | 低功耗定时器 / 数据观察点计数器 | LPTIM 可在 STOP 模式维持心跳 | 配置复杂，5ms 粒度任务少用 |

选型直觉：

- 已有 SysTick 且 1ms 粒度可用 → 分频（04 的选择，零成本）
- 定时器外设富余、或需与系统 tick 无关的独立节拍 → 独立 TIM
- 裸机 Super Loop 且主循环无长阻塞 → 主循环轮询最干净
- 进低功耗还要响应按键 → LPTIM

## 3. ISR 上下文跑状态机的隐患

04 把 `button_ticks()` 放在 SysTick ISR，意味着每个按键回调（LED 翻转）也在 ISR 里执行。本例回调够快没问题；回调里一旦出现 printf / Flash 写等慢操作，必须把状态机推进挪回主循环——呼应 [[MultiButton状态机与按键逻辑分析]] 中"LONG_PRESS_HOLD 200Hz 回调必须轻"的结论。
