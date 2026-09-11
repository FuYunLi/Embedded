---
status: done
created: 2026-09-11
tags:
  - c/parsing
  - embedded/uart-protocol
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/08_rocketpi_uart_control_led/main.c"
---

# 08 函数协作全景：一次完整交互的全链路跟踪

> [[08_rocketpi_uart_control_led|主笔记]] 按模块讲了结构，[[08_rocketpi_uart_control_led_3|逐类精读]] 按类别讲了写法。这篇回答最后一个问题：**这些函数如何咬合在一起，把上电、敲一条命令、收到回复这三段经历完整跑通**。像看一部电影，不拆镜头，只跟剧情。

## 全景地图：把函数名钉在三个时间线上

先把 20+ 个函数按"什么时候被调用"钉在三张图上，孤立的函数名就有了坐标系：

**时间线一：上电时刻**（main 初始化段，只走一次）

```
main
 ├─ MX_GPIO_Init / MX_USART2_UART_Init     （CubeMX 生成，硬件就绪）
 ├─ console_print_examples                 ─→ console_send_string（×4 行欢迎语）
 ├─ console_print_gpio_map                 ─→ console_pin_index / console_port_name
 │                                            ─→ snprintf → console_send_string（逐灯发）
 └─ console_send_prompt                    ─→ console_send_string（发 "\r\n> "）
```

上电时刻做的事：**宣告身份（欢迎语）→ 自报家门（gpio_map，上位机自动发现硬件）→ 摆好姿势等命令（提示符）**。此时两个字符串原语（pin_index/port_name）唯一一次出场，使命是把配置表翻译成 PC 能读的 JSON。发送侧所有输出都经由 send_string 这个总闸门。

**时间线二：等待时刻**（main 主循环，大部分时间停在这里）

```
while (1) {
  HAL_UART_Receive(&huart2, &byte, 1, HAL_MAX_DELAY)   ← 程序在此沉睡，直到串口来字节
  console_handle_input_byte(byte)                       ← 醒来只干一小口，立刻回去睡
}
```

主循环只做两件事：**等一个字节、喂给行组装**。这就是"阻塞式交互"的含义：程序的整个生命周期被压缩成"等字节→处理一个字节→继续等"的循环，没有其他任务在跑。

**时间线三：一条命令的完整生命周期**（每收到 \n 触发一次）——下文展开

## 主剧情：一条命令从键盘到 LED 的完整旅程

以输入 `{"led":["B","G"],"state":[1,0]}` 为例，跟踪每个函数的出场顺序和数据变化。

### 第一幕：字节进入（handle_input_byte，被调用 32 次）

用户在终端敲下这行字，PC 串口助手逐字节发出。主循环每收到一个字节就调用一次 handle_input_byte。前 30 个字节走的是最简单的路径：

```
byte='{':  不是\r不是\n，缓冲未满 → g_rx_buffer[0]='{'，g_rx_length=1
byte='"':  → g_rx_buffer[1]='"'，g_rx_length=2
...（依此类推，纯追加）
```

两个特殊字节需要拦截：输入末尾的 `\r`（PC 发送 \r\n 两连发）直接被吞——**\r 没有语义，\n 才是命令结束信号**；如果中途缓冲满 127 字节，则清零 g_rx_length、报 "command too long"、重发提示符——**牺牲这一条，保住后续所有命令的可用性**。

### 第二幕：结算触发（第 32 个字节 \n 到达）

```
byte='\n' → console_process_command_buffer()   ← 整行已就位
           → g_rx_length = 0                    ← 缓冲复位，随时接下一条
           → console_send_prompt()              ← 先摆好提示符？不——见下
```

实际顺序是：process 先跑完（里面可能输出应答），然后才清缓冲、发提示符。**提示符是"我准备好了"的信号**，出现在应答之后，用户看到的光标永远停在下一条命令该出现的地方。

### 第三幕：解析（process 内部，解析引擎全员出场）

process_command_buffer 做的第一件事是 `g_rx_buffer[g_rx_length] = '\0'`——把字节缓冲变成合法 C 字符串（g_rx_length 被 handle_input_byte 保证不越过 SIZE-1，这里写 '\0' 不会越界，**两个函数共同维护同一条不变式，各管一半**）。

然后是两次解析调用，注意游标变量 value 的位置变化：

```
console_parse_led_targets(json, &command)
 └─ value = console_find_json_value(json, "\"led\"")
     │  strstr 找到位置 1 的 "led"，跳过键+冒号+空白
     │  value 现指向 → ["B","G"],"state":[1,0]}
     └─ *value=='[' → 进入数组循环：
         ├─ value=skip_spaces(value)                       位置不变（[后无空格）
         ├─ console_parse_led_token(&value, cmd) 第一次
         │    扫出 "B" → str_case_equal 配 ALL？否
         │    → find_led_by_token("B") → 查 g_led_config 命中下标 0
         │    → add_led_index：led_index[0]=0，led_count=1
         │    → *cursor 回写，value 现指 → ,"G"],"state":[1,0]}
         ├─ *value==',' → ++value 跳过逗号
         ├─ console_parse_led_token 第二次 → "G" → 下标 1，led_count=2
         └─ *value==']' → 跳过，break

console_parse_state_values(json, &command)   同样的数组循环
 └─ "1" → state_token 数字分支 → number=1 → states[0]=true，state_count=1
 └─ "0" → states[1]=false，state_count=2
```

看清楚这里的分工：**find_json_value 只负责"找到入口"，token 解析器负责"推进和录入"，add/find 负责查表，游标（value）是贯穿全场的那根线**。每个解析函数都在做同一件事的变体：读一字符判断形态 → 扫描到边界 → 翻译成数字 → 回写游标。

### 第四幕：语义关卡与执行

```
state_count(2) == led_count(2) ✓ 通过校验
console_apply_command(&command)
 └─ i=0: state_index = (state_count==1) ? 0 : 0 = 0   ← 一一对应分支
 │       console_set_led_state(0, true)
 │        → HAL_GPIO_WritePin(GPIOB, LED_B_Pin, RESET)   ← LED_B 亮（低电平点亮）
 └─ i=1: state_index = 1
         console_set_led_state(1, false)
          → HAL_GPIO_WritePin(..., SET)                    ← LED_G 灭
```

**这是程序开始运行以来第一次有函数碰 GPIO**。此前 33 次函数调用全部只动了内存和 UART——解析、校验、攒结构体。一旦碰硬件，就已是校验过的确定结果，不存在"点了 B 才发现 G 非法"的半执行状态。原子性不是靠锁实现的，是靠**执行时机放到最后**实现的。

### 第五幕：汇报（数字世界翻译回文本）

```
console_report_result(&command)
 └─ snprintf 链式拼装（buf 游标 pos 逐步推进）：
    {"status":"ok","led":["B","G"],"state":[1,0]}\r\n
 └─ console_send_string → HAL_UART_Transmit → PC
```

汇报函数把 LedCommand_t 翻译回 JSON，注意它回查 g_led_config 取短名 B——**配置表是双向字典**：解析时名字→下标，汇报时下标→名字。

## 依赖关系图：谁认识谁

把三幕里所有调用关系抽出来，得到整张代码的调用图（箭头=调用）：

```
main ──→ handle_input_byte ──→ process_command_buffer ──┬→ find_json_value
                                                        ├→ parse_led_token ──┬→ str_case_equal
                                                        │                    ├→ find_led_by_token ──→ g_led_config
                                                        │                    └→ add_led_index
                                                        ├→ parse_state_token（同构）
                                                        ├→ apply_command ──→ set_led_state ──→ HAL_GPIO_WritePin
                                                        └→ report_result ──→ snprintf ──→ send_string ──→ HAL_UART_Transmit
```

三条贯穿全图的规律：

1. **左边的函数永远不知道右边的存在**（handle_input_byte 不知道有解析这回事）——这保证任何一层可以单独替换：把 handle_input_byte 换成 07 的 DMA 回调，解析层零改动
2. **数据有两个形态、两条通道**：字符串形态只在"游标线"（find→token→value）上流动，数字形态只在"结构体线"（cmd）上流动，在 process 这一点交汇后彻底分手——**字符串通道进，结构体通道出，永不回流**
3. **硬件访问只有两个终点**：HAL_UART_Transmit（send_string 内）和 HAL_GPIO_WritePin（set_led_state 内）。全文件 1000 行，碰硬件的就这两处

## 数据视角：四个存储区在每个阶段的快照

| 时刻 | g_rx_buffer | g_rx_length | command | GPIO |
|---|---|---|---|---|
| 上电 | 全零 | 0 | 不存在 | 复位态 |
| 收到 30 字节后 | {"led":["B","G"],"state":[1,0] | 30 | 不存在 | 复位态 |
| \n 结算时 | 同上+隐式'\0' | 0（已清） | 不存在→ | 复位态 |
| 解析完成 | 原样（没人改它） | 0 | {下标[0,1], 状态[1,0], 2,2} | 复位态 |
| apply 后 | 原样 | 0 | 同上（只读） | B亮 G灭 |
| 汇报后 | 等待新数据 | 0 | 栈回收 | B亮 G灭 |

注意两个细节：**行缓冲在解析期间原样保留**（解析器只读它），**command 生存在栈上、函数返回即消失**——所以本方案天然可重入：连续来两条命令，各自有独立的 command，互不干扰。

## 最坏情况跟踪：一条会被拒绝的命令走多远

输入 `{"led":"X","state":1}`：

1. handle_input_byte 正常收完（28 字节）
2. process → parse_led_targets → find_json_value 找到 led 入口 → parse_led_token 扫出 "X"
3. find_led_by_token("X")：外层 3 颗灯 × 内层 3 别名 = 9 次 str_case_equal 全部失配 → 返回 -1
4. parse_led_token 返回 false → parse_led_targets 返回 false → **流程在第三幕中途退出**
5. process 捕获失败 → report_error 输出 {"status":"error",...} → send_prompt

注意 GPIO 全程未动、也没有"半个 command 被执行"——**失败路径的退出点在执行之前**，这就是把校验全部前置换来的安全。最坏情况（超长命令）在第一幕就被拦截，付出的代价只是清缓冲+一句报错。

## 收束：这个骨架能装多少东西

把本篇的调用图抽象成文字，就是任何命令交互系统的通用剧情：

```
等字节 → 攒行 → 定位字段 → 逐 token 推进翻译 → 查表转数字 → 校验语义 → 一次执行 → 结构化应答
```

11 的 shell（shell）在这条线上补行编辑和命令表；13 的雷达（雷达帧解析）把"定位字段"换成帧头同步、把 token 推进换成定长字段切片；AT 框架再把"执行"换成"转发给模组并等应答"。函数可以无限变多，**三幕结构（收→解→行）和两条数据通道（字符串进、结构体出）不变**。

## 关联笔记

- [[08_rocketpi_uart_control_led]] — 主笔记：逐模块结构精读
- [[08_rocketpi_uart_control_led_3|静态辅助函数逐类精读]] — 本文各函数的写法拆解
- [[08_rocketpi_uart_control_led_6|同类需求识别与设计迁移]] — 这个骨架还能复制到哪里
- [[07_rocketpi_uart_echo]] — 接收层的工程版（DMA），可替换第一幕
