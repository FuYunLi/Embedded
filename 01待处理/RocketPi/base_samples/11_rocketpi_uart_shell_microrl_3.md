---
status: done
created: 2026-09-12
tags:
  - embedded/shell
  - c/api-design
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/11_rocketpi_uart_shell_microrl/main.c"
---

# 命令系统与补全机制：main.c 的应用层实现

> [[11_rocketpi_uart_shell_microrl|11 主笔记]] 的架构全景提到了命令表和三个回调，[[11_rocketpi_uart_shell_microrl_2|microrl 库核心解析]] 讲了库内部怎么处理字节。这篇拆解 main.c 应用层：命令表怎么设计、execute 回调怎么分发、Tab 补全怎么根据上下文提供候选词。

## 三组表驱动数据

main.c 用了三张表来组织命令系统，和 [[08_rocketpi_uart_control_led|08]] 的 `g_led_config` 同思想——数据进表、逻辑进函数。

### 命令描述表

```c
static const shell_cmd_desc_t kShellCmdTable[] = {
    {"help",    "List available commands"},
    {"version", "Show shell information"},
    {"echo",    "Echo back the provided text"},
    {"led",     "Control on-board LEDs"},
};
```

**作用**：`help` 命令遍历这张表打印命令列表。加新命令时这里加一行 + `shell_execute` 加一个 `strcmp` 分支 + `shell_complete` 加补全逻辑。

### LED 描述表

```c
static const led_desc_t kLedTable[] = {
    {"blue", LED_B_GPIO_Port, LED_B_Pin},
    {"green", LED_G_GPIO_Port, LED_G_Pin},
    {"pink", LED_P_GPIO_Port, LED_P_Pin},
};
```

**作用**：`led` 命令查这张表找 LED 名→端口→引脚。和 08 的 `g_led_config` 同构，但名字更友好（`blue` 而非 `B`）——shell 是人直接敲的，不需要缩写。

### LED 动作表

```c
static const char *kLedActionTable[] = {"on", "off", "toggle"};
```

**作用**：Tab 补全用——敲 `led blue ` 按 Tab 时，从这张表里找候选。

## 三个回调函数

### shell_print：输出回调

```c
static void shell_print(const char *str){
    if ((str == NULL) || (huart2.gState == HAL_UART_STATE_RESET)) return;
    while (*str != '\0') {
        if (*str == '\n') {
            const uint8_t newline[2] = {'\r', '\n'};
            HAL_UART_Transmit(&huart2, (uint8_t *)newline, sizeof(newline), HAL_MAX_DELAY);
        } else {
            uint8_t ch = (uint8_t)(*str);
            HAL_UART_Transmit(&huart2, &ch, 1, HAL_MAX_DELAY);
        }
        str++;
    }
}
```

**作用**：microrl 的所有输出（提示符、命令回显、补全列表、光标移动）都经过这个函数。

**`\n` → `\r\n` 转换**：microrl 内部用 `\n`（config.h 配置 `_ENDL_LF`），但串口终端需要 `\r\n` 两个字符才能正确换行。shell_print 自动转换——**microrl 不知道终端需要 `\r\n`，shell_print 负责适配**。

**`huart2.gState` 检查**：如果 UART 还没初始化（gState == RESET），不发送——防止初始化阶段调 microrl_init（`_ENABLE_INIT_PROMPT` 打印提示符）时卡死。

### shell_execute：命令执行回调

```c
static int shell_execute(int argc, const char * const * argv){
    if (argc <= 0) return 0;

    if (strcmp(argv[0], "help") == 0) {
        // 遍历 kShellCmdTable 打印命令列表
        return 0;
    }
    if (strcmp(argv[0], "version") == 0) {
        shell_print("microrl "); shell_print(MICRORL_LIB_VER);
        shell_print_line(" on USART2 (115200 8N1)");
        return 0;
    }
    if (strcmp(argv[0], "echo") == 0) {
        for (int i = 1; i < argc; ++i) {
            shell_print(argv[i]);
            if (i < (argc - 1)) shell_print(" ");
        }
        shell_print(ENDL);
        return 0;
    }
    if (strcmp(argv[0], "led") == 0) {
        return shell_cmd_led(argc, argv);
    }

    shell_print("Unknown command: "); shell_print(argv[0]); shell_print(ENDL);
    shell_print_line("Type 'help' to list commands.");
    return 0;
}
```

**microrl 已经分好词了**——`argv[0]` 是命令名，`argv[1]` 开始是参数，`argc` 是 token 总数。应用层只需 strcmp 匹配命令名然后分发。对比 08 要自己手写 JSON 解析器（几十行），11 的命令分发只需几行 strcmp。

**echo 的实现**：`argv[1]` 到 `argv[argc-1]` 逐个打印，中间加空格。注意 argv 里的空格已经被 microrl 替换成 `\0`，但原始空格位置对 echo 来说不影响——它只打印每个 token 的内容。

### shell_complete：Tab 补全回调

```c
static char **shell_complete(int argc, const char * const * argv){
    size_t idx = 0;
    memset((void *)s_shell_completion, 0, sizeof(s_shell_completion));

    const char *current = "";
    if (argc > 0) {
        current = argv[argc - 1];   // 当前正在输入的词
        if (current == NULL) current = "";
    }

    // 第一个词 → 补全命令名
    if (argc <= 1) {
        for (i: kShellCmdTable)
            if (shell_prefix_match(current, name))
                idx = shell_add_completion(name, idx);
        return s_shell_completion;
    }

    // 不是 led 命令 → 不补全
    if (strcmp(argv[0], "led") != 0) return s_shell_completion;

    // led 命令的第二个词 → 补全 LED 名
    if (argc == 2) {
        for (i: kLedTable)
            if (shell_prefix_match(current, led.name))
                idx = shell_add_completion(led.name, idx);
    }
    // led 命令的第三个词 → 补全动作
    else if (argc == 3) {
        for (i: kLedActionTable)
            if (shell_prefix_match(current, action))
                idx = shell_add_completion(action, idx);
    }

    return s_shell_completion;
}
```

**上下文感知**：根据 argc 和 argv[0] 决定补全什么——

| 用户输入 | argc | argv | 补全内容 |
|---|---|---|---|
| `[Tab]` | 0 | — | 命令名（help/version/echo/led） |
| `h[Tab]` | 1 | ["h"] | 命令名前缀匹配 "h" → ["help"] |
| `led [Tab]` | 2 | ["led",""] | LED 名（blue/green/pink） |
| `led b[Tab]` | 2 | ["led","b"] | 前缀匹配 "b" → ["blue"] |
| `led blue [Tab]` | 3 | ["led","blue",""] | 动作（on/off/toggle） |
| `echo [Tab]` | 2 | ["echo",""] | 不补全（没匹配分支） |

**当前词 `current`**：取 `argv[argc-1]`——最后一个 token。如果光标在空格后（如 `led `），microrl 的 split 会追加一个空 token，`current` 就是空字符串，前缀匹配全部命中。

### shell_prefix_match：前缀匹配

```c
static int shell_prefix_match(const char *token, const char *candidate){
    if ((token == NULL) || (*token == '\0')) return 1;   // 空 token → 全部匹配
    size_t prefix_len = strlen(token);
    return strncmp(candidate, token, prefix_len) == 0;
}
```

**空 token 返回 1**：光标在空格后时，所有候选词都匹配——列出全部选项。

### shell_add_completion：追加候选词

```c
static size_t shell_add_completion(const char *candidate, size_t index){
    if ((candidate != NULL) && (index < (ARRAY_SIZE(s_shell_completion) - 1U))) {
        s_shell_completion[index++] = candidate;
    }
    return index;
}
```

`s_shell_completion` 是静态数组，最后一个元素保持 NULL（microrl 用 NULL 判断列表结束）。返回值是下一个可用索引——**链式调用**：`idx = shell_add_completion(a, idx); idx = shell_add_completion(b, idx);`

## shell_cmd_led：LED 命令的完整实现

```c
static int shell_cmd_led(int argc, const char * const * argv){
    if (argc < 3) { shell_print_led_usage(); return -1; }

    const led_desc_t *led = shell_find_led(argv[1]);
    if (led == NULL) {
        shell_print("Unknown LED name: "); shell_print(argv[1]); shell_print(ENDL);
        shell_print_led_usage();
        return -1;
    }

    const char *action = argv[2];
    if (shell_str_casecmp(action, "on") == 0) {
        HAL_GPIO_WritePin(led->port, led->pin, LED_ON_STATE);
        shell_print("LED "); shell_print(led->name); shell_print_line(" is ON");
        return 0;
    }
    if (shell_str_casecmp(action, "off") == 0) {
        HAL_GPIO_WritePin(led->port, led->pin, LED_OFF_STATE);
        shell_print("LED "); shell_print(led->name); shell_print_line(" is OFF");
        return 0;
    }
    if (shell_str_casecmp(action, "toggle") == 0) {
        HAL_GPIO_TogglePin(led->port, led->pin);
        shell_print("LED "); shell_print(led->name); shell_print_line(" toggled");
        return 0;
    }

    shell_print("Unknown LED action: "); shell_print(action); shell_print(ENDL);
    shell_print_led_usage();
    return -1;
}
```

**查表 → 匹配动作 → 操作 GPIO → 反馈文本**。和 08 的 `console_apply_command` 同样是表驱动，但 11 多了 `toggle`（HAL_GPIO_TogglePin）——08 的 JSON 命令只有开/关，shell 命令更灵活。

**错误处理**：参数不足→打印用法；LED 名不匹配→打印提示+用法；动作不匹配→打印提示+用法。每次错误都给用户可操作的反馈（"该输什么"），不只是报错。

## shell_str_casecmp：大小写不敏感比较

```c
static int shell_str_casecmp(const char *lhs, const char *rhs){
    while ((*lhs != '\0') && (*rhs != '\0')) {
        char lc = (*lhs >= 'A' && *lhs <= 'Z') ? (char)(*lhs + ('a' - 'A')) : *lhs;
        char rc = (*rhs >= 'A' && *rhs <= 'Z') ? (char)(*rhs + ('a' - 'A')) : *rhs;
        if (lc != rc) return (int)(lc - rc);
        ++lhs; ++rhs;
    }
    return (int)((unsigned char)*lhs - (unsigned char)*rhs);
}
```

和 [[08_rocketpi_uart_control_led_3|08 的 str_case_equal]] 思路相同但返回值不同：08 返回 bool（相等/不相等），11 返回 int（差值，可做排序）。实现上 11 用手工大小写转换（`+ ('a'-'A')`），08 用 `toupper`——11 更轻量但只处理 ASCII 字母。

## 从 08 到 11 的命令格式演变

| 维度 | 08 JSON | 11 Shell |
|---|---|---|
| 命令示例 | `{"led":"B","state":1}` | `led blue on` |
| 分词 | 手写 JSON 解析器（~100 行） | microrl split（~20 行） |
| 字段查找 | strstr 锚定 + 游标推进 | argv 下标直接取 |
| 类型判断 | cJSON_IsString/IsNumber/IsBool | strcmp/strcasecmp |
| 扩展性 | 加字段改解析器 | 加参数改 argc 检查 |
| 人可读性 | 差（要记忆 JSON 格式） | 好（自然语言） |
| 机器可读性 | 好（结构化） | 差（需要文档） |
| 嵌套能力 | 有（JSON 数组/对象） | 无（空格分隔，一层） |

**选择依据**：人机交互用 shell（11），机器间通信用 JSON（08/09）。两种格式不是优劣问题，是适用场景问题。

## 关联笔记

- [[11_rocketpi_uart_shell_microrl]] — 主笔记：架构全景与中断接收
- [[11_rocketpi_uart_shell_microrl_1|中断接收与环形缓冲]] — 接收层升级
- [[11_rocketpi_uart_shell_microrl_2|microrl 库核心解析]] — 库内部实现
- [[08_rocketpi_uart_control_led|08 主笔记]] — 表驱动命令系统的前身
- [[08_rocketpi_uart_control_led_3|08 函数逐类精读]] — str_case_equal 对照
- [[09_rocketpi_uart_control_led_cjson|09 主笔记]] — JSON 格式命令的对照
