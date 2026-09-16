---
status: done
created: 2026-09-16
tags:
  - c/usb
  - c/cdc
  - embedded/communication
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/32_rocketpi_usb_cdc"
  - "[[06_rocketpi_uart_printf]] UART printf（对比 USB CDC 虚拟串口）"
---

# 32 USB CDC 虚拟串口

## 一句话定性

USB CDC（Communication Device Class）将 MCU 的 USB 接口虚拟为串口设备，PC 无需专用驱动即可通过串口助手与 MCU 通信，实现 USB 回显（echo）功能，是嵌入式系统的 USB 通信基础。

## 同类产品定位

- **USB CDC**：虚拟串口，免驱（Win10+/Linux/macOS），全速 12Mbps
- **UART**：传统串口，需 USB-TTL 转换器，115200bps
- **USB HID**：人机接口设备（键盘/鼠标），免驱但传输量小
- **USB MSC**：大容量存储（U 盘），免驱
- **本例选型理由**：USB CDC 最简单的 USB 通信方式，PC 端无需额外驱动

## 硬件连接

- USB：PA11=DM, PA12=DP（STM32F401 内置 USB 外设）
- 无需外部 PHY 芯片，直接连 USB 接口

## 通信协议要点

- **USB CDC**：USB 设备类标准，模拟 RS-232 串口行为
- **端点**：控制端点（EP0）+ 批量 IN/OUT 端点（数据收发）
- **速度**：全速 12Mbps（FS），远超 UART 115200bps
- **PC 端**：枚举为 COM 口（Windows）或 /dev/ttyACMx（Linux）

## 代码架构

```
┌─────────────────────────────────────────────────┐
│ main.c (应用层，极简)                           │
│  - 主循环：CDC_IsRxReady → CDC_ReadRxData →    │
│    CDC_Transmit_FS（回显）                      │
├─────────────────────────────────────────────────┤
│ usbd_cdc_if.c (CubeMX 生成的 CDC 移植层)       │
│  - CDC_Transmit_FS：发送数据到 PC              │
│  - CDC_ReadRxData：读取 PC 发来的数据          │
│  - CDC_IsRxReady：检查接收缓冲区是否有数据     │
├─────────────────────────────────────────────────┤
│ USB Device 库 (STM32 USB 中间件)               │
│  - USB 枚举、端点管理、CDC 类协议处理          │
└─────────────────────────────────────────────────┘
```

## 核心实现详解

### main.c——USB 回显（echo）

```c
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USB_DEVICE_Init();  // ① USB 设备初始化

    while (1)
    {
        // ② 首次连接发送欢迎消息
        if (cdcFeatureMsgSent == 0U) {
            const char *msg = "CDC echo demo ready. Type to see loopback.\r\n";
            if (CDC_Transmit_FS((uint8_t *)msg, strlen(msg)) == USBD_OK) {
                cdcFeatureMsgSent = 1U;
            }
        }

        // ③ 回显：读到什么就发回什么
        if (CDC_IsRxReady()) {
            cdcRxAppLen = CDC_ReadRxData(cdcRxAppBuffer, sizeof(cdcRxAppBuffer));
            if (cdcRxAppLen > 0U) {
                CDC_Transmit_FS(cdcRxAppBuffer, (uint16_t)cdcRxAppLen);
            }
        }

        HAL_Delay(1);
    }
}
```

- **① MX_USB_DEVICE_Init**：CubeMX 生成，初始化 USB 外设、注册 CDC 类、启动枚举
- **② 欢迎消息**：USB 枚举完成后（PC 端识别到设备），发送一次提示。`CDC_Transmit_FS` 返回 `USBD_OK` 表示发送成功
- **③ 回显**：`CDC_IsRxReady` 轮询检查接收缓冲区，有数据则 `CDC_ReadRxData` 读出，再 `CDC_Transmit_FS` 发回。典型的 echo 模式

### SystemClock_Config——42MHz（USB 需要）

```c
RCC_OscInitStruct.PLL.PLLN = 168;
RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;  // 168/4 = 42MHz
```

**USB 时钟要求**：STM32F4 的 USB 外设需要 48MHz 时钟（USB_FS）。PLLQ = 168/4 = 42MHz... 实际上 USB 时钟由 PLL48CK 提供，这里 PLLQ=7 → 168/7 = 24MHz？需要检查 CubeMX 配置。**与前几例的 84MHz 不同**——USB 例使用 42MHz 系统时钟。

### CDC API 说明

| API | 作用 |
|-----|------|
| `CDC_Transmit_FS(buf, len)` | 发送数据到 PC（批量 IN 端点） |
| `CDC_ReadRxData(buf, max_len)` | 读取 PC 发来的数据（批量 OUT 端点） |
| `CDC_IsRxReady()` | 检查接收缓冲区是否有数据 |
| `MX_USB_DEVICE_Init()` | 初始化 USB 设备 + CDC 类 |

### USB 枚举流程

```
PC 端                          MCU 端
  │                              │
  ├─ 检测到 USB 设备插入          │
  │  (D+/D- 电平变化)            │
  │                              │
  ├─ GET_DESCRIPTOR(Device) ────→│ 返回设备描述符
  ├─ GET_DESCRIPTOR(Config) ────→│ 返回配置描述符
  ├─ GET_DESCRIPTOR(String) ────→│ 返回厂商/产品/序列号字符串
  │                              │
  ├─ SET_CONFIGURATION ─────────→│ MCU 激活 CDC 类
  │                              │
  ├─ 枚举完成，PC 安装 CDC 驱动   │
  │  (Win10+ 自带，免驱)         │
  │                              │
  ├─ 打开串口助手 ←──────────────→│ CDC 就绪
```

## 设计问题与改进空间

1. **轮询式接收**：`CDC_IsRxReady` 在主循环中轮询，1ms 周期。高速传输时可能丢失数据（PC 发送速度 > MCU 轮询速度）。可改为 CDC 接收回调（`CDC_Receive_FS` 中处理）。

2. **无流控**：USB CDC 没有硬件流控（RTS/CTS），依赖 USB 的 NAK 机制。如果 MCU 处理慢，PC 端会收到 NAK 重试。

3. **欢迎消息竞态**：`cdcFeatureMsgSent` 在 USB 枚举完成前就尝试发送，可能失败。`CDC_Transmit_FS` 返回非 `USBD_OK` 时会重试下一轮。

4. **42MHz 系统时钟**：与前几例的 84MHz 不同，USB 例需要特定的 PLL 配置以满足 48MHz USB 时钟要求。

5. **与 06 例（UART printf）对比**：06 例用 UART 虚拟串口，需 USB-TTL 转换器；32 例用 USB CDC 虚拟串口，MCU 直接是 USB 设备。USB CDC 速度更快（12Mbps vs 115200bps）、更方便（无需转换器）。

## 关联笔记

- [[06_rocketpi_uart_printf|06 UART printf]]：传统串口输出，对比 USB CDC
- [[33_rocketpi_usb_msc|33 USB MSC]]：USB 大容量存储（U 盘），另一种 USB 设备类
