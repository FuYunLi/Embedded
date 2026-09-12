---
status: done
created: 2026-09-12
tags:
  - embedded/ymodem
  - embedded/file-transfer
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/10_rocketpi_uart_ymodem/ymodem.c"
---

# Ymodem 与文件传输概念：MCU 不关心后缀

> [[10_rocketpi_uart_ymodem|10 主笔记]] 的"文件的概念"一节只给了结论。这篇展开讨论：普通串口收发二进制数据和 Ymodem 的区别是什么、"文件"在 MCU 端意味着什么、PC 和 MCU 各自做了什么、MCU 是否需要解码器。

## 普通串口二进制收发 vs Ymodem

普通串口确实能收发大量二进制数据——波特率对、线路好、不丢字节，就能工作。但缺少三层语义：

### 第一层：帧边界

普通串口是一条无结构的字节流——收到 0x01 0x02 0x03...，不知道"这一帧到哪结束"。07 的 DMA+Idle 用空闲间隔做帧边界（RX 线空闲超 1 字节时间 = 一帧结束），但这是物理层的近似——如果两个字节之间恰好没有空闲，就会把两帧粘成一帧。

Ymodem 用帧头字节（SOH/STX）+ 长度字段做帧边界——**不依赖物理层时序，纯协议层解决**。收到 SOH 就知道后面是 128+4 字节，收到 STX 就知道后面是 1024+4 字节。这是二进制协议比"空闲间隔法"可靠的根本原因。

### 第二层：错误检测

普通串口没有任何校验——噪声导致一个 bit 翻转，收方不知道。08/09 的文本协议也没有校验（错一个字节整行乱，但人能看出来）。

Ymodem 有两级校验：序号取反（`seq + inv == 0xFF`，快速粗筛——如果加起来不是 0xFF，说明序号字段被破坏了）→ CRC16-CCITT（对整个数据块做多项式除法，任何 bit 翻转都会导致余数不同）。**这是二进制协议的标配**——文本协议可以靠人眼看出来错了，二进制数据人看不懂，必须有自动校验。

### 第三层：可靠传输

普通串口发了就完了——发方不知道收方有没有收到。如果收方缓冲溢出、DMA 没来得及搬、噪声破坏了数据，字节就丢了。

Ymodem 有 ACK/NAK 应答：每发一个数据块，收方回 ACK（收到了）或 NAK（坏了，重发）。发方有重试上限（retry_max=10），超过就放弃。**这是"可靠传输"的核心**——发方能确认数据被正确接收，不能就重来。对比：普通串口发 1000 字节，如果中间丢了 1 个字节，整个数据就废了；Ymodem 丢了 1 个块，重发这个块就行。

这三层叠加起来：**帧边界 + 错误检测 + 可靠应答 = 能保证"发什么就收什么"**。这就是 Ymodem 相比"裸串口收发二进制数据"增加的全部功能。

## "文件"在 MCU 端意味着什么

### PC 端的文件

Windows/Linux 的文件 = 文件系统（FAT32/ext4/NTFS）管理的一坨字节，有文件名、大小、创建时间、权限等元信息。操作系统提供 open/read/write/close API，应用程序通过文件描述符操作文件。**文件后缀 .txt/.bin 只是命名约定**——把 a.txt 改名 a.bin，文件内容一个字节都没变，只是文件管理器会用不同图标打开它。

### MCU 端的"文件"

MCU 没有操作系统，没有文件系统（当前例程没有），Flash 里就是一坨连续的字节。**对 MCU 来说，"文件"就是"从某个地址开始的一段字节"**——0x08040000 开始的 256KB Flash 就是全部"存储空间"。

当前例程的 `flash_open_write` 忽略了文件名（`(void)out_dir; (void)name;`），直接写到固定地址——MCU 不需要知道"这个文件叫什么"，它只关心"字节存到哪"。

### 文件名和大小在 Ymodem 里的角色

Block 0 的数据格式：`文件名\0大小字符串\0`（其余填 0）。这是 Ymodem 协议在字节流之上叠加的**文件语义层**——告诉接收方"接下来的数据叫什么名字、有多大"。

MCU 可以选择：
- **用文件名**：创建一个文件系统条目（如果有文件系统的话），按文件名存储
- **忽略文件名**：写到固定地址（当前例程的做法）
- **用大小**：预分配空间、判断 Flash 容量是否够、截断最后一块的多余填充

当前例程用大小做了两件事：进度显示（`fsz` 传给 prog_tick 算百分比）和最后一块截断（`if (fsz>=0 && wlen>(fsz-done)) wlen=(fsz-done)`）。

## txt 和 bin 的区别：传输层不关心

从 Ymodem 传输的角度：**txt 和 bin 没有区别**。两者都是字节流，Ymodem 不会因后缀不同而区别处理。

区别在"使用方"：

| 场景 | PC 端做了什么 | MCU 端做了什么 |
|---|---|---|
| PC 发 test.txt 给 MCU | SecureCRT 读 PC 上的 test.txt，填文件名 "test.txt" 进 Block 0，逐块发字节 | MCU 收字节存进 Flash。如果应用层读出来当字符串 printf 打印，就是 txt |
| PC 发 firmware.bin 给 MCU | 同上，文件名填 "firmware.bin" | MCU 存进 Flash。如果应用层跳转到那个地址执行，就是固件升级 |
| MCU 发数据给 PC | SecureCRT 根据 Block 0 的文件名 "rocketpi_demo.txt" 在 PC 上创建文件保存 | MCU 把 Flash/内存里的字节按协议发出，Block 0 填自己定的文件名 |

**MCU 不需要"解码器"**——解码是应用程序的事，不是传输协议的事。协议层只管"字节从 A 搬到 B"，不关心字节的含义。这和 [[08_rocketpi_uart_control_led|08]] 的"解析不碰硬件、执行不看字符串"是同一思想：**传输层和应用层各管各的**。

## PC 端和 MCU 端的分工

```
PC 端（串口助手）                    MCU 端
──────────────────                ──────────────────
1. 有文件系统，知道文件叫什么        1. 没有文件系统，不知道"文件"是什么
2. 读文件内容 → 字节流              2. 收字节流 → 存到 Flash/内存
3. 填文件名进 Block 0               3. 从 Block 0 提取文件名（可用可不用）
4. 按协议发字节 + CRC               4. 校验 CRC → ACK/NAK
5. 等 ACK，NAK 就重发               5. 写存储 → 发 ACK/NAK
6. 收到后按文件名保存                6. 不关心 PC 怎么保存
```

**PC 端的文件处理是串口助手内置的**——SecureCRT、Tera Term、minicom 都自带 Ymodem 实现。开发者不需要写上位机代码，只需要在 MCU 端实现协议。这也是 Ymodem 在嵌入式领域流行的原因之一：**PC 端工具已经标准化了，MCU 端实现协议就行**。

## 如果 MCU 端也有文件系统

真实产品中 MCU 端通常有文件系统（LittleFS、FatFS），这时 `YStore` 的实现会变成：

```c
store.open_write = lfs_open;      // LittleFS 打开文件
store.write      = lfs_write;     // LittleFS 写入
store.close_write= lfs_close;     // LittleFS 关闭
```

文件名从 Block 0 提取后传给 `lfs_open`，MCU 端就会按文件名存储——这时候 MCU 端和 PC 端一样有"文件"的概念了。但协议引擎（ymodem.c）不需要改——**它只调 `store->write(buf, len)`，不关心底层是 Flash 还是 LittleFS**。这就是 [[10_rocketpi_uart_ymodem_1|依赖注入]] 的价值。

## 关联笔记

- [[10_rocketpi_uart_ymodem]] — 主笔记：协议帧结构与传输流程
- [[10_rocketpi_uart_ymodem_1|依赖注入详解]] — YStore 接口如何隔离存储层
- [[08_rocketpi_uart_control_led_4|串口文本协议谱系]] — 文本 vs 二进制的全局定位
- [[08_rocketpi_uart_control_led|08 主笔记]] — "解析不碰硬件、执行不看字符串"的分层思想
