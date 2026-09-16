---
status: done
created: 2026-09-16
tags:
  - c/crc
  - c/checksum
  - embedded/data-integrity
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/40_rocketpi_crc"
---

# 40 CRC 校验算法

## 一句话定性

CRC（Cyclic Redundancy Check）是嵌入式系统中最常用的数据完整性校验算法，本例实现 CRC4/7/8/16/24/32/32C 共 7 种软件版本，覆盖从短帧校验到网络传输的全部场景。

## CRC 在嵌入式中的应用

| CRC 宽度 | 典型应用 |
|----------|---------|
| CRC4 | 短帧校验（如某些传感器协议） |
| CRC7 | SD 卡命令校验 |
| CRC8 | 1-Wire、SMBus、某些 I2C 传感器 |
| CRC16 | Modbus、USB、XMODEM、Ymodem（10 例） |
| CRC24 | BLE、OpenPGP |
| CRC32 | 以太网、ZIP、PNG、STM32 HAL CRC 外设 |
| CRC32C | iSCSI、Btrfs、SCTP |

## 代码架构

```
40_rocketpi_crc/
├── crc4_sw.c      — CRC4 软件实现
├── crc7_sw.c      — CRC7 软件实现
├── crc8_sw.c      — CRC8 软件实现（多种多项式）
├── crc16_sw.c     — CRC16 软件实现（4 种变体）
├── crc24_sw.c     — CRC24 软件实现（查表法）
├── crc32_sw.c     — CRC32 软件实现（查表法）
├── crc32c_sw.c    — CRC32C 软件实现（查表法）
├── crc32k_4_2_sw.c — CRC32 变体（Koopman 多项式）
├── crc_test.c     — 统一测试框架
├── main.c         — 入口
└── debug_driver.c — 调试输出
```

## 核心实现：逐位算法 vs 查表法

### CRC16 逐位算法

```c
uint16_t crc16(uint16_t poly, uint16_t seed, const uint8_t *src, size_t len)
{
    uint16_t crc = seed;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(src[i] << 8U);  // ① 数据移入高位
        for (size_t j = 0; j < 8; j++) {   // ② 逐位处理
            if (crc & 0x8000U) {
                crc = (crc << 1U) ^ poly;   // ③ 最高位为1：左移+异或多项式
            } else {
                crc = crc << 1U;            // ④ 最高位为0：仅左移
            }
        }
    }
    return crc;
}
```

- **① 数据移入高位**：将当前字节异或到 CRC 高位，引入新数据
- **② 逐位处理**：每个字节 8 位，逐位移位
- **③④ 条件异或**：最高位为 1 时异或多项式，为 0 时仅左移。这是 CRC 的核心——多项式除法的位操作实现

### CRC16 CCITT 优化算法

```c
uint16_t crc16_ccitt(uint16_t seed, const uint8_t *src, size_t len)
{
    for (; len > 0; len--) {
        uint8_t e, f;
        e = seed ^ *src;
        ++src;
        f = e ^ (e << 4);
        seed = (seed >> 8) ^ ((uint16_t)f << 8) ^
               ((uint16_t)f << 3) ^ ((uint16_t)f >> 4);
    }
    return seed;
}
```

**优化原理**：逐位算法每字节循环 8 次，CCITT 算法每字节只需 4 次异或+移位，无循环。通过数学变换将 8 步压缩为 4 步。

### CRC16 反射算法

```c
uint16_t crc16_reflect(uint16_t poly, uint16_t seed, const uint8_t *src, size_t len)
{
    uint16_t crc = seed;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)src[i];        // 数据移入低位（非高位）
        for (size_t j = 0; j < 8; j++) {
            if (crc & 0x0001U) {
                crc = (crc >> 1U) ^ poly; // 最低位为1：右移+异或
            } else {
                crc = crc >> 1U;           // 最低位为0：仅右移
            }
        }
    }
    return crc;
}
```

**反射 vs 正常**：正常 CRC 从高位开始移位（MSB first），反射 CRC 从低位开始（LSB first）。某些协议（如 Modbus、USB）要求反射。

### CRC24/32 查表法

CRC24 和 CRC32 使用预计算的 256 项查找表，每次处理 1 字节直接查表，无需逐位循环：

```c
// CRC32 查表法（简化）
uint32_t crc32(uint32_t seed, const uint8_t *src, size_t len)
{
    uint32_t crc = seed;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ src[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;  // 最终异或
}
```

**查表法 vs 逐位法**：
- 逐位法：每字节循环 8 次，代码小，速度慢
- 查表法：每字节查表 1 次，代码大（256 项 × 4 字节 = 1KB），速度快 8 倍

## CRC 参数说明

每个 CRC 变体有 4 个关键参数：

| 参数 | 含义 |
|------|------|
| 多项式（Poly） | 除法器，决定检错能力 |
| 初始值（Seed） | CRC 寄存器初始值 |
| 输入反转（Reflect In） | 数据位是否反转后再处理 |
| 输出反转（Reflect Out） | 最终 CRC 是否反转 |
| 最终异或值（XOR Out） | 最终 CRC 与某值异或 |

**不同协议的 CRC16 变体**：

| 变体 | 多项式 | 初始值 | 反射 | 应用 |
|------|--------|--------|------|------|
| CRC16 | 0x8005 | 0x0000 | 否 | IBM |
| CRC16-CCITT | 0x1021 | 0xFFFF | 否 | XMODEM、Ymodem |
| CRC16-Modbus | 0x8005 | 0xFFFF | 是 | Modbus RTU |
| CRC16-USB | 0x8005 | 0xFFFF | 是 | USB |

## 设计问题与改进空间

1. **逐位法性能**：CRC16/8/4 用逐位法，每字节 8 次循环。对大数据量（>1KB）应改用查表法（256 项表，~512B Flash）。

2. **`__weak` 标记**：所有 CRC 函数标记为 `__weak`，允许硬件 CRC 外设覆盖。STM32F401 有硬件 CRC-32 外设，可加速 CRC32 计算。

3. **无硬件 CRC 外设对比**：本例只用软件实现，没有调用 STM32 HAL CRC 外设。可加硬件版本对比性能差异。

4. **多项式参数硬编码**：每种 CRC 变体的多项式在函数内部固定。可改为参数化（如 `crc16(poly, seed, ...)`），但会增加调用复杂度。

5. **与 10 例（Ymodem）关联**：10 例用 CRC16-CCITT 校验 Ymodem 帧，本例的 `crc16_ccitt` 函数就是那个算法的通用版本。

## 关联笔记

- [[10_rocketpi_uart_ymodem|10 Ymodem 文件传输]]：CRC16-CCITT 在 Ymodem 协议中的应用
- [[13_rocketpi_uart_radar|13 UART 雷达]]：XOR 校验，对比 CRC 的检错能力
