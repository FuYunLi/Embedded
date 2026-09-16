---
status: done
created: 2026-09-16
tags:
  - c/usb
  - c/msc
  - embedded/storage
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/33_rocketpi_usb_msc"
  - "[[32_rocketpi_usb_cdc]] USB CDC 虚拟串口（对比 USB MSC 存储）"
  - "[[28_rocketpi_sdio_card]] SD 卡块设备（对比 USB MSC 块设备）"
---

# 33 USB MSC 大容量存储

## 一句话定性

USB MSC（Mass Storage Class）将 MCU 的 RAM 模拟为 U 盘，PC 通过 USB 直接读写 MCU 内存，实现 64KB RAM 盘，掉电即失，适合调试和临时数据交换。

## 同类产品定位

- **USB MSC**：大容量存储类，PC 免驱，自动挂载为 U 盘
- **USB CDC**（32 例）：虚拟串口，字节流通信
- **SD 卡**（28 例）：外部块设备，大容量持久化
- **本例选型理由**：USB MSC 最直观的 USB 存储演示，无需外部存储介质

## 硬件连接

- USB：PA11=DM, PA12=DP（同 32 例，STM32F401 内置 USB 外设）
- 无外部存储：64KB 片上 SRAM 模拟 U 盘

## 通信协议要点

- **USB MSC**：块设备协议，PC 发送 SCSI 命令（READ(10)/WRITE(10)）读写块
- **块大小**：512 字节（标准扇区大小）
- **容量**：64KB = 128 个扇区
- **文件系统**：PC 端需格式化为 FAT/FAT12（首次连接提示"未格式化"）

## 代码架构

```
┌─────────────────────────────────────────────────┐
│ main.c (应用层，极简)                           │
│  - MSC_FlashStorage_Init() → MX_USB_DEVICE_Init()
│  - 主循环为空（USB 中断驱动）                   │
├─────────────────────────────────────────────────┤
│ usbd_storage_if.c (CubeMX 生成的 MSC 移植层)   │
│  - 64KB RAM 数组模拟块设备                      │
│  - MSC 读写回调：读 RAM / 写 RAM               │
│  - 范围检查 + 扇区映射                          │
├─────────────────────────────────────────────────┤
│ USB Device 库 (STM32 USB 中间件)               │
│  - USB 枚举、MSC 类协议、SCSI 命令处理         │
└─────────────────────────────────────────────────┘
```

## 与 32 例（USB CDC）的关键差异

| 维度 | 32 USB CDC | 33 USB MSC |
|------|-----------|------------|
| 设备类型 | 虚拟串口 | U 盘 |
| 数据格式 | 字节流 | 块设备（512B/扇区） |
| PC 端表现 | COM 口 | 可移动磁盘 |
| 驱动 | Win10+ 免驱 | 免驱 |
| 存储介质 | 无 | 64KB RAM |
| 文件系统 | 无 | FAT12（PC 格式化） |
| 掉电保持 | 无 | 无（RAM） |

## 核心实现详解

### main.c——初始化 + 空循环

```c
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MSC_FlashStorage_Init();   // ① 初始化 64KB RAM 盘
    MX_USB_DEVICE_Init();      // ② USB 设备初始化 + MSC 类注册

    while (1) {}               // ③ 主循环为空，USB 全由中断驱动
}
```

- **① MSC_FlashStorage_Init**：在 USB 初始化前调用，将 64KB RAM 填充为 0xFF（空闪存状态）
- **② MX_USB_DEVICE_Init**：CubeMX 生成，注册 MSC 类，PC 枚举为 U 盘
- **③ 空循环**：USB MSC 完全由中断驱动（USB 枚举、SCSI 命令、数据传输），主循环无需做任何事

### usbd_storage_if.c——RAM 盘实现

CubeMX 生成的 MSC 移植层，核心是 4 个回调函数：

```c
// 64KB RAM 盘
static uint8_t ram_disk[64 * 1024];  // 65536 字节 = 128 扇区 × 512 字节

// 读扇区回调
static int8_t STORAGE_Read_FS(uint8_t lun, uint8_t *buf,
                               uint32_t blk_addr, uint16_t blk_len)
{
    uint32_t offset = blk_addr * 512;
    uint32_t length = blk_len * 512;

    if (offset + length > sizeof(ram_disk)) return USBD_FAIL;  // 范围检查

    memcpy(buf, &ram_disk[offset], length);
    return USBD_OK;
}

// 写扇区回调
static int8_t STORAGE_Write_FS(uint8_t lun, const uint8_t *buf,
                                uint32_t blk_addr, uint16_t blk_len)
{
    uint32_t offset = blk_addr * 512;
    uint32_t length = blk_len * 512;

    if (offset + length > sizeof(ram_disk)) return USBD_FAIL;

    memcpy(&ram_disk[offset], buf, length);
    return USBD_OK;
}
```

- **块地址 → 字节偏移**：`blk_addr * 512`，PC 发送的扇区号转换为 RAM 数组偏移
- **范围检查**：防止 PC 发送越界扇区号导致内存踩踏
- **memcpy 直接读写**：RAM 盘就是普通数组，无 Flash 擦写延迟

### MSC_FlashStorage_Init——RAM 盘初始化

```c
void MSC_FlashStorage_Init(void)
{
    memset(ram_disk, 0xFF, sizeof(ram_disk));  // 填充 0xFF（空闪存状态）
}
```

填充 0xFF 而非 0x00：模拟空 Flash 的状态（NOR Flash 擦除后全为 1）。PC 格式化时会写入 FAT 表和根目录。

### PC 端使用流程

```
1. MCU 上电 → USB 枚举 → PC 显示"可移动磁盘"
2. 首次连接：PC 提示"未格式化" → 右键格式化为 FAT12
3. 格式化后：可正常拖拽文件读写
4. MCU 复位/断电：数据丢失（RAM 盘）
```

### USB MSC 协议简述

```
PC 端                              MCU 端
  │                                  │
  ├─ USB 枚举（同 CDC）              │
  │                                  │
  ├─ SCSI INQUIRY ─────────────────→│ 返回设备信息
  ├─ SCSI READ CAPACITY ───────────→│ 返回 128 扇区 × 512B
  ├─ SCSI READ(10) ────────────────→│ 读 RAM 盘数据
  ├─ SCSI WRITE(10) ───────────────→│ 写 RAM 盘数据
  │                                  │
  ├─ PC 格式化为 FAT12               │
  │                                  │
  ├─ 正常文件读写                    │
```

## 设计问题与改进空间

1. **64KB 容量太小**：只能存几个小文件。可扩展为外部 Flash（W25Qxx，1~128MB）或 SD 卡（34/28 例）。

2. **掉电丢失**：RAM 盘数据不持久化。可改为 Flash 后台存储（读写时自动同步到 Flash）。

3. **无写保护**：PC 可以任意读写 RAM 盘。可加写保护标志或只读模式。

4. **单 LUN**：只支持一个逻辑单元（一个 U 盘）。可扩展为多 LUN（多个虚拟磁盘）。

5. **与 32 例的 USB 设计对比**：32 例用 CDC 类（字节流），33 例用 MSC 类（块设备）。USB 类决定了 PC 端的行为——CDC 是串口，MSC 是磁盘。

6. **与 28 例（SD 卡）对比**：28 例是真实块设备（SD 卡），33 例是虚拟块设备（RAM）。两者都以 512 字节扇区为单位读写，但 28 例有物理存储介质，33 例掉电即失。

## 关联笔记

- [[32_rocketpi_usb_cdc|32 USB CDC 虚拟串口]]：另一种 USB 设备类
- [[28_rocketpi_sdio_card|28 SDIO SD 卡]]：真实块设备，对比 RAM 盘虚拟块设备
- [[34_rocketpi_w25qxx|34 SPI Flash]]：外部 Flash 存储，可扩展为 USB MSC 介质
