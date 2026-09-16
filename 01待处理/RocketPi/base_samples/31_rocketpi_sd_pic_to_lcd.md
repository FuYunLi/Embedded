---
status: done
created: 2026-09-16
tags:
  - c/fatfs
  - c/spi
  - c/display
  - embedded/multimedia
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/31_rocketpi_sd_pic_to_lcd"
  - "[[29_rocketpi_sdio_card_fatfs]] FATFS 文件系统（31 例的存储层）"
  - "[[25_rocketpi_spi_lcd_bitmap]] ST7789 LCD 驱动（31 例的显示层）"
  - "[[30_rocketpi_sd_audio_to_i2s]] SD 卡音频播放（对比 SD 卡图片显示）"
---

# 31 SD 卡图片到 LCD 显示

## 一句话定性

从 SD 卡读取 RGB565 格式的 BIN 图片文件，通过 FATFS 文件系统遍历目录，分批读取并逐行发送到 ST7789 LCD 显示，实现 SD 卡图片幻灯片播放，是嵌入式系统的图片显示方案。

## 代码架构

```
main.c (20KB，全部业务逻辑)
├── fatfs_test：FATFS 基本读写验证
├── fatfs_speed_test：SD 卡读写速度测试
├── fatfs_playback_frames_optimized：主循环图片播放
│   ├── lcd_populate_frame_list：扫描目录收集 .bin 文件名
│   └── lcd_draw_frame_batched：分批读取+逐行显示单张图片
└── lcd_is_bin_frame：文件名后缀过滤
```

## 核心设计

### 目录扫描 + 文件名缓存

```c
static LCD_FrameEntry s_frame_cache[600];  // 最多缓存 600 个文件名
lcd_populate_frame_list("PIC_BIN", s_frame_cache, 600, &s_frame_count);
```

用 `f_opendir` + `f_readdir` 遍历 SD 卡目录，过滤 `.bin` 后缀文件，将文件名缓存到静态数组。只扫描一次（`s_cache_valid` 标志），后续循环直接使用缓存。

### 分批显示单张图片

```c
static FRESULT lcd_draw_frame_batched(const char *directory, const char *file_name,
                                      uint8_t *batch_buffer, UINT batch_rows)
{
    f_open(&frame_file, frame_path, FA_READ);

    uint32_t row = 0;
    while (row < 240) {
        UINT rows_this = min(batch_rows, 240 - row);
        f_read(&frame_file, batch_buffer, rows_this * 480, &bytes_read);
        ST7789_DrawBitmap(0, row, 240, rows_this, batch_buffer);  // 逐批显示
        row += rows_this;
    }

    f_close(&frame_file);
}
```

- **batch_rows = 120**：每次读 120 行 = 120×480 = 57,600 字节（~56KB），分 2 次显示完整 240×240 图片
- **逐批显示**：不等整张图片读完就开始显示，减少首帧延迟
- **DrawBitmap 复用**：调用 25 例的 `ST7789_DrawBitmap`，内部处理小端转大端

### 主循环图片播放

```c
static void fatfs_playback_frames_optimized(void)
{
    f_mount(&SDFatFS, SDPath, 1);

    // 首次扫描目录
    if (!s_cache_valid) {
        lcd_populate_frame_list("PIC_BIN", s_frame_cache, 600, &s_frame_count);
        s_cache_valid = 1;
    }

    // 逐帧显示
    for (UINT idx = 0; idx < s_frame_count; ++idx) {
        lcd_draw_frame_batched("PIC_BIN", s_frame_cache[idx].name, batch_buffer, 120);
        HAL_Delay(25);  // 帧间隔
    }

    f_mount(NULL, SDPath, 0);
}
```

- **25ms 帧间隔**：约 40 FPS 理论值，实际受 SD 卡读取速度限制
- **循环播放**：主循环 `while(1)` 中反复调用，实现无限幻灯片

### 文件名过滤

```c
static bool lcd_is_bin_frame(const char *name)
{
    // 跳过空名和隐藏文件（以.开头）
    // 比较后缀是否为 ".bin"（不区分大小写）
}
```

用 `tolower` 逐字符比较，兼容 `.BIN`/`.Bin`/`.bin` 等大小写变体。

## 与 25/30 例的关键差异

| 维度 | 25 LCD 位图 | 30 SD 音频 | 31 SD 图片 |
|------|------------|------------|------------|
| 数据源 | Flash 数组 | SD 卡 PCM 文件 | SD 卡 BIN 文件 |
| 显示方式 | 一次性 DrawBitmap | 无（音频） | 分批 DrawBitmap |
| 目录遍历 | 无 | 单文件 | f_opendir 扫描 |
| 文件名缓存 | 无 | 无 | 600 条静态数组 |
| 分批策略 | 无（整张） | 双缓冲 DMA | 120 行/批 |
| 帧率 | N/A | N/A | ~40 FPS 理论 |

## 设计问题与改进空间

1. **600 条文件名缓存**：`LCD_FrameEntry[600]` × 13 字节 = 7.8KB 静态 RAM。对 96KB SRAM 的 F401RE 可接受，但可改为动态分配或减少上限。

2. **25ms 帧间隔过短**：实际 SD 卡读取 + LCD 传输可能超过 25ms，`HAL_Delay(25)` 只是额外等待。真实帧率取决于 SD 卡读取速度（~3MB/s 1 位模式下，115KB/帧 ≈ 38ms）。

3. **无图片格式解析**：只支持原始 RGB565 BIN 格式，不支持 JPEG/PNG/BMP。嵌入式解码 JPEG 需要额外库（如 TJpgDec）。

4. **f_opendir 每次循环都调用**：`s_cache_valid` 只在首次扫描，但如果 SD 卡被热插拔，缓存失效。可加卡检测逻辑。

5. **与 30 例的架构对比**：30 例是"SD 卡→音频→I2S 播放"，31 例是"SD 卡→图片→LCD 显示"。两者都用 FATFS 读取 SD 卡，但输出目标不同（音频外设 vs 显示外设）。

## 关联笔记

- [[25_rocketpi_spi_lcd_bitmap|25 SPI LCD 位图]]：ST7789 DrawBitmap，31 例复用
- [[29_rocketpi_sdio_card_fatfs|29 FATFS 文件系统]]：目录遍历和文件读写，31 例复用
- [[30_rocketpi_sd_audio_to_i2s|30 SD 卡音频]]：SD 卡多媒体播放的姊妹篇
