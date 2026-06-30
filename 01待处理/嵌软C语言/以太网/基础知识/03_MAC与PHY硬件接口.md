---
status: todo
created: 2026-06-30
tags:
  - eth/hardware
  - eth/basic
  - todo
references: []
---

# MAC与PHY硬件接口

> [!NOTE]
> 本笔记为以太网基础知识体系中的硬件接口笔记，覆盖 MAC 与 PHY 的职责分工、MII/RMII 接口信号定义以及 SMI 管理接口（MDIO/MDC）的配置机制。

---

## 1. 核心概念

以太网通信在单片机内部由两个独立硬件模块协作完成：**MAC** 负责"帧级数据处理"（组装帧、校验 CRC、过滤地址），**PHY** 负责"信号级收发"（线编码、驱动电信号、检测链路状态）。两者之间通过标准化接口（MII 或 RMII）连接信号线，通过 SMI（MDIO/MDC）连接管理配置线。理解这套分工与接口，是配置以太网引脚和调试链路问题的前提。

---

## 2. 原理详解

### 2.1 MAC 与 PHY 的职责分工

| 模块 | 职责 | 具体功能 |
|---|---|---|
| **MAC** | 帧级处理 | 组装/解析以太网帧首部、CRC 生成与校验、MAC 地址过滤、DMA 描述符管理 |
| **PHY** | 信号级收发 | 线编码（MII: 4bit并行, RMII: 2bit并行）、电信号驱动与接收、链路状态检测（Link/Speed）、自协商（Auto-Negotiation） |

**一句话总结**：MAC 是"软件看得到的帧处理器"，PHY 是"软件看不到的信号翻译器"。LwIP 只和 MAC 打交道，PHY 由 MAC 通过 SMI 间接配置。

---

### 2.2 MII 与 RMII 接口对比

MII（Media Independent Interface）是传统标准接口，RMII（Reduced MII）是其精简版，减少引脚数量：

| 特性 | MII | RMII |
|---|---|---|
| **数据位宽** | 4 bit（TXD[0:3], RXD[0:3]） | 2 bit（TXD[0:1], RXD[0:1]） |
| **时钟频率** | 25 MHz（100M）/ 2.5 MHz（10M） | 50 MHz（100M/10M 统一） |
| **信号线总数** | 16 根（不含 SMI） | 8 根（不含 SMI） |
| **引脚占用** | 多（不适合引脚紧张的 MCU） | 少（嵌入式首选） |

**MII 完整信号定义（16根）**：

| 信号名 | 方向 | 功能 |
|---|---|---|
| TX_CLK | PHY→MAC | 发送时钟（25/2.5MHz） |
| TX_EN | MAC→PHY | 发送使能 |
| TXD[0:3] | MAC→PHY | 发送数据（4bit并行） |
| TX_ER | MAC→PHY | 发送错误指示 |
| RX_CLK | PHY→MAC | 接收时钟（25/2.5MHz） |
| RX_DV | PHY→MAC | 接收数据有效 |
| RXD[0:3] | PHY→MAC | 接收数据（4bit并行） |
| RX_ER | PHY→MAC | 接收错误指示 |
| CRS | PHY→MAC | 载波侦听 |
| COL | PHY→MAC | 冲突检测（半双工用） |

**RMII 完整信号定义（8根）**：

| 信号名 | 方向 | 功能 |
|---|---|---|
| REF_CLK | PHY→MAC | 50MHz 参考时钟（统一100M/10M） |
| TX_EN | MAC→PHY | 发送使能 |
| TXD[0:1] | MAC→PHY | 发送数据（2bit并行） |
| CRS_DV | PHY→MAC | 载波侦听+数据有效（合并信号） |
| RXD[0:1] | PHY→MAC | 接收数据（2bit并行） |
| RX_ER | PHY→MAC | 接收错误指示 |

> [!IMPORTANT]
> RMII 将 MII 的 CRS 和 RX_DV 合并为 **CRS_DV**，并将双时钟合并为单一 **REF_CLK**。这是引脚减少的关键设计。嵌入式项目几乎全部使用 **RMII**，除非有特殊兼容性需求才选 MII。

---

### 2.3 SMI 管理接口：MDIO/MDC

SMI（Station Management Interface）是 MAC 读写 PHY 内部寄存器的专用管理总线，仅使用 **2 根信号线**：

| 信号 | 方向 | 功能 |
|---|---|---|
| **MDC** | MAC→PHY | 管理时钟，由 MAC 驱动，最高 2.5MHz |
| **MDIO** | 双向 | 管理数据，MAC 写时由 MAC 驱动，PHY 读时由 PHY 驱动 |

SMI 通信帧格式（32 bit）：

```
| START(2bit=01) | OP(2bit:10=读,01=写) | PHY_ADDR(5bit) | REG_ADDR(5bit) | DATA(16bit) | TURN(2bit) |
```

**关键 PHY 寄存器**：

| 寄存器地址 | 名称 | 作用 |
|---|---|---|
| 0 | **BMCR**（Basic Mode Control） | 重置 PHY、强制速率/双工模式、启用自协商 |
| 1 | **BMSR**（Basic Mode Status） | 链路状态、自协商完成标志、速率能力 |
| 2 | **PHYID1** | PHY 厂商 ID 高位 |
| 3 | **PHYID2** | PHY 厂商 ID 低位+型号 |

```c
/* 通过 SMI 读取 PHY 链路状态的示例 */
uint16_t phy_status = ETH_ReadPHYRegister(PHY_ADDR, 1); /* 读 BMSR */
if (phy_status & PHY_LINK_STATUS) {
    /* 链路已连接 */
}
```

> [!TIP]
> MAC 初始化时必须先通过 SMI 配置 PHY（重置、自协商），然后才能启动 DMA 收发。这是初始化流程中不可或缺的步骤。详见 [[01_MAC初始化与DMA描述符#MAC 控制器初始化流程|MAC初始化与DMA描述符]]。

---

## 总结速查

- **MAC** 负责帧级处理（组装帧/CRC/地址过滤/DMA），**PHY** 负责信号级收发（编码/驱动/链路检测/自协商）
- **RMII** 是嵌入式首选接口，仅 8 根信号线（2bit并行+50MHz统一时钟），**MII** 需要 16 根（4bit并行+双时钟）
- **SMI（MDIO/MDC）** 是 MAC 配置 PHY 的管理总线，仅 2 根线，读写 PHY 寄存器（如链路状态BMSR）
- **PHY 自协商**在初始化时通过 BMCR 寄存器启动，链路状态通过 BMSR 寄存器读取
- LwIP **只和 MAC 打交道**，PHY 由 MAC 通过 SMI 间接管理

---

## 待深入 / 遗留疑问

- [ ] PHY 自协商失败时的处理策略：是否需要手动强制速率和双工模式？对稳定性有何影响？
- [ ] RMII 的 REF_CLK 来源选择：由 PHY 输出 vs 由 MCU 外部晶振提供，哪种更可靠？
- [ ] 不同 PHY 芯片（如 LAN8720 vs DP83848）的寄存器布局差异对驱动代码的影响？

---

## 关联笔记

- [[MAC初始化与DMA描述符#MAC 控制器初始化流程|MAC初始化与DMA描述符]] — MAC 初始化中必须通过 SMI 配置 PHY 的步骤
- [[02_网络分层模型与数据封装#TCP/IP 四层模型|网络分层模型与数据封装]] — MAC 对应链路层，PHY 对应物理层
- [[09_以太网例程实操指南]] — 实际例程中检查 PHY 链路状态的代码对照
