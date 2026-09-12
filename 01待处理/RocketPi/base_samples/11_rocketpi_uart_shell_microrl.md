---
status: done
created: 2026-09-12
tags:
  - stm32/uart
  - embedded/shell
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/11_rocketpi_uart_shell_microrl/main.c"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/11_rocketpi_uart_shell_microrl/microrl.c"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/11_rocketpi_uart_shell_microrl/microrl.h"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/11_rocketpi_uart_shell_microrl/config.h"
---

# 11_rocketpi_uart_shell_microrl

> 代码：`Embedded_Code/01待处理/rocketpi/base_samples/11_rocketpi_uart_shell_microrl/`（config.h + microrl.h + microrl.c + main.c，共约 35KB）
> 硬件：STM32F401RE（RocketPi），USART2 115200
> 目标：用 microrl 库实现一个功能完整的串口命令行——支持退格、方向键、历史记录、Tab 补全、命令执行。是 [[08_rocketpi_uart_control_led|08]] 的成熟形态。

## 交互效果

```
RocketPi USART2 microrl shell ready.

IRin > led blue on
LED blue is ON
IRin > led green toggle
LED green toggled
IRin > led          ← 按 Tab
blue green pink      ← 列出候选
IRin > led bl        ← 按 Tab
IRin > led blue      ← 补全为 blue（唯一候选自动补全+空格）
IRin > ↑             ← 方向键上翻历史
IRin > led blue on   ← 恢复上一条命令
IRin > echo hello world
hello world
IRin > version
microrl 1.5.1 on USART2 (115200 8N1)
IRin > help
Available commands:
  help - List available commands
  version - Show shell information
  echo - Echo back the provided text
  led - Control on-board LEDs
IRin > ^C            ← Ctrl+C
IRin > ^U            ← Ctrl+U 清除当前行
```

## 在 08→11 演变链中的位置

```
08 手写行缓冲 + JSON 解析 + 表驱动执行
  ↓ 行编辑缺失（无退格/历史/补全）
  ↓ 接收阻塞（主循环被串口独占）
  ↓ 命令格式对人不友好（JSON）
11 microrl 行编辑 + 空格分词 + 回调式命令执行
  ✓ 退格/方向键/历史/Tab 补全
  ✓ 中断接收 + 环形缓冲（主循环可干别的）
  ✓ 自然语言命令（led blue on）
```

08 实现了"接收→解析→执行→汇报"的骨架，11 在此基础上补全了**终端协议的其余部分**（ANSI 转义序列处理、行编辑、历史、补全）。两者的关系是：08 教你怎么造轮子，11 教你怎么用现成的轮子。

## 架构：四个文件的分工

| 文件 | 角色 | 行数 |
|---|---|---|
| `config.h` | microrl 库配置（缓冲大小/历史长度/功能开关/换行符/提示符） | ~70 |
| `microrl.h` | 库接口定义 + 键码常量 + microrl_t 结构体 | ~110 |
| `microrl.c` | 库核心：行编辑/历史/补全/ANSI 转义/分词 | ~550 |
| `main.c` | 应用层：中断回调→环形缓冲→microrl→命令表→LED 控制 | ~350 |

**microrl 是一个通用库**——config.h/microrl.h/microrl.c 不需要改，只需在 main.c 里注册回调。应用层与库的接口只有三个回调函数：`print`（输出）、`execute`（命令执行）、`get_completion`（Tab 补全）。

## 主循环与中断的关系

```
中断（ISR）                          主循环（while 1）
────────────                        ────────────────
HAL_UART_RxCpltCallback              shell_process_input()
 ├─ shell_queue_char(字节)            ├─ 从环形缓冲取字节
 ├─ shell_start_rx()                  ├─ \r→\n 转换+吞 \r\n
 └─ 返回                              └─ microrl_insert_char(字节)
                                        ├─ 可打印字符→插入行缓冲
                                        ├─ \n→split分词→execute回调
                                        ├─ Tab→get_completion回调
                                        ├─ ESC [ A/B→历史翻阅
                                        └─ BS/DEL→退格
```

**ISR 尽量短**：中断里只做"存字节+重启接收"两个操作，耗时的行编辑/命令执行全在主循环。这是嵌入式中断处理的黄金法则。详见 [[11_rocketpi_uart_shell_microrl_1|中断接收与环形缓冲]]。

## 环形缓冲区

```c
static volatile uint8_t s_shell_rx_buffer[128];
static volatile uint16_t s_shell_rx_head;   // 写指针（中断写）
static volatile uint16_t s_shell_rx_tail;   // 读指针（主循环读）
```

`volatile` 必须——head 在中断里写、主循环里读，没有 volatile 编译器可能优化掉读取。缓冲满时新字节丢弃（不阻塞中断），比 08 的"超长清零重来"更安全。详见 [[11_rocketpi_uart_shell_microrl_1|中断接收与环形缓冲]]。

## microrl 库核心入口

```c
void microrl_insert_char(microrl_t * pThis, int ch);
```

每收到一个字节调一次。内部是一个大 switch，按字符类型分派：

| 字符 | 处理 | 效果 |
|---|---|---|
| `\n` | `new_line_handler` | 分词→execute 回调→清缓冲→打印提示符 |
| `\t` | `microrl_get_complite` | 调 get_completion 回调→插入候选词 |
| `ESC` | 进入转义状态机 | 后续 `ESC [ A` = ↑历史上翻，`ESC [ C` = →右移 |
| `DEL`/`BS` | `microrl_backspace` | 删光标前一个字符+ANSI 重绘 |
| `^U` | 循环 backspace | 删到行首 |
| `^K` | `\033[K` | 删到行尾 |
| `^A` | `terminal_reset_cursor` | 跳行首 |
| `^E` | `terminal_move_cursor` | 跳行尾 |
| `^P`/`^N` | `hist_search` | 历史上/下翻（同 ↑↓） |
| `^R` | 重绘 | 新行+提示符+重绘当前命令 |
| `^C` | sigint 回调 | 应用层定义（当前为空） |
| 可打印 | `microrl_insert_text` | 在光标位置插入字符+ANSI 重绘 |

microrl 的全部行编辑能力来自这个 switch——**一个入口、一个状态机、一个缓冲**。详见 [[11_rocketpi_uart_shell_microrl_2|microrl 库核心解析]]。

## 命令表与回调注册

```c
// main.c 初始化段
microrl_init(&s_shell, shell_print);                              // 注册输出回调
microrl_set_execute_callback(&s_shell, shell_execute);            // 注册命令执行回调
microrl_set_complete_callback(&s_shell, shell_complete);          // 注册 Tab 补全回调
```

三个回调的签名：

```c
void shell_print(const char *str);                                // 输出字符串到串口
int  shell_execute(int argc, const char * const * argv);          // 命令执行
char **shell_complete(int argc, const char * const * argv);       // Tab 补全
```

**microrl 不关心命令是什么**——它只负责行编辑和分词，执行逻辑全在回调里。这是回调式扩展（callback-based extension）：库通过函数指针让应用层注入行为。详见 [[11_rocketpi_uart_shell_microrl_3|命令系统与补全机制]]。

## 命令列表

| 命令 | 用法 | 功能 |
|---|---|---|
| `help` | `help` | 列出所有命令及说明 |
| `version` | `version` | 显示 microrl 版本和串口参数 |
| `echo` | `echo <文本...>` | 回显参数 |
| `led` | `led <blue\|green\|pink> <on\|off\|toggle>` | 控制 LED |

加新命令只需两步：kShellCmdTable 加一行 + shell_execute 加一个 `strcmp` 分支。详见 [[11_rocketpi_uart_shell_microrl_3|命令系统与补全机制]]。

## Tab 补全的工作流程

```
用户敲: led bl[Tab]
         ↓
microrl 检测到 \t → 调 microrl_get_complite
  ↓ split 分词到光标位置: argv=["led","bl"], argc=2
  ↓ 调 shell_complete(2, ["led","bl"])
    ↓ argv[0]=="led" && argc==2 → 在 kLedTable 里前缀匹配 "bl"
    ↓ "blue" 前缀匹配 → 候选: ["blue"]
    ↓ 唯一候选 → 插入 "ue" + 自动补空格
  ↓ 重绘行: "led blue "
```

多个候选时打印列表 + 插入公共前缀：

```
用户敲: led [Tab]
  ↓ 候选: ["blue","green","pink"] → 打印列表
  ↓ 公共前缀: ""（无公共前缀）→ 不插入
用户敲: led b[Tab]
  ↓ 候选: ["blue"] → 插入 "lue "（唯一候选+空格）
```

详见 [[11_rocketpi_uart_shell_microrl_3|命令系统与补全机制]]。

## 与 08 的逐维度对照

| 维度 | 08 | 11 |
|---|---|---|
| 接收 | 阻塞 HAL_UART_Receive | 中断 + 环形缓冲 |
| 行编辑 | 无（只认 \r\n） | 退格/方向键/历史/Tab/Ctrl 系列 |
| 分词 | 手写 JSON 解析器（几十行） | microrl split（空格分词，一行 strcmp） |
| 命令格式 | `{"led":"B","state":1}` | `led blue on` |
| 解析复杂度 | 高（游标+锚定+三分支） | 低（strcmp 一行） |
| 执行 | apply_command（表驱动） | shell_cmd_led（表驱动） |
| 应答 | JSON 格式化 | 纯文本 |
| 主循环 | 死等字节 | 可并行 |
| 新增功能 | — | 退格/历史/Tab 补全/Ctrl 系列 |

**核心变化**：命令格式从机器友好的 JSON 变成人友好的自然语言，解析复杂度从"手写游标解析器"降到"strcmp"——**把解析的复杂度从应用层转移到了 microrl 库的分词层**。

## config.h 裁剪指南

microrl 的功能全靠宏开关，不用的功能不占资源：

| 宏 | 启用时增加 | 禁用时省掉 |
|---|---|---|
| `_USE_HISTORY` | ring_history_t（64B RAM）+ ~120 行代码 | 无历史记录，无 ↑↓ |
| `_USE_COMPLETE` | ~80 行代码 | 无 Tab 补全 |
| `_USE_ESC_SEQ` | escape_process 状态机 + ~50 行代码 | 无方向键/Home/End |
| `_USE_CTLR_C` | sigint 回调指针 + 1 行 switch | 无 Ctrl+C |
| `_USE_LIBC_STDIO` | snprintf（~800B Flash on AVR） | 用自制 u16bit_to_str |
| `_ENABLE_INIT_PROMPT` | microrl_init 时打印提示符 | 首次按回车才出提示符 |

嵌入式资源紧张时，优先禁用 HISTORY（省 RAM）和 LIBC_STDIO（省 Flash）。

## 与前例连线

- [[08_rocketpi_uart_control_led|08 主笔记]] — 11 的前身，手写行缓冲/解析/执行
- [[08_rocketpi_uart_control_led_1|串口交互终端的意义]] — shell 的定义、命令注册两流派、microrl 的定位
- [[07_rocketpi_uart_echo]] — 11 的中断接收层与 07 的 DMA 接收是同一思路
- [[10_rocketpi_uart_ymodem_1|依赖注入]] — microrl 的回调机制是另一种"注入"

## 值得记住的三个设计词

- **回调式扩展**：microrl 通过三个回调（execute/complete/print）让应用层注入行为，库本身不关心命令内容
- **中断+环形缓冲**：ISR 存字节、主循环处理——嵌入式串口接收的标准模式
- **上下文感知补全**：Tab 补全根据当前输入位置（第几个参数）提供不同候选词，不是简单字符串匹配
