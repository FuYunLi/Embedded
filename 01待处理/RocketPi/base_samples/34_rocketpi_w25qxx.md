---
status: done
created: 2026-09-16
tags:
  - c/spi
  - c/flash
  - embedded/storage
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/34_rocketpi_w25qxx"
  - "[[15_rocketpi_i2c_at24cxx]] I2C EEPROM（对比 SPI Flash）"
  - "[[28_rocketpi_sdio_card]] SD 卡（对比 SPI Flash）"
  - "[[33_rocketpi_usb_msc]] USB MSC（可扩展 Flash 为 U 盘介质）"
---

# 34 SPI Flash W25QXX

## 一句话定性

W25QXX 是 SPI 接口的 NOR Flash 存储芯片，容量 1~128MB，支持扇区擦除+页编程，掉电保持数据，用于嵌入式系统的固件存储、数据日志、文件系统等。

## 同类产品定位

- **W25Q64**：8MB SPI NOR Flash，最常用型号
- **W25Q128**：16MB，更大容量
- **AT25EEPROM**（15 例）：I2C EEPROM，容量小（256B~512KB），字节可写
- **SD 卡**（28 例）：SDIO 接口，大容量（GB 级），块设备
- **本例选型理由**：W25Q64 容量适中、SPI 接口简单、支持 XIP（就地执行），适合学习 SPI Flash

## 硬件连接

- SPI：PB13=SCK, PB14=MISO, PB15=MOSI, PB12=CS
- 本例使用 SPI2（与 I2S 共用外设，不同例程用不同功能）

## 通信协议要点

- **SPI 模式**：Mode 0（CPOL=0, CPHA=0）或 Mode 3（CPOL=1, CPHA=1）
- **命令集**：读 ID（0x9F）、读数据（0x03）、页编程（0x02）、扇区擦除（0x20）、芯片擦除（0xC7）
- **写入约束**：必须先擦除（Erased=0xFF）再写入（只能 1→0），擦除粒度 4KB 扇区
- **页编程**：最大 256 字节/页，跨页需分次写入
- **LibDriver 风格**：543KB 驱动代码（含全系列 W25QXX 支持），函数指针注入

## 代码架构

```
┌─────────────────────────────────────────────────┐
│ main.c (应用层)                                 │
│  - w25qxx_register_test：寄存器读写测试         │
│  - w25qxx_read_test：数据读写测试               │
├─────────────────────────────────────────────────┤
│ driver_w25qxx_register_test.c (寄存器测试)      │
│  - 状态寄存器/安全寄存器/ID 寄存器读写验证      │
├─────────────────────────────────────────────────┤
│ driver_w25qxx_read_test.c (数据测试)            │
│  - 扇区擦除+页编程+读回校验                     │
├─────────────────────────────────────────────────┤
│ driver_w25qxx_basic.c (封装层)                  │
│  - 静态 handle + 简化 API                       │
├─────────────────────────────────────────────────┤
│ driver_w25qxx.c (驱动层，LibDriver，543KB)     │
│  - 全系列 W25QXX 支持，完整 SPI Flash 命令集   │
├─────────────────────────────────────────────────┤
│ driver_w25qxx_interface.c (平台适配层)          │
│  - SPI 读写 + CS 控制 + 延时                   │
├─────────────────────────────────────────────────┤
│ spi.c (CubeMX 生成)                            │
│  - SPI2 初始化                                  │
└─────────────────────────────────────────────────┘
```

## 与 15 例（EEPROM）的关键差异

| 维度 | 15 AT24CXX EEPROM | 34 W25QXX Flash |
|------|-------------------|-----------------|
| 接口 | I2C | SPI |
| 容量 | 256B~512KB | 1~128MB |
| 写入单位 | 字节 | 页（256B） |
| 擦除需求 | 无（直接覆写） | 必须先擦除（4KB 扇区） |
| 擦除次数 | 100 万次 | 10 万次 |
| 写入速度 | ~5ms/字节 | ~0.5ms/页 |
| 读取速度 | ~400kHz | ~50MHz（SPI） |
| 驱动来源 | LibDriver | LibDriver（543KB） |

## 核心设计：SPI Flash 的擦写约束

W25QXX 是 NOR Flash，写入有严格约束：

```
1. 擦除：将整块（4KB 扇区）变为 0xFF
2. 编程：只能将 1→0，不能 0→1
3. 因此：修改数据必须先擦除再编程，不能原地覆写
```

这与 EEPROM（15 例）完全不同——EEPROM 可以直接覆写字节，Flash 必须"先擦后写"。

## 关键命令

| 命令 | 代码 | 作用 |
|------|------|------|
| JEDEC ID | 0x9F | 读取制造商+设备 ID |
| Read Data | 0x03 | 从地址读取数据（无长度限制） |
| Page Program | 0x02 | 写入最多 256 字节（需先擦除） |
| Sector Erase | 0x20 | 擦除 4KB 扇区 |
| Block Erase | 0xD8 | 擦除 64KB 块 |
| Chip Erase | 0xC7 | 擦除整片 |
| Read Status | 0x05 | 读取忙标志/WEL 等状态 |
| Write Enable | 0x06 | 写使能（编程/擦除前必须发） |

## 写入流程

```
1. Write Enable (0x06)
2. Sector Erase (0x20 + 3 字节地址)  → 擦除 4KB
3. 等待忙标志清除（轮询 Status Register）
4. Write Enable (0x06)
5. Page Program (0x02 + 3 字节地址 + 最多 256B 数据)  → 写入一页
6. 等待忙标志清除
7. 重复 4-6 直到写完所有数据
```

## 读取测试逻辑

```c
// w25qxx_read_test 简化流程
w25qxx_init(&handle);           // 初始化
w25qxx_read(&handle, id);       // 读 JEDEC ID
w25qxx_sector_erase(&handle, addr);  // 擦除扇区
w25qxx_write(&handle, addr, data, len);  // 写入
w25qxx_read(&handle, addr, buf, len);    // 读回
verify(buf, data);              // 校验
```

## 设计问题与改进空间

1. **543KB 驱动代码**：LibDriver 的 W25QXX 驱动包含全系列支持（W25Q16~W25Q128），代码量巨大。本例只用 W25Q64，可大幅裁剪。

2. **擦除-写入的原子性**：擦除和写入是两步操作，中间断电会导致数据丢失。工程代码需加掉电保护（如日志式写入、双区备份）。

3. **写入前必须 Write Enable**：每次编程/擦除前都要发 0x06 命令。忘记发会导致写入静默失败。

4. **忙标志轮询**：编程/擦除期间芯片不响应新命令，必须轮询 Status Register 的 WIP 位。阻塞式等待，CPU 空转。

5. **页边界问题**：页编程最多 256 字节，跨页写入会回绕到页首。驱动库应内部处理分页，但需验证。

## 关联笔记

- [[15_rocketpi_i2c_at24cxx|15 I2C EEPROM]]：小容量存储，对比 SPI Flash
- [[28_rocketpi_sdio_card|28 SDIO SD 卡]]：大容量存储，对比 SPI Flash
- [[35_rocketpi_flash_littlefs|35 Flash LittleFS]]：在 W25QXX 上运行 LittleFS 文件系统
- [[33_rocketpi_usb_msc|33 USB MSC]]：可将 Flash 扩展为 USB U 盘介质
