---
status: todo
created: 2026-06-30
tags:
  - eth/practice
  - eth/basic
  - todo
references: []
---

# Wireshark抓包实战

> [!NOTE]
> 本笔记为以太网基础知识体系中的进阶实操笔记，覆盖 Wireshark 的安装与配置、核心过滤器语法、重点观测场景以及"代码与抓包对照"方法论。将抽象的 C 语言代码与直观的抓包数据包结合起来，是理解以太网通信的**最有效路径**。

---

## 1. 核心概念

Wireshark 是网络调试的"X 光机"——它能将网线上传输的每一个比特都解剖成人类可读的协议字段。对于嵌入式开发者，Wireshark 的价值在于：**你在单片机代码中写的结构体、你注册的回调函数、你发出的 tcp_write——所有这些抽象操作，在 Wireshark 中都能看到对应的物理数据包**。代码和抓包对照，是从"纸上谈兵"到"实战理解"的关键飞跃。

---

## 2. 原理详解

### 2.1 安装与基本配置

1. **下载安装**：从 wireshark.org 下载安装包，安装时勾选 Npcap（抓包驱动）
2. **选择网卡**：启动后选择与单片机连接的网卡（通常是"以太网"或"本地连接"）
3. **启动抓包**：点击左上角"开始捕获"按钮
4. **停止抓包**：测试完成后点击红色"停止"按钮

> [!TIP]
> 如果单片机和电脑通过路由器连接，选择连接路由器的网卡；如果直连（单片机→电脑网口），选择该网口对应的网卡。

---

### 2.2 核心过滤器语法速查

Wireshark 捕获的包量极大，必须使用过滤器聚焦目标数据：

| 过滤器类型 | 语法 | 示例 |
|---|---|---|
| 按协议 | 协议名 | `arp`、`icmp`、`tcp`、`udp` |
| 按源/目的 IP | ip.addr | `ip.addr == 192.168.1.10` |
| 按源 IP | ip.src | `ip.src == 192.168.1.10` |
| 按端口 | tcp.port / udp.port | `tcp.port == 80` |
| 组合 | 逻辑运算符 | `tcp && ip.addr == 192.168.1.10` |
| 排除 | ! | `!arp`（过滤掉ARP包，只看数据包） |

> [!IMPORTANT]
> **实操建议**：先用 `icmp` 过滤器观察 Ping 包，再用 `tcp.port == 你的端口` 观察 TCP 通信，最后用 `!arp` 过滤掉大量 ARP 广播噪声。

---

### 2.3 重点观测场景

#### ARP 请求与应答

1. 单片机首次 Ping 电脑时，先发出 ARP 请求广播
2. 在 Wireshark 中过滤 `arp`，观察：
   - **请求包**：Opcode=1(Request)，Sender MAC=单片机MAC，Target IP=电脑IP
   - **应答包**：Opcode=2(Reply)，Sender MAC=电脑MAC，Sender IP=电脑IP
3. 对应代码中的 ARP 表自动填充

#### TCP 三次握手观测

1. 单片机发起 TCP 连接（tcp_connect）
2. 在 Wireshark 中过滤 `tcp.port == 你的端口 && ip.addr == 单片机IP`
3. 观察三个包：
   - **[SYN]** 包：Flags 中 SYN=1，Seq=初始序列号x
   - **[SYN, ACK]** 包：Flags 中 SYN=1+ACK=1，Seq=y，Ack=x+1
   - **[ACK]** 包：Flags 中 ACK=1，Seq=x+1，Ack=y+1
4. 将 Wireshark 中的序列号与 LwIP 代码中的 `pcb->snd_nxt` 对照

#### TCP 四次挥手观测

1. 单片机或电脑关闭 TCP 连接（tcp_close）
2. 观察四个包的 FIN 标志位和序列号递增

#### UDP 数据传输

1. 过滤 `udp.port == 你的端口`
2. 观察 UDP 极简首部：仅 Source Port + Dest Port + Length + Checksum
3. 与 TCP 首部对比：没有序列号、没有确认号、没有窗口大小

---

### 2.4 代码与抓包对照方法论

**核心思路**：在 C 代码中操作的结构体字段，在 Wireshark 中都能逐字段看到对应值。

对照步骤：
1. **在单片机代码中设置断点或打印日志**：记录关键操作时刻（如 tcp_connect 调用时间）
2. **在 Wireshark 中同步观察**：找到对应时间点的数据包
3. **逐字段对照**：
   - C 代码中的 `struct eth_hdr` → Wireshark 中的 Ethernet II 层展开
   - C 代码中的 `struct ip_hdr` → Wireshark 中的 Internet Protocol 层展开
   - C 代码中的 `struct tcp_hdr` → Wireshark 中的 Transmission Control Protocol 层展开
4. **特别注意字节序**：Wireshark 显示的值是网络字节序（大端），C 代码中打印的值可能是主机字节序（小端）

> [!TIP]
> Wireshark 默认将多字节字段显示为十进制，你可以在首部展开中切换为十六进制显示，与 C 代码中的十六进制值直接对照。字节序转换的必要性在对照中一目了然——详见 [[大小端无缝转换机制#核心转换函数与宏|大小端无缝转换机制]]。

---

### 2.5 实战技巧

| 功能 | 操作方法 | 用途 |
|---|---|---|
| **Follow TCP Stream** | 右键包 → Follow → TCP Stream | 查看完整 TCP 会话内容（HTTP请求/响应等） |
| **IO Graph** | 统计 → IO Graph | 分析吞吐量随时间的变化曲线 |
| **Packet Comments** | 右键包 → Add Packet Comment | 标注关键包的代码对应关系 |
| **Time Reference** | 右键包 → Set Time Reference | 以某个包为时间零点，方便计算时序 |
| **导出特定包** | 文件 → Export Specified Packets | 只保存过滤后的包，方便分享 |

---

## 3. 深度补充：TCP 重传包在 Wireshark 中的识别

当网络出现丢包或 LwIP 的 `sys_check_timeouts()` 调用不及时时，TCP 会触发重传。Wireshark 能够精确识别并标注重传包——这是调试 LwIP 性能问题的重要手段。

### 3.1 Wireshark 自动标注的重传类型

Wireshark 会在数据包列表的 Info 列自动标注以下重传类型：

| 标注文字 | 含义 | 触发原因 |
|---|---|---|
| **[TCP Retransmission]** | 普通超时重传 | 发送包在 RTO 时间内未收到 ACK |
| **[TCP Fast Retransmission]** | 快速重传 | 连续收到 3 个重复 ACK |
| **[TCP Out-of-Order]** | 乱序包 | 序列号不连续（非丢失，只是到达顺序错乱） |
| **[TCP Dup ACK]** | 重复 ACK | 接收方收到乱序包后，反复确认最后一个连续序列号 |

### 3.2 如何过滤重传包

```
# 过滤所有重传包（快速重传+超时重传）
tcp.analysis.retransmission

# 过滤所有与 TCP 分析异常相关的包
tcp.analysis.flags

# 组合使用：只看单片机 IP 的重传情况
tcp.analysis.retransmission && ip.addr == 192.168.1.10
```

### 3.3 重传包出现意味着什么？

- **少量偶发重传**：可能是局域网中的交换机队列短时拥塞，属于正常现象。
- **大量连续重传**：说明 LwIP 的 `sys_check_timeouts()` 调用间隔过长，导致 TCP 定时器超时。立即检查主循环是否有阻塞操作。
- **重传后跟着 [RST]**：对方主动拒绝了重传，连接被强制重置，检查服务端是否已关闭连接。

## 4. 深度补充：Wireshark 常见抓包异常情景分析

| 观察到的现象 | 原因分析 | 对应代码检查点 |
|---|---|---|
| 只有 ARP 请求，无 ARP 应答 | 目标 IP 不在线 / 不在同一子网 | 检查 IP 地址和子网掩码配置 |
| ARP 应答正常，但 Ping 超时 | ICMP 校验和错误 / IP 层路由错误 | 检查 ICMP 结构体 packed 属性和 checksum 计算 |
| 首次 Ping 超时，后续正常 | PHY 自协商未完成就启动 DMA | 在初始化中添加自协商等待（见 PHY 笔记） |
| 能 Ping 通但 TCP 连不上 | 目标端口未监听 / 防火墙拦截 | 检查服务端防火墙，或换一个端口测试 |
| TCP 连接后立刻断开（RST）| 对方 PCB 配置限制 / 端口权限问题 | 检查 LwIP 的 MEMP_NUM_TCP_PCB 是否用尽 |
| 数据传输中途卡住不动 | `tcp_recved()` 未调用，窗口冻结 | 检查每个 recv 回调是否调用了 tcp_recved |
| 大量 [TCP Retransmission] | sys_check_timeouts 调用太慢 | 缩短主循环阻塞操作，提高定时器调用频率 |

---

## 总结速查

- **Wireshark** 是以太网调试的 X 光机，将每比特解剖为人类可读的协议字段
- **过滤器**是核心工具：`arp`/`icmp`/`tcp.port`/`ip.addr` 是最常用语法
- **ARP 观测**：过滤 arp，看请求广播+应答单播的时间差和 Opcode
- **TCP 观测**：过滤 tcp.port，看三次握手 SYN→SYN+ACK→ACK 的序列号递增
- **代码对照**：C 结构体字段与 Wireshark 层展开逐字段对照，特别注意字节序差异
- **Follow TCP Stream** 是查看完整会话的最便捷功能

---

## 待深入 / 遗留疑问

- [ ] Wireshark 远程抓包（rpcapd）在嵌入式场景中的部署方法？能否直接抓单片机网口的包？
- [ ] TCP 重传包在 Wireshark 中如何识别？对调试 LwIP 重传机制有何帮助？
- [ ] Wireshark 的 TCP 流量分析图（ Stevens Graph）如何解读？对理解滑动窗口有何帮助？

---

## 关联笔记

- [[04_ARP与ICMP协议#ARP 请求与应答流程|ARP与ICMP协议]] — 抓包验证 ARP 请求广播
- [[TCP协议与可靠传输#三次握手流程|TCP协议与可靠传输]] — 抓包验证握手序列号
- [[UDP协议与无连接通信#UDP 首部结构|UDP协议与无连接通信]] — 抓包对比 UDP 极简首部
- [[以太网例程实操指南#渐进实践路线|以太网例程实操指南]] — 例程运行时同步抓包
- [[大小端无缝转换机制#核心转换函数与宏|大小端无缝转换机制]] — 抓包中网络字节序与代码中主机字节序对照
- [[网络分层模型与数据封装#逐层封装与解封装|网络分层模型与数据封装]] — Wireshark 逐层展开协议栈的视图对照
