---
status: done
created: 2026-09-12
tags:
  - embedded/shell
  - c/library
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/11_rocketpi_uart_shell_microrl/microrl.c"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/11_rocketpi_uart_shell_microrl/microrl.h"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/11_rocketpi_uart_shell_microrl/config.h"
---

# microrl 库核心解析：行编辑、历史、补全与 ANSI 转义

> [[11_rocketpi_uart_shell_microrl|11 主笔记]] 说"microrl 是一个通用嵌入式 readline 库"——这篇拆解它的内部实现。microrl.c 约 550 行，提供退格、方向键、历史记录、Tab 补全、Ctrl 系列快捷键，全部通过一个入口函数 `microrl_insert_char` 实现。

## 库的对外接口

microrl 的使用模式是"注册回调+喂字节"：

```c
microrl_t s_shell;
microrl_init(&s_shell, shell_print);                          // 初始化+注册输出回调
microrl_set_execute_callback(&s_shell, shell_execute);        // 注册命令执行回调
microrl_set_complete_callback(&s_shell, shell_complete);      // 注册 Tab 补全回调

// 每收到一个字节
microrl_insert_char(&s_shell, ch);
```

**库不知道字节从哪来**（可能是 UART 中断、USB CDC、SPI），也不知道输出到哪去（可能是 UART printf、RTT、LCD）。它只关心：收到字节→处理→调 print 回调输出。**接口完全解耦**，和 [[10_rocketpi_uart_ymodem_1|10 的依赖注入]] 同思想。

## microrl_t 结构体

```c
typedef struct {
    char escape_seq;              // 转义序列内部状态
    char escape;                  // 是否在转义序列中
    char tmpch;                   // CRLF 检测临时字符
    ring_history_t ring_hist;     // 历史记录
    char *prompt_str;             // 提示符字符串
    char cmdline[101];            // 命令行缓冲
    int cmdlen;                   // 命令长度
    int cursor;                   // 光标位置
    int (*execute)(int, const char* const*);      // 命令执行回调
    char **(*get_completion)(int, const char* const*);  // Tab 补全回调
    void (*print)(const char*);                   // 输出回调
    void (*sigint)(void);                         // Ctrl+C 回调
} microrl_t;
```

**`cmdline` vs `cmdlen` vs `cursor`**：cmdline 是字符缓冲（含 `\0` 分隔的 token），cmdlen 是有效内容长度，cursor 是光标位置（可不在末尾——用户用 ←→ 移动了光标）。三者独立变化：插入字符时 cmdlen++和 cursor++，退格时 cmdlen--和 cursor--，←移动时只有 cursor--。

## microrl_insert_char：总入口的大 switch

```c
void microrl_insert_char(microrl_t * pThis, int ch){
    if (pThis->escape) {
        escape_process(pThis, ch);       // 转义序列状态机
    } else {
        switch (ch) {
            case KEY_LF:  new_line_handler(pThis); break;        // \n → 执行命令
            case KEY_HT:  microrl_get_complite(pThis); break;    // Tab → 补全
            case KEY_ESC: pThis->escape = 1; break;              // ESC → 进入转义
            case KEY_NAK: /* ^U 删到行首 */ break;
            case KEY_VT:  /* ^K 删到行尾 */ break;
            case KEY_ENQ: /* ^E 跳行尾 */ break;
            case KEY_SOH: /* ^A 跳行首 */ break;
            case KEY_ACK: /* ^F 右移 */ break;
            case KEY_STX: /* ^B 左移 */ break;
            case KEY_DLE: /* ^P 历史上翻 */ break;
            case KEY_SO:  /* ^N 历史下翻 */ break;
            case KEY_DEL: case KEY_BS: microrl_backspace(pThis); break;
            case KEY_DC2: /* ^R 重绘 */ break;
            case KEY_ETX: /* ^C sigint */ break;
            default: microrl_insert_text(pThis, &ch, 1); break;  // 可打印字符
        }
    }
}
```

**两个状态**：`escape==0` 时正常模式（处理可打印字符和控制字符），`escape==1` 时转义模式（等待 `ESC [` 后的 A/B/C/D 等）。收到 `ESC` 设置 `escape=1`，后续字节进 `escape_process` 处理完后清除。

## 行编辑：插入、退格、光标移动

### microrl_insert_text：在光标位置插入文本

```c
static int microrl_insert_text(microrl_t * pThis, char * text, int len){
    if (pThis->cmdlen + len < _COMMAND_LINE_LEN) {
        memmove(pThis->cmdline + pThis->cursor + len,
                pThis->cmdline + pThis->cursor,
                pThis->cmdlen - pThis->cursor);     // 后移现有内容
        for (i = 0; i < len; i++) {
            pThis->cmdline[pThis->cursor + i] = text[i];
            if (text[i] == ' ') pThis->cmdline[pThis->cursor + i] = 0;  // 空格→\0
        }
        pThis->cursor += len;
        pThis->cmdlen += len;
        pThis->cmdline[pThis->cmdlen] = '\0';
        return true;
    }
    return false;
}
```

**空格→`\0`**：这是 microrl 分词的基础——空格被替换成 `\0`，cmdline 内部变成 `led\0blue\0on\0`，split 函数按 `\0` 分割 token。**这是 microrl 最巧妙的设计**：不需要额外的 token 边界数组，cmdline 本身就是 token 数组。

**memmove 后移**：光标在中间时插入字符，后面的内容要后移。`memmove` 处理重叠区域（源和目标有交叉），比 `memcpy` 安全。

### microrl_backspace：退格

```c
static void microrl_backspace(microrl_t * pThis){
    if (pThis->cursor > 0) {
        terminal_backspace(pThis);        // ANSI: 光标左移+覆盖空格+光标左移
        memmove(pThis->cmdline + pThis->cursor - 1,
                pThis->cmdline + pThis->cursor,
                pThis->cmdlen - pThis->cursor + 1);   // 前移后续内容
        pThis->cursor--;
        pThis->cmdlen--;
    }
}
```

**`terminal_backspace`** 发送 `\033[D \033[D`——ANSI 转义序列：`\033[D` 光标左移一格，空格覆盖当前字符，再左移一格。三个字符串实现"视觉退格"。

### terminal_move_cursor：光标移动

```c
static void terminal_move_cursor(microrl_t * pThis, int offset){
    if (offset > 0) snprintf(str, 16, "\033[%dC", offset);     // 右移
    else if (offset < 0) snprintf(str, 16, "\033[%dD", -offset); // 左移
    pThis->print(str);
}
```

`\033[N C` = 光标右移 N 列，`\033[N D` = 光标左移 N 列。这是 ANSI X3.64 标准（VT100 终端，1978 年），今天的终端模拟器全部兼容。**microrl 的所有视觉效果（光标移动、清行、重绘）都靠 ANSI 转义序列实现**——它假设对端是一个 VT100 兼容终端。

## 历史记录（ring_history_t）

### 存储结构

```c
typedef struct {
    char ring_buf[64];   // 环形缓冲
    int begin;           // 最旧记录起始位置
    int end;             // 下一条写入位置
    int cur;             // 当前浏览位置（0=未浏览，1=最新一条，2=第二新...）
} ring_history_t;
```

环形缓冲 64 字节，每条命令的存储格式：`[长度(1B)] [命令内容(NB)]`。`begin` 指向最旧记录的长度字节，`end` 指向下一条写入位置。`cur` 是浏览索引——按 ↑ 时 cur++（更旧），按 ↓ 时 cur--（更新）。

### hist_save_line：保存一条命令

```c
static void hist_save_line(ring_history_t * pThis, char * line, int len){
    if (len > _RING_HISTORY_LEN - 2) return;   // 太长不存
    while (!hist_is_space_for_new(pThis, len)) {
        hist_erase_older(pThis);                // 空间不够删最旧的
    }
    pThis->ring_buf[pThis->end] = len;          // 写长度
    // 写内容（处理环形回绕：可能分两段 memcpy）
    ...
    pThis->end = (pThis->end + len + 1) % _RING_HISTORY_LEN;
    pThis->cur = 0;   // 重置浏览位置
}
```

**环形回绕处理**：如果 `end` 到缓冲末尾放不下整条命令，分两段写——前半段写到末尾，后半段写到开头。这是环形缓冲的标准处理，和 [[11_rocketpi_uart_shell_microrl_1|环形缓冲笔记]] 的 shell_queue_char 同模式。

### hist_restore_line：恢复历史命令

按 `cur` 索引从 begin 开始遍历，跳过 `cur-1` 条记录，复制第 `cur` 条到 cmdline。处理环形回绕同上。

## Tab 补全（microrl_get_complite）

```c
static void microrl_get_complite(microrl_t * pThis){
    char const * tkn_arr[_COMMAND_TOKEN_NMB];
    int status = split(pThis, pThis->cursor, tkn_arr);   // 分词到光标位置
    if (pThis->cmdline[pThis->cursor-1] == '\0')
        tkn_arr[status++] = "";      // 光标在空格后→追加空 token
    char ** compl_token = pThis->get_completion(status, tkn_arr);  // 调应用层

    if (compl_token[0] != NULL) {
        if (compl_token[1] == NULL) {
            // 唯一候选→直接插入剩余部分+空格
            microrl_insert_text(pThis, compl_token[0]+已输入长度, 剩余长度);
            microrl_insert_text(pThis, " ", 1);
        } else {
            // 多个候选→打印列表+插入公共前缀
            int len = common_len(compl_token);   // 找所有候选的公共前缀
            ...
        }
        terminal_reset_cursor(pThis);
        terminal_print_line(pThis, 0, pThis->cursor);   // 重绘行
    }
}
```

**`common_len`**：找所有候选字符串的最长公共前缀。逐字符比较，第一个不一致的位置就是公共前缀长度。如果公共前缀比已输入的长，就插入差额部分。

**唯一候选自动加空格**：补全后如果只有一个候选，自动追加空格——告诉用户"这个词完了，可以输入下一个参数"。

## ANSI 转义序列状态机（escape_process）

```c
static int escape_process(microrl_t * pThis, char ch){
    if (ch == '[') {
        pThis->escape_seq = _ESC_BRACKET;   // ESC [ → 等待指令字符
        return 0;
    } else if (pThis->escape_seq == _ESC_BRACKET) {
        if (ch == 'A') { hist_search(pThis, _HIST_UP); return 1; }     // ↑
        if (ch == 'B') { hist_search(pThis, _HIST_DOWN); return 1; }   // ↓
        if (ch == 'C') { cursor++; terminal_move_cursor(1); return 1; } // →
        if (ch == 'D') { cursor--; terminal_move_cursor(-1); return 1; }// ←
        if (ch == '7') { escape_seq = _ESC_HOME; return 0; }           // Home 序列前半
        if (ch == '8') { escape_seq = _ESC_END; return 0; }            // End 序列前半
    } else if (ch == '~') {
        if (pThis->escape_seq == _ESC_HOME) { cursor=0; return 1; }    // Home
        if (pThis->escape_seq == _ESC_END)  { cursor=cmdlen; return 1; }// End
    }
    return 1;   // 未知序列→退出转义模式
}
```

**状态机只有两个状态**：`_ESC_BRACKET`（收到 `ESC [`，等待指令字符）和 `_ESC_HOME/_ESC_END`（收到 `ESC 7/8`，等待 `~`）。终端发送方向键时发出 `ESC [ A`（↑）、`ESC [ B`（↓）、`ESC [ C`（→）、`ESC [ D`（←）——三字节序列。

**`return 0` 表示继续等后续字节，`return 1` 表示转义序列处理完毕**。回到 `microrl_insert_char` 的 `escape_process` 调用处，return 1 时清除 `pThis->escape = 0`，回到正常模式。

## split：空格分词

```c
static int split(microrl_t * pThis, int limit, char const ** tkn_arr){
    int i = 0, ind = 0;
    while (1) {
        while ((pThis->cmdline[ind] == '\0') && (ind < limit)) ind++;  // 跳过 \0
        if (ind >= limit) return i;
        tkn_arr[i++] = pThis->cmdline + ind;    // token 起始地址
        while ((pThis->cmdline[ind] != '\0') && (ind < limit)) ind++;  // 跳过非 \0
        if (ind >= limit) return i;
    }
}
```

**cmdline 内部格式**：`led\0blue\0on\0`——空格被 `microrl_insert_text` 替换成了 `\0`。split 只需跳过连续 `\0` 找 token 起点，再跳过连续非 `\0` 找终点。token 起始地址直接指向 cmdline 内部（不拷贝），所以 argv 指向的是 cmdline 的不同位置。

**限制 `_COMMAND_TOKEN_NMB`**：超过 8 个 token 返回 -1，execute 不调用。防止输入超长命令导致 tkn_arr 越界。

## new_line_handler：命令执行的完整流程

```c
void new_line_handler(microrl_t * pThis){
    terminal_newline(pThis);              // 换行
    hist_save_line(...);                  // 保存历史
    status = split(pThis, pThis->cmdlen, tkn_arr);   // 分词
    if (status > 0 && pThis->execute != NULL)
        pThis->execute(status, tkn_arr);  // 调应用层回调
    print_prompt(pThis);                  // 打印提示符
    pThis->cmdlen = 0; pThis->cursor = 0; // 清空缓冲
    memset(pThis->cmdline, 0, _COMMAND_LINE_LEN);
}
```

**执行后清空**：cmdlen=0、cursor=0、cmdline 全零——准备好接收下一条命令。history 的 cur 也重置为 0（下次按 ↑ 从最新开始翻）。

## microrl 的设计哲学

1. **零动态内存分配**：所有缓冲都是静态数组（cmdline[101]、ring_buf[64]），不用 malloc——嵌入式友好
2. **功能靠宏裁剪**：config.h 里注释掉 `_USE_HISTORY` 就没有历史，注释掉 `_USE_COMPLETE` 就没有补全——不用的功能不占资源
3. **库不关心 I/O**：通过 print 回调输出，通过 insert_char 喂字节——解耦硬件
4. **ANSI 转义做视觉**：所有光标移动/清行/重绘都靠 `\033[...` 序列——假设对端是 VT100 兼容终端

## 关联笔记

- [[11_rocketpi_uart_shell_microrl]] — 主笔记：架构全景与命令系统
- [[11_rocketpi_uart_shell_microrl_1|中断接收与环形缓冲]] — 接收层升级
- [[11_rocketpi_uart_shell_microrl_3|命令系统与补全机制]] — 应用层的回调实现
- [[08_rocketpi_uart_control_led_1|串口交互终端的意义]] — VT100/ANSI 转义序列的历史背景
- [[10_rocketpi_uart_ymodem_1|依赖注入]] — microrl 的回调机制与 YPort 的函数指针同思想
