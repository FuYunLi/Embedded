---
status: done
created: 2026-09-12
tags:
  - c/cjson
  - c/text-parsing
  - embedded/library
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/09_rocketpi_uart_control_led_cjson/main.c"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/09_rocketpi_uart_control_led_cjson/cJSON.c"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/09_rocketpi_uart_control_led_cjson/cJSON.h"
---

# cJSON 背景、移植与 API 速查

> [[09_rocketpi_uart_control_led_cjson|09 主笔记]] 用 cJSON 完成了 LED 控制命令解析，但没讲 cJSON 本身是什么、怎么移植到新工程、有哪些 API 可用。这篇从最基础的"程序为什么需要处理文本"出发，补全这些背景。

## 程序为什么需要处理文本

两个程序交换信息时，必须把内存里的结构体（数字、布尔、数组）变成一串字节发过去，对方收到后再从字节还原成结构体。**把结构体变成字节 = 编码（序列化），把字节变回结构体 = 解码（反序列化）**。

最原始的编码方式就是文本：`{"led":"B","state":1}`——人能直接读、串口助手能直接显示、调试时一眼就能看出对错。二进制编码（如 `0x01 0x01`）更省带宽，但人看不懂，调试必须先翻译。[[08_rocketpi_uart_control_led_4|08 的协议谱系笔记]] 讲过：**调试期几乎全用文本，量产期才换二进制**。

文本交换的核心问题是：双方必须提前约定格式——键怎么写、值怎么写、怎么分隔、怎么嵌套。这个"约定"就是数据交换格式，JSON 是其中最流行的一种。

## 键值对：文本结构化的最小单元

**键值对（key-value pair）**是所有结构化文本的基础：一个名字（键）对应一个数据（值），中间用分隔符隔开。

```
led = B            ← 最简单的键值对（INI/TOML 风格）
"led" : "B"        ← JSON 风格（键和字符串值都加引号）
led: B             ← YAML 风格（不加引号）
```

键值对解决的问题是"位置无关"：`{"state":1,"led":"B"}` 和 `{"led":"B","state":1}` 语义相同——靠键名认值，不靠排列顺序。对比 CSV（`B,1`）必须约定"第一列是灯、第二列是状态"，顺序一变就错。

多个键值对用逗号分隔、用花括号包裹，就是一个 JSON 对象。数组用方括号包裹、元素用逗号分隔。这两种容器可以任意嵌套，理论上能表达任何复杂结构。

## JSON 语法速览

JSON（JavaScript Object Notation）由 Douglas Crockford 在 2001 年提出，基于 JavaScript 对象字面量语法，但作为数据格式与语言无关。ECMA-404 标准定义了正式语法（2013 年）。

六种数据类型：

| 类型 | 示例 | cJSON 判断宏 |
|---|---|---|
| 对象（object） | `{"led":"B","state":1}` | `cJSON_IsObject` |
| 数组（array） | `["B","G","P"]` | `cJSON_IsArray` |
| 字符串（string） | `"B"` | `cJSON_IsString` |
| 数字（number） | `1`、`3.14`、`-5` | `cJSON_IsNumber` |
| 布尔（bool） | `true`、`false` | `cJSON_IsBool` |
| 空值（null） | `null` | `cJSON_IsNull` |

语法规则极简：
- 对象用 `{}` 包围，键值对用 `,` 分隔，键和值之间用 `:`
- 数组用 `[]` 包围，元素用 `,` 分隔
- 字符串必须用双引号（`"`），数字不加引号
- 空白（空格/制表/换行）在结构符号之间任意出现，不影响语义

08 的手写解析器只实现了对象、数组、字符串、数字的子集；09 换 cJSON 后六种全支持，多余字段自动忽略，畸形 JSON 也能检测。

## cJSON 是什么

cJSON 是一个用纯 ANSI C（C89）编写的 JSON 解析与生成库，由 Dave Gamble 于 2009 年前后创建，MIT 许可证。设计目标：**一个 .c + 一个 .h，零外部依赖，能放进任何 C 项目**。

| 特性 | 说明 |
|---|---|
| 文件 | `cJSON.c`（约 80KB / 2600 行）+ `cJSON.h`（约 6KB） |
| 标准 | C89，无 C99 特性，所有编译器通吃 |
| 依赖 | 只依赖标准库 `<string.h>`、`<stdlib.h>`、`<math.h>`、`<ctype.h>`、`<float.h>`、`<limits.h>` |
| 内存 | 全部走 `malloc`/`free`，可挂自定义 Hooks |
| 线程安全 | Parse/Delete 各自独立、不共享全局状态，多线程可并行（各自 root） |
| 嵌入式适配 | 可挂 `pvPortMalloc`/`vPortFree`（FreeRTOS）或自定义分配器 |
| 官方仓库 | [github.com/cJSON/cJSON](https://github.com/cJSON/cJSON)（社区维护，Dave Gamble 已移交） |

嵌入式项目广泛使用它：单文件集成、C89 兼容、不依赖 POSIX/操作系统 API、内存分配器可替换。RT-Thread、ESP-IDF、LiteOS 等主流嵌入式框架都内置或可选 cJSON。

## 移植到新工程

cJSON 的"移植"本质就是把两个文件丢进工程。具体步骤：

### 第一步：获取文件

从 [cJSON 官方仓库](https://github.com/cJSON/cJSON) 下载 `cJSON.c` 和 `cJSON.h`，放到工程的任意目录（09 例里与 main.c 同级）。不需要其他文件。

### 第二步：加入编译

在 IDE/构建系统中把 `cJSON.c` 加入源文件列表：
- **Keil MDK**：右键 Source Group → Add Existing Files → 选 cJSON.c
- **STM32CubeIDE**：拖入 src 目录，自动识别
- **Makefile/CMake**：`SRCS += cJSON.c` 或 `add_library(cJSON cJSON.c)`

### 第三步：处理内存分配器（可选）

默认情况下 cJSON 使用标准库的 `malloc`/`free`。裸机程序（无 RTOS）一般自带 libc 的 malloc 实现，直接可用。

**FreeRTOS 环境**需要替换分配器：

```c
#include "cJSON.h"

void cJSON_init_hooks(void)
{
    cJSON_Hooks hooks = {
        .malloc_fn = pvPortMalloc,
        .free_fn   = vPortFree
    };
    cJSON_InitHooks(&hooks);
}
```

在调用 `cJSON_Parse` 之前调一次 `cJSON_init_hooks()` 即可。此后 cJSON 内部所有节点分配都走 FreeRTOS 的堆管理，内存统计工具能正确统计。

### 第四步：处理栈深度（风险评估）

cJSON_Parse 内部递归深度 = JSON 嵌套层级。`{"a":{"b":{"c":1}}}` 嵌套 3 层，递归 3 层，每层约 100~200 字节栈帧。**恶意深嵌套可打爆栈**。

09 的命令格式嵌套不超过 2 层，栈消耗可忽略。如果对端输入不可控（如用户可通过串口发任意 JSON），工程代码应加深度限制——cJSON 本身无此机制，需要在 Parse 之前检查输入字符串的嵌套深度（数花括号/方括号的净深度）。

## cJSON 对外 API（按常用度排序）

cJSON 的 API 设计思路：把 JSON 字符串解析成一棵内存中的树（每个节点是一个 `cJSON` 结构体），然后按键名/下标在树上查询，最后用完释放整棵树。下面按实际使用频率排序，每个 API 给出：作用、形参、返回值、用法示例。

### 第一梯队：几乎每个项目都用（Parse / Delete / GetObjectItem）

#### cJSON_Parse——把字符串变成树

```c
cJSON *cJSON_Parse(const char *value);
```

**作用**：把一个 JSON 字符串解析成一棵 cJSON 节点树，返回树的根节点指针。解析失败返回 NULL。

**形参**：`value` — 以 `\0` 结尾的 JSON 字符串。

**返回值**：`cJSON *` 根节点（成功），NULL（失败，可通过 `cJSON_GetErrorPtr()` 获取出错位置）。

**示例**：
```c
const char *json = "{\"led\":\"B\",\"state\":1}";
cJSON *root = cJSON_Parse(json);
if (root == NULL)
{
    const char *error = cJSON_GetErrorPtr();
    // error 指向出错位置
    return;
}
// ... 在树上查询 ...
cJSON_Delete(root);  // 用完必须释放
```

> [!WARNING]
> `cJSON_Parse` 内部调用 `malloc` 为每个 JSON 节点分配内存。**必须与 `cJSON_Delete` 配对**，否则内存泄漏。

#### cJSON_Delete——释放整棵树

```c
void cJSON_Delete(cJSON *item);
```

**作用**：递归释放 cJSON 节点树的所有内存。传入 NULL 安全（什么都不做）。

**形参**：`item` — 根节点指针（或任意子节点，会递归释放其所有子树）。

**示例**：
```c
cJSON *root = cJSON_Parse(json);
// ... 使用 ...
cJSON_Delete(root);  // 释放整棵树，root 指针失效
root = NULL;         // 防悬垂指针，好习惯
```

#### cJSON_GetObjectItem——按键名取子节点

```c
cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string);
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *string);
```

**作用**：在 JSON 对象中按键名查找子节点。`CaseSensitive` 版本区分大小写，无 CaseSensitive 的版本大小写不敏感。

**形参**：`object` — cJSON 对象节点；`string` — 键名字符串。

**返回值**：找到的子节点指针，NULL 表示没找到。

**示例**：
```c
cJSON *led = cJSON_GetObjectItemCaseSensitive(root, "led");
if (led == NULL)
{
    // 没有 "led" 键
}
else if (cJSON_IsString(led) && (led->valuestring != NULL))
{
    printf("led = %s\n", led->valuestring);  // 输出: led = B
}
```

**09 的用法**：先查 `"led"`，NULL 再查 `"LED"`（手动大小写兑底），因为用的是 CaseSensitive 版本。

### 第二梯队：按类型取值（IsXxx + value 字段）

cJSON 节点的值不通过 getter 函数获取，而是直接读节点结构体的成员字段。但必须先用 `IsXxx` 宏判断类型再读——**读错字段是未定义行为**。

| 类型 | 判断宏 | 取值字段 | 取值示例 |
|---|---|---|---|
| 字符串 | `cJSON_IsString(node)` | `node->valuestring` | `"B"` → `valuestring` = `"B"` |
| 数字 | `cJSON_IsNumber(node)` | `node->valuedouble` 或 `node->valueint` | `1` → `valuedouble` = `1.0`，`valueint` = `1` |
| 布尔 | `cJSON_IsBool(node)` | `cJSON_IsTrue(node)` | `true` → `cJSON_IsTrue` 返回 1 |
| 空值 | `cJSON_IsNull(node)` | （无值字段） | `null` → 只判断类型 |

> [!TIP]
> `valueint` 是 `valuedouble` 的 int 截断版本（`(int)valuedouble`），不是独立的整型存储。JSON 规范只有一种 number 类型，cJSON 内部统一用 double。

**示例**：
```c
cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
if (state != NULL)
{
    if (cJSON_IsNumber(state))
    {
        int val = state->valueint;       // 1
        double dbl = state->valuedouble; // 1.0
    }
    else if (cJSON_IsBool(state))
    {
        bool on = cJSON_IsTrue(state);   // true
    }
    else if (cJSON_IsString(state))
    {
        const char *s = state->valuestring; // "on"
    }
}
```

**09 的用法**：`console_state_from_json` 就是按这个顺序判断的（Bool → Number → String）。

### 第三梯队：遍历数组（IsArray + ArrayForEach）

#### cJSON_IsArray——判断节点是否为数组

```c
cJSON_bool cJSON_IsArray(const cJSON *item);
```

**作用**：判断节点类型是否为数组。返回非零表示是数组。

#### cJSON_ArrayForEach——遍历数组元素

```c
cJSON_ArrayForEach(element, array)
```

**作用**：宏，展开为 `for(element = array->child; element; element = element->next)` 的链表遍历。cJSON 内部用单链表存数组/对象的子节点。

**示例**：
```c
cJSON *led_array = cJSON_GetObjectItemCaseSensitive(root, "led");
if (cJSON_IsArray(led_array))
{
    cJSON *element = NULL;
    cJSON_ArrayForEach(element, led_array)
    {
        if (cJSON_IsString(element) && (element->valuestring != NULL))
        {
            printf("  led: %s\n", element->valuestring);
        }
    }
}
```

输入 `{"led":["B","G"]}` → 输出：
```
  led: B
  led: G
```

**09 的用法**：`console_parse_led_targets` 里用 `cJSON_ArrayForEach` 遍历 led 数组，逐元素调用 `console_parse_led_token`。

### 第四梯队：创建与生成（AddXxx + Print）

这几个 API 用于**生成** JSON（把内存中的数据变成 JSON 字符串），09 没用到（09 的应答仍然手写 snprintf），但在其他场景（如配置文件保存、上报数据）很常用。

#### cJSON_CreateString / cJSON_CreateNumber / cJSON_CreateBool

```c
cJSON *cJSON_CreateString(const char *string);
cJSON *cJSON_CreateNumber(double num);
cJSON *cJSON_CreateBool(cJSON_bool boolean);
```

**作用**：创建一个值节点（不挂到任何父节点下）。返回新节点指针。

#### cJSON_CreateObject / cJSON_CreateArray

```c
cJSON *cJSON_CreateObject(void);
cJSON *cJSON_CreateArray(void);
```

**作用**：创建一个空对象/空数组节点。

#### cJSON_AddItemToObject / cJSON_AddItemToArray

```c
cJSON_bool cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item);
cJSON_bool cJSON_AddItemToArray(cJSON *array, cJSON *item);
```

**作用**：把一个节点挂到对象（按键名）或数组（追加到末尾）下。**节点的所有权转移给父节点**——父节点被 Delete 时子节点一并释放，调用方不应再单独 Delete 子节点。

#### cJSON_Print / cJSON_PrintUnformatted

```c
char *cJSON_Print(const cJSON *item);          // 带缩进格式化
char *cJSON_PrintUnformatted(const cJSON *item); // 紧凑无空白
```

**作用**：把 cJSON 节点树序列化成 JSON 字符串。返回 `malloc` 分配的字符串，**调用方必须 `free()` 释放**。

**生成 JSON 的完整示例**：
```c
cJSON *root = cJSON_CreateObject();
cJSON_AddItemToObject(root, "status", cJSON_CreateString("ok"));

cJSON *led_array = cJSON_CreateArray();
cJSON_AddItemToArray(led_array, cJSON_CreateString("B"));
cJSON_AddItemToArray(led_array, cJSON_CreateString("G"));
cJSON_AddItemToObject(root, "led", led_array);

char *json_str = cJSON_PrintUnformatted(root);
// json_str = {"status":"ok","led":["B","G"]}

console_send_string(json_str);
free(json_str);   // cJSON_Print 分配的字符串要 free
cJSON_Delete(root); // 树也要 Delete
```

> [!WARNING]
> `cJSON_Print` / `cJSON_PrintUnformatted` 返回的字符串是 `malloc` 分配的，必须用 `free` 释放（不是 `cJSON_Delete`）。生成 JSON 有**两次内存责任**：树的 Delete + 字符串的 free。

**09 为什么没用这个**：应答格式固定、字段少，手写 snprintf 零堆分配、输出格式完全可控。`cJSON_PrintUnformatted` 走堆 + 需要 free，对固定格式的应答是多余开销。但若应答格式复杂或动态变化（如嵌套数组长度不定），用 cJSON 生成比手写 snprintf 安全得多。

### 第五梯队：进阶查询（ArrayGetItem / ArraySize / 附加检查）

#### cJSON_GetArraySize——取数组长度

```c
int cJSON_GetArraySize(const cJSON *array);
```

**作用**：返回数组元素个数。

#### cJSON_GetArrayItem——按下标取元素

```c
cJSON *cJSON_GetArrayItem(const cJSON *array, int index);
```

**作用**：按下标（从 0 开始）取数组元素。效率是 O(n)（链表遍历），不是 O(1)。

#### cJSON_HasObjectItem——检查键是否存在

```c
cJSON_bool cJSON_HasObjectItem(const cJSON *object, const char *string);
```

**作用**：等价于 `cJSON_GetObjectItem(...) != NULL`，语义更清晰。

### 第六梯队：错误处理

#### cJSON_GetErrorPtr——获取解析出错位置

```c
const char *cJSON_GetErrorPtr(void);
```

**作用**：`cJSON_Parse` 返回 NULL 后调用，返回指向输入字符串中出错位置的指针。用于调试时定位 JSON 语法错误。

> [!WARNING]
> 返回的是**输入字符串内部的指针**，不是拷贝。如果输入字符串已被释放，返回的是悬垂指针。

## API 使用流程速记

```
解析流程：  cJSON_Parse → cJSON_GetObjectItem → cJSON_IsXxx → 读 value 字段 → cJSON_Delete
生成流程：  cJSON_CreateObject → cJSON_AddItemToObject → cJSON_PrintUnformatted → free 字符串 → cJSON_Delete 树
数组遍历：  cJSON_GetObjectItem → cJSON_IsArray → cJSON_ArrayForEach → cJSON_IsXxx → 读 value
错误定位：  cJSON_Parse 返回 NULL → cJSON_GetErrorPtr → 打印出错位置
```

## 关联笔记

- [[09_rocketpi_uart_control_led_cjson]] — 09 主笔记：解析层替换详解
- [[08_rocketpi_uart_control_led|08 主笔记]] — 手写版对照基线
- [[08_rocketpi_uart_control_led_4|串口文本协议谱系]] — 文本 vs 二进制的全局定位
- [[08_rocketpi_uart_control_led_2|08 数据结构与 API 设计]] — 为什么需要中间表示
