---
status: done
created: 2026-09-11
tags:
  - c/parsing
  - c/functions
  - embedded/uart-protocol
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/08_rocketpi_uart_control_led/main.c"
---

# 08 静态辅助函数逐类精读

> 08 共 21 个 static 函数，分四类，每类内部高度同构。本文逐类逐个拆解到"能独立复刻"的程度。函数命名前缀 `console_` 是模块前缀（static + 前缀 = 小项目的土法命名空间）。函数之间如何协作完成一次完整交互，见 [[08_rocketpi_uart_control_led_5|函数协作全景]]。

每个函数在代码块之前给出：（1）形参含义；（2）在当前例程中谁调用它、输入什么、输出什么；（3）简明的客观描述。代码块之后给出逐行细节。

## 第一类：I/O 网关与行组装（4 个）

**特征：处在系统的边界上——对外碰硬件 UART，对内提供纯文本/纯字节接口，是唯一允许知道 huart2 存在的函数群。**

### console_send_string

```c
static void console_send_string(const char *msg);
```

**作用**：把一个以 `\0` 结尾的 C 字符串通过 USART2 阻塞发出去，发完才返回。全文件所有对外输出（欢迎语、提示符、错误 JSON、成功 JSON）最终都经过这个函数，没有例外。

**形参**：`msg` — 以 `\0` 结尾的字符串，可为 NULL（NULL 时静默返回不发任何内容）。

**输入/输出**：输入 = 一个 C 字符串；输出 = 该字符串的字节出现在 USART2 的 TX 引脚上（PC 串口助手收到）。

**调用者**：`console_send_prompt`、`console_print_examples`、`console_print_gpio_map`、`console_report_result`、`console_report_error`——即全文件所有输出场景。

```c
static void console_send_string(const char *msg)
{
  if (msg == NULL)          // ① 防御：NULL 字符串
  {
    return;
  }
  HAL_UART_Transmit(&huart2, (uint8_t *)msg, strlen(msg), HAL_MAX_DELAY);
}
```

- **`(uint8_t *)msg` 强转原理**：HAL 的接口设计成"字节流"视角，const char* 是"文本"视角。C 字符串底层都是字节，强转只是让编译器闭嘴，无运行时代价。注意 HAL 不改缓冲所以丢掉 const 是安全的，但这是「知道 HAL 不写」才有的豁免——一般规则是永不强转掉 const
- **strlen(msg) 每次重算**：缓冲里没有 \0 就会读到越界。这是这个函数的隐藏前提：**msg 必须是合法 C 字符串**。发送已知长度的二进制数据时不能用它（要用带 len 参数的接口）
- **HAL_MAX_DELAY 无限等**：命令交互是人机节奏，阻塞无所谓；但若这是协议引擎的发送函数，超时应设有限值。**同一个函数体，换个场景超时语义就变**

**设计意图**：全文件只有这一个函数知道 huart2 怎么用。以后换 UART3、换 DMA 发送、换 RTOS 队列，只改这一个函数，调用者零感知。

### console_send_prompt

```c
static void console_send_prompt(void);
```

**作用**：向串口输出 `\r\n> `，在 PC 终端上显示为换行后跟一个 `>` 提示符，告诉用户"现在可以输入下一条命令了"。

**形参**：无。

**输入/输出**：输入 = 无；输出 = 4 个字节（`\r` `\n` `>` 空格）出现在 TX 引脚。

**调用者**：`console_process_command_buffer` 结束后（不管成功还是报错）、`console_handle_input_byte` 超长错误后——即每条命令处理完、程序回到等待状态时。

```c
static void console_send_prompt(void)
{
  console_send_string("\r\n> ");
}
```

一行函数也值得独立存在：抽成函数后，未来加时间戳（`[12:33]> `）、加 ANSI 颜色只改一处。\r\n 双字符的原因见 [[08_rocketpi_uart_control_led_1|交互终端笔记]]。

### console_print_examples

```c
static void console_print_examples(void);
```

**作用**：上电时打印 4 行欢迎语和示例命令，让用户打开串口助手就知道板子在等什么格式的输入。

**形参**：无。

**输入/输出**：输入 = 无；输出 = 4 行文本出现在 TX（`RocketPi UART LED console ready.` + `Example commands:` + 两行示例 JSON）。

**调用者**：`main`，上电时只调一次（在 MX_GPIO_Init 和 MX_USART2_UART_Init 之后、主循环之前）。

```c
static void console_print_examples(void)
{
  console_send_string("RocketPi UART LED console ready.\r\n");
  console_send_string("Example commands:\r\n");
  console_send_string("  {\"led\":\"B\",\"state\":1}\r\n");
  console_send_string("  {\"led\":[\"B\",\"G\"],\"state\":[1,0]}\r\n");
}
```

连续四条 send_string 而不是拼一个大字符串：每条独立成句、可读性高，避免栈上开大缓冲。代价是四次 HAL 调用各有少量开销——上电时刻无所谓。

### console_handle_input_byte

```c
static void console_handle_input_byte(uint8_t data);
```

**作用**：主循环每收到一个字节就调用它。它负责把字节追加到行缓冲 `g_rx_buffer`，遇到 `\n` 则触发整行结算（调用 process），遇到缓冲溢出则清零并报错。这是行组装层的全部逻辑。

**形参**：`data` — 从 HAL_UART_Receive 收到的单个字节（uint8_t，范围 0x00~0xFF）。

**输入/输出**：输入 = 一个字节 + 当前的 g_rx_buffer/g_rx_length 状态；输出 = （1）正常：字节追加进缓冲，g_rx_length+1；（2）\n：调用 process → apply → report 整条链，然后清缓冲发提示符；（3）溢出：清缓冲报错发提示符。

**调用者**：`main` 主循环，每次 HAL_UART_Receive 返回一个字节时。

```c
static void console_handle_input_byte(uint8_t data)
{
  if (data == '\r') return;          // ① 吞 \r
  if (data == '\n')                  // ② \n = 结算信号
  {
    console_process_command_buffer();
    g_rx_length = 0U;
    console_send_prompt();
    return;
  }
  if (g_rx_length >= (UART_RX_BUFFER_SIZE - 1U))  // ③ 超长防御
  {
    g_rx_length = 0U;
    console_report_error("command too long");
    console_send_prompt();
    return;
  }
  g_rx_buffer[g_rx_length++] = (char)data;        // ④ 正常追加
}
```

- **① 为什么先判 \r**：PC 端发送 \r\n 两连发。若顺序反了，\n 先触发结算并清零缓冲，随后到来的 \r 会落到干净缓冲里，成为下一条命令的第一个垃圾字节
- **①②③④ 的顺序纪律**：控制字符（\r\n）永远不占缓冲空间，所以它们的判断必须在追加之前；超长检查必须在追加之前；\n 结算在超长之前（\n 不走长度计数，不存在"缓冲满且当前是 \n"的冲突）。这个顺序不是风格，是正确性
- **③ 的 `-1` 和 `>=` 精确含义**：SIZE-1 是最后一个合法写入位（留给结算时的 '\0'）。g_rx_length 等于 SIZE-1 时，再收任何数据字节就要把 '\0' 的位置挤掉，所以用 >= 拦截。**③ 与 process_command_buffer 里的 `g_rx_buffer[g_rx_length] = '\0'` 是同一条不变式的两端**
- **`(char)data` 强转**：HAL 是字节流视角（uint8_t），缓冲是文本视角（char）。纯 ASCII 命令下两者无损
- **错误路径也发提示符**：报错后缓冲已清零，重发提示符告诉用户"可以重新输入"

## 第二类：字符串原语（4 个）

**特征：与业务完全无关的纯函数，任何项目可原样复制。**

### console_skip_spaces

```c
static const char *console_skip_spaces(const char *ptr);
```

**作用**：从 ptr 指向的位置开始，跳过所有空白字符（空格、制表符等），返回第一个非空白字符的位置。用于解析 JSON 时忽略键值对之间的可选空白。

**形参**：`ptr` — 指向待扫描字符串的某个位置，可为 NULL（NULL 时返回 NULL）。

**输入/输出**：输入 = 字符串指针；输出 = 跳过空白后的新指针（指向第一个非空白字符，或 '\0'）。

**调用者**：`console_find_json_value`（跳过键前、冒号后、值前的空白）、`console_parse_led_targets` / `console_parse_state_values`（跳过数组中逗号和方括号周围的空白）。

```c
static const char *console_skip_spaces(const char *ptr)
{
  while ((ptr != NULL) && (*ptr != '\0') && isspace((unsigned char)*ptr))
  {
    ++ptr;
  }
  return ptr;
}
```

- `ptr != NULL`：防御调用者传空。**放循环条件里是每圈都判，浪费；放函数开头判一次更高效**——这里是为了单表达式返回
- `*ptr != '\0'`：**必停条件**。防御的是若字符集异常导致 isspace('\0') 为真时循环冲出缓冲区的情况
- **`(unsigned char)*ptr` 强转是 ctype 系列的铁律**：isspace 的入参约定为「EOF 或 unsigned char 范围」。char 若是 signed（ARM GCC 默认 signed），高位字符（0x80+）经符号扩展变成负数 → 未定义行为。**所有 isxxx/toupper/tolower 都必须这么写**

### console_str_case_equal

```c
static bool console_str_case_equal(const char *lhs, const char *rhs);
```

**作用**：逐字符忽略大小写比较两个字符串是否完全相同（长度和内容都匹配才返回 true）。用于命令关键词匹配（如用户输 `b`/`B`/`Blue` 都能匹配配置表里的 `B`）。

**形参**：`lhs`、`rhs` — 两个以 `\0` 结尾的字符串，任一可为 NULL（NULL 时返回 false）。

**输入/输出**：输入 = 两个字符串；输出 = bool（true=完全匹配忽略大小写，false=不匹配或有 NULL）。

**调用者**：`console_find_led_by_token`（逐别名匹配 LED 名）、`console_parse_state_token`（匹配 `on`/`off`/`true`/`false` 等关键词）。

```c
static bool console_str_case_equal(const char *lhs, const char *rhs)
{
  if ((lhs == NULL) || (rhs == NULL)) return false;
  while ((*lhs != '\0') && (*rhs != '\0'))
  {
    const int ca = toupper((unsigned char)*lhs);
    const int cb = toupper((unsigned char)*rhs);
    if (ca != cb) return false;
    ++lhs; ++rhs;
  }
  return (*lhs == '\0') && (*rhs == '\0');
}
```

为什么不用 libc 的 `strcasecmp`？**它不是标准 C**（POSIX 有，Windows 叫 _stricmp，Keil 可能没有）——嵌入式追求可移植，12 行自己写比查编译器文档快。

### console_port_name

```c
static const char *console_port_name(GPIO_TypeDef *port);
```

**作用**：把 HAL 的 GPIO 端口指针（如 `GPIOB`）翻译成人可读的字符串（如 `"GPIOB"`）。只在上电时 `console_print_gpio_map` 打印 LED→GPIO 映射表时使用。

**形参**：`port` — GPIO 端口的 HAL 句柄指针（GPIOA~GPIOH），实际是比较地址值。

**输入/输出**：输入 = 一个 GPIO 端口指针；输出 = 对应的名称字符串常量（如 `"GPIOB"`），失败时返回 `"UNKNOWN"`。

**调用者**：`console_print_gpio_map`（上电打印 LED 配置映射时）。

```c
static const char *console_port_name(GPIO_TypeDef *port)
{
  if (port == GPIOA) return "GPIOA";
  if (port == GPIOB) return "GPIOB";
  if (port == GPIOC) return "GPIOC";
  if (port == GPIOD) return "GPIOD";
  if (port == GPIOE) return "GPIOE";
  if (port == GPIOH) return "GPIOH";
  return "UNKNOWN";
}
```

**指针身份比较**：`port == GPIOA` 比较的是外设基址（GPIOA 宏展开是 `(GPIO_TypeDef *)0x40020000` 这类常量指针），比的是地址数值，合法且高效。外设指针身份唯一，不存在两个 GPIOA。

### console_pin_index

```c
static uint32_t console_pin_index(uint16_t pin);
```

**作用**：把 HAL 的 GPIO 引脚位图（如 `GPIO_PIN_5`，值为 `1<<5=0x20`）翻译成人可读的引脚编号（如 `5`）。只在上电打印配置映射时使用。

**形参**：`pin` — 引脚位图，16 位中只有 1 位为 1（如 0x0020 = pin 5）。

**输入/输出**：输入 = 引脚位图；输出 = 引脚编号（0~15），失败返回 0xFFFFFFFF（哨兵值，因为 0 是合法引脚号）。

**调用者**：`console_print_gpio_map`（上电打印 LED 配置映射时）。

```c
static uint32_t console_pin_index(uint16_t pin)
{
  for (uint32_t i = 0U; i < 16U; ++i)
  {
    if (pin == (uint16_t)(1U << i)) return i;
  }
  return 0xFFFFFFFFU;      // 无效哨兵
}
```

16 次循环找 `1<<i == pin`。位图→编号也可以用 `__CLZ`（前导零计数）指令一条完成，但那是 CMSIS 扩展；教学代码用可移植写法。**哨兵值必须从合法值域外取**——调用方 `pin_index <= 15U` 判断才可靠。

## 第三类：解析引擎（8 个）——核心重点

**特征：以「游标 + 查表」为骨架，函数间用统一签名组合。**

### console_find_json_value

```c
static const char *console_find_json_value(const char *json, const char *key);
```

**作用**：在 JSON 字符串中定位指定键的值的起始位置。先用 strstr 找到键（含引号锚定），再跳过键本身和冒号，返回值区域的第一个字符。**返回的是原字符串内部的指针，不是拷贝**。

**形参**：`json` — 完整的 JSON 命令字符串；`key` — 键名，调用方传带引号的形式（如 `"\"led\""`）。

**输入/输出**：输入 = JSON 字符串 + 键名；输出 = 指向值区域首字符的指针（如 `{"led":"B",...}` → 指向 `"B"` 的 `"`），找不到或格式不符返回 NULL。

**调用者**：`console_parse_led_targets`（找 `"led"` 键）、`console_parse_state_values`（找 `"state"` 键）。

```c
static const char *console_find_json_value(const char *json, const char *key)
{
  if ((json == NULL) || (key == NULL)) return NULL;
  const char *location = strstr(json, key);   // ① 找键（带引号）
  if (location == NULL) return NULL;
  location += strlen(key);                    // ② 跳过键本身
  location = console_skip_spaces(location);   // ③ 键值间可能有空格
  if (*location != ':') return NULL;          // ④ 必须是冒号
  ++location;
  return console_skip_spaces(location);       // ⑤ 值前可能有空格
}
```

**带引号搜键**（调用方传 `"\"led\""`）是本函数最重要的设计：只搜 `led` 会误匹配 `my_led_config` 或值里的文本 "led"；`"led"` 五字符（含双引号）在 JSON 里只能作为键出现。**利用格式特征缩小搜索歧义**——比写一个真正的词法器便宜百倍。副作用：值里若恰好有 `"led":` 文本会误命中——**这是本方案与真 JSON 解析器的差距**，09 换库就是消除这类缺陷。

### console_parse_led_token

```c
static bool console_parse_led_token(const char **cursor, LedCommand_t *cmd);
```

**作用**：从游标位置解析一个带引号的 LED 名称字符串（如 `"B"`），在配置表中查找对应的 LED 下标，并将下标追加到 cmd 的 led_index 数组中。如果 LED 名是 `ALL` 则展开为全部 LED。

**形参**：`cursor` — 二级指针，指向调用方的游标变量（输入当前读取位置，成功时回写推进后的新位置）；`cmd` — 命令结构体，函数向其 led_index/led_count 写入数据。

**输入/输出**：输入 = 游标指向 `"` 开头的字符串 + 空 cmd；输出 = （成功）游标推进到引号之后、cmd 追加了 LED 下标，返回 true；（失败）游标不动，cmd 不变，返回 false。

**调用者**：`console_parse_led_targets`——数组形态下在 while 循环中逐元素调用，单值形态下调用一次。

```c
static bool console_parse_led_token(const char **cursor, LedCommand_t *cmd)
{
  if ((cursor == NULL) || (*cursor == NULL) || (cmd == NULL)) return false;

  const char *ptr = *cursor;          // ① 从游标取当前读入位置
  if (*ptr != '"') return false;      // ② LED 名必须是 "..." 形式
  ++ptr;
  const char *start = ptr;            // ③ 记下名字起点
  while ((*ptr != '\0') && (*ptr != '"')) ++ptr;   // ④ 扫到闭引号
  if (*ptr != '"') return false;      // ⑤ 没闭引号 = 畸形

  const size_t length = (size_t)(ptr - start);     // ⑥ 指针相减得长度
  if ((length == 0U) || (length >= LED_NAME_MAX_LEN)) return false;

  char token[LED_NAME_MAX_LEN] = {0};  // ⑦ 栈上定长缓冲
  memcpy(token, start, length);        // ⑧ 复制出来
  token[length] = '\0';
  ++ptr;                               // ⑨ 越过闭引号

  if (console_str_case_equal(token, "ALL")) { /* 展开 0..LED_TOTAL */ }
  const int8_t led_index = console_find_led_by_token(token);
  if (led_index < 0) return false;     // ⑩ 查表失败
  if (!console_add_led_index(cmd, (uint8_t)led_index)) return false;
  *cursor = ptr;                       // ⑪ 全部成功才回写游标！
  return true;
}
```

**二级指针签名是本类的灵魂**。函数内部 ptr 前进只是改了栈上的形参副本，调用方看不到。要「把消耗后的位置告诉调用方」，必须拿到调用方指针变量的地址——即 `const char **`。

**⑪ 原子回写**：所有失败分支都在 `*cursor = ptr` 之前 return，保证「失败时调用方的游标不动」——成功才推进。**先在局部 ptr 上折腾、终局才回写**，是游标解析器的通用模式。

**⑧ 为什么要复制出来**：JSON 字符串若含转义（`\"`），原地扫描会在转义符处误判边界。复制到独立缓冲后，后续比较都在「干净数据」上做。LED_NAME_MAX_LEN=16 的容量限制在⑦之前就拦截了超长输入。

### console_parse_state_token

```c
static bool console_parse_state_token(const char **cursor, LedCommand_t *cmd);
```

**作用**：从游标位置解析一个状态值，支持三种形态——带引号的字符串（`"on"`）、裸字母（`on`）、数字（`1`），将其翻译成 bool 追加到 cmd 的 states 数组中。

**形参**：`cursor` — 二级指针，指向调用方的游标变量（输入当前读取位置，成功时回写推进后的新位置）；`cmd` — 命令结构体，函数向其 states/state_count 写入数据。

**输入/输出**：输入 = 游标指向值的首字符（可能是 `"`、字母或数字）+ 当前 cmd；输出 = （成功）游标推进到值之后、cmd 追加了 bool 状态，返回 true；（失败）游标不动，返回 false。

**调用者**：`console_parse_state_values`——数组形态下在 while 循环中逐元素调用，单值形态下调用一次。

结构是三分支：

```
if (*ptr == '"')          → 扫引号内文本 → str_case_equal 匹配 on/true/enable/off/false/disable
else if (isalpha(...))    → 扫字母串 → 同样匹配（容忍无引号写法）
else                      → 可选负号 + isdigit 逐位 number*10 + (*ptr-'0') → 非零为真
```

新增知识点：
- **'5'-'0'=5 原理**：ASCII '0'~'9' 是 0x30~0x39 连续排列，数字字符减 '0' 即其数值
- **负号的语义化处理**：state:-1 → value=false——负数按「关」处理，宽容解析
- **数字溢出**：number 是 uint32_t，128 字节行缓冲最多 126 个数字位，理论可溢出。教学代码未处理，工程代码应判溢出

### console_parse_led_targets

```c
static bool console_parse_led_targets(const char *json, LedCommand_t *cmd);
```

**作用**：在 JSON 字符串中找到 `"led"` 键，判断其值是数组还是单个字符串，调用 `console_parse_led_token` 逐个解析，把所有目标 LED 的下标写入 cmd。这是解析引擎的上层编排函数——token 解析器只认单个值，本函数负责循环和分流。

**形参**：`json` — 完整的 JSON 命令字符串；`cmd` — 命令结构体，函数向其 led_index/led_count 写入数据。

**输入/输出**：输入 = JSON 字符串 + 空 cmd；输出 = （成功）cmd 的 led_index 填满了目标 LED 下标，led_count 更新，返回 true；（失败）cmd 不变，返回 false。

**调用者**：`console_process_command_buffer`，在解析阶段的第一个步骤调用。

```c
static bool console_parse_led_targets(const char *json, LedCommand_t *cmd)
{
  if ((json == NULL) || (cmd == NULL)) return false;

  const char *value = console_find_json_value(json, "\"led\"");
  if (value == NULL)
  {
    value = console_find_json_value(json, "\"LED\"");   // ① 键名大写兑底
  }
  if (value == NULL) return false;

  if (*value == '[')          // ② 数组形态
  {
    ++value;                  // 跳过 '['
    while (true)
    {
      value = console_skip_spaces(value);
      if (*value == ']') { ++value; break; }          // ③ 空数组/收尾
      if (!console_parse_led_token(&value, cmd)) return false;  // ④ 逐元素
      value = console_skip_spaces(value);
      if (*value == ',') { ++value; continue; }       // ⑤ 逗号继续
      if (*value == ']') { ++value; break; }          // ⑥ 正常结束
      return false;                                    // ⑦ 其他字符 = 畸形
    }
  }
  else if (*value == '"')     // ⑧ 单值形态
  {
    if (!console_parse_led_token(&value, cmd)) return false;
  }
  else
  {
    return false;            // ⑨ 既不是数组也不是字符串
  }

  return (cmd->led_count > 0U);   // ⑩ 最终有效性
}
```

- **① 的大小写兑底**：宽容处理 "LED" 全大写变体，混合大小写 "Led" 不认——宽容有边界
- **②⑧⑨ 按首字符分流**：'[' 进数组、'"' 进单值、其他拒绝
- **③⑥ 两处 ']' 检查的区别**：③ 处理空数组和循环收尾；⑥ 处理元素后直接收尾。两处都要 `++value` 消费掉 ']'
- **⑤⑦ 与畸形输入**：`[x,,y]` 双逗号在第二轮 parse_led_token 看到 ',' 失败（④ 拦截）；`[x y]` 落进 ⑦

### console_parse_state_values

```c
static bool console_parse_state_values(const char *json, LedCommand_t *cmd);
```

**作用**：与 parse_led_targets 同构的姊妹函数——在 JSON 字符串中找到 `"state"` 键，判断其值是数组还是单值，调用 `console_parse_state_token` 逐个解析，把所有状态值的 bool 结果写入 cmd。

**形参**：`json` — 完整的 JSON 命令字符串；`cmd` — 命令结构体，函数向其 states/state_count 写入数据。

**输入/输出**：输入 = JSON 字符串 + 已填入 led 信息的 cmd；输出 = （成功）cmd 的 states 填满了对应状态值，state_count 更新，返回 true；（失败）cmd 不变，返回 false。

**调用者**：`console_process_command_buffer`，在 parse_led_targets 成功之后调用。

与 led_targets 的差异只有两处：

1. **token 解析器换成 parse_state_token**——查表逻辑不同而已，框架零改动。**同构代码的复用方式不是抽象成宏/模板（C 做起来难看），而是接受两份平行实现，靠命名对称性（led_/state_）保持可对照性**
2. **led 的单值分支必须检查 '"' 开头，state 的单值直接调 token**——因为 led 的值域只有字符串一种形态，分流在 targets 层做；state 的值域有三形态（引号串/裸字母/数字），分流下沉到 state_token 内部。**分流位置由值域的形态数决定**

### console_add_led_index

```c
static bool console_add_led_index(LedCommand_t *cmd, uint8_t index);
```

**作用**：向 cmd 的 led_index 数组追加一个 LED 下标，如果该下标已存在（重复）则静默返回成功，如果数组已满则返回失败。负责去重和容量控制。

**形参**：`cmd` — 命令结构体；`index` — 要追加的 LED 下标（0~LED_TOTAL-1）。

**输入/输出**：输入 = cmd + 一个下标；输出 = （成功）cmd 的 led_index 追加了新下标、led_count+1，返回 true；（重复）cmd 不变，返回 true；（满）cmd 不变，返回 false。

**调用者**：`console_parse_led_token`，每解析出一个 LED 名称后调用一次。`"ALL"` 关键字会连续调用 LED_TOTAL 次。

```c
static bool console_add_led_index(LedCommand_t *cmd, uint8_t index)
{
  if (cmd == NULL) return false;
  for (size_t i = 0U; i < cmd->led_count; ++i)   // ① 查重
  {
    if (cmd->led_index[i] == index) return true; //    重复 → 静默成功
  }
  if (cmd->led_count >= LED_TOTAL) return false; // ② 满了 → 失败
  cmd->led_index[cmd->led_count++] = index;      // ③ 追加
  return true;
}
```

- **重复返回 true 不是 false**：`{"led":["B","B"],"state":[1,0]}` 语义自相矛盾，但把重复当「用户想要 B」，静默去重比报错体验好
- **先查重后查容量**：顺序反了会把「重复导致的满」误判为溢出错误

### console_find_led_by_token

```c
static int8_t console_find_led_by_token(const char *token);
```

**作用**：在配置表 `g_led_config` 中查找与给定别名匹配的 LED 下标。外层遍历 3 颗 LED、内层遍历每颗的 3 个别名，命中返回下标（0~2），全部未命中返回 -1。

**形参**：`token` — 用户输入的 LED 名称字符串（如 `"B"`、`"blue"`、`"LED_B"`）。

**输入/输出**：输入 = 一个字符串；输出 = 匹配的 LED 下标（0~2），或 -1 表示未找到。

**调用者**：`console_parse_led_token`，解析出 LED 名称字符串后调用。

外层遍历 LED、内层遍历别名，命中即返回下标。`int8_t` 返回类型：合法下标 0~2，失败 -1——**有符号类型天然表达「找不到」**。`LED_TOKEN_COUNT=3` 与表数据绑定，别名加到第四个必须改宏。

### console_process_command_buffer

```c
static void console_process_command_buffer(void);
```

**作用**：收到完整命令行后的总入口。将行缓冲变为合法 C 字符串，按顺序调用解析（led → state）、语义校验（count 匹配）、执行（apply）、汇报（report），失败时汇报错误。**是全文件的编排中心**，连接行组装层和解析/执行/汇报层。

**形参**：无（直接读全局变量 g_rx_buffer 和 g_rx_length）。

**输入/输出**：输入 = g_rx_buffer 中积累的一行命令 + g_rx_length；输出 = LED 状态被改变 + TX 上出现 JSON 应答（成功或错误）。

**调用者**：`console_handle_input_byte`，当收到 `\n` 字节时调用。

```c
g_rx_buffer[g_rx_length] = '\0';   // ① 行缓冲补终 0
if (g_rx_length == 0U) return;     // ② 空行静默忽略

LedCommand_t command = {0};        // ③ 中间表示清零起步
if (!console_parse_led_targets(g_rx_buffer, &command)) { 报错; return; }
if (!console_parse_state_values(g_rx_buffer, &command)) { 报错; return; }
if (!(count 校验)) { 报错; return; }   // ④ 语义关卡
console_apply_command(&command);   // ⑤ 执行
console_report_result(&command);   // ⑥ 汇报
```

- **① 补 '\0'**：handle_input_byte 保证 g_rx_length ≤ SIZE-1，所以下标 g_rx_length 不越界——两个函数共同维护同一条不变式
- **③ = {0} 清零**：count 字段必须从 0 起步，否则 add 函数的追加位置是随机值
- **④ 状态数量校验放这里**：它是唯一需要同时看 led_count 和 state_count 的规则

## 第四类：执行与汇报（5 个）

**特征：只认 LedCommand_t，把数字世界翻译回硬件动作或文本。**

### console_set_led_state

```c
static void console_set_led_state(uint8_t index, bool enabled);
```

**作用**：根据 LED 下标和开关状态，调用 HAL 写 GPIO 引脚电平。全文件唯一真正操作硬件 GPIO 的函数。

**形参**：`index` — LED 下标（0~2），超出范围静默返回；`enabled` — true=点亮，false=熄灭（实际电平由 LED_ACTIVE_LOW 宏决定）。

**输入/输出**：输入 = LED 下标 + 开关状态；输出 = 对应 GPIO 引脚电平被改变（高或低，由板级极性决定）。

**调用者**：`console_apply_command`，在 for 循环中对每个目标 LED 调用一次。

```c
static void console_set_led_state(uint8_t index, bool enabled)
{
  if (index >= LED_TOTAL) return;    // 防御：非法下标
  HAL_GPIO_WritePin(g_led_config[index].port,
                    g_led_config[index].pin,
                    enabled ? LED_ON_STATE : LED_OFF_STATE);
}
```

防御检查看似多余（调用方 apply 已保证 index 合法）——**网关函数的防御是对未来负责**：三个月后有人写新调用路径（定时器闪烁、另一个命令），这行检查是最后防线。

### console_apply_command

```c
static void console_apply_command(const LedCommand_t *cmd);
```

**作用**：遍历 cmd 中所有目标 LED，根据 state_count 决定广播还是逐一对应，调用 console_set_led_state 设置每颗灯的状态。**是执行阶段的唯一函数**。

**形参**：`cmd` — 已完成解析和校验的命令结构体（led_index/states/count 全部就绪）。

**输入/输出**：输入 = 填满的 cmd；输出 = 多颗 LED 的 GPIO 电平被改变。

**调用者**：`console_process_command_buffer`，在 count 校验通过之后调用。

```c
const size_t state_index = (cmd->state_count == 1U) ? 0U : i;
```

一行兼容两种命令：`state:[1]` 广播给所有 LED；`state:[1,0]` 一一对应。**前提是上游已校验**（state_count 是 1 或等于 led_count），执行层信任上游。

### console_report_result

```c
static void console_report_result(const LedCommand_t *cmd);
```

**作用**：把 cmd 中的 LED 名称和状态翻译成 JSON 应答字符串，通过串口发给 PC。格式如 `{"status":"ok","led":["B","G"],"state":[1,0]}`。

**形参**：`cmd` — 已执行完毕的命令结构体。

**输入/输出**：输入 = cmd（读取 led_index/states/count）+ g_led_config（回查短名）；输出 = JSON 字符串出现在 TX 引脚。

**调用者**：`console_process_command_buffer`，在 apply 成功之后调用。

```c
char buffer[192];
int length = snprintf(buffer, sizeof(buffer), "{\"status\":\"ok\",\"led\":[");
for (size_t i = 0U; i < cmd->led_count; ++i)
{
  const char *delimiter = (i + 1U < cmd->led_count) ? "," : "";
  length += snprintf(&buffer[length], (size_t)(sizeof(buffer) - (size_t)length),
                     "\"%s\"%s", g_led_config[cmd->led_index[i]].tokens[0], delimiter);
}
...末段 "]}\r\n"
```

这是 [[06_rocketpi_uart_printf_3|06_3 hexdump]] 三件套（`&buf[pos]`、`sizeof-pos`、`pos+=n`）的实战应用。**192 字节容量**：最坏情况（3 颗灯全用最长别名 LED_B）约 70 字节，2.7 倍余量。**tokens[0] 取短名**：应答用 `B` 不用 `LED_B`——应答是人看的，短名优先。

### console_report_error

```c
static void console_report_error(const char *message);
```

**作用**：把错误原因字符串包装成 `{"status":"error","msg":"原因"}` 格式的 JSON，通过串口发给 PC。

**形参**：`message` — 错误原因字符串（如 `"unknown led"`、`"command too long"`），可为 NULL（NULL 时 msg 字段填 `"unknown"`）。

**输入/输出**：输入 = 错误原因字符串；输出 = 错误 JSON 出现在 TX 引脚。

**调用者**：`console_handle_input_byte`（超长错误）、`console_process_command_buffer`（解析/校验失败）、`console_parse_led_token`（未知 LED）、`console_parse_state_token`（非法状态值）等所有失败路径。

```c
(void)snprintf(buffer, sizeof(buffer),
               "{\"status\":\"error\",\"msg\":\"%s\"}\r\n",
               (message != NULL) ? message : "unknown");
```

`(message != NULL) ? message : "unknown"`——入参可能为 NULL 的兜底。**错误处理代码自身必须是最健壮的代码**。160 字节容量：msg 最长约 22 字符 + 模板 34 字符，余量充足。

### console_print_gpio_map

```c
static void console_print_gpio_map(void);
```

**作用**：上电时把配置表 `g_led_config` 的全部 LED 信息翻译成 JSON 格式输出给 PC。格式如 `{"led_config":[{"name":"B","port":"GPIOB","pin":5},{"name":"G","port":"GPIOB","pin":4},...]}\r\n`，让上位机程序自动发现板上有哪些灯、接在哪个引脚。

**形参**：无。

**输入/输出**：输入 = g_led_config 配置表（只读）；输出 = JSON 映射表出现在 TX 引脚。

**调用者**：`main`，上电时调一次（在 print_examples 之后、send_prompt 之前）。

snprintf 里 `%lu` 配 `(unsigned long)` 强转——size_t/uint32_t 在不同平台宽度不同，**printf 家族的可移植写法是显式提升到格式符期望的类型**。

## 四类函数总表

| 类别 | 成员 | 复用价值 | 依赖 |
|---|---|---|---|
| I/O 网关与行组装 | send_string, send_prompt, print_examples, handle_input_byte | 边界层，换硬件只改这 | HAL_UART，无业务 |
| 字符串原语 | skip_spaces, str_case_equal, port_name, pin_index | 全项目通用 | libc ctype，无业务 |
| 解析引擎 | find_json_value, parse_led_token, parse_state_token, parse_led_targets, parse_state_values, add_led_index, find_led_by_token, process_buffer | 本协议专用 | 原语类 + 配置表 |
| 执行汇报 | set_led_state, apply_command, report_result, report_error, print_gpio_map | 本业务专用 | 中间表示 + 网关 |

21 个函数 4+4+8+5 全覆盖。依赖箭头严格从上往下、从左往右——**这就是这套代码可以独立测试的原理**：解析引擎用假 cmd 就能单测，不接硬件。

## 关联笔记

- [[08_rocketpi_uart_control_led]] — 主笔记：架构与三层流水线
- [[08_rocketpi_uart_control_led_2|数据结构与 API 设计]] — 本篇各函数为何存在的设计推演
- [[08_rocketpi_uart_control_led_5|函数协作全景]] — 四类函数如何咬合成一次完整交互
- [[06_rocketpi_uart_printf_3|06_3 hexdump 详解]] — snprintf 链式拼装原理出处
- [[06_rocketpi_uart_printf_4|06_4 C 标准库补课]] — ctype 陷阱与字符串函数家族
