---
status: done
created: 2026-09-16
tags:
  - c/littlefs
  - c/spi
  - c/flash
  - embedded/storage
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/36_rocketpi_w25qxx_littlefs"
  - "[[35_rocketpi_flash_littlefs]] 内部 Flash LittleFS（36 例的对照组）"
  - "[[34_rocketpi_w25qxx]] SPI Flash W25QXX 驱动（36 例复用）"
  - "[[29_rocketpi_sdio_card_fatfs]] FATFS 文件系统（对比 LittleFS）"
---

# 36 W25QXX LittleFS 文件系统

## 一句话定性

将 LittleFS 文件系统移植到外部 SPI Flash（W25Q64，8MB），通过 LibDriver 的 W25QXX 驱动实现文件级读写，容量远大于 35 例的内部 Flash 方案，是嵌入式系统的大容量掉电安全存储。

## 同类产品定位

- **35 例**：LittleFS + 内部 Flash（256KB），容量小但零成本
- **36 例**：LittleFS + 外部 SPI Flash（8MB），容量大但需额外芯片
- **29 例**：FATFS + SD 卡（GB 级），PC 兼容但无磨损均衡
- **本例定位**：34+35 的整合，外部 SPI Flash + LittleFS 文件系统

## 硬件连接

- SPI Flash：PB13=SCK, PB14=MISO, PB15=MOSI, PB12=CS（同 34 例）
- 无其他外部器件

## 代码架构

```
┌─────────────────────────────────────────────────┐
│ main.c (应用层)                                 │
│  - 挂载 LittleFS → 写入文件 → 读回验证        │
├─────────────────────────────────────────────────┤
│ lfs_nor_flash_port.c (LittleFS→W25QXX 移植层) │
│  - read/prog/erase/sync 四回调                 │
│  - 懒初始化：首次访问时才初始化 W25QXX 驱动    │
│  - 页编程分片：跨页写入自动拆分                │
├─────────────────────────────────────────────────┤
│ lfs_flash_port.c (LittleFS→内部 Flash 移植层)  │
│  - 35 例的移植层，36 例中保留但未使用          │
├─────────────────────────────────────────────────┤
│ lfs.c (LittleFS 库，197KB)                     │
├─────────────────────────────────────────────────┤
│ driver_w25qxx_*.c (LibDriver，543KB)           │
│  - W25QXX SPI Flash 驱动                       │
├─────────────────────────────────────────────────┤
│ spi.c (CubeMX 生成)                            │
└─────────────────────────────────────────────────┘
```

## 与 35 例（内部 Flash LittleFS）的关键差异

| 维度 | 35 内部 Flash | 36 外部 SPI Flash |
|------|-------------|------------------|
| 存储介质 | STM32 内部 Flash | W25Q64 SPI Flash |
| 容量 | 256KB | 8MB |
| 接口 | 内存映射（直接 memcpy） | SPI 命令（read/page_program/erase） |
| 擦除粒度 | 128KB 扇区 | 4KB 扇区 |
| block_cycles | 100 | 500 |
| prog_size | 4 字节（字编程） | 256 字节（页编程） |
| 懒初始化 | 无（CubeMX 已初始化） | 有（首次访问时初始化 W25QXX） |
| 代码依赖 | HAL Flash API | LibDriver W25QXX（543KB） |

## 核心实现详解

### lfs_nor_flash_port.c——LittleFS→W25QXX 移植层

#### lfs_config 配置

```c
const struct lfs_config lfs_nor_flash_port_cfg = {
    .read = lfs_nor_flash_read,
    .prog = lfs_nor_flash_prog,
    .erase = lfs_nor_flash_erase,
    .sync = lfs_nor_flash_sync,
    .read_size = 16,           // 最小读取单位
    .prog_size = 256,          // 最小编程单位（页大小）
    .block_size = LFS_NOR_FLASH_BLOCK_SIZE,   // 4KB（扇区大小）
    .block_count = LFS_NOR_FLASH_BLOCK_COUNT, // 2048（8MB / 4KB）
    .cache_size = 256,         // 缓存大小 = 页大小
    .lookahead_size = 16,      // 磨损均衡前瞻
    .block_cycles = 500,       // 每块擦写 500 次后轮换
};
```

**与 35 例对比**：`prog_size = 256`（W25QXX 页编程）vs `prog_size = 4`（内部 Flash 字编程）。这决定了 LittleFS 写入的最小粒度。

#### lfs_nor_flash_read——读取

```c
static int lfs_nor_flash_read(const struct lfs_config *cfg, lfs_block_t block,
                              lfs_off_t off, void *buffer, lfs_size_t size)
{
    if (!lfs_nor_flash_range_valid(block, off, size)) return LFS_ERR_CORRUPT;

    // 懒初始化
    if (!g_w25qxx_ready && lfs_nor_flash_hw_init() != LFS_ERR_OK) return LFS_ERR_IO;

    uint32_t address = block * LFS_NOR_FLASH_BLOCK_SIZE + off;
    if (w25qxx_read(&g_w25qxx_handle, address, buffer, size) != 0U) return LFS_ERR_IO;

    return LFS_ERR_OK;
}
```

**与 35 例对比**：35 例直接 `memcpy` 从内存映射地址读取；36 例通过 SPI 命令 `w25qxx_read` 从外部 Flash 读取。

#### lfs_nor_flash_prog——页编程

```c
static int lfs_nor_flash_prog(const struct lfs_config *cfg, lfs_block_t block,
                              lfs_off_t off, const void *buffer, lfs_size_t size)
{
    uint32_t address = block * LFS_NOR_FLASH_BLOCK_SIZE + off;
    const uint8_t *data = (const uint8_t *)buffer;

    // ① 页边界分片
    while (size > 0U) {
        uint32_t page_offset = address % 256;
        uint32_t chunk = 256 - page_offset;  // 当前页剩余空间
        if (chunk > size) chunk = size;

        if (w25qxx_page_program(&g_w25qxx_handle, address, data, chunk) != 0U)
            return LFS_ERR_IO;

        address += chunk;
        data += chunk;
        size -= chunk;
    }
    return LFS_ERR_OK;
}
```

**① 页边界分片**：W25QXX 页编程最大 256 字节，跨页写入会回绕到页首。移植层将跨页写入拆分为多次页编程，每次不超过页边界。

#### lfs_nor_flash_erase——扇区擦除

```c
static int lfs_nor_flash_erase(const struct lfs_config *cfg, lfs_block_t block)
{
    uint32_t address = block * LFS_NOR_FLASH_BLOCK_SIZE;
    if (w25qxx_sector_erase_4k(&g_w25qxx_handle, address) != 0U)
        return LFS_ERR_IO;
    return LFS_ERR_OK;
}
```

4KB 扇区擦除，与 35 例的 128KB 扇区擦除相比粒度更细，擦除速度更快。

#### lfs_nor_flash_hw_init——懒初始化

```c
static int lfs_nor_flash_hw_init(void)
{
    if (g_w25qxx_ready) return LFS_ERR_OK;  // 已初始化则跳过

    // 注入回调
    DRIVER_W25QXX_LINK_INIT(&g_w25qxx_handle, w25qxx_handle_t);
    DRIVER_W25QXX_LINK_SPI_QSPI_INIT(&g_w25qxx_handle, w25qxx_interface_spi_qspi_init);
    // ... 其他回调 ...

    // 设置型号和接口
    w25qxx_set_type(&g_w25qxx_handle, W25Q64);
    w25qxx_set_interface(&g_w25qxx_handle, W25QXX_INTERFACE_SPI);
    w25qxx_init(&g_w25qxx_handle);

    g_w25qxx_ready = true;
    return LFS_ERR_OK;
}
```

**懒初始化**：首次调用 read/prog/erase 时才初始化 W25QXX 驱动。避免 LittleFS 格式化或挂载失败时的无谓初始化。

### main.c——LittleFS 测试

```c
// 与 35 例几乎相同的测试逻辑
lfs_nor_flash_mount(&g_lfs);  // 挂载（失败则格式化后重新挂载）
lfs_file_open(&g_lfs, &file, "w25qxx.txt", LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
lfs_file_write(&g_lfs, &file, payload, strlen(payload));
lfs_file_close(&g_lfs, &file);
// ... 读回验证 ...
lfs_nor_flash_unmount(&g_lfs);
```

### 双移植层设计

36 例同时保留了两个移植层：
- `lfs_flash_port.c`：内部 Flash（Sector 5，同 35 例但用 Sector 5 而非 6~7）
- `lfs_nor_flash_port.c`：外部 SPI Flash（W25Q64）

main.c 只调用 `lfs_nor_flash_*` 系列函数，`lfs_flash_port.c` 保留备用。**同一个 LittleFS 库，两个不同的底层实现**，展示了 LittleFS 的可移植性。

## 设计问题与改进空间

1. **543KB LibDriver 代码**：同 34 例，全系列 W25QXX 支持代码量巨大。本例只用 W25Q64，可大幅裁剪。

2. **页编程分片的效率**：`lfs_nor_flash_prog` 每次最多写 256 字节，大文件写入需要多次 SPI 命令。可改为 DMA 批量传输。

3. **`block_cycles = 500`**：W25Q64 擦写寿命 10 万次，500 次轮换意味着每块最多擦写 500 次就换下一块。8MB / 4KB = 2048 块，理论总擦写次数 = 2048 × 500 = 102.4 万次。

4. **无掉电恢复测试**：同 35 例，应加断电恢复测试验证 LittleFS 的核心优势。

5. **与 35 例的移植层对比**：35 例的 `lfs_port.c` 用 HAL Flash API（内存映射+字编程），36 例的 `lfs_nor_flash_port.c` 用 LibDriver W25QXX（SPI 命令+页编程）。两者实现了相同的 `lfs_config` 接口，但底层完全不同。

## 关联笔记

- [[35_rocketpi_flash_littlefs|35 Flash LittleFS]]：内部 Flash LittleFS，36 例的对照组
- [[34_rocketpi_w25qxx|34 SPI Flash W25QXX]]：W25QXX 驱动，36 例复用
- [[29_rocketpi_sdio_card_fatfs|29 FATFS 文件系统]]：FAT 文件系统，对比 LittleFS
