---
status: todo
created: 2026-09-09
tags:
  - stm32/gpio
  - rocketpi/base_samples
  - embedded/fsm
  - todo
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/04_rocketpi_key_multi_button/main.c"
  - "[[MultiButton状态机与按键逻辑分析]]"
---

# 04_rocketpi_key_multi_button

> 代码：`Embedded_Code/01待处理/rocketpi/base_samples/04_rocketpi_key_multi_button/`（main.c、multi_button.c）
> 硬件：STM32F401RE（RocketPi），按键 + LED_B/G/P；库源码 multi_button.c，头文件宏定义见原仓库 `component/MultiButton/multi_button.h`

## 实验目标

把按键判断外包给 MultiButton 库，用状态机把时间维度（按多久、按几次）纳入识别，一个按键输出单击/双击/长按三种语义事件，分别翻转三颗 LED。

## 关键点 1：心跳从哪来

本目录只有 main.c + multi_button.c，库里没有任何后台任务，必须有人周期性调 `button_ticks()`。原仓库 `stm32f4xx_it.c` 里藏在 SysTick 中断：

```c
void SysTick_Handler(void)
{
  HAL_IncTick();
  static uint32_t s_button_tick_divider = 0U;
  if (++s_button_tick_divider >= TICKS_INTERVAL) {  // TICKS_INTERVAL = 5
    s_button_tick_divider = 0U;
    button_ticks();        // 每 5ms 推进一次所有按键状态机
  }
}
```

1ms 的 HAL SysTick 分频 5 次 → **5ms 心跳**。这是库的生命线，没有它状态机永远停在 IDLE。

### TICKS_INTERVAL 的一个坑

`TICKS_INTERVAL = 5` 同时承担两个角色：

- 约定 `button_ticks()` 的调用周期（5ms）
- 换算阈值：`SHORT_TICKS = 300/5 = 60`、`LONG_TICKS = 1000/5 = 200`（心跳数）

移植时若直接在 1ms 定时器里裸调 `button_ticks()` 而不改它，所有阈值缩水 5 倍（长按 200ms 就触发）。

## 关键点 2：main.c 的使用范式

```c
button_init(&g_user_button, ButtonPinLevel, USER_BUTTON_ACTIVE_LEVEL, 0);
button_attach(&g_user_button, BTN_SINGLE_CLICK, ButtonSingleClickHandler);
button_attach(&g_user_button, BTN_DOUBLE_CLICK, ButtonDoubleClickHandler);
button_attach(&g_user_button, BTN_LONG_PRESS_START, ButtonLongPressHandler);
button_start(&g_user_button);
```

- `ButtonPinLevel`：库不碰硬件，读电平是注入的函数指针，内部只包一句 `HAL_GPIO_ReadPin`
- `USER_BUTTON_ACTIVE_LEVEL = GPIO_PIN_SET`：高电平有效
- 单击→LED_B，双击→LED_G，长按→LED_P；`while(1)` 空转，一切在回调里发生

## 关键点 3：button_ticks() 每个 tick 做什么

```
button_ticks() → 遍历链表 head_handle → 每个按键跑一遍 button_handler()
                                          ├─ ① 读 GPIO（调注入的回调）
                                          ├─ ② ticks++（非 IDLE 状态才计）
                                          ├─ ③ 消抖：连续 3 tick(15ms) 一致才采信新电平
                                          └─ ④ 状态机 switch 推进 → 触发 EVENT_CB
```

5 状态流转：

```
IDLE --按下--> PRESS --松手--> RELEASE --超时300ms--> 结算回 IDLE
                |                ^          |
                |                +--又按下--+
                |                    → REPEAT（连击，repeat++）
                +--ticks>200(1s)--> LONG_HOLD（长按，持续派发 HOLD）
```

本例体验到的现象：

| 操作 | 事件链 | LED |
|---|---|---|
| 单击 | PRESS_DOWN → PRESS_UP → SINGLE_CLICK（等 300ms 才来） | B 翻转 |
| 双击 | PRESS_DOWN → PRESS_UP → PRESS_REPEAT → PRESS_UP → DOUBLE_CLICK | G 翻转 |
| 长按 1s | PRESS_DOWN → LONG_PRESS_START | P 翻转 |

消抖、逐状态时序、REPEAT 转 PRESS 的 ticks/repeat 清零修复等细节，见 [[MultiButton状态机与按键逻辑分析]]。

## 关键点 4：Button 结构体的位域

```c
struct _Button {
    uint16_t ticks;
    uint8_t  repeat       : 4;   // 0~15，与 PRESS_REPEAT_MAX_NUM 绑定
    uint8_t  event        : 4;   // 0~15，事件枚举上限
    uint8_t  state        : 3;   // 0~7，5 个状态够用
    uint8_t  debounce_cnt : 3;   // 0~7，故 DEBOUNCE_TICKS 注释 MAX 7
    uint8_t  active_level : 1;
    uint8_t  button_level : 1;
    ...
};
```

6 个小字段若各占 1 字节要 6 字节，位域打包后 4+4、3+3+1+1 各凑满 1 字节，**共 2 字节**。

位域三个代价：

1. **上限即溢出点**：`state: 3` 意味着枚举最大 7。debounce_cnt 是 3 bit，DEBOUNCE_TICKS 配成 8 就永远数不满，消抖失效——字段范围和业务上限绑死，加枚举值前要数 bit
2. **布局不可移植**：标准不规定 bit 在字节内的排列，ARM 和 x86 可能排出不同布局。本地 RAM 变量没问题，但不能 memcpy 整个结构体做通信/存储
3. **非原子访问**：写 4 bit 位域实际是"读字节→改 bit→写字节"三步。本例 repeat 和 event 同处一字节，若主循环和 SysTick 同时各改一个，会丢失更新。MultiButton 状态推进全部在 `button_ticks()`（SysTick）单一上下文里，所以安全；自己扩展时别在主循环里直接改这些字段

## 02→03→04 演进闭环

- 02 轮询：CPU 死等，只有"按/没按"
- 03 中断：CPU 解放，标志位只记得"发生过一次"
- 04 状态机：一个 5ms 心跳 + 一个 Button 结构体，把"多久、几次"纳入判断，白嫖出单击/双击/长按/连击语义——代价是单击响应固定延迟 300ms（等双击窗口结算）
