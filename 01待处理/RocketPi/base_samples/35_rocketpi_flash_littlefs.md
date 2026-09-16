---
status: done
created: 2026-09-16
tags:
  - c/littlefs
  - c/filesystem
  - c/flash
  - embedded/storage
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/35_rocketpi_flash_littlefs"
  - "[[29_rocketpi_sdio_card_fatfs]] FATFS 文件系统（对比 LittleFS）"
  - "[[34_rocketpi_w25qxx]] SPI Flash（35 例用内部 Flash）"
---

# 35 Flash LittleFS 文件系统

## 一句话定性

LittleFS 是专为 NOR Flash 设计的日志式文件系统，掉电安全、磨损均衡，在 STM32F401 内部 Flash 上运行，实现文件级读写，适合嵌入式系统的配置存储、日志记录等。

## 同类产品定位

- **LittleFS**：日志式文件系统，掉电安全，磨损均衡，适合 NOR Flash
- **FatFS**（29 例）：FAT 文件系统，适合 SD 卡等块设备，PC 兼容
- **SPIFFS**：SPI Flash 文件系统，无目录支持，已停维护
- **本例选型理由**：LittleFS 最适合 NOR Flash（内部 Flash/SPI Flash），掉电安全

## 硬件连接

- 无外部器件：使用 STM32F401 内部 Flash（512KB）
- Flash 分区：Sector 6~7（共 256KB）用于 LittleFS

## 通信协议要点

- 无通信协议，直接读写内部 Flash 寄存器
- Flash 写入约束：必须先擦除（扇区→0xFFFFFFFF），再编程（只能 1→0）
- LittleFS 掩盖了擦写细节，提供文件级 API

## 代码架构

```
┌─────────────────────────────────────────────────┐
│ main.c (应用层)                                 │
│  - LittleFS_Test：挂载→写入→读回→验证          │
├─────────────────────────────────────────────────┤
│ lfs_port.c (移植层)                             │
│  - lfs_port_read/prog/erase/sync               │
│  - HAL Flash API 封装                           │
│  - lfs_config 配置结构体                        │
├─────────────────────────────────────────────────┤
│ lfs.c (LittleFS 库，197KB)                     │
│  - 文件系统核心：日志、磨损均衡、掉电恢复      │
├─────────────────────────────────────────────────┤
│ HAL Flash 驱动                                  │
│  - HAL_FLASH_Program / HAL_FLASHEx_Erase       │
└─────────────────────────────────────────────────┘
```

## 与 29 例（FATFS）的关键差异

| 维度 | 29 FATFS/SD 卡 | 35 LittleFS/内部 Flash |
|------|----------------|----------------------|
| 存储介质 | SD 卡（块设备） | 内部 Flash（NOR Flash） |
| 文件系统 | FAT12/16/32/exFAT | LittleFS（日志式） |
| 掉电安全 | FAT 表可能损坏 | 日志式，掉电安全 |
| 磨损均衡 | 无 | 有（block_cycles=100） |
| PC 兼容 | 是（读卡器直接读） | 否（需专用工具） |
| 容量 | GB 级 | 256KB（本例） |
| 目录支持 | 有 | 有 |
| API | f_open/f_read/f_write | lfs_file_open/read/write |

## 核心实现详解

### lfs_port.c——移植层

LittleFS 通过 4 个回调函数与底层 Flash 交互：

```c
const struct lfs_config lfs_port_cfg = {
    .read = lfs_port_read,      // 读取
    .prog = lfs_port_prog,      // 编程（写入）
    .erase = lfs_port_erase,    // 擦除
    .sync = lfs_port_sync,      // 同步（内部 Flash 无需）
    .read_size = 16,            // 最小读取单位
    .prog_size = 4,             // 最小编程单位（32 位字）
    .block_size = LFS_PORT_FLASH_BLOCK_SIZE,  // 扇区大小（128KB）
    .block_count = LFS_PORT_FLASH_BLOCK_COUNT, // 扇区数（2）
    .cache_size = 256,          // 缓存大小
    .lookahead_size = 16,       // 磨损均衡前瞻缓冲
    .block_cycles = 100,        // 每块擦写 100 次后轮换
};
```

### lfs_port_read——读取

```c
static int lfs_port_read(const struct lfs_config *cfg, lfs_block_t block,
                         lfs_off_t off, void *buffer, lfs_size_t size)
{
    // 范围检查
    if (!lfs_port_range_valid(block, off, size)) return LFS_ERR_CORRUPT;

    // 直接从 Flash 地址 memcpy
    const void *src = (const void *)(lfs_port_block_address(block) + off);
    memcpy(buffer, src, size);
    return LFS_ERR_OK;
}
```

内部 Flash 读取就是内存映射直接读，无额外操作。

### lfs_port_prog——编程（写入）

```c
static int lfs_port_prog(const struct lfs_config *cfg, lfs_block_t block,
                         lfs_off_t off, const void *buffer, lfs_size_t size)
{
    uint32_t address = lfs_port_block_address(block) + off;

    HAL_FLASH_Unlock();
    lfs_port_clear_flash_flags();

    // 对齐处理：先字节→再字→再半字→再字节
    lfs_port_flash_program(address, (const uint8_t *)buffer, size);

    HAL_FLASH_Lock();
    return LFS_ERR_OK;
}
```

**对齐处理**：STM32F4 内部 Flash 支持字节/半字/字编程，但字编程最快。`lfs_port_flash_program` 先处理未对齐的头部字节，再批量字编程，最后处理尾部。

### lfs_port_erase——擦除

```c
static int lfs_port_erase(const struct lfs_config *cfg, lfs_block_t block)
{
    FLASH_EraseInitTypeDef erase = {
        .TypeErase = FLASH_TYPEERASE_SECTORS,
        .Banks = FLASH_BANK_1,
        .VoltageRange = FLASH_VOLTAGE_RANGE_3,
        .Sector = LFS_PORT_FIRST_SECTOR + block,  // Sector 6 或 7
        .NbSectors = 1,
    };

    HAL_FLASH_Unlock();
    HAL_FLASHEx_Erase(&erase, &sector_error);
    HAL_FLASH_Lock();
    return LFS_ERR_OK;
}
```

**扇区映射**：`block 0 = Sector 6`，`block 1 = Sector 7`。STM32F401 的 Sector 6/7 各 128KB，共 256KB 给 LittleFS。

### main.c——LittleFS 测试

```c
static void LittleFS_Test(void)
{
    // ① 挂载（失败则格式化后重新挂载）
    err = lfs_port_mount(&g_lfs);
    if (err != LFS_ERR_OK) {
        lfs_port_format(&g_lfs);
        err = lfs_port_mount(&g_lfs);
    }

    // ② 写入文件
    lfs_file_open(&g_lfs, &file, "flash.txt", LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    lfs_file_write(&g_lfs, &file, payload, strlen(payload));
    lfs_file_close(&g_lfs, &file);

    // ③ 读回验证
    lfs_file_open(&g_lfs, &file, "flash.txt", LFS_O_RDONLY);
    lfs_file_read(&g_lfs, &file, read_buffer, sizeof(read_buffer) - 1);
    lfs_file_close(&g_lfs, &file);

    // ④ 卸载
    lfs_port_unmount(&g_lfs);
}
```

**自动格式化**：首次使用或 Flash 损坏时，`lfs_mount` 失败 → `lfs_format` → 重新 `lfs_mount`。与 29 例 FATFS 的自动格式化逻辑相同。

## 设计问题与改进空间

1. **256KB 容量**：STM32F401 总共 512KB Flash，程序代码占一部分，留给 LittleFS 的只有 Sector 6~7（256KB）。对配置存储够用，但不适合大文件。

2. **擦写寿命**：STM32 内部 Flash 擦写寿命约 1 万次。`block_cycles=100` 意味着每块擦写 100 次后轮换到下一块，延长整体寿命。

3. **读取期间不能写入**：Flash 读取是内存映射，但写入需要解锁+擦除+编程，期间 Flash 不可读。LittleFS 内部处理了这个约束，但应用层需注意不在写入期间读取。

4. **无掉电恢复测试**：测试代码只做了基本读写验证，没有模拟掉电恢复。LittleFS 的核心优势是掉电安全，应加断电恢复测试。

5. **与 29 例的架构对比**：29 例用 CubeMX 生成的 FatFS 移植层（diskio.c），35 例手写 LittleFS 移植层（lfs_port.c）。两者都是"文件系统库 + 移植层 + 底层驱动"的三层架构。

## 关联笔记

- [[29_rocketpi_sdio_card_fatfs|29 FATFS 文件系统]]：FAT 文件系统，对比 LittleFS
- [[34_rocketpi_w25qxx|34 SPI Flash]]：外部 NOR Flash，可在其上运行 LittleFS
- [[36_rocketpi_w25qxx_littlefs|36 W25QXX LittleFS]]：在外部 SPI Flash 上运行 LittleFS
