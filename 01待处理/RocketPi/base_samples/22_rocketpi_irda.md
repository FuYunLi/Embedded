---
status: done
created: 2026-09-14
tags:
  - c/ir-remote
  - c/interrupt
  - embedded/input
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/22_rocketpi_irda"
  - "[[19_rocketpi_hcsr04]] TIM1 微秒时间戳（复用同一机制）"
  - "[[13_rocketpi_uart_radar]] UART 协议解析状态机（对比 IR 协议解析）"
---

# 22 红外遥控 NEC 解码

## 一句话定性

红外遥控接收模块（VS1838B）接收 38kHz 载波的红外信号，解调后输出 NEC 编码的脉冲序列，MCU 通过 GPIO 中断 + 微秒时间戳解码出地址和命令字节，用于嵌入式系统的遥控输入。

## 同类产品定位

- **VS1838B**：红外接收头，38kHz 载波，抗干扰好，成本 ~¥0.5
- **TSOP4838**：同类红外接收头，引脚兼容
- **红外发射管**：配合 PWM 调制 38kHz 载波，实现红外发射
- **本例选型理由**：VS1838B 最常用，NEC 协议最广泛（电视、空调、机顶盒遥控器），适合学习红外解码

## 硬件连接

- 接收引脚：GPIO 输入（外部中断），连接 VS1838B 的 OUT 引脚
- VS1838B：VCC=3.3V，GND，OUT→MCU
- 信号特性：空闲高电平，收到红外信号变低（载波解调后的包络）

## 通信协议要点

- **NEC 协议**：38kHz 载波，脉冲距离编码（Pulse Distance Coding）
- **帧结构**：引导码（9ms 低 + 4.5ms 高）+ 地址 8 位 + 地址反码 8 位 + 命令 8 位 + 命令反码 8 位
- **位编码**：逻辑 1 = 562.5µs 低 + 1687.5µs 高；逻辑 0 = 562.5µs 低 + 562.5µs 高
- **重复码**：长按按键时发送，9ms 低 + 2.25ms 高 + 562.5µs 低
- **总帧时间**：约 67.5ms（不含重复码）

## 代码架构

```
┌─────────────────────────────────────────────────┐
│ main.c (应用层)                                 │
│  - 调用 ir_remote_receive_test(100)            │
├─────────────────────────────────────────────────┤
│ driver_ir_remote_receive_test.c (测试层)        │
│  - 回调函数 + 超时等待 + 结果打印               │
├─────────────────────────────────────────────────┤
│ driver_ir_remote.c (驱动层，LibDriver，38KB)   │
│  - ir_remote_irq_handler：边沿时间差→协议解码  │
│  - 状态机：引导码→地址→命令→校验               │
├─────────────────────────────────────────────────┤
│ driver_ir_remote_interface.c (平台适配层)       │
│  - TIM1 微秒时间戳（复用 19 例机制）           │
│  - GPIO 中断回调                               │
└─────────────────────────────────────────────────┘
```

## 与 19 例（HC-SR04）的关键差异

| 维度 | 19 HC-SR04 | 22 IR 遥控 |
|------|-----------|------------|
| 信号类型 | 超声波回波 | 红外脉冲序列 |
| 测量方式 | GPIO 轮询 ECHO | GPIO 中断 + 时间戳 |
| 时间精度 | 1µs | 1µs |
| 数据量 | 1 个距离值 | 32 位（地址+命令） |
| 协议复杂度 | 无协议 | NEC 协议（状态机解码） |
| TIM1 使用 | 溢出中断扩展 | 溢出中断扩展（完全复用） |
| 驱动来源 | LibDriver | LibDriver（38KB，含 NEC 协议状态机） |

## 核心实现详解

### NEC 协议时序

```
引导码：
├── 9ms 低 ──┤── 4.5ms 高 ──┤

逻辑 1：
├── 562.5µs 低 ──┤── 1687.5µs 高 ──┤

逻辑 0：
├── 562.5µs 低 ──┤── 562.5µs 高 ──┤

重复码：
├── 9ms 低 ──┤── 2.25ms 高 ──┤── 562.5µs 低 ──┤
```

**脉冲距离编码**：位值由低电平后的高电平持续时间决定。562.5µs = 0，1687.5µs = 1。低电平时间固定（562.5µs）。

### ir_remote_irq_handler——边沿中断解码

**作用**：每次 GPIO 边沿中断调用此函数，记录时间戳差值，驱动 NEC 协议状态机解码。

```c
// 驱动层内部（38KB，不展开全部）
uint8_t ir_remote_irq_handler(ir_remote_handle_t *handle)
{
    // 读取当前时间戳
    ir_remote_time_t now;
    handle->timestamp_read(&now);

    // 计算与上次中断的时间差
    uint32_t diff_us = ...;

    // 存入解码缓冲区
    handle->decode[handle->decode_len].diff_us = diff_us;
    handle->decode_len++;

    // 驱动状态机：引导码→地址→命令→校验
    // 解码完成后调用 receive_callback
}
```

**解码缓冲区**：`ir_remote_decode_t decode[128]`，存储每次边沿的时间差。NEC 一帧约 34 次边沿变化（引导码 + 32 位数据），128 足够。

### ir_remote_interface_timestamp_read——微秒时间戳

**作用**：读取 TIM1 计数器和溢出次数，合成 64 位微秒时间戳。关中断保护读取一致性。

```c
uint8_t ir_remote_interface_timestamp_read(ir_remote_time_t *t)
{
    uint32_t primask = __get_PRIMASK();  // ① 保存中断状态
    __disable_irq();                      // ② 关中断

    uint64_t base_us = s_tim1_elapsed_us;
    uint32_t counter = __HAL_TIM_GET_COUNTER(&htim1);

    if (__HAL_TIM_GET_FLAG(&htim1, TIM_FLAG_UPDATE) != RESET) {
        base_us += 65536;                 // ③ 溢出补偿
        counter = __HAL_TIM_GET_COUNTER(&htim1);
    }

    if (primask == 0U) __enable_irq();    // ④ 恢复中断

    uint64_t total_us = base_us + counter;
    t->s = total_us / 1000000ULL;
    t->us = (uint32_t)(total_us % 1000000ULL);
}
```

**与 19 例对比**：
- 19 例：`do-while` 循环检测溢出变化（无锁）
- 22 例：关中断保护（原子读取）

关中断更严格——19 例的 do-while 在溢出频率极高时可能多次重试，22 例一次搞定。但关中断期间其他中断被延迟（~1µs），对时间敏感系统有影响。

**③ 溢出补偿**：关中断后检查 UPDATE 标志，如果在读 `s_tim1_elapsed_us` 和读 `counter` 之间发生了溢出（标志已置位但中断还没执行），手动补偿 65536µs。

### a_receive_callback——解码回调

**作用**：NEC 解码完成后，驱动层调用此回调，传入解码结果。

```c
static void a_receive_callback(ir_remote_t *data)
{
    switch (data->status) {
    case IR_REMOTE_STATUS_OK:
        printf("ir_remote: add is 0x%02X and cmd is 0x%02X.\n",
               data->address, data->command);
        gs_flag = 1;  // 通知主循环
        break;
    case IR_REMOTE_STATUS_REPEAT:
        printf("ir_remote: irq repeat.\n");
        break;
    case IR_REMOTE_STATUS_ADDR_ERR:
    case IR_REMOTE_STATUS_CMD_ERR:
    case IR_REMOTE_STATUS_FRAME_INVALID:
        // 错误处理
        break;
    }
}
```

**NEC 帧校验**：
- 地址 + 地址反码 = 0xFF（8 位）
- 命令 + 命令反码 = 0xFF（8 位）
- 校验失败 → ADDR_ERR 或 CMD_ERR

**重复码**：长按按键时，遥控器每 110ms 发送一次重复码（9ms 低 + 2.25ms 高）。驱动层识别为 `IR_REMOTE_STATUS_REPEAT`。

### ir_remote_receive_test——超时等待测试

```c
uint8_t ir_remote_receive_test(uint32_t times)
{
    for (i = 0; i < times; i++) {
        gs_flag = 0;
        timeout = 500;  // 5s 超时

        while (timeout != 0) {
            if (gs_flag != 0) break;  // 收到红外信号
            timeout--;
            HAL_Delay(10);
        }

        if (timeout == 0) return 1;  // 超时退出
    }
}
```

**中断 + 轮询混合模式**：GPIO 中断触发 `ir_remote_irq_handler`，解码完成后在回调中置 `gs_flag`；主循环轮询 `gs_flag` 等待结果。这是嵌入式系统的经典模式：中断处理时间敏感部分，主循环处理结果。

### ir_remote_handle_t——LibDriver 注入结构

```c
typedef struct {
    uint8_t (*timestamp_read)(ir_remote_time_t *t);   // 微秒时间戳
    void (*delay_ms)(uint32_t ms);
    void (*debug_print)(const char *const fmt, ...);
    void (*receive_callback)(ir_remote_t *data);       // 解码完成回调
    uint8_t inited;
    ir_remote_decode_t decode[128];                    // 解码缓冲区
    uint16_t decode_len;
    ir_remote_time_t last_time;                        // 上次中断时间
    ir_remote_t last_code;                             // 上次解码结果
} ir_remote_handle_t;
```

**与 15/19 例对比**：同为 LibDriver 风格，但 IR 驱动多了 `receive_callback`（异步回调）和 `decode[128]`（状态缓冲区）。这是因为它需要累积多次中断才能解码一帧，不像 I2C/超声波那样一次调用完成。

## 设计问题与改进空间

1. **driver_ir_remote.c 38KB**：LibDriver 的 NEC 解码驱动包含 NEC、RC5、RC6 等多种协议支持，代码量大。本例只用 NEC，可裁剪到 ~5KB。

2. **关中断保护的时间**：`timestamp_read` 关中断约 1µs，在高频中断场景下可能影响其他中断响应。可改为 19 例的 do-while 无锁方案。

3. **解码缓冲区 128 个元素**：每个元素 12 字节（时间戳 + diff_us），共 1.5KB RAM。对 STM32F401RE（96KB SRAM）来说可接受，但可优化为环形缓冲区。

4. **`receive_callback` 在中断上下文调用**：`ir_remote_irq_handler` 在 GPIO 中断中调用，解码完成后直接调用 `receive_callback`。回调中的 `printf` 是阻塞的，在中断上下文中不安全。应设标志让主循环处理。

5. **无红外发射**：本例只接收不发射。完整的红外遥控系统需要发射端（PWM 调制 38kHz 载波 + NEC 编码），可作为补充笔记。

## 关联笔记

- [[19_rocketpi_hcsr04|19 HC-SR04 超声波]]：TIM1 微秒时间戳复用
- [[13_rocketpi_uart_radar|13 UART 雷达]]：另一种协议解析状态机（UART 帧 vs NEC 帧）
- [[03_rocketpi_key_irq|03 按键中断]]：GPIO 中断基础（IR 解码在此基础上扩展）
