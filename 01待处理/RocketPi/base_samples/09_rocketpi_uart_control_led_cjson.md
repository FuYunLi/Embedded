---
status: done
created: 2026-09-12
tags:
  - stm32/uart
  - c/parsing
  - c/cjson
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/09_rocketpi_uart_control_led_cjson/main.c"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/09_rocketpi_uart_control_led_cjson/cJSON.c"
---

# 09_rocketpi_uart_control_led_cjson

> 代码：`Embedded_Code/01待处理/rocketpi/base_samples/09_rocketpi_uart_control_led_cjson/main.c`（约 13KB）+ `cJSON.c`（约 80KB / 2600 行）
> 硬件：STM32F401RE（RocketPi），USART2 115200，LED_B/G/P 三颗灯
> 目标：与 [[08_rocketpi_uart_control_led|08]] 完全相同——串口收 JSON 命令 → 解析 → 控制任意 LED 组合 → 回 JSON 状态。**唯一变化：解析层从手写换成 cJSON 库。**

## 交互效果（与 08 完全相同，多出几种合法输入）

```
> {"led":"B","state":1}                    ← 同 08
{"status":"ok","led":["B"],"state":[1]}
> {"led":["B","G"],"state":[1,0]}          ← 同 08
> {"led":"B","state":true}                 ← cJSON 新增：bool 字面量
> {"led":"B","state":"on"}                 ← 同 08（字符串形态保留）
> { "led" : "B", "state" : 1 }              ← cJSON 新增：键值间随意加空格/缩进
> {"led":["B","G"],"state":[1,0],"extra":99}  ← cJSON 新增：多余字段静默忽略
> {invalid                                   ← cJSON 新增：畸形 JSON 也能识别并报错
{"status":"error","msg":"invalid json"}
```

08 的手写解析器遇到后三种全部崩溃或误判，09 靠 cJSON 零成本获得这些容错。

## 架构对比：变化集中在哪

```
08：字节流 → 行缓冲 → 手写游标解析（6 个函数） → LedCommand_t → 执行 → 汇报
09：字节流 → 行缓冲 → cJSON_Parse → cJSON API 查询 → LedCommand_t → 执行 → 汇报
                ↑ 不变                ↑ 整体替换                ↑ 不变       ↑ 不变   ↑ 不变
```

前两层（行组装）和后两层（执行+汇报）一字不改。**配置表 g_led_config、中间表示 LedCommand_t、广播语义、LED_ACTIVE_LOW 宏全部照搬**。变的是中间解析层，以及因为引入堆分配而在 process 里新增的 cJSON_Delete 配对。

09 共 22 个 static 函数：08 的 15 个原封不动 + 6 个消失（手写解析相关）+ 2 个新增（state_from_json、state_from_string）+ 1 个签名变化（parse_led_token）。

## 不变的 15 个函数

I/O 网关（send_string、send_prompt、print_examples、handle_input_byte）、字符串原语（skip_spaces、str_case_equal、port_name、pin_index）、查表辅助（find_led_by_token、add_led_index）、执行汇报（set_led_state、apply_command、report_result、report_error、print_gpio_map）——**代码与 08 完全相同，不重复列出**。逐个精读见 [[08_rocketpi_uart_control_led_3|08 静态辅助函数逐类精读]]。

## 消失的 4 个函数（08 有、09 无）

这些是 08 手写解析的核心，09 全部由 cJSON 内部功能替代：

| 08 函数 | 08 做的事 | 09 由谁替代 |
|---|---|---|
| `console_find_json_value` | strstr 锚定式键查找 + 跳冒号 + 跳空白 | `cJSON_GetObjectItemCaseSensitive` |
| `console_parse_led_targets`（游标版） | `*value=='['` 分流 + while 循环 + 逗号/方括号消费 | 重写为 cJSON 版（见下） |
| `console_parse_state_token`（游标版） | 引号串/裸字母/数字三分支 + 游标推进 | `console_state_from_json` + `cJSON_IsBool/IsNumber/IsString` |
| `console_parse_state_values`（游标版） | 同上分流循环 | 重写为 cJSON 版（见下） |

**消失的逻辑层**：08 里手动管理的游标推进（`*cursor = ptr`）、终结符消费（`++ptr` 越过 `]`/`"`）、`skip_spaces` 链——cJSON 内部全做完了。**逐字符纪律全部内化进了库，这是手写→用库的最大体感差距。**

## 解析层替换详解：08 手写 vs 09 cJSON

### 键查找：strstr 锚定 → cJSON_GetObjectItem

```c
// 08：手写——strstr 带引号搜键，手动跳冒号跳空白
const char *value = console_find_json_value(json, "\"led\"");

// 09：库调用——一行，内部做完整的词法分析
const cJSON *led = cJSON_GetObjectItemCaseSensitive(root, "led");
if (led == NULL)
{
  led = cJSON_GetObjectItemCaseSensitive(root, "LED");   // 大小写兑底
}
```

08 的 find_json_value 用 strstr 找 `"led"` 五字符锚定——速度快但有误命中风险（值里若恰好有 `"led":` 文本会误匹配）。09 的 cJSON_GetObjectItem 做的是**真正的键值对匹配**：先解析成语法树，再按键名查节点，不可能误命中。大小写兑底仍然保留（先查 `led` 再查 `LED`）。

**cJSON_GetObjectItemCaseSensitive** 返回的是 `cJSON *` 节点指针（NULL 表示没找到），节点内包含类型信息（string/number/bool/array/object）和值——后续按类型提取即可。08 返回的是 `const char *` 指向原字符串内部位置，类型信息要靠调用方自己判断首字符。

### 数组/单值分流：手动字符判断 → cJSON_IsString/IsArray

```c
// 08：手写——看首字符分流 '[' vs '"'，while 循环手动消费逗号和方括号
if (*value == '[')
{
  ++value;
  while (true)
  {
    value = console_skip_spaces(value);
    if (*value == ']') { ++value; break; }
    if (!console_parse_led_token(&value, cmd)) return false;
    value = console_skip_spaces(value);
    if (*value == ',') { ++value; continue; }
    if (*value == ']') { ++value; break; }
    return false;
  }
}
else if (*value == '"') { ... }

// 09：库调用——类型判断 + 宏遍历
if (cJSON_IsString(led) && (led->valuestring != NULL))
{
  return console_parse_led_token(led->valuestring, cmd, error_msg);
}

if (cJSON_IsArray(led))
{
  const cJSON *element = NULL;
  cJSON_ArrayForEach(element, led)
  {
    if (!cJSON_IsString(element) || (element->valuestring == NULL)) { 报错; return false; }
    if (!console_parse_led_token(element->valuestring, cmd, error_msg)) { return false; }
    parsed = true;
  }
  if (!parsed) { 报错; return false; }   // 空数组 []
  return true;
}
```

`cJSON_ArrayForEach(element, array)` 是 cJSON 提供的遍历宏，内部是链表遍历（`for(element = array->child; element; element = element->next)`）。**不再需要手动判断逗号、方括号、空格**——JSON 语法的分隔符处理全部内化进了 cJSON_Parse。

`cJSON_IsString(element) && (element->valuestring != NULL)` 双重检查：IsString 判断节点类型，valuestring 非空判断值有效——cJSON 文档建议两个都查，防御极端情况（如节点类型对但值被置空）。

**空数组 `[]` 的处理**：08 里空数组在 while 循环第一轮看到 `]` 就 break，cmd.led_count 仍为 0，最终靠 `(cmd->led_count > 0U)` 收尾检查拦截。09 用 `parsed` 布尔标记区分"循环根本没进去"和"循环进去了但全部失败"——`if (!parsed)` 对应空数组，语义更清晰。

### token 解析器签名变化

```c
// 08：游标方式——输入输出都是指针位置
static bool console_parse_led_token(const char **cursor, LedCommand_t *cmd);

// 09：已提取的字符串 + error 出参
static bool console_parse_led_token(const char *token, LedCommand_t *cmd, const char **error_msg);
```

**`const char **cursor` → `const char *token`**：08 需要回写游标消耗位置，09 不需要——cJSON 已经把字符串提取好了（`element->valuestring`），传进来的就是干净的零结尾字符串。**游标机制整体消失的标志**。

**新增 `const char **error_msg` 出参**：08 没有这个（错误隐式约定，调用方猜）。09 把错误原因显式传出——失败时 `*error_msg = "unknown led"`，process 层拿到后直接喂给 report_error。**上位机收到的 msg 更精准**。

函数内部逻辑不变：ALL 展开 → find_led_by_token 查表 → add_led_index 追加/去重。

## 09 新增的两个函数

### console_state_from_json——cJSON 类型分派器

```c
static bool console_state_from_json(const cJSON *item, bool *value);
```

**作用**：根据 cJSON 节点的实际类型（bool / number / string），提取布尔状态值。是 08 手写 `parse_state_token` 三分支的库版等价物。

**形参**：`item` — cJSON 节点指针（由 cJSON_ArrayForEach 遍历得到，或由 cJSON_GetObjectItem 直接取得）；`value` — 输出参数，提取的 bool 值写入此处。

**输入/输出**：输入 = cJSON 节点（可能是 `true`/`false`、`1`/`0`、`"on"`/`"off"`）；输出 = bool 值写入 `*value`，返回 true 表示成功。

**调用者**：`console_parse_state_token`，对每个 state 元素调用一次。

```c
static bool console_state_from_json(const cJSON *item, bool *value)
{
  if (cJSON_IsBool(item))      { *value = cJSON_IsTrue(item);         return true; }
  if (cJSON_IsNumber(item))    { *value = (item->valuedouble != 0.0); return true; }
  if (cJSON_IsString(item)...) { return console_state_from_string(item->valuestring, value); }
  return false;
}
```

三个分支对应三种 JSON 值类型：

- **`cJSON_IsBool`**：输入是 `true`/`false`（JSON 布尔字面量）。`cJSON_IsTrue` 直接返回布尔值。**这是 09 新增的输入形态**——08 手写解析器不认 `true`/`false`，只认 `1`/`0` 和 `"on"`/`"off"`
- **`cJSON_IsNumber`**：输入是 `1`/`0`（JSON 数字）。`item->valuedouble != 0.0` 判断——cJSON 内部用 double 存所有数字（JSON 规范只有一种 number 类型）。F401RE 无硬件 FPU，浮点比较走软件仿真，开销几微秒，对人机交互无所谓
- **`cJSON_IsString`**：输入是 `"on"`/`"off"`（JSON 字符串）。提取 `valuestring` 后交给 `console_state_from_string` 做文本匹配

**语法剥离**：08 的 parse_state_token 同时处理语法（扫字符到边界）和语义（翻译成 bool），09 把语法交给 cJSON，这里只剩语义分派。**这就是「库替你做了词法分析，你只管语义」的直观感受。**

### console_state_from_string——字符串→bool 翻译

```c
static bool console_state_from_string(const char *token, bool *value);
```

**作用**：把字符串形式的状态值翻译成 bool。支持关键词（`on`/`true`/`enable` → true，`off`/`false`/`disable` → false）和数字字符串（`"1"` → true，`"0"` → false，`"-1"` → false）。

**形参**：`token` — cJSON 提取的干净字符串（无引号，已零结尾）；`value` — 输出参数。

**输入/输出**：输入 = 字符串（如 `"on"`、`"1"`、`"-5"`）；输出 = bool 值写入 `*value`，返回 true 表示成功。

**调用者**：`console_state_from_json`，当 cJSON 节点类型为 String 时调用。

这个函数**与 08 手写版几乎一样**（str_case_equal 匹配关键词 + 逐位数字解析），但因为输入是已提取的干净字符串而非游标，少了引号扫描和闭引号消费那段代码。**它证明：即便用了库，字符串→bool 的翻译层永远要手写。**

关键知识点（同 08 逐类精读 [[08_rocketpi_uart_control_led_3#console_parse_state_token|parse_state_token]]）：
- `'5'-'0' = 5`：ASCII '0'~'0x39' 连续排列，减 '0' 得数值
- 负号语义化：`-1` → false（负数按「关」处理）
- `isdigit((unsigned char)*ptr)` 的 unsigned char 铁律

## process_command_buffer 对比——堆分配的代价

```c
// 08 版：无库，无堆，整个过程零分配
LedCommand_t command = {0};
if (!console_parse_led_targets(g_rx_buffer, &command)) { 报错; return; }
if (!console_parse_state_values(g_rx_buffer, &command)) { 报错; return; }
if (!(count 校验)) { 报错; return; }
console_apply_command(&command);
console_report_result(&command);

// 09 版：引入堆分配，必须配对 Delete
cJSON *root = cJSON_Parse(g_rx_buffer);      // ← 堆分配开始
if (root == NULL) { report_error("invalid json"); return; }

LedCommand_t command = {0};
const char *error_msg = NULL;

if (!console_parse_led_targets(root, &command, &error_msg))  // ← 失败路径 1
{
  cJSON_Delete(root);                       // ← 必须在 return 前释放！
  report_error(error_msg);
  return;
}
if (!console_parse_state_values(root, &command, &error_msg)) // ← 失败路径 2
{
  cJSON_Delete(root);                       // ← 同上
  report_error(error_msg);
  return;
}
cJSON_Delete(root);                          // ← 成功路径也要释放

// ...count 校验、apply、report（此时 root 已释放，command 独立存在）
```

**cJSON_Delete(root) 出现三次**——两条失败路径和一条成功路径，各带一次释放。漏掉任何一个就是内存泄漏。**引入堆分配后，每条 return 路径都必须带上释放责任。** 08 没有这个问题（command 在栈上，函数返回自动回收）。

root 在 count 校验之前就释放了——因为这条规则只看 LedCommand_t（state_count vs led_count），与 cJSON 无关。但 root 此时已释放，若 count 校验里意外引用 root 就是 use-after-free。08 没有这类问题（command 和字符串缓冲共存于栈/全局，无生命周期差异）。

## cJSON 带来的新功能清单

| 新功能 | 08 能否实现 | 09 实现方式 |
|---|---|---|
| **语法容错**：`{ "led" : "B" , "state" : 1 }` 键值间随意加空格 | 不能（strstr 锚定后手动跳空白，多层嵌套空格仍会漏） | cJSON_Parse 内部词法器处理任意空白 |
| **畸形 JSON 识别**：`{invalid` 直接报错 | 不能（08 的 strstr 找不到键就返回 NULL，报 "missing led field"，用户看不出是 JSON 语法错） | `cJSON_Parse` 返回 NULL → 报 "invalid json"，上位机知道是格式问题 |
| **多余字段静默忽略**：`{"led":"B","state":1,"extra":99}` | 不能（strstr 会找到 extra 里的字符，行为不可预测） | cJSON 按键名查节点，`"extra"` 没人查就不会被处理 |
| **bool 字面量**：`"state":true` | 不能（08 只认数字和字符串） | `cJSON_IsBool` + `cJSON_IsTrue` |
| **嵌套扩展能力**：将来命令升级为 `{"led":{"name":"B","blink":100}}` | 不能（手写解析器深度写死为 2 层） | cJSON 天然支持任意深度嵌套 |
| **大小写不敏感键查找**（cJSON 可选） | 手动枚举（08 查 `"led"` 再查 `"LED"`） | `cJSON_GetObjectItemCaseSensitive` 名字虽带 "CaseSensitive"，但 cJSON 也提供 `cJSON_GetObjectItem` 做大小写不敏感版本 |

## cJSON 的代价清单

| 代价 | 数值 | 影响 |
|---|---|---|
| Flash | cJSON.c 编译后约 8~10KB | F401RE 有 512KB，绰绰有余；小片子（如 F0/F1 32~128KB）需要评估 |
| 堆内存 | 每个 JSON 节点约 32 字节，一条 `"led":["B","G"],"state":[1,0]` 约 7 节点 ≈ 224 字节 | 必须 `cJSON_Delete` 与 `cJSON_Parse` 严格配对；漏配对 = 内存泄漏 |
| 栈深度 | cJSON_Parse 递归深度 = JSON 嵌套层级 | 恶意深嵌套可打爆栈（教学代码无深度限制，工程代码要加） |
| 移植性 | 需要实现 `malloc`/`free` | FreeRTOS 下要用 `pvPortMalloc` 挂 cJSON 的 Hooks；裸机用标准 libc malloc |

## 解析不碰硬件、执行不看字符串——中间表示的隔离价值

09 最有力的证据是：**解析层整体替换了，执行层零改动**。set_led_state / apply_command / report_result / report_error 与 08 一字不差——它们只认 LedCommand_t，不关心这个结构体是手写的游标解析填的、还是 cJSON 库填的。

这意味着：
- 08 → 09 的迁移**只需要替换 parse 系列函数**，执行层/汇报层/配置表/中间表示全部不动
- 反过来，如果将来 09 的 cJSON 太重，要换成更轻的库（如 [jsmn](https://github.com/zserge/jsmn)、cJson-min），也只需要重写 parse 层
- **中间表示（LedCommand_t）是接口，解析器是实现，接口稳定则实现可替换**——这就是 [[08_rocketpi_uart_control_led|08 主笔记]] 讲的"中间表示隔离价值"的库版实证

## 与前例连线

- [[08_rocketpi_uart_control_led|08 主笔记]] — 手写版完整实现，本篇的对照基线
- [[08_rocketpi_uart_control_led_3|08 函数逐类精读]] — 消失的 4 个手写函数的写法详解
- [[08_rocketpi_uart_control_led_2|08 数据结构与 API 设计]] — LedCommand_t / LedConfig_t 的设计推演（09 原样复用）
- [[08_rocketpi_uart_control_led_4|08 串口文本协议谱系]] — "无结构命令 → 完整结构化数据交换"的第四层
- [[07_rocketpi_uart_echo]] — 接收侧的工程版方案，可替换 09 的阻塞接收

## 值得记住的三个设计词

- **库替你做词法，你只管语义**：cJSON_Parse 处理了引号、转义、空白、嵌套、类型判断；09 的解析函数只剩 "按类型提取值 → 查表 → 写入 cmd" 的纯语义逻辑
- **堆分配配对**：引入 cJSON_Parse 后，每条 return 路径都必须带 cJSON_Delete——这是"用库"的隐性成本，不是免费的
- **接口稳定则实现可替换**：LedCommand_t 是接口、解析器是实现——08 换 09 只换实现，执行层零改动；将来换别的 JSON 库也同理
