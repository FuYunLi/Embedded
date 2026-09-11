---
status: done
created: 2026-09-11
tags:
  - stm32/uart
  - c/parsing
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/08_rocketpi_uart_control_led/main.c"
---

# 08_rocketpi_uart_control_led

> 代码：`Embedded_Code/01待处理/rocketpi/base_samples/08_rocketpi_uart_control_led/main.c`（单文件 20KB，42 例中第一个应用级程序）
> 硬件：STM32F401RE（RocketPi），USART2 115200，LED_B/G/P 三颗灯
> 目标：串口收伪 JSON 命令 → 解析 → 控制任意 LED 组合 → 回 JSON 状态

## 交互效果

```
> {"led":"B","state":1}                    ← 用户输入
{"status":"ok","led":["B"],"state":[1]}    ← 板子回
> {"led":["B","G"],"state":[1,0]}          ← 批量：B 亮 G 灭
> {"led":"ALL","state":0}                  ← 全灭
> {"led":"B","state":"on"}                 ← 状态支持文本
> {"led":"X","state":1}                    ← 未知灯名
{"status":"error","msg":"missing led field"}
```

完整的"控制台命令交互"雏形——为什么要做这种交互、shell/AT 终端谱系见 [[08_rocketpi_uart_control_led_1|串口交互终端的意义]]；11 的 shell 是它的成熟形态，09 用 cJSON 库重写同款解析形成手写 vs 库的对照。

## 架构主线：三层流水线

```
main 循环逐字节收            console_handle_input_byte         console_process_command_buffer
─────────────────           ──────────────────────────        ─────────────────────────────
HAL_UART_Receive(1字节)  →   拼 \r\n 行缓冲 / 防溢出    →      解析 → 执行 → 汇报
```

三个阶段每层只干一件事：**接收、组装、解析执行**。所有命令行交互程序（shell、AT 框架）都是这个骨架的放大版。

## 逐模块精读

### 1. 配置表驱动：命令到硬件的映射收敛成一张表

```c
typedef struct
{
  GPIO_TypeDef *port;
  uint16_t pin;
  const char *tokens[LED_TOKEN_COUNT];   // 三个别名
} LedConfig_t;

static const LedConfig_t g_led_config[] =
{
  {LED_B_GPIO_Port, LED_B_Pin, {"B", "BLUE", "LED_B"}},
  {LED_G_GPIO_Port, LED_G_Pin, {"G", "GREEN", "LED_G"}},
  {LED_P_GPIO_Port, LED_P_Pin, {"P", "PINK", "LED_P"}}
};

enum { LED_TOTAL = sizeof(g_led_config) / sizeof(g_led_config[0]) };
```

**"数据进表、逻辑进函数"是消 if-else 的核心手段**。加一个新 LED = 表里加一行，查找/执行/汇报代码零修改。对照：如果用 switch-case 写死，每加一个 LED 要改五六处。

- `tokens[3]`：每个灯三个别名，用户输 `B`/`blue`/`LED_B` 都认——别名表比"规范化输入"友好得多
- `enum { LED_TOTAL = sizeof/sizeof }`：C 里"数组元素个数自动推导"的标准技巧，编译期常量，表加行数字自动变
- 查找函数 `console_find_led_by_token` 双层循环（先灯后别名）+ `console_str_case_equal` 大小写不敏感比较（逐字符 toupper 后比，比 `strcasecmp` 可移植性好——后者不是标准 C）

和 [[04_rocketpi_key_multi_button]] 的 `cb[]` 回调数组同思想：**表驱动设计**（table-driven）的最小样板。

### 2. 逐字节接收 + 行缓冲（注意：故意退回了朴素方案）

```c
while (1)
{
  uint8_t byte = 0U;
  if (HAL_UART_Receive(&huart2, &byte, 1U, HAL_MAX_DELAY) == HAL_OK)
  {
    console_handle_input_byte(byte);
  }
}
```

07 刚学了 DMA+Idle，08 反而用**阻塞逐字节接收**——这不是退步，是取舍：命令交互是"人敲键盘"节奏，逐字节阻塞完全够用，还省掉 DMA 配置。但代价清楚可见：`HAL_MAX_DELAY` 死等下一个字节，**主循环被串口独占**，这程序没法并行干别的。07 是工程版（资源高效），08 是教学版（结构清晰），同一问题的两端。

`console_handle_input_byte` 是行编辑的教科书实现，三个防御一个不少：

```c
static void console_handle_input_byte(uint8_t data)
{
  if (data == '\r') return;        // ① 吞掉 \r：PC 发 \r\n 两个字符，\r 无语义
  if (data == '\n')                // ② \n 触发整行结算
  {
    console_process_command_buffer();
    g_rx_length = 0U;
    console_send_prompt();
    return;
  }
  if (g_rx_length >= (UART_RX_BUFFER_SIZE - 1U))   // ③ 超长：清零重来
  {
    g_rx_length = 0U;
    console_report_error("command too long");
    console_send_prompt();
    return;
  }
  g_rx_buffer[g_rx_length++] = (char)data;
}
```

- `-1` 留的那格给结尾 `'\0'`（结算时 `g_rx_buffer[g_rx_length] = '\0'`）
- **超长命令清零重来而不是丢弃字节死等**——用户粘贴超长内容后缓冲复位，下一条命令还能正常进，程序永远有响应

### 3. 伪 JSON 解析：二级指针游标 + token 解析器组合

命令是伪 JSON（`{"led":["B","G"],"state":[1,0]}`），解析**没引库**（09 才用 cJSON），手写了一套指针游标解析器。三个核心函数：

```c
console_find_json_value(json, "\"led\"")   // strstr 找键 → 跳过冒号 → 返回值起点
console_parse_led_token(&value, cmd)       // 解析一个 "X" 字符串 token
console_parse_state_token(&value, cmd)     // 解析一个状态值（数字/文本）
```

**`const char **cursor` 二级指针是精髓**：解析函数内部消耗了若干字符，要把"新位置"传回给调用者——改的是调用者的指针变量本身，所以传指针的指针：

```c
static bool console_parse_led_token(const char **cursor, LedCommand_t *cmd)
{
  const char *ptr = *cursor;      // 从游标取当前位置
  ...
  *cursor = ptr;                  // 解析完把新位置写回
  return true;
}
```

内存语义：cursor 是"调用者那个指针变量的地址"，`*cursor` 解引用拿到/改写指针的值。所有 token 解析器统一签名 `(const char **cursor, LedCommand_t *cmd) → bool`，**同构签名让它们能自由组合**——数组场景循环调用、单值场景直接调用，`parse_led_targets`/`parse_state_values` 里 `if (*value == '[')` 分流的就是这两种情况。

**键查找为什么带引号搜**：`strstr(json, "\"led\"")` 搜的是 `"led"` 五个字符（含双引号）——防止匹配到值里的纯文本 `led`（比如 `{"name":"led"}` 会误匹配），引号把"这是键"锚死了。手写解析器里这种**利用格式特征锚定**的细节，比查表更省事。

**手写整数解析**（state_token 的数字分支）：

```c
uint32_t number = 0U;
while (isdigit((unsigned char)*ptr))
{
  number = (number * 10U) + (uint32_t)(*ptr - '0');   // '5' - '0' = 5
  ++ptr;
}
```

`'5' - '0'` 得数值 5——ASCII 数字 0x30~0x39 连续排列的标准化红利。逐位"乘 10 加新位"就是 atoi 的内核。负号分支单独处理（`-1` → value=false），所以 `state:-1` 也能语义化成"关"。

### 4. 解析与执行严格分离：LedCommand_t 是中间表示

```c
LedCommand_t command = {0};                    // ① 解析阶段：只填这个结构体
if (!console_parse_led_targets(g_rx_buffer, &command)) { 报错; return; }
if (!console_parse_state_values(g_rx_buffer, &command)) { 报错; return; }
if (!((command.state_count == 1U) || (command.state_count == command.led_count)))
{ 报错; return; }                              // 数量校验
console_apply_command(&command);               // ② 执行阶段：纯读结构体
console_report_result(&command);               // ③ 汇报阶段
```

```c
typedef struct
{
  uint8_t led_index[LED_TOTAL];   // 解析出的 LED 下标（去重后）
  bool states[LED_TOTAL];         // 对应状态
  size_t led_count;
  size_t state_count;
} LedCommand_t;
```

`LedCommand_t` 是**中间表示**（IR）：解析函数绝对不碰 GPIO，执行函数绝对不看字符串。三重好处：

- **原子性**：解析/校验全过才执行，不会半开半关
- **可换协议**：改成 AT 命令风格只重写解析层
- **可测试**：解析层可以脱离硬件单独测

`apply` 的广播逻辑一行巧思：

```c
const size_t state_index = (cmd->state_count == 1U) ? 0U : i;
```

一个 state 广播给所有 LED、多个 state 一一对应，两种语义用同一个循环兼容。

`console_add_led_index` 的去重：`{"led":["B","B"],"state":[1,0]}` 这种自相矛盾输入，重复索引直接跳过，最后 B 只有一个状态——防御性解析的细节。

### 5. 板级差异隔离：LED_ACTIVE_LOW 编译期开关

```c
#define LED_ACTIVE_LOW      1
#if (LED_ACTIVE_LOW == 1)
#define LED_ON_STATE        GPIO_PIN_RESET
#define LED_OFF_STATE       GPIO_PIN_SET
#else
#define LED_ON_STATE        GPIO_PIN_SET
#define LED_OFF_STATE       GPIO_PIN_RESET
#endif
```

低电平点亮（02 解析过的板级事实：RESET 亮）做成宏开关，换板子改一行重编译，逻辑代码只认 `LED_ON/OFF_STATE` 抽象。**板级差异在宏层解决，不渗进函数**。

### 6. JSON 回复生成：06 学的 snprintf 游标套路实战

```c
int length = snprintf(buffer, sizeof(buffer), "{\"status\":\"ok\",\"led\":[");
for (size_t i = 0U; i < cmd->led_count; ++i)
{
  const char *delimiter = (i + 1U < cmd->led_count) ? "," : "";
  length += snprintf(&buffer[length], (size_t)(sizeof(buffer) - (size_t)length),
                     "\"%s\"%s", g_led_config[cmd->led_index[i]].tokens[0], delimiter);
}
length += snprintf(&buffer[length], ..., "],\"state\":[");
...
```

[[06_rocketpi_uart_printf_3|06_3]] 的 `line+pos / sizeof-pos / pos+=` 三件套原样应用：动态数组 JSON、逗号只在中间元素后（`(i+1U<count) ? "," : ""`）、每次拼接带剩余空间限制。另外 `console_print_gpio_map` 上电打印 LED→GPIO 映射（`console_port_name` 把 GPIO_TypeDef* 翻译成 "GPIOA" 字符串、`console_pin_index` 把位图还原成编号 0~15），让 PC 端能自动发现硬件配置——上位机交互的常规设计。

## 与前例连线

- **06**：发送侧（console_send_string 是简化版 uart_puts）+ snprintf 套路实战
- **07**：接收侧对照——07 DMA 高效但复杂，08 阻塞朴素但主循环被独占。把 08 改造成 07 的 DMA+Idle 版是绝佳练习
- **04**：表驱动思想同源（g_led_config ↔ cb[] 回调数组）
- **09**：cJSON 库版解析即将登场——手写 vs 库的对照实验，看完能体会"什么该自己写、什么该交给库"
- **11**：shell 是这套"接收→解析→执行→汇报"的成熟形态（microrl 库 + 命令表）

## 值得记住的三个设计词

- **表驱动**：数据进 const 表、逻辑进函数，扩展加行不改码
- **中间表示**：解析与执行之间放一个纯数据结构体，两侧解耦
- **游标解析**：`const char **cursor` 统一签名的 token 解析器，自由组合

这套词汇量在 13（雷达帧解析）、11（shell）里会反复出现。

## 关联笔记

- [[08_rocketpi_uart_control_led_1|串口交互终端的意义]] — 为什么做交互终端、shell/AT/NMEA 谱系
- [[08_rocketpi_uart_control_led_2|数据结构与 API 设计]] — 从需求反推每个结构体/函数边界的存在理由
- [[08_rocketpi_uart_control_led_3|静态辅助函数逐类精读]] — 四类函数逐个拆到可复刻
- [[08_rocketpi_uart_control_led_4|串口文本协议谱系]] — 08 在协议学习路线上的定位，衔接 10/11/13
- [[06_rocketpi_uart_printf]] — 发送侧基建与 snprintf 游标套路出处
- [[06_rocketpi_uart_printf_3|hexdump 详解]] — snprintf 链式拼接的原理详解
- [[06_rocketpi_uart_printf_4|C 标准库补课]] — strstr/isdigit/isalpha/toupper 本例全用上了
- [[04_rocketpi_key_multi_button]] — 表驱动思想同源
- [[07_rocketpi_uart_echo]] — 接收侧的工程版方案对照
