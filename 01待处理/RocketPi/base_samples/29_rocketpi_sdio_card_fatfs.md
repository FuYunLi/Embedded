---
status: done
created: 2026-09-16
tags:
  - c/fatfs
  - c/filesystem
  - embedded/storage
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/29_rocketpi_sdio_card_fatfs"
  - "[[28_rocketpi_sdio_card]] SDIO SD 卡裸读写（29 例的基础）"
  - "[[15_rocketpi_i2c_at24cxx]] I2C EEPROM（对比文件系统级存储）"
---

# 29 SDIO SD 卡 FATFS 文件系统

## 一句话定性

在 28 例 SDIO SD 卡裸读写基础上移植 FatFS 文件系统，实现文件级读写（创建/写入/读取/删除），支持自动格式化和读写速度测试，用于嵌入式系统的文件存储（日志、配置、音频、固件等）。

## 同类产品定位

- **FatFS**：开源 FAT 文件系统模块，ANSI C 编写，MIT 许可，适合嵌入式
- **LittleFS**：专为 NOR Flash 设计的日志文件系统，掉电安全
- **SPIFFS**：SPI Flash 文件系统，无目录支持
- **本例选型理由**：FatFS 支持 FAT12/16/32/exFAT，兼容 PC 读卡器，最通用

## 硬件连接

- SDIO：同 28 例（PC8~PC11=D0~D3, PC12=CLK, PD2=CMD）
- 本例使用 1 位总线宽度（同 28 例规避 4 位 bug）

## 通信协议要点

- **FAT 文件系统**：文件分配表，支持 FAT12/16/32/exFAT
- **簇（Cluster）**：文件存储的最小分配单位，通常 4KB~32KB
- **目录项**：32 字节，存储文件名、属性、起始簇号、文件大小
- **FatFS API**：f_mount → f_open → f_write/f_read → f_close → f_unlink

## 代码架构

```
┌─────────────────────────────────────────────────┐
│ main.c (应用层 + 测试)                          │
│  - FATFS_Test：基本文件读写验证                 │
│  - FATFS_SpeedTest：读写速度测试                │
├─────────────────────────────────────────────────┤
│ fatfs/ (CubeMX 生成的 FatFS 移植层)            │
│  - SDFatFS / SDFile / SDPath 全局变量           │
│  - diskio.c：FatFS → HAL SDIO 桥接             │
├─────────────────────────────────────────────────┤
│ FatFS 库 (第三方)                               │
│  - f_mount / f_open / f_write / f_read /       │
│    f_close / f_unlink / f_mkfs / f_sync        │
├─────────────────────────────────────────────────┤
│ sdio.c + HAL SD 驱动 (同 28 例)                │
└─────────────────────────────────────────────────┘
```

## 与 28 例（裸读写）的关键差异

| 维度 | 28 裸读写 | 29 FATFS |
|------|----------|----------|
| 访问方式 | 块地址（0, 1, 2...） | 文件路径（"rocketpi.txt"） |
| 最小单位 | 512 字节块 | 任意字节（FatFS 内部分块） |
| 目录支持 | 无 | 有（FAT 目录结构） |
| 跨平台 | 需专用工具读取 | PC 读卡器直接读取 |
| 格式化 | 无 | f_mkfs 自动格式化 |
| 掉电安全 | 块级完整 | 文件级可能损坏（FAT 表） |
| 代码复杂度 | 低（HAL 直接调用） | 中（FatFS 移植 + API） |

## 核心实现详解

### FATFS_Test——基本文件读写验证

**作用**：挂载 SD 卡 → 创建文件 → 写入字符串 → 关闭 → 重新打开 → 读回 → 比对验证。

```c
static void FATFS_Test(void)
{
    const char test_file[] = "rocketpi.txt";
    const char test_payload[] = "RocketPi FATFS SDIO write/read demo.\r\n";

    // ① 挂载 SD 卡
    res = f_mount(&SDFatFS, (TCHAR const*)SDPath, 1);

    // ② 如果没有文件系统，自动格式化
    if (res == FR_NO_FILESYSTEM) {
        printf("fatfs: no filesystem, formatting...\r\n");
        res = f_mkfs((TCHAR const*)SDPath, FM_ANY, 0, mkfs_work, 4096);
        // 格式化后重新挂载
        res = f_mount(&SDFatFS, (TCHAR const*)SDPath, 1);
    }

    // ③ 创建并写入文件
    res = f_open(&SDFile, test_file, FA_CREATE_ALWAYS | FA_WRITE);
    res = f_write(&SDFile, test_payload, payload_len, &bytes_written);
    f_close(&SDFile);

    // ④ 重新打开并读取
    res = f_open(&SDFile, test_file, FA_READ);
    res = f_read(&SDFile, read_buffer, sizeof(read_buffer) - 1, &bytes_read);
    f_close(&SDFile);

    // ⑤ 比对验证
    if ((bytes_read == bytes_written) &&
        (strncmp(read_buffer, test_payload, payload_len) == 0)) {
        printf("fatfs: verification OK\r\n");
    }

    // ⑥ 卸载
    f_mount(NULL, (TCHAR const*)SDPath, 0);
}
```

- **① f_mount**：挂载文件系统到指定路径（`SDPath` 由 CubeMX 生成，通常为 `"0:/"`）。第二个参数 `1` 表示立即挂载
- **② 自动格式化**：`FR_NO_FILESYSTEM` 表示 SD 卡没有 FAT 文件系统。`f_mkfs` 创建 FAT32/exFAT，`mkfs_work` 是 4KB 工作缓冲区（栈上分配）
- **③ f_open 标志**：`FA_CREATE_ALWAYS` 总是创建新文件（已存在则覆盖），`FA_WRITE` 允许写入
- **④ f_read**：读取到 `read_buffer`，留 1 字节给 `\0` 终止符
- **⑥ f_mount(NULL, ...)**：卸载文件系统，刷新缓存

### FATFS_SpeedTest——读写速度测试

**作用**：写入指定大小的数据到文件，计时并计算写入/读取速度（KB/s）。

```c
static void FATFS_SpeedTest(uint32_t kilobytes)
{
    uint32_t total_bytes = kilobytes * 1024;
    static uint8_t transfer_buffer[4096];

    // ⑦ 填充测试数据
    for (uint32_t i = 0; i < 4096; ++i) {
        transfer_buffer[i] = (uint8_t)(i & 0xFF);
    }

    // ⑧ 写入测试
    f_mount(&SDFatFS, SDPath, 1);
    f_open(&SDFile, "sd_speed.bin", FA_CREATE_ALWAYS | FA_WRITE);

    start_tick = HAL_GetTick();
    while (remaining > 0) {
        uint32_t chunk = min(remaining, 4096);
        f_write(&SDFile, transfer_buffer, chunk, &bytes_io);
        remaining -= chunk;
    }
    f_sync(&SDFile);  // ⑨ 刷新到 SD 卡
    elapsed_ms = HAL_GetTick() - start_tick;
    speed_kbs = total_bytes * 1000 / (elapsed_ms * 1024);
    printf("write done in %lu ms (%lu KB/s)\r\n", elapsed_ms, speed_kbs);

    f_close(&SDFile);

    // ⑩ 读取测试
    f_open(&SDFile, "sd_speed.bin", FA_READ);
    start_tick = HAL_GetTick();
    while (remaining > 0) {
        f_read(&SDFile, transfer_buffer, chunk, &bytes_io);
        remaining -= bytes_io;
    }
    elapsed_ms = HAL_GetTick() - start_tick;
    speed_kbs = total_bytes * 1000 / (elapsed_ms * 1024);
    printf("read done in %lu ms (%lu KB/s)\r\n", elapsed_ms, speed_kbs);

    // ⑪ 清理：删除测试文件 + 卸载
    f_unlink("sd_speed.bin");
    f_mount(NULL, SDPath, 0);
}
```

- **⑧ 4KB 分块写入**：`transfer_buffer` 是 4KB 栈缓冲，每次写 4KB，循环直到写完。512KB 测试 = 128 次写入
- **⑨ f_sync**：强制将 FatFS 缓存刷新到 SD 卡，确保数据落盘后再计时
- **⑩ 读取速度**：通常比写入快（SD 卡读性能优于写）
- **⑪ 清理**：`f_unlink` 删除测试文件，`f_mount(NULL, ...)` 卸载

### FatFS 关键 API 说明

| API | 作用 |
|-----|------|
| `f_mount(fs, path, opt)` | 挂载/卸载文件系统。opt=1 立即挂载，fs=NULL 卸载 |
| `f_open(fp, path, mode)` | 打开文件。mode: FA_READ/FA_WRITE/FA_CREATE_ALWAYS 等 |
| `f_write(fp, buff, btw, bw)` | 写入 btw 字节，实际写入 bw 字节 |
| `f_read(fp, buff, btr, br)` | 读取 btr 字节，实际读取 br 字节 |
| `f_close(fp)` | 关闭文件，刷新元数据 |
| `f_sync(fp)` | 刷新数据到存储介质（不关闭文件） |
| `f_unlink(path)` | 删除文件 |
| `f_mkfs(path, fmt, au, work, len)` | 格式化存储介质，创建文件系统 |

### 自动格式化逻辑

```c
res = f_mount(&SDFatFS, SDPath, 1);
if (res == FR_NO_FILESYSTEM) {
    // SD 卡没有 FAT 文件系统
    res = f_mkfs(SDPath, FM_ANY, 0, mkfs_work, 4096);
    // FM_ANY: 自动选择 FAT12/16/32/exFAT
    // 0: 使用默认簇大小
    // mkfs_work: 4KB 工作缓冲区
    res = f_mount(&SDFatFS, SDPath, 1);  // 重新挂载
}
```

**首次使用自动格式化**：新 SD 卡或未格式化的卡，FatFS 返回 `FR_NO_FILESYSTEM`，自动调用 `f_mkfs` 创建文件系统。之后 PC 读卡器可直接读取。

### CubeMX 生成的 FatFS 移植层

```c
// fatfs.c (CubeMX 生成)
FATFS SDFatFS;    // 文件系统对象
FIL SDFile;        // 文件对象
char SDPath[4];    // 逻辑驱动路径（"0:/"）
```

`diskio.c` 是 FatFS 的底层磁盘 I/O 接口，桥接 FatFS 到 HAL SDIO 驱动：
- `disk_read()` → `HAL_SD_ReadBlocks_DMA()` + `sdcard_test_wait_ready()`
- `disk_write()` → `HAL_SD_WriteBlocks_DMA()` + `sdcard_test_wait_ready()`
- `disk_ioctl()` → `GET_SECTOR_SIZE`/`GET_BLOCK_SIZE` 等

## 设计问题与改进空间

1. **4KB 栈缓冲区**：`FATFS_Test` 和 `FATFS_SpeedTest` 各有 4KB 栈缓冲，总计 8KB。对嵌入式系统来说偏大，可改为静态缓冲区。

2. **f_mkfs 的 4KB 工作缓冲**：`mkfs_work[4096]` 在栈上分配，格式化大容量卡时可能不够。FatFS 文档建议工作缓冲区 ≥ 1 个扇区（通常 512B），4KB 足够。

3. **无错误恢复**：`f_write`/`f_read` 失败后直接 goto cleanup，没有重试逻辑。SD 卡偶尔会有瞬态错误，重试一次可能成功。

4. **f_sync 的时机**：SpeedTest 在写完后调 `f_sync` 确保落盘，但 FATFS_Test 没有调（直接 f_close）。`f_close` 内部会调 `f_sync`，但如果写入后立即断电，未 sync 的数据可能丢失。

5. **1 位总线速度限制**：同 28 例，SDIO 1 位模式 ~3MB/s。4 位模式可翻倍但有已知 bug。

6. **与 28 例的架构演进**：28 例直接操作块设备（裸读写），29 例加了文件系统抽象层。从"块地址"到"文件路径"的升级，是存储应用的质变。

## 关联笔记

- [[28_rocketpi_sdio_card|28 SDIO SD 卡裸读写]]：29 例的基础，块级操作
- [[35_rocketpi_flash_littlefs|35 Flash LittleFS]]：另一种文件系统（日志式，适合 NOR Flash）
- [[27_rocketpi_i2s|27 I2S 音频]]：SD 卡存储音频文件的典型应用场景
- [[30_rocketpi_sd_audio_to_i2s|30 SD 卡音频到 I2S]]：从 SD 卡读取音频文件播放
