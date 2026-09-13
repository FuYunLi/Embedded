---
status: done
created: 2026-09-13
tags:
  - c/i2c
  - c/bit-bang
  - embedded/bus-protocol
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/14_rocketpi_i2c_aht30/soft_i2c.c"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/14_rocketpi_i2c_aht30/soft_i2c.h"
---

# 14 补充：软 I2C 位操作详解

> 本文逐行解析 `soft_i2c.c` 的位操作实现。软 I2C = 用 GPIO 手工翻转 SCL/SDA 电平来模拟 I2C 协议时序，相当于用代码实现硬件 I2C 外设的全部工作。走完这遍，I2C 协议的每一步"在线上发生了什么"就清楚了。主笔记见 [[14_rocketpi_i2c_aht30]]。

## 接口总览

`soft_i2c.h` 定义了 4 个公开函数：

| 函数 | 作用 |
|---|---|
| `soft_i2c_bus_init` | 初始化总线结构体，拉高 SCL/SDA |
| `soft_i2c_master_transmit` | 起始 + 写地址 + 写数据 + 停止 |
| `soft_i2c_master_receive` | 起始 + 读地址 + 读数据 + 停止 |
| `soft_i2c_master_write_read` | 起始 + 写数据 + 重复起始 + 读数据 + 停止 |

内部实现拆成 7 个静态函数：

| 函数 | 作用 |
|---|---|
| `soft_i2c_prepare_bus` | 确保已初始化 |
| `soft_i2c_delay` | 半周期延时 |
| `soft_i2c_drive_scl` | 驱动 SCL + 时钟拉伸处理 |
| `soft_i2c_drive_sda` | 驱动 SDA |
| `soft_i2c_start` / `soft_i2c_repeated_start` | 起始/重复起始条件 |
| `soft_i2c_stop` | 停止条件 |
| `soft_i2c_write_byte` / `soft_i2c_read_byte` | 字节级读写 + ACK 处理 |

## 依赖注入设计

`soft_i2c_bus_t` 结构体不直接碰任何硬件，所有 GPIO 操作通过函数指针注入：

```c
typedef soft_i2c_status_t (*soft_i2c_pin_write_fn)(void *ctx, soft_i2c_pin_state_t state);
typedef soft_i2c_pin_state_t (*soft_i2c_pin_read_fn)(void *ctx);
typedef void (*soft_i2c_delay_fn)(uint32_t ticks, void *ctx);

typedef struct {
    soft_i2c_pin_io_t scl;      // SCL 引脚的 write/read 回调 + ctx
    soft_i2c_pin_io_t sda;      // SDA 引脚的 write/read 回调 + ctx
    soft_i2c_delay_fn delay_fn; // 可选自定义延时回调（NULL 则用 NOP 循环）
    void *delay_ctx;            // 延时回调上下文
    uint32_t delay_ticks;       // 半周期延时节拍数
    uint32_t stretch_timeout_ticks; // 时钟拉伸超时
    uint8_t initialized;        // 初始化标志
} soft_i2c_bus_t;
```

- **`void *ctx`**：GPIO 回调的上下文指针。`driver_aht30.c` 里指向 `aht30_gpio_pin_t {port, pin}`，这样同一个 soft_i2c 可以驱动不同引脚的多条 I2C 总线
- **`delay_fn` 为 NULL 时**：用默认 NOP 循环。可注入 DWT 计数器或定时器回调获得更精确的延时
- **`initialized` 标志**：`prepare_bus` 检查此标志，未初始化则自动调用 `bus_init`。允许"先声明后初始化"的懒初始化模式

## soft_i2c_bus_init——总线初始化

**作用**：校验回调函数指针、填充默认延时参数、拉高 SCL/SDA 进入空闲态。

**形参**：`bus` — 总线结构体指针，调用方分配并填好 scl/sda 回调。

**输入/输出**：输入 = 半初始化的 bus（回调已填，延时可能为 0）；输出 = bus 完全就绪（initialized=1），SCL/SDA 均为高电平。

**调用者**：`aht30_bus_init`（driver_aht30.c），上电时调一次。

```c
soft_i2c_status_t soft_i2c_bus_init(soft_i2c_bus_t *bus)
{
    if ((bus == NULL) ||
        (bus->scl.write == NULL) || (bus->scl.read == NULL) ||
        (bus->sda.write == NULL) || (bus->sda.read == NULL)) {
        return SOFT_I2C_STATUS_INVALID_PARAM;
    }

    if (bus->delay_ticks == 0U) {
        bus->delay_ticks = SOFT_I2C_DEFAULT_DELAY_TICKS;  // 64
    }

    if (bus->stretch_timeout_ticks == 0U) {
        bus->stretch_timeout_ticks = SOFT_I2C_DEFAULT_STRETCH_TICKS;  // 4000
    }

    soft_i2c_drive_scl(bus, SOFT_I2C_PIN_SET);
    soft_i2c_drive_sda(bus, SOFT_I2C_PIN_SET);

    bus->initialized = 1U;
    return SOFT_I2C_STATUS_OK;
}
```

- **四个 NULL 检查**：scl/sda 各需 write 和 read 两个回调，缺任何一个都无法工作。放最前面避免后续调用 NULL 函数指针崩溃
- **默认值填充**：调用方可以不填延时参数（留 0），这里兜底。**修改调用方传入的结构体**——这是"初始化函数"的特权，其他函数只读不写
- **拉高 SCL/SDA**：I2C 空闲态 = 两条线都高。`drive_scl` 内部会检查时钟拉伸（从设备是否还拉着 SCL），`drive_sda` 纯写

## soft_i2c_delay——半周期延时

**作用**：产生 I2C 时序中的半周期延时（SCL 高电平宽度、SDA 建立时间等都靠它）。

**形参**：`bus` — 总线结构体，读取延时参数。

**输入/输出**：无返回值，阻塞指定时间。

**调用者**：start/stop/write_byte/read_byte 中每一步电平变化前后。

```c
static void soft_i2c_delay(const soft_i2c_bus_t *bus)
{
    if ((bus != NULL) && (bus->delay_fn != NULL)) {
        bus->delay_fn(bus->delay_ticks, bus->delay_ctx);
        return;
    }

    uint32_t ticks = (bus == NULL || bus->delay_ticks == 0U) ? SOFT_I2C_DEFAULT_DELAY_TICKS : bus->delay_ticks;
    for (volatile uint32_t i = 0; i < ticks; ++i) {
        __NOP();
    }
}
```

- **优先用自定义回调**：注入 DWT 计数器后精度达 12ns（84MHz），NOP 循环在 -O0 和 -O2 下差 3~5 倍
- **`volatile` 关键字**：防止编译器优化掉空循环。没有 volatile，-O2 会把整个循环删掉
- **`__NOP()`**：ARM 内联汇编 `nop`，占一个时钟周期。比空循环体 `{}` 更可靠——某些编译器连 `{}` 都可能优化掉

## soft_i2c_drive_scl——驱动 SCL + 时钟拉伸

**作用**：设置 SCL 电平，当拉高时检查从设备是否在做时钟拉伸（保持 SCL 低电平要求主机等待）。

**形参**：`bus` — 总线结构体；`state` — 目标电平（SET/RESET）。

**输入/输出**：输入 = 目标电平；输出 = SCL 被设置到目标电平。返回 TIMEOUT 表示从设备拉伸超时。

**调用者**：start/stop/write_byte/read_byte 中所有 SCL 操作。

```c
static soft_i2c_status_t soft_i2c_drive_scl(soft_i2c_bus_t *bus, soft_i2c_pin_state_t state)
{
    soft_i2c_status_t status = bus->scl.write(bus->scl.ctx, state);
    if (status != SOFT_I2C_STATUS_OK) {
        return status;
    }

    if (state == SOFT_I2C_PIN_SET) {
        uint32_t timeout = (bus->stretch_timeout_ticks == 0U)
                               ? SOFT_I2C_DEFAULT_STRETCH_TICKS
                               : bus->stretch_timeout_ticks;
        while (bus->scl.read(bus->scl.ctx) == SOFT_I2C_PIN_RESET) {
            if (timeout-- == 0U) {
                return SOFT_I2C_STATUS_TIMEOUT;
            }
        }
    }

    return SOFT_I2C_STATUS_OK;
}
```

**时钟拉伸机制**：

```
主机拉高 SCL  ──────┐
                    │
从设备还拉着 SCL 低  ├── 这段时间主机必须等
                    │
从设备释放 SCL  ─────┘
主机才能继续
```

- **为什么只在 `PIN_SET` 时检查**：主机拉低 SCL 时，从设备不会拉伸（拉伸只发生在从设备需要更多时间处理数据时，此时主机已释放 SCL）
- **轮询读 SCL**：`bus->scl.read(bus->scl.ctx)` 读回实际引脚电平。如果从设备还拉着，读回来是 RESET，继续等
- **`timeout--` 倒计时**：防从设备死锁。AHT30 配置 8000 ticks（约 100µs@84MHz），足够传感器完成内部操作
- **`timeout-- == 0U` 的边界**：当 timeout=1 时 `timeout--` 返回 1，不等于 0，继续循环；timeout=0 时 `timeout--` 返回 0（无符号下溢到 UINT32_MAX），此时等于 0，退出。**下一轮循环 timeout 已是 UINT32_MAX，但已经 return 了不会走到**

## soft_i2c_drive_sda——驱动 SDA

**作用**：设置 SDA 电平。比 SCL 简单，没有时钟拉伸。

```c
static soft_i2c_status_t soft_i2c_drive_sda(soft_i2c_bus_t *bus, soft_i2c_pin_state_t state)
{
    return bus->sda.write(bus->sda.ctx, state);
}
```

一个包装层：统一返回值类型（`soft_i2c_status_t`），同时保持与 `drive_scl` 对称的调用风格。

## soft_i2c_start——起始条件

**作用**：在 SCL 高电平期间，SDA 从高到低跳变，产生 I2C 起始条件。总线从空闲态进入"已占用"态。

**形参**：`bus` — 总线结构体。

**输入/输出**：输入 = 总线空闲态（SCL=高，SDA=高）；输出 = 起始条件已发出（SCL=低，SDA=低）。

**调用者**：`soft_i2c_master_transmit`、`soft_i2c_master_receive`、`soft_i2c_master_write_read`。

```c
static soft_i2c_status_t soft_i2c_start(soft_i2c_bus_t *bus)
{
    soft_i2c_status_t status = soft_i2c_drive_sda(bus, SOFT_I2C_PIN_SET);
    if (status != SOFT_I2C_STATUS_OK) return status;

    status = soft_i2c_drive_scl(bus, SOFT_I2C_PIN_SET);
    if (status != SOFT_I2C_STATUS_OK) return status;
    soft_i2c_delay(bus);

    status = soft_i2c_drive_sda(bus, SOFT_I2C_PIN_RESET);
    if (status != SOFT_I2C_STATUS_OK) return status;
    soft_i2c_delay(bus);

    status = soft_i2c_drive_scl(bus, SOFT_I2C_PIN_RESET);
    if (status != SOFT_I2C_STATUS_OK) return status;
    soft_i2c_delay(bus);

    return SOFT_I2C_STATUS_OK;
}
```

**时序图**：

```
SCL  ────────┐       ┌────
             │       │
             └───────┘
SDA  ────┐           ┌────
         │           │
         └───────────┘
         ↑ 起始条件：SCL 高时 SDA 下降沿
```

步骤拆解：
1. `drive_sda(SET)` — 确保 SDA 先拉高（可能上次 stop 后已是高，但显式设置更安全）
2. `drive_scl(SET) + delay` — SCL 拉高，此时总线空闲
3. `drive_sda(RESET) + delay` — **SCL 仍为高**，SDA 从高变低 = 起始条件
4. `drive_scl(RESET) + delay` — SCL 拉低，准备发送第一个数据位。此后 SDA 可以自由变化（SCL 低时 SDA 变化不算起始/停止）

**为什么第一步先拉高 SDA**：如果上次操作后 SDA 仍是低（比如刚发完一个 0x00 的最后一位），直接拉高 SCL 不会产生起始条件——需要先保证 SDA 是高再做下降沿。

## soft_i2c_repeated_start——重复起始条件

**作用**：在不释放总线（不发 STOP）的情况下，再次发起起始条件。用于"先写后读"的组合事务（如寄存器读取：写寄存器地址 → 重复起始 → 读数据）。

```c
static soft_i2c_status_t soft_i2c_repeated_start(soft_i2c_bus_t *bus)
{
    soft_i2c_status_t status = soft_i2c_drive_sda(bus, SOFT_I2C_PIN_SET);
    if (status != SOFT_I2C_STATUS_OK) return status;
    soft_i2c_delay(bus);

    status = soft_i2c_drive_scl(bus, SOFT_I2C_PIN_SET);
    if (status != SOFT_I2C_STATUS_OK) return status;
    soft_i2c_delay(bus);

    status = soft_i2c_drive_sda(bus, SOFT_I2C_PIN_RESET);
    if (status != SOFT_I2C_STATUS_OK) return status;
    soft_i2c_delay(bus);

    status = soft_i2c_drive_scl(bus, SOFT_I2C_PIN_RESET);
    if (status != SOFT_I2C_STATUS_OK) return status;
    soft_i2c_delay(bus);

    return SOFT_I2C_STATUS_OK;
}
```

与 `soft_i2c_start` 几乎相同，唯一区别是**第一步多了 `soft_i2c_delay`**。原因：`start` 假设总线空闲（SCL/SDA 都已高），而 `repeated_start` 前一次操作刚结束（SCL 刚拉低），需要先拉高 SDA 并等半拍，再拉高 SCL，才能产生合法的"高电平期间 SDA 下降沿"。

## soft_i2c_stop——停止条件

**作用**：在 SCL 高电平期间，SDA 从低到高跳变，产生 I2C 停止条件。总线释放回空闲态。

```c
static void soft_i2c_stop(soft_i2c_bus_t *bus)
{
    soft_i2c_drive_sda(bus, SOFT_I2C_PIN_RESET);
    soft_i2c_delay(bus);
    soft_i2c_drive_scl(bus, SOFT_I2C_PIN_SET);
    soft_i2c_delay(bus);
    soft_i2c_drive_sda(bus, SOFT_I2C_PIN_SET);
    soft_i2c_delay(bus);
}
```

**时序图**：

```
SCL  ────────────┐       ┌────
                 │       │
                 └───────┘
SDA  ────┐               ┌────
         │               │
         └───────────────┘
                     ↑ 停止条件：SCL 高时 SDA 上升沿
```

步骤：
1. `drive_sda(RESET) + delay` — 确保 SDA 先拉低（最后一次 ACK 或数据位的最后一位可能已是低）
2. `drive_scl(SET) + delay` — SCL 拉高
3. `drive_sda(SET) + delay` — **SCL 仍为高**，SDA 从低变高 = 停止条件

**为什么返回 void**：stop 不检查返回值。即使 drive_sda/scl 失败，总线状态已经不可逆（从设备可能已看到部分时序），重试 stop 无意义。这也是所有公开函数（transmit/receive/write_read）最后都调 stop 的原因——不管前面成功还是失败，总要释放总线。

## soft_i2c_write_byte——写一字节 + 读 ACK

**作用**：在 SCL 时钟配合下，逐位发送一个字节（MSB 先发），然后释放 SDA 读取从设备的 ACK/NACK。

**形参**：`bus` — 总线结构体；`value` — 要发送的字节。

**输入/输出**：输入 = 一个字节；输出 = 字节已发到 SDA 线上，从设备的 ACK 已读取。返回 OK 表示收到 ACK，ERROR 表示收到 NACK。

**调用者**：`soft_i2c_master_transmit`（发地址+数据）、`soft_i2c_master_write_read`（发地址+寄存器地址）、`soft_i2c_master_receive`（发地址）。

```c
static soft_i2c_status_t soft_i2c_write_byte(soft_i2c_bus_t *bus, uint8_t value)
{
    for (int8_t bit = 7; bit >= 0; --bit) {
        soft_i2c_pin_state_t state = ((value >> bit) & 0x01U) ? SOFT_I2C_PIN_SET : SOFT_I2C_PIN_RESET;
        soft_i2c_status_t status = soft_i2c_drive_sda(bus, state);
        if (status != SOFT_I2C_STATUS_OK) return status;
        soft_i2c_delay(bus);

        status = soft_i2c_drive_scl(bus, SOFT_I2C_PIN_SET);
        if (status != SOFT_I2C_STATUS_OK) return status;
        soft_i2c_delay(bus);

        status = soft_i2c_drive_scl(bus, SOFT_I2C_PIN_RESET);
        if (status != SOFT_I2C_STATUS_OK) return status;
        soft_i2c_delay(bus);
    }

    // ACK 时钟
    soft_i2c_status_t status = soft_i2c_drive_sda(bus, SOFT_I2C_PIN_SET);
    if (status != SOFT_I2C_STATUS_OK) return status;
    soft_i2c_delay(bus);

    status = soft_i2c_drive_scl(bus, SOFT_I2C_PIN_SET);
    if (status != SOFT_I2C_STATUS_OK) return status;
    soft_i2c_delay(bus);

    soft_i2c_pin_state_t ack = bus->sda.read(bus->sda.ctx);

    status = soft_i2c_drive_scl(bus, SOFT_I2C_PIN_RESET);
    if (status != SOFT_I2C_STATUS_OK) return status;
    soft_i2c_delay(bus);

    return (ack == SOFT_I2C_PIN_RESET) ? SOFT_I2C_STATUS_OK : SOFT_I2C_STATUS_ERROR;
}
```

**数据位发送循环**（8 次）：

```
SDA  ──X──────────X──────────X──  (数据位在 SCL 低时设置)
SCL  ────┐  ┌──────┐  ┌────────
         │  │      │  │
         └──┘      └──┘
         ↑采样      ↑采样
```

每一轮：
1. `drive_sda(state)` — SCL 低电平期间设置数据位（`state = (value>>bit) & 1`）
2. `delay` — SDA 建立时间（数据在 SCL 上升沿前必须稳定）
3. `drive_scl(SET) + delay` — SCL 拉高，从设备在此刻采样 SDA
4. `drive_scl(RESET) + delay` — SCL 拉低，准备下一位

**MSB 先发**：`bit = 7` 开始，先发最高位。I2C 协议规定字节传输顺序。

**ACK 时钟**（数据位之后）：

```
SDA  ────(释放)──────(读ACK)──
SCL  ────────┐  ┌──────────
             │  │
             └──┘
             ↑从设备拉低=ACK
```

1. `drive_sda(SET)` — 主机释放 SDA（拉高），把总线控制权交给从设备
2. `delay + drive_scl(SET) + delay` — SCL 拉高，从设备在 SCL 高期间拉低 SDA 表示 ACK
3. `bus->sda.read(bus->sda.ctx)` — **读回 SDA 实际电平**。RESET=从设备拉低=ACK；SET=从设备未拉低=NACK
4. `drive_scl(RESET)` — SCL 拉低，结束这个字节的传输

**ACK=RESET（低电平）的原因**：从设备通过拉低 SDA 来应答，主机读到低电平说明从设备"听到了"。

## soft_i2c_read_byte——读一字节 + 发 ACK/NACK

**作用**：在 SCL 时钟配合下，逐位读取一个字节（MSB 先收），然后根据是否为最后一个字节决定发 ACK（继续读）或 NACK（停止读）。

**形参**：`bus` — 总线结构体；`value` — 输出缓冲区指针；`last_byte` — 是否为最后一个字节（1=发 NACK，0=发 ACK）。

**输入/输出**：输入 = 总线状态 + last_byte 标志；输出 = 读到的字节写入 *value，返回状态码。

**调用者**：`soft_i2c_master_receive`（读数据）、`soft_i2c_master_write_read`（读数据）。

```c
static soft_i2c_status_t soft_i2c_read_byte(soft_i2c_bus_t *bus, uint8_t *value, int last_byte)
{
    uint8_t result = 0U;
    soft_i2c_status_t status = soft_i2c_drive_sda(bus, SOFT_I2C_PIN_SET);
    if (status != SOFT_I2C_STATUS_OK) return status;

    for (int8_t bit = 7; bit >= 0; --bit) {
        status = soft_i2c_drive_scl(bus, SOFT_I2C_PIN_SET);
        if (status != SOFT_I2C_STATUS_OK) return status;
        soft_i2c_delay(bus);

        if (bus->sda.read(bus->sda.ctx) == SOFT_I2C_PIN_SET) {
            result |= (uint8_t)(1U << bit);
        }

        status = soft_i2c_drive_scl(bus, SOFT_I2C_PIN_RESET);
        if (status != SOFT_I2C_STATUS_OK) return status;
        soft_i2c_delay(bus);
    }

    // ACK/NACK
    status = soft_i2c_drive_sda(bus, last_byte ? SOFT_I2C_PIN_SET : SOFT_I2C_PIN_RESET);
    if (status != SOFT_I2C_STATUS_OK) return status;
    soft_i2c_delay(bus);

    status = soft_i2c_drive_scl(bus, SOFT_I2C_PIN_SET);
    if (status != SOFT_I2C_STATUS_OK) return status;
    soft_i2c_delay(bus);

    status = soft_i2c_drive_scl(bus, SOFT_I2C_PIN_RESET);
    if (status != SOFT_I2C_STATUS_OK) return status;
    soft_i2c_delay(bus);

    soft_i2c_drive_sda(bus, SOFT_I2C_PIN_SET);

    if (value != NULL) {
        *value = result;
    }

    return SOFT_I2C_STATUS_OK;
}
```

**数据位读取循环**（8 次）：

```
SDA  ──(从设备驱动)──────────
SCL  ────────┐  ┌──────────
             │  │
             └──┘
             ↑主机在此刻读 SDA
```

每一轮：
1. `drive_scl(SET) + delay` — SCL 拉高，从设备在 SCL 低电平期间已设置好数据位
2. `bus->sda.read` — 主机读 SDA。SET 则 `result |= (1<<bit)` 置位
3. `drive_scl(RESET) + delay` — SCL 拉低，从设备可以改变 SDA 准备下一位

**ACK/NACK 发送**：

```
主机发 ACK (非最后字节)         主机发 NACK (最后字节)
SDA  ────(拉低)──────          SDA  ────(释放/高)──────
SCL  ────────┐  ┌──            SCL  ────────┐  ┌──
             │  │                           │  │
             └──┘                           └──┘
从设备看到低=继续发              从设备看到高=停止发
```

- **`last_byte ? SET : RESET`**：最后一个字节发 NACK（SET=高电平=不拉低），告诉从设备"我不要再读了"；非最后字节发 ACK（RESET=低电平），从设备继续发下一个字节
- **最后的 `drive_sda(SET)`**：释放 SDA，让总线回到高电平，为后续 STOP 条件做准备

## soft_i2c_master_transmit——完整写事务

**作用**：起始 + 写地址（写方向）+ 逐字节写数据 + 停止。任一字节收到 NACK 则中止。

```c
soft_i2c_status_t soft_i2c_master_transmit(soft_i2c_bus_t *bus, uint8_t address,
                                           const uint8_t *data, size_t size)
{
    if ((data == NULL) && (size > 0U)) return SOFT_I2C_STATUS_INVALID_PARAM;

    soft_i2c_status_t status = soft_i2c_prepare_bus(bus);
    if (status != SOFT_I2C_STATUS_OK) return status;

    int started = 0;
    status = soft_i2c_start(bus);
    if (status != SOFT_I2C_STATUS_OK) goto done;
    started = 1;

    status = soft_i2c_write_byte(bus, address & (uint8_t)~0x01U);  // ① 写方向位
    if (status != SOFT_I2C_STATUS_OK) goto done;

    for (size_t i = 0; i < size; ++i) {
        status = soft_i2c_write_byte(bus, data[i]);
        if (status != SOFT_I2C_STATUS_OK) goto done;
    }

done:
    if (started) soft_i2c_stop(bus);
    return status;
}
```

- **① `address & ~0x01`**：I2C 地址最低位是方向位，0=写、1=读。`~0x01` 清除最低位确保是写方向。`driver_aht30.h` 里 `AHT30_I2C_ADDRESS = (0x38<<1) = 0x70`，已左移，最低位自然为 0
- **`started` 标志**：start 成功后才调 stop。start 失败时总线状态不确定，强行 stop 可能产生非法时序
- **NACK 中止**：任何 `write_byte` 返回 ERROR（收到 NACK），直接跳到 done 发 STOP。从设备 NACK 通常表示地址错误或设备忙

## soft_i2c_master_receive——完整读事务

**作用**：起始 + 写地址（读方向）+ 逐字节读数据 + 停止。最后一个字节发 NACK，其余发 ACK。

```c
soft_i2c_status_t soft_i2c_master_receive(soft_i2c_bus_t *bus, uint8_t address,
                                          uint8_t *data, size_t size)
{
    if ((data == NULL) && (size > 0U)) return SOFT_I2C_STATUS_INVALID_PARAM;
    if (size == 0U) return SOFT_I2C_STATUS_OK;

    soft_i2c_status_t status = soft_i2c_prepare_bus(bus);
    if (status != SOFT_I2C_STATUS_OK) return status;

    int started = 0;
    status = soft_i2c_start(bus);
    if (status != SOFT_I2C_STATUS_OK) goto done;
    started = 1;

    status = soft_i2c_write_byte(bus, address | 0x01U);  // ① 读方向位
    if (status != SOFT_I2C_STATUS_OK) goto done;

    for (size_t i = 0; i < size; ++i) {
        const int last_byte = (i + 1U) == size;
        status = soft_i2c_read_byte(bus, &data[i], last_byte);
        if (status != SOFT_I2C_STATUS_OK) goto done;
    }

done:
    if (started) soft_i2c_stop(bus);
    return status;
}
```

- **① `address | 0x01`**：最低位置 1 = 读方向
- **`last_byte` 判断**：`(i+1)==size` 时为最后一个字节，`read_byte` 内部发 NACK，从设备停止发送
- **size=0 提前返回**：空读取无需起始/停止

## soft_i2c_master_write_read——组合事务

**作用**：先写后读的组合操作。典型场景：向从设备写寄存器地址 → 重复起始 → 读寄存器数据。这是 I2C 最常用的模式。

```c
soft_i2c_status_t soft_i2c_master_write_read(soft_i2c_bus_t *bus, uint8_t address,
                                             const uint8_t *tx_data, size_t tx_size,
                                             uint8_t *rx_data, size_t rx_size)
{
    // ... 参数校验和 prepare_bus ...

    int started = 0;
    if (tx_size > 0U) {
        status = soft_i2c_start(bus);
        started = 1;
        status = soft_i2c_write_byte(bus, address & ~0x01U);  // 写地址
        for (size_t i = 0; i < tx_size; ++i) {
            status = soft_i2c_write_byte(bus, tx_data[i]);    // 写数据
        }
    }

    if (rx_size > 0U) {
        if (!started) {
            status = soft_i2c_start(bus);         // 纯读：发普通起始
        } else {
            status = soft_i2c_repeated_start(bus); // 先写后读：发重复起始
        }
        status = soft_i2c_write_byte(bus, address | 0x01U);   // 读地址
        for (size_t i = 0; i < rx_size; ++i) {
            const int last_byte = (i + 1U) == rx_size;
            status = soft_i2c_read_byte(bus, &rx_data[i], last_byte);
        }
    }

    if (started) soft_i2c_stop(bus);
    return status;
}
```

**关键设计**：

- **`started` 标志的双重含义**：（1）是否需要 stop；（2）是否需要 repeated_start。tx_size>0 时 started=1，rx 阶段看到 started=1 就发 repeated_start
- **纯读场景**：tx_size=0 时 started=0，rx 阶段发普通 start + 读地址。等价于 `master_receive`
- **纯写场景**：rx_size=0 时只执行 tx 阶段。等价于 `master_transmit`

**I2C 寄存器读取完整时序**：

```
S ─ 地址+W ─ 寄存器地址 ─ Sr ─ 地址+R ─ 数据[0] ACK ─ 数据[1] NACK ─ P
│          │              │              │              │
起始    写方向        重复起始       读方向        最后字节发NACK
```

## 完整事务流程：AHT30 读温湿度

以 `aht30_read_raw` 为例，跟踪一次完整的 I2C 事务：

```
1. aht30_bus_transmit({0xAC, 0x33, 0x00}, 3)
   → soft_i2c_master_transmit(bus, 0x70, {0xAC,0x33,0x00}, 3)
   → start + write_byte(0x70) + write_byte(0xAC) + write_byte(0x33) + write_byte(0x00) + stop
   → 线上：S [0x70+W] ACK [0xAC] ACK [0x33] ACK [0x00] ACK P

2. HAL_Delay(80)  // 传感器内部转换

3. aht30_bus_receive(raw, 6)
   → soft_i2c_master_receive(bus, 0x70, raw, 6)
   → start + write_byte(0x71) + read_byte×5(ACK) + read_byte(NACK) + stop
   → 线上：S [0x71+R] ACK [raw0] ACK [raw1] ACK [raw2] ACK [raw3] ACK [raw4] ACK [raw5] NACK P
```

## 设计问题与改进空间

1. **NOP 延时精度**：`delay_ticks=80` 在 -O2 下约 80 个时钟周期 ≈ 0.95µs@84MHz，I2C 标准模式要求 SCL 高电平 ≥ 4µs。当前配置偏快但仍可工作（AHT30 容忍更宽范围）。工程代码应用 DWT 或定时器。

2. **`soft_i2c_stop` 不检查返回值**：设计假设 GPIO write 不会失败（硬件连接正常时成立）。但在热插拔或断线场景下，可能需要检测并上报。

3. **无仲裁处理**：多主机 I2C 需要总线仲裁（两个主机同时发数据时，先发现 SDA 与自己发的不一致的主机退出）。当前实现假设单主机，不处理仲裁。

4. **`soft_i2c_master_write_read` 的错误路径**：tx 阶段失败后仍会进入 rx 阶段（因为没有 goto cleanup）。应加 `if (status != OK) goto cleanup`。

## 关联笔记

- [[14_rocketpi_i2c_aht30]] — 主笔记：AHT30 驱动架构与数据转换
- [[15_rocketpi_i2c_at24cxx|15 I2C EEPROM]] — 同为 I2C 器件，EEPROM 存储器
- [[13_rocketpi_uart_radar|13 UART 雷达]] — 对比 UART vs I2C 协议差异
