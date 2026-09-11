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

> 08 共 21 个 static 函数，名字多、看似碎片。实际上它们分四类，每类内部高度同构，掌握一类就掌握一类函数的通用写法。本文逐类逐个拆解到"能独立复刻"的程度，四个小节标题即函数名，可从其他笔记精确链接跳转。函数命名前缀 `console_` 是模块前缀，避免全局命名空间污染（static + 前缀是小项目的土法命名空间）。函数之间如何协作完成一次完整交互，见 [[08_rocketpi_uart_control_led_5|函数协作全景]]。

## 第一类：I/O 网关与行组装（4 个）

**特征：处在系统的边界上——对外碰硬件 UART，对内提供纯文本/纯字节接口，是唯一允许知道 huart2 存在的函数群。**

### console_send_string——一切输出的总闸门

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
- **HAL_MAX_DELAY 无限等**：命令交互是人机节奏，阻塞无所谓；但若这是协议引擎的发送函数，超时应设有限值。**同一个函数体，换个场景超时语义就变**——这是"函数语义由使用场景定义"的例子

**设计意图**：全文件只有这一个函数知道 huart2 怎么用。以后换 UART3、换 DMA 发送、换 RTOS 队列，只改这一个函数，调用者零感知。这就是网关函数（gateway/choke point）的价值。

### console_send_prompt——会话节奏控制器

```c
static void console_send_prompt(void)
{
  console_send_string("\r\n> ");
}
```

一行函数也值得独立存在：**"什么时候发提示符"是一个独立决策点**，嵌在代码里的字面量是魔法值，抽成函数后，未来加时间戳（`[12:33]> `）、加 ANSI 颜色只改一处。\r\n 双字符的原因在 [[08_rocketpi_uart_control_led_1|交互终端笔记]]：终端把 \r（回车）和 \n（换行）当两个动作。

### console_print_examples——开机引导

```c
static void console_print_examples(void)
{
  console_send_string("RocketPi UART LED console ready.\r\n");
  console_send_string("Example commands:\r\n");
  console_send_string("  {\"led\":\"B\",\"state\":1}\r\n");
  console_send_string("  {\"led\":[\"B\",\"G\"],\"state\":[1,0]}\r\n");
}
```

只在上电调用一次。两个观察点：
- **连续四条 send_string 而不是拼一个大字符串一次发**：每条独立成句、可读性高，也避免了在栈上开一个大缓冲做拼接。代价是四次 HAL 调用各有少量开销——上电时刻无所谓，这个判断和 [[08_rocketpi_uart_control_led_3#console_report_result——snprintf 链式拼装|report_result]] 里"发送粒度合并"不矛盾：**一次性多段内容才值得拼，固定几行直接发**
- **和 print_gpio_map 是一对**：print_examples 是人可读的引导（告诉人怎么用），print_gpio_map 是机器可读的自描述（告诉上位机有什么资源）——开机输出同时面向两类"读者"

### console_handle_input_byte——行组装的门卫

主笔记从流水线视角讲过它的三个防御，这里补行级细节：

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

- **① 为什么先判 \r**：PC 端发送 \r\n 两连发。若顺序反了，\n 先触发结算并清零缓冲，随后到来的 \r 会落到干净缓冲里，成为下一条命令的第一个垃圾字节。\r 必须在一切之前无条件吞掉
- **①②③④ 的顺序纪律**：控制字符（\r\n）永远不占缓冲空间，所以它们的判断必须在追加之前；超长检查必须在追加之前；\n 结算在超长之前（\n 不走长度计数，不存在"缓冲满且当前是 \n"的冲突）。这个顺序不是风格，是正确性
- **③ 的 `-1` 和 `>=` 精确含义**：SIZE-1 是最后一个合法写入位（留给结算时的 '\0'）。g_rx_length 等于 SIZE-1 时，再收任何数据字节就要把 '\0' 的位置挤掉，所以用 >= 拦截。**③ 与 [[08_rocketpi_uart_control_led_3#console_process_command_buffer——解析编排层|process_command_buffer]] 里的 `g_rx_buffer[g_rx_length] = '\0'` 是同一条不变式的两端**：这边保证长度永不越过 SIZE-1，那边才能放心写下标 g_rx_length
- **`(char)data` 强转**：HAL 是字节流视角（uint8_t），缓冲是文本视角（char）。纯 ASCII 命令下两者无损；隐含前提是"命令是 ASCII"——若将来命令带高位字节，char 的符号性会干扰 ctype 判断（见第二类 str_case_equal 的 unsigned char 铁律）
- **错误路径也发提示符**：报错后缓冲已清零，重发提示符告诉用户"可以重新输入"——错误恢复不能只报错不给出口

## 第二类：字符串原语（4 个）

**特征：与业务完全无关的纯函数，任何项目可原样复制。**

### console_skip_spaces——游标推进器

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

逐个条件拆：
- `ptr != NULL`：防御调用者传空。**放循环条件里是每圈都判，浪费；放函数开头判一次更高效**——这里是为了单表达式返回。工程上更常见的写法是开头 `if (ptr == NULL) return NULL;`
- `*ptr != '\0'`：**必停条件**。若没有它，无空格时 '\0' 也匹配 isspace 吗？不匹配，循环自然停。但防御的是另一件事：若字符集异常导致 isspace('\0') 为真，循环就冲出缓冲区——不信任 libc 宏对边界值的实现，显式拦住
- **`(unsigned char)*ptr` 强转是 ctype 系列的铁律**：isspace 的入参约定为「EOF 或 unsigned char 范围」。char 若是 signed（ARM GCC 默认 signed），中文/高位字符（0x80+）经符号扩展变成负数，数组索引为负 → 未定义行为。**所有 isxxx/toupper/tolower 都必须这么写**，这是 C 标准库最著名的陷阱之一

**返回值设计**：返回推进后的指针而不是 void，让调用方 `value = console_skip_spaces(value);` 一行完成推进——函数式风格，配合游标解析体系（见第三类）。

### console_str_case_equal——不依赖 locale 的 strcasecmp

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

为什么不用 libc 的 `strcasecmp`？**它不是标准 C**（POSIX 有，Windows 叫 _stricmp，Keil 可能没有）——嵌入式追求可移植，12 行自己写比查编译器文档快。注意逐字符 toupper 比「先复制整串再统一转大写」省一次拷贝。

**结尾双 \0 判断**是易错点：循环退出有两种原因——有人提前不等了（已 return false），或有人到了末尾。两种都走到结尾判断时，`*lhs == '\0' && *rhs == '\0'` 保证**两个串同时走完**（"abc" vs "abcd" 会在循环里分出胜负：'\0' 经 toupper 不等于 'D'，但依赖这个巧合不可靠，显式判断双终止符才是稳的）。

### console_port_name / console_pin_index——调试视角转换器

```c
static const char *console_port_name(GPIO_TypeDef *port)
{
  if (port == GPIOA) return "GPIOA";
  ...
  return "UNKNOWN";
}

static uint32_t console_pin_index(uint16_t pin)
{
  for (uint32_t i = 0U; i < 16U; ++i)
  {
    if (pin == (uint16_t)(1U << i)) return i;
  }
  return 0xFFFFFFFFU;      // 无效哨兵
}
```

这两个只服务于上电的 gpio_map 打印。要点：
- **指针身份比较**：`port == GPIOA` 比较的是外设基址（GPIOA 宏展开是 `(GPIO_TypeDef *)0x40020000` 这类常量指针），比的是地址数值，合法且高效。外设指针身份唯一，不存在两个 GPIOA
- **pin_index 的线性扫描**：16 次循环找 1<<i == pin。位图→编号也可以用 `__CLZ`（前导零计数）指令一条完成，但那是 CMSIS 扩展；教学代码用可移植写法。**查表/扫描 vs 位指令是可移植性 vs 效率的典型取舍**
- **失败哨兵 0xFFFFFFFF**：pin_index 找不到匹配时返回它而不是 0（0 是合法引脚号！）。**哨兵值必须从合法值域外取**——调用方 `pin_index <= 15U` 判断才可靠

## 第三类：解析引擎（8 个）——核心重点

**特征：以「游标 + 查表」为骨架，函数间用统一签名组合。**

### console_find_json_value——锚定式键查找

```c
static const char *console_find_json_value(const char *json, const char *key)
{
  if ((json == NULL) || (key == NULL)) return NULL;
  const char *location = strstr(json, key);   // ① 找键（调用方传 "\"led\"" 带引号）
  if (location == NULL) return NULL;
  location += strlen(key);                    // ② 跳过键本身
  location = console_skip_spaces(location);   // ③ 键值间可能有空格
  if (*location != ':') return NULL;          // ④ 必须是冒号，否则不是键值对
  ++location;
  return console_skip_spaces(location);       // ⑤ 值前可能有空格
}
```

**带引号搜键**（调用方传 `"\"led\""`）是本函数最重要的设计：只搜 `led` 会误匹配 `my_led_config` 或值里的文本 "led"；`"led"` 五字符（含双引号）在 JSON 里只能作为键出现。**利用格式特征缩小搜索歧义**——比写一个真正的词法器便宜百倍，是手写解析器的性价比招数。副作用也看清楚：值里若恰好有 `"led":` 文本会误命中——**这是本方案与真 JSON 解析器的差距**，09 换库就是消除这类缺陷，也是「伪 JSON」的"伪"字来源。

**⑤ 的三段式「跳过键→验证冒号→跳空白」**：每步都是独立小操作，链式推进。手写解析器的通用节奏：**定位→推进→验证→推进**。

### console_parse_led_token——游标签名详解

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
  memcpy(token, start, length);        // ⑧ 复制出来——为什么不直接用指针？
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

**二级指针签名是本类的灵魂**。为什么不能是 `const char *cursor`？函数内部 ptr 前进只是改了栈上的形参副本，调用方看不到。要「把消耗后的位置告诉调用方」，必须拿到调用方指针变量的地址——即 `const char **`。调用侧：

```c
value = console_skip_spaces(value);
if (!console_parse_led_token(&value, cmd)) ...   // &value 传入
// 此处 value 已自动指向 token 之后
```

**⑪ 原子回写**：所有失败分支都在 `*cursor = ptr` 之前 return，保证「失败时调用方的游标不动」——成功才推进。这样调用方可以在一个失败后尝试别的分支而不必重置位置。**先在局部 ptr 上折腾、终局才回写**，是游标解析器的通用模式。

**⑧ 为什么要复制出来**：JSON 字符串若含转义（`\"`），原地扫描会在转义符处误判边界。复制到独立缓冲后，后续 strcmp 类比较都在「干净数据」上做。另外 token 是栈缓冲，**LED_NAME_MAX_LEN=16 的容量限制在⑦之前就拦截了超长输入**，防御与缓冲配套。

**⑨ 越过闭引号**：容易漏的一步——不 `++ptr` 的话游标停在 `"` 上，下一个 token 解析第一个字符就看到 `"` 而失败。**每个终结符都要显式消费**，这是逐字符解析器的纪律。

### console_parse_state_token——多类型分派

这个函数是 led_token 的姊妹版，区别在状态值有三种形态：`"on"`（带引号字符串）、`on`（裸字母）、`1`（数字）。结构是三分支：

```
if (*ptr == '"')          → 扫引号内文本 → str_case_equal 匹配 on/true/enable/off/false/disable
else if (isalpha(...))    → 扫字母串 → 同样匹配（容忍无引号写法）
else                      → 可选负号 + isdigit 逐位 number*10 + (*ptr-'0') → 非零为真
```

新增知识点：
- **'5'-'0'=5 原理**：ASCII '0'~'9' 是 0x30~0x39 连续排列（标准化红利），数字字符减 '0' 即其数值。`number = number*10 + digit` 是逐位读数的标准手法（atoi/strtol 内核），首位 number=0，0*10+5=5，再 5*10+2=52……
- **负号的语义化处理**：state:-1 解析出 negative=true，value=false——负数按「关」处理，把「非法输入」翻译成「合理意图」而非报错。宽容解析是命令交互的友好性设计，但注意：**宽容要有边界**（负号后必须紧跟数字，否则 return false），否则解析器成了猜谜器
- **数字溢出**：number 是 uint32_t，128 字节行缓冲最多 126 个数字位，理论可溢出。教学代码未处理；工程代码应判 `number > (UINT32_MAX - digit) / 10`。**知道边界在哪、明说不处理，和不知道，是两个层次**

### console_parse_led_targets——数组/单值分流循环

上面两个 token 解析器只认"单个值"，本函数负责判断值的形态并组织循环——**token 引擎之上的编排**：

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

- **① 的大小写兑底**：JSON 规范键名大小写敏感，这里宽容处理 "LED" 全大写变体。注意局限：混合大小写 "Led" 不认——宽容有边界，枚举常见的两三种而不是无限制
- **②⑧⑨ 按首字符分流**：'[' 进数组、'"' 进单值、其他拒绝——JSON 语法的自然分形，**分流依据是值的首字符**
- **③⑥ 两处 ']' 检查的区别**：③ 处理"空数组 `[]`"和"循环第二轮后的收尾"；⑥ 处理"元素后直接收尾"。两处都要 `++value` 消费掉 ']'（终结符显式消费纪律，同 token 解析器的⑨）
- **⑤⑦ 与畸形输入**：`[x,,y]` 双逗号在跳过第一个逗号后，第二轮循环里 parse_led_token 看到 ',' 不是 '"' 而失败（④ 拦截）；`[x y]` 元素后是空格+字母，skip 后既非 ',' 也非 ']'，落进 ⑦。**每条非法路径都有对应的拦截点**，画解析器状态转移图时逐条对号
- **⑩ 冗余保险**：当前实现下走到这里的路径 led_count 必然 ≥1（单值成功或数组至少收一个元素），`led_count > 0` 是语义冗余。它的价值看未来：若 token 解析器将来宽容零长 token，这条就是最后防线。**防御性收尾检查防的是代码的演变方向，不是当前路径**

### console_parse_state_values——同构的姊妹分流

与 led_targets 循环骨架完全相同（①大写兑底 → 分流 → while 循环 → 有效性收尾），差异只有两处，这个差异本身就是知识点：

1. **token 解析器换成 parse_state_token**——查表逻辑不同而已，框架零改动。**同构代码的复用方式不是抽象成宏/模板（C 做起来难看），而是接受两份平行实现，靠命名对称性（led_/state_）保持可对照性**
2. **led 的单值分支必须检查 '"' 开头（⑧），state 的单值直接调 token**——因为 led 的值域只有字符串一种形态，分流在 targets 层做；state 的值域有三形态（引号串/裸字母/数字），分流下沉到 state_token 内部。**分流位置由值域的形态数决定**：形态单一→外层分流；形态多样→下沉到 token 层内部分派

### console_add_led_index——去重与容量的合体

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

- **重复返回 true 不是 false**：`{"led":["B","B"],"state":[1,0]}` 语义自相矛盾，但把重复当「用户想要 B」，静默去重比报错体验好。**数据清洗型函数的宽容原则：能明确推断意图就不报错**
- **先查重后查容量**：顺序反了会把「重复导致的满」误判为溢出错误。防御分支的顺序也有语义

### console_find_led_by_token——双层查表

外层遍历 LED、内层遍历别名，命中即返回下标。`int8_t` 返回类型是个细节：合法下标 0~2，失败 -1——**有符号类型天然表达「找不到」**，这也是 libc find 类函数返回 -1 的传统。`LED_TOKEN_COUNT=3` 与表数据绑定，别名加到第四个必须改宏——「编译期常量与数据耦合」是这个方案的简单性代价，改成 tokens 指针+NULL 结尾可解耦但更绕。

### console_process_command_buffer——解析编排层

```c
g_rx_buffer[g_rx_length] = '\0';   // ① 行缓冲补终 0（见下）
if (g_rx_length == 0U) return;     // ② 空行静默忽略

LedCommand_t command = {0};        // ③ 中间表示清零起步
if (!console_parse_led_targets(g_rx_buffer, &command)) { 报错; return; }
if (!console_parse_state_values(g_rx_buffer, &command)) { 报错; return; }
if (!(count 校验)) { 报错; return; }   // ④ 语义关卡
console_apply_command(&command);   // ⑤ 执行
console_report_result(&command);   // ⑥ 汇报
```

- **① 补 '\0' 在写指针处**：[[08_rocketpi_uart_control_led_3#console_handle_input_byte——行组装的门卫|handle_input_byte]] 保证 g_rx_length ≤ SIZE-1，所以 g_rx_buffer[g_rx_length] 不会越界——**两个函数共同维护的不变式**。行组装层负责「永不越界」，结算层负责「补终 0」，各管一半
- **③ = {0} 清零**：count 字段必须从 0 起步，否则 add 函数的追加位置是随机值——「结构体使用前必须清零」在 C 里没有机制保证，靠初始化纪律
- **④ 状态数量校验放这里**：它是唯一需要同时看 led_count 和 state_count 的规则，放哪个解析函数里都会造成不对称，编排层是它的家

## 第四类：执行与汇报（5 个）

**特征：只认 LedCommand_t，把数字世界翻译回硬件动作或文本。**

### console_set_led_state——引脚写入的唯一入口

```c
static void console_set_led_state(uint8_t index, bool enabled)
{
  if (index >= LED_TOTAL) return;    // 防御：非法下标
  HAL_GPIO_WritePin(g_led_config[index].port,
                    g_led_config[index].pin,
                    enabled ? LED_ON_STATE : LED_OFF_STATE);
}
```

全文件唯一写 GPIO 的地方。防御检查看似多余（调用方 apply 已保证 index 合法）——**网关函数的防御是对未来负责**：三个月后有人写新调用路径（定时器闪烁、另一个命令），这行检查是最后防线。enabled→ON/OFF 的翻译在 08 主笔记讲过：板级极性差异被 LED_ACTIVE_LOW 宏吸收在编译期。

### console_apply_command——广播语义的执行层

```c
const size_t state_index = (cmd->state_count == 1U) ? 0U : i;
```

一行兼容两种命令：`state:[1]` 广播给所有 LED；`state:[1,0]` 一一对应。**把语义分派收敛成一行表达式**的前提是上游已校验（state_count 是 1 或等于 led_count），执行层信任上游——层级信任链是分层的代价红利。

### console_report_result——snprintf 链式拼装

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

这是 [[06_rocketpi_uart_printf_3|06_3 hexdump]] 三件套（`&buf[pos]`、`sizeof-pos`、`pos+=n`）的实战应用，本处只补三个新点：

- **192 字节容量的推演（最坏情况）**：头部 20 + 3 个 LED 项（每项 `"LED_B",` 最长 9）+ 中段 12 + 3 个状态项（2）+ 尾部 3 ≈ 70 字节。192 有 2.7 倍余量——**容量必须按最坏情况算再放大**，不是拍脑袋
- **正常情况**：`{"led":"B","state":1}` 产出约 40 字节，串口 115200 发送耗时 40×10bit/115200≈3.5ms，远小于人感知阈值——性能由场景定义，不追求极限
- **tokens[0] 取短名**：应答用 `B` 不用 `LED_B`——应答是人看的，短名优先；别名表在解析时宽容、在生成时收敛

### console_report_error——错误应答模板

```c
(void)snprintf(buffer, sizeof(buffer),
               "{\"status\":\"error\",\"msg\":\"%s\"}\r\n",
               (message != NULL) ? message : "unknown");
```

`(message != NULL) ? message : "unknown"`——入参可能为 NULL 的兑底翻译。**错误路径上再崩就是雪上加霜**，错误处理代码自身必须是最健壮的代码。160 字节容量：msg 最长约 22 字符 + 模板 34 字符，余量充足。

### console_print_gpio_map——自描述输出

上电即打印 LED 配置 JSON（名字/端口/引脚）。这是「设备自描述」设计：上位机收到后自动知道有哪些灯可控制，不必人工查文档。snprintf 里 `%lu` 配 `(unsigned long)` 强转——size_t/uint32_t 在不同平台宽度不同，**printf 家族的可移植写法是显式提升到格式符期望的类型**。

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
