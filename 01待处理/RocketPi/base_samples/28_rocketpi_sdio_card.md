---
status: done
created: 2026-09-16
tags:
  - c/sdio
  - c/storage
  - embedded/peripheral
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/28_rocketpi_sdio_card"
  - "[[15_rocketpi_i2c_at24cxx]] I2C EEPROM（对比 SDIO SD 卡）"
  - "[[34_rocketpi_w25qxx]] SPI Flash（对比 SDIO SD 卡）"
---

# 28 SDIO SD 卡读写

## 一句话定性

SDIO（Secure Digital I/O）是 SD 卡的标准高速接口，MCU 通过 SDIO 总线以块为单位读写 SD 卡，支持 1/4 位总线宽度和 DMA 传输，用于嵌入式系统的大容量存储（音频、图片、日志、固件升级等）。

## 同类产品定位

- **SD 卡**：最普及的可移动存储，容量 2MB~2TB，成本 ~¥10（16GB）
- **eMMC**：焊接式存储，容量更大，速度更快，不可更换
- **SPI Flash（W25Qxx）**：焊接式，容量小（1~128MB），但接口简单
- **I2C EEPROM（AT24CXX）**：容量最小（256B~512KB），适合小量配置数据
- **本例选型理由**：SD 卡容量大、可更换、SDIO 接口速度快，适合学习块设备读写

## 硬件连接

- SDIO 4 线：CLK + CMD + D0~D3
- 引脚分配：PC8=D0, PC9=D1, PC10=D2, PC11=D3, PC12=CLK, PD2=CMD
- 本例使用 1 位总线宽度（SDIO_BUS_WIDE_1B），注释提到 4 位模式有已知 bug

## 通信协议要点

- **SD 卡协议**：命令-响应模式，CMD0~CMD58 定义了初始化、读写、状态查询等操作
- **块读写**：SD 卡以块（通常 512 字节）为最小读写单位
- **地址模式**：SDSC 用字节地址，SDHC/SDXC 用块地址
- **DMA 传输**：`HAL_SD_ReadBlocks_DMA` / `HAL_SD_WriteBlocks_DMA` 非阻塞
- **卡状态轮询**：写入后需等待卡进入 TRANSFER 状态

## 代码架构

```
┌─────────────────────────────────────────────────┐
│ main.c (应用层)                                 │
│  - sdcard_test_run(0x00, 1024)                 │
├─────────────────────────────────────────────────┤
│ driver_sdcard_test.c (测试层)                  │
│  - sdcard_test_run：初始化+信息+读写校验       │
│  - sdcard_test_process_range：分块写入+读回+比对│
│  - sdcard_test_fill_pattern：生成测试数据       │
│  - sdcard_test_verify_pattern：校验读回数据     │
├─────────────────────────────────────────────────┤
│ sdio.c (CubeMX 生成)                           │
│  - SDIO 初始化 + DMA 配置                      │
│  - HAL_SD_ReadBlocks_DMA / WriteBlocks_DMA     │
├─────────────────────────────────────────────────┤
│ HAL SD 驱动 (STM32 HAL 库)                     │
│  - SD 卡初始化、命令发送、块读写               │
└─────────────────────────────────────────────────┘
```

## 与 15 例（EEPROM）的关键差异

| 维度 | 15 AT24CXX EEPROM | 28 SDIO SD 卡 |
|------|-------------------|---------------|
| 接口 | I2C | SDIO |
| 容量 | 256B | 2MB~2TB |
| 最小读写单位 | 1 字节 | 512 字节（块） |
| 速度 | ~400kHz | ~25MHz（1 位）/ ~50MHz（4 位） |
| 地址宽度 | 8/16 位 | 32 位块地址 |
| 可更换 | 否 | 是 |
| 文件系统 | 无 | FAT32/exFAT（29 例） |
| 驱动来源 | LibDriver | STM32 HAL 库 |

## 核心实现详解

### sdcard_test_run——完整测试流程

**作用**：初始化 SD 卡、打印卡信息、执行块读写校验。

```c
uint8_t sdcard_test_run(uint32_t block_addr, uint32_t block_count)
{
    HAL_SD_CardInfoTypeDef info;

    // ① 初始化 SD 卡 + 配置 1 位总线 + 获取卡信息
    if (sdcard_test_prepare_card(&info) != 0U) return 1U;

    // ② 打印卡信息
    sdcard_test_print_info(&info);

    // ③ 块读写校验
    if (sdcard_test_process_range(&info, block_addr, block_count) != 0U) return 1U;

    printf("sdcard: test completed successfully\r\n");
    return 0U;
}
```

**main.c 调用**：`sdcard_test_run(0x00, 1024)` — 从块 0 开始测试 1024 块（512KB）。

### sdcard_test_prepare_card——SD 卡初始化

**作用**：初始化 SD 卡硬件、配置 1 位总线宽度、获取卡信息。

```c
static uint8_t sdcard_test_prepare_card(HAL_SD_CardInfoTypeDef *info)
{
    // ④ 初始化 SD 卡（发送 CMD0~CMD8，完成卡识别和初始化流程）
    status = HAL_SD_InitCard(&hsd);

    // ⑤ 配置 1 位总线宽度（避免 4 位模式的已知 bug）
    status = HAL_SD_ConfigWideBusOperation(&hsd, SDIO_BUS_WIDE_1B);

    // ⑥ 获取卡信息
    status = HAL_SD_GetCardInfo(&hsd, info);
}
```

- **④ SD 卡初始化流程**：CMD0（复位）→ CMD8（检查电压）→ ACMD41（初始化）→ CMD2（获取 CID）→ CMD3（获取 RCA）→ CMD7（选中卡）
- **⑤ 1 位总线**：注释提到 SDIO 4 位模式在 STM32F4 HAL 库中有已知 bug，使用 1 位模式规避

### sdcard_test_process_range——分块读写校验

**作用**：在指定块范围内，循环执行"生成数据→写入→等待→读回→校验"。

```c
static uint8_t sdcard_test_process_range(const HAL_SD_CardInfoTypeDef *info,
                                         uint32_t block_addr, uint32_t block_count)
{
    uint8_t buffer[4096];  // ⑦ 4KB 缓冲区
    uint32_t max_chunk_blocks = 4096 / block_size;  // 每次最多 8 块（512B/块）

    while (remaining_blocks > 0) {
        uint32_t chunk_blocks = min(remaining_blocks, max_chunk_blocks);

        // ⑧ 生成测试数据
        sdcard_test_fill_pattern(buffer, chunk_bytes, current_block);

        // ⑨ 写入 SD 卡
        sdcard_test_write_blocks(buffer, current_block, chunk_blocks);
        sdcard_test_wait_ready(5000);  // 等待写入完成

        // ⑩ 读回数据
        sdcard_test_read_blocks(buffer, current_block, chunk_blocks);
        sdcard_test_wait_ready(5000);

        // ⑪ 校验数据
        sdcard_test_verify_pattern(buffer, chunk_bytes, current_block);

        // ⑫ 进度输出
        if ((processed_blocks % 65536) == 0) {
            printf("sdcard: progress %lu/%lu blocks\n", processed_blocks, total_blocks);
        }
    }
}
```

- **⑦ 4KB 缓冲区**：栈上分配，每轮最多写 8 块（8×512=4096）。比 EEPROM 的 66 字节缓冲大得多
- **⑧ 测试数据生成**：`buffer[i] = (i + seed) & 0xFF`，每块数据不同（seed=块地址），确保不同块不会误读
- **⑨⑩ DMA 或阻塞**：`SDCARD_TEST_USE_DMA` 宏控制。DMA 模式下 `HAL_SD_WriteBlocks_DMA` 立即返回，`wait_ready` 轮询卡状态
- **⑪ 数据校验**：逐字节比对，不匹配时打印偏移和期望/实际值
- **⑫ 进度输出**：每 65536 块（32MB）打印一次，避免刷屏

### sdcard_test_wait_ready——卡状态轮询

**作用**：轮询 SD 卡状态，等待进入 TRANSFER 状态（可接受新命令）。

```c
static uint8_t sdcard_test_wait_ready(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    do {
        HAL_SD_CardStateTypeDef state = HAL_SD_GetCardState(&hsd);
        if (state == HAL_SD_CARD_TRANSFER) return 0;  // 就绪
        if (state == HAL_SD_CARD_ERROR) return 1;      // 错误
        HAL_Delay(1);
    } while ((HAL_GetTick() - start) < timeout_ms);

    return 1;  // 超时
}
```

**为什么需要 wait_ready**：SD 卡写入是异步的——`HAL_SD_WriteBlocks_DMA` 返回时数据可能还在卡内部缓冲区。必须等卡状态变为 TRANSFER 才能进行下一次操作。

### sdcard_test_print_info——卡信息打印

```c
static void sdcard_test_print_info(const HAL_SD_CardInfoTypeDef *info)
{
    printf("sdcard: type=%s, version=%s, class=%lu\n", ...);
    printf("sdcard: RCA=%lu, logical blocks=%lu, logical block size=%lu bytes\n", ...);
    printf("sdcard: capacity=%lu MB\n", capacity_mb);
}
```

- **CardType**：SDSC（标准容量）、SDHC/SDXC（高/超大容量）、Secured
- **CardVersion**：1.x（旧）或 2.x+（新，支持 SDHC/SDXC）
- **Class**：速度等级（Class 2/4/6/10），影响最低持续写入速度
- **RCA**：Relative Card Address，总线上多卡时的地址标识

### SDIO 4 位模式 bug

main.c 注释提到：`sdio 4bit bug: https://community.st.com/...`

这是 STM32F4 HAL 库的已知问题——SDIO 4 位模式在某些 SD 卡上初始化失败。规避方案是使用 1 位模式（速度减半但稳定）。

## 设计问题与改进空间

1. **1 位总线速度限制**：SDIO 1 位模式理论最大 ~25MHz × 1bit = 25Mbps ≈ 3MB/s。4 位模式可翻倍到 ~12MB/s。已知 bug 限制了性能。

2. **4KB 栈缓冲区**：`uint8_t buffer[4096]` 在栈上分配，对嵌入式系统来说偏大。可改为静态缓冲区或动态分配。

3. **测试数据覆盖**：`sdcard_test_fill_pattern` 生成的是线性递增数据（`(i+seed)&0xFF`），只能检测基本读写错误。更严格的测试应包含随机数据、全 0/全 1、地址相关模式等。

4. **无文件系统**：28 例直接操作块设备（裸读写），没有文件系统。29 例将引入 FATFS 实现文件级读写。

5. **`sdcard_test_wait_ready` 的 1ms 延时**：每次轮询 `HAL_Delay(1)`，CPU 空转。可改为 DMA 完成中断 + 信号量（RTOS 场景）或回调（裸机场景）。

## 关联笔记

- [[15_rocketpi_i2c_at24cxx|15 I2C EEPROM]]：小容量存储（256B），对比 SD 卡大容量存储
- [[34_rocketpi_w25qxx|34 SPI Flash]]：中容量存储（1~128MB），对比 SDIO 速度
- [[29_rocketpi_sdio_card_fatfs|29 SD 卡 FATFS]]：在 28 例基础上加文件系统
- [[27_rocketpi_i2s|27 I2S 音频]]：SD 卡存储音频数据的典型应用场景
