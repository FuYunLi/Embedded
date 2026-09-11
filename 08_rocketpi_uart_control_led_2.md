---
status: done
created: 2026-09-11
tags:
  - c/api-design
  - c/data-structure
  - embedded/uart-protocol
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/08_rocketpi_uart_control_led/main.c"
---

# 08 数据结构与 API 设计：从零规划一套命令交互

> [[08_rocketpi_uart_control_led|08 主笔记]] 按代码顺序精读了模块；这篇反着来——**假装代码还没写，只看需求，推演每个数据结构和 API 为什么长这样**。目标是让你独立设计第三套同类方案（第一套 08 手写、第二套 09 cJSON）。

## 需求推演：先问"信息从哪来到哪去"

需求只有一句话：串口收命令，控制三颗灯，回报结果。动手前先推信息流：

```
PC 人脑里的意图（"把蓝灯打开"）
   → 文本命令 {"led":"B","state":1}        （编码：人可见格式）
   → UART 字节流                             （传输）
   → MCU 内部表示                            （解析目标）
   → GPIO 电平                               （执行）
   → 文本应答 → UART → PC                    （回报）
```

设计全部围绕两个问题展开：
1. **"MCU 内部表示"长什么样**（数据结构问题）
2. **每个环节的函数边界怎么划**（API 问题）

## 核心数据结构一：命令的中间表示 `LedCommand_t`

### 为什么必须有这个结构

最直接的写法是解析函数里直接点灯：收到 `B` 就 `HAL_GPIO_WritePin`。但马上遇到麻烦：`{"led":["B","G"],"state":[1,0]}` 必须解析完**两条** LED 和**两条** state 之后才知道全貌——第一条 B 亮了，第二条才发现 state 数对不上怎么办？灯已经亮了，收不回来。

所以规则是：**解析阶段一个引脚都不碰，把所有信息攒进一个结构；攒齐、校验通过，再一次性执行**。这就是中间表示（Intermediate Representation）。

### 字段设计的逐项推演

```c
typedef struct
{
  uint8_t led_index[LED_TOTAL];   // 为什么不是 char 名字？
  bool states[LED_TOTAL];         // 为什么是 bool？
  size_t led_count;               // 为什么不用 0 作结束标记？
  size_t state_count;
} LedCommand_t;
```

**led_index 存下标（uint8_t）而不是名字（char*）**：名字只在解析时有用，一旦定位到配置表第几行，名字就完成使命了。存下标有三个好处：执行时 O(1) 回查配置表、避开悬垂指针风险（名字若指向行缓冲，结算后就被清了）、占 1 字节。这步"从字符串世界转入数字世界"的转换，正是解析层的全部意义。

**数组容量 = LED_TOTAL**：一条命令最多操作所有灯，3 颗灯 3 个容量，数学上界，永远不会溢出——`ALL` 关键字展开后就是 LED_TOTAL 个。

**count 与数组分离**：C 数组没有自带长度。可选方案是用 0xFF 哨兵标记结尾，但那样判断"有没有 LED"要扫描数组，而 `led_count > 0` 一个比较就行。**显式计数是嵌入式结构体的惯例**，也和 `size_t` 返回长度的 libc 风格一致。

**states 用 bool**：灯的状态本来就只有两态。有人会想存 uint8_t 方便以后扩 PWM 亮度——不要。YAGNI 原则：需求变了再改，改的成本远低于现在为假想需求支付的复杂度。

### 校验规则也属于数据结构设计

```c
(command.state_count == 1U) || (command.state_count == command.led_count)
```

这条"state 数 = 1（广播）或 = led 数（一一对应），其他都拒绝"的规则写在执行前，而不是散在解析里——**把业务约束集中在一个关卡**，解析器保持纯粹（只管语法），这里管语义。语法/语义分层是所有解析器（编译器、shell、AT 框架）的通用分法。

## 核心数据结构二：配置表 `LedConfig_t`

```c
typedef struct
{
  GPIO_TypeDef *port;          // 为什么存指针不存 GPIOA 宏？
  uint16_t pin;                // 为什么是位图不是编号？
  const char *tokens[LED_TOKEN_COUNT];  // 为什么三个别名？
} LedConfig_t;
```

**port 存 `GPIO_TypeDef *`**：`HAL_GPIO_WritePin` 的第一个参数就是这种指针类型。CubeMX 生成的 `LED_B_GPIO_Port` 宏展开后就是 `GPIOB`（指针常量），存进表里执行时零转换。如果存 "GPIOB" 字符串，执行时还得字符串→指针映射表，多一层多余查表。

**pin 存位图（`GPIO_PIN_5` 即 1<<5）**：HAL 的接口约定如此。这就是为什么打印调试信息时需要 `console_pin_index` 反推编号——位图是机器视角，编号是人视角，两个视角的转换函数值一个，因为**存什么格式由下游 API 决定，不为好看**。

**tokens 三别名**：`B`（快）、`BLUE`（可读）、`LED_B`（与原理图丝印一致）。这是"用户不读你的文档"设计观：宁可查找函数多循环几次，不要求用户记住唯一拼写。别名机制还让硬件改名时（比如换板后 B 变成 D）老命令不失效。

**static const**：const 把表放进 Flash 不占 RAM；static 限定本文件——这张表是解析层的私有数据，外部不许直接碰（想操作灯走 apply 函数），**数据隐藏保证"执行逻辑只经由中间表示"这条规则无法被绕过**。

## API 设计：函数怎么切、签名怎么定

数一遍 08 的静态函数，按职责归类后其实就六种角色。设计任何命令交互系统时，可以按这六种角色规划自己的函数清单：

| 角色 | 08 中的函数 | 判定标准 |
|---|---|---|
| 传输 | send_string/send_prompt/print_examples | 只搬字节，不懂业务 |
| 行组装 | handle_input_byte/process_command_buffer | 只认 \r\n，不懂命令语义 |
| 解析 | find_json_value/parse_led_token/parse_state_token/parse_led_targets/parse_state_values | 只产 LedCommand_t，不碰硬件 |
| 语义校验 | process_command_buffer 里的 count 检查 | 只查 LedCommand_t 内部一致性 |
| 执行 | apply_command/set_led_state | 只读 LedCommand_t，不看字符串 |
| 汇报 | report_result/report_error/print_gpio_map | 只把 LedCommand_t 变回文本 |

### 切分原则一：依赖单向流动

```
传输 → 行组装 → 解析 → 校验 → 执行 → 汇报
```

每个函数只调用它右边的角色。检查你的设计是否合格，就看**有没有逆向依赖**：解析函数里发错误消息（解析→传输）就是逆向，错误信息应该塞进中间表示或错误参数，由最外层统一汇报。08 用 `const char **error_msg` 出参收集错误原因、process 函数统一上报，就是这个用意（09 版更明显）。

### 切分原则二：粒度以"可组合"为准

`parse_led_token`（解析单个 LED 名）被两个场景复用：单值 `"led":"B"` 直接调一次，数组场景循环调。如果当初把单值/数组合并写成一个 200 行大函数，数组解析就要复制粘贴。**粒度切在"调用方需要以不同方式组合你"的地方**——这是函数拆分不是拍脑袋，是看调用图决定的。

### 切分原则三：状态尽量收敛

整个系统只有两个全局状态：`g_rx_buffer + g_rx_length`（行组装层）和 `g_led_config`（只读配置）。命令处理过程中的其他一切数据都在 `LedCommand_t command` 局部变量里，函数栈帧一回收就没了。**全局状态越少，重入/并发问题越少**——以后把这套代码放进 RTOS 多任务环境，只有这两个全局需要加锁，其余天然线程安全。

## 错误处理设计：三级信息传递

命令交互的错误报告要回答三个问题：语法错了吗、错在哪、用户怎么改。08 的方案：

1. **解析函数内部**：return false 就够——细节调用方不关心
2. **跨函数传递**：错误原因字符串（"unknown led"）——08 手写版靠返回值隐式约定，09 版的 `const char **error_msg` 出参是显式正解
3. **面向用户**：`{"status":"error","msg":"..."}` 结构化应答——机器可读，PC 端脚本也能解析

对比裸 printf("error!") 的差距：结构化应答让上位机可以自动化测试（发命令→等 JSON→判断 status 字段）。**从第一天就把应答设计成机器可读的**，是 08 比"能跑就行"的代码高一档的地方。

## 自检清单：设计同类系统时的十问

1. 我的"中间表示"是什么结构？解析层是否完全不碰硬件？
2. 执行层是否完全不看输入字符串？
3. 配置表是否 static const？加一个新设备要改几处代码？
4. 行缓冲满了怎么办？（清零重来 vs 丢弃字节，前者保证后续命令可用）
5. `\r` `\n` 分开处理了吗？（PC 端发的是两个字符）
6. 数量类校验（state vs led）集中在执行前一处了吗？
7. 错误应答机器可读吗？
8. 全局可变状态有几个？能否再减？
9. 解析函数签名统一了吗？能否数组/单值两用？
10. 板级差异（如低电平点亮）是否隔离在宏/配置层？

这十条就是 08 全部代码的骨架。答得上这十问，09 的 cJSON 版在你眼里就只是"第 3 条换了个实现"。

## 关联笔记

- [[08_rocketpi_uart_control_led]] — 主笔记：逐模块精读与三层流水线
- [[08_rocketpi_uart_control_led_1|串口交互终端的意义]] — 为什么要做这套交互
- [[08_rocketpi_uart_control_led_3|静态辅助函数逐类精读]] — 六类函数逐行拆解
- [[09_rocketpi_uart_control_led_cjson]] — 解析层换库后的同构实现
