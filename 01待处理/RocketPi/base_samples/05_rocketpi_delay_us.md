---
status: todo
created: 2026-09-10
tags:
  - stm32/timer
  - rocketpi/base_samples
  - todo
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/05_rocketpi_delay_us/main.c"
---

# 05_rocketpi_delay_us

> 代码：`Embedded_Code/01待处理/rocketpi/base_samples/05_rocketpi_delay_us/main.c`（tim.c/tim.h 原仓库补读）
> 硬件：STM32F401RE（RocketPi），TIM11 挂 APB2 84MHz，PC10 输出翻转波形

## 问题背景：HAL_Delay 为什么不够用

`HAL_Delay` 最小单位 1ms。但很多外设要**微秒级**时序：WS2812B 位时序 ±150ns、DHT11 应答拉低 20~40µs、软件模拟 SPI/I2C。这些用 HAL_Delay 做不了，05 用 TIM11 计数器解决。

## 方案：定时器当秒表，不产生中断

TIM11 配置（原仓库 tim.c）：

```c
htim11.Init.Prescaler = 84-1;      // 84MHz ÷ 84 = 1MHz → 1 tick = 1µs
htim11.Init.Period   = 65535;      // 16 位计满自动回绕
```

main 里 `HAL_TIM_Base_Start(&htim11)` 启动，纯计数不开中断。**CNT 寄存器就是一个持续运行的微秒表。**

## 微秒表 + 16 位回绕减法

```c
uint16_t micros16(void)
{
    return __HAL_TIM_GET_COUNTER(&htim11);   // 直接读 CNT 寄存器
}

void delay_us(uint16_t us)
{
    uint16_t start = micros16();
    while ((uint16_t)(micros16() - start) < us)   // 无符号回绕减法
        ;
}
```

三个关键点：

**1. 回绕减法是灵魂。** CNT 从 65535 跳回 0 时减法不出错：无符号减法天然按模 2¹⁶ 运算。start=65530、CNT=3 时，`3 - 65530` 在 uint16_t 下 = `65536 - 65527 = 9`——正好是流逝微秒数。前提：单次 `us ≤ 65535`（上限约 65ms），成立时数学严格正确。

**2. `(uint16_t)` 强转不是装饰。** 两个 uint16_t 相减会被整型提升为 int，结果是负数或大正数，必须转回 uint16_t 才能利用模回绕。漏掉这步，回绕时刻会死循环。

**3. 忙等但极准。** 对比 SysTick 时间戳方案（03 消抖用的，ms 粒度）：SysTick 受中断响应和 HAL tick 校准影响，误差 µs~10µs 级；TIM11 直接数时钟，分辨率 1µs、误差 <1µs，读的是硬件计数器，不受软件延迟影响。

## 测试方法：寄存器直写的必要性

```c
while (1) {
    GPIOC->ODR ^= (1 << 10);   // 直写 ODR 翻转 PC10（注释里留了 HAL 版对比）
    delay_us(1);
}
```

- `HAL_GPIO_TogglePin`：参数检查 + BSRR，多层调用十几条指令
- `GPIOC->ODR ^= (1<<10)`：读-改-写，三条指令左右

1µs 间隔下，**函数调用开销本身就是延时误差的大头**——HAL 版翻转一次可能 1~2µs，实际波形就不是 1µs 了。测 µs 级延时必须寄存器直写，这是例程刻意展示的对比：当延时精度和调用开销同量级时，HAL 抽象的成本就显形了。

（注意：ODR 读-改-写在 ISR 同时改同端口其他脚时有竞态，标准做法写 BSRR；本例单脚翻转无碍。）

## 与 03 的呼应

同一思想——"读硬件计数器算流逝时间"——两个粒度两种用途：

- 03 SysTick 时间戳：ms 粒度，消抖判断
- 05 TIM11 秒表：µs 粒度，精确阻塞延时

SysTick 是系统级资源（HAL_Delay 靠它），TIM11 是专职微秒秒表的专用外设。

## 遗留思考

单次 delay_us 上限 65535µs，代码没做参数检查——传 70000 会怎样？（70000 > 65535，回绕后永远凑不满等待条件或提前退出，需要拆分多次调用或换 32 位定时器如 TIM2/TIM5。）
