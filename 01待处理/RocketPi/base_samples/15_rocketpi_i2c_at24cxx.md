---
status: done
created: 2026-09-13
tags:
  - c/i2c
  - c/eeprom
  - embedded/storage
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/15_rocketpi_i2c_at24cxx"
---

# 15 I2C EEPROM AT24CXX

## 一句话定性

AT24CXX 是 I2C 接口的 EEPROM（电可擦除可编程只读存储器），掉电不丢失数据，用于存储配置参数、校准数据等小量关键数据。

## 同类产品定位

- **同系列**：AT24C01(128B)/AT24C02(256B)/AT24C04(512B)/AT24C08(1KB)/AT24C16(2KB)/AT24C32(4KB)/AT24C64(8KB)/AT24C128(16KB)/AT24C256(32KB)/AT24C512(64KB)/AT24CM01(128KB)/AT24CM02(256KB)
- **竞品**：FM24xx（铁电 RAM，写入无限次但贵 5~10 倍）、W25Qxx（SPI Flash，容量大但擦写粒度 4KB，不适合小量频繁写入）
- **本例选型理由**：I2C 接口、容量可选（本例 AT24C02 = 256B）、写入次数 100 万次、数据保持 100 年、成本低（~¥0.5）

## 硬件连接与地址

- I2C 地址：0x50（7 位），HAL 左移后 0xA0。地址低 3 位由 A2/A1/A0 引脚决定，可挂 8 片同型号
- 引脚分配：PB8=SCL，PB9=SDA（开漏上拉）
- 本例使用软件 I2C（复用 14 例的 soft_i2c 库）

## 通信协议要点

- **地址宽度**：AT24C01/02/04/08/16 用 8 位地址（1 字节）；AT24C32/64/128/256/512 用 16 位地址（2 字节）；AT24CM01/02 也用 16 位地址
- **页写入**：AT24C02 页大小 8 字节，写入不能跨页（页边界自动回绕到页起始）
- **写入延时**：内部写入周期最大 5ms，写入后需等待才能进行下一次操作
- **字节读取**：随机读取 = 写地址（伪写）+ 重复起始 + 读数据

## 代码架构

```
┌─────────────────────────────────────────────────┐
│ main.c (应用层)                                 │
│  - 调用 at24cxx_read_test 跑读写校验            │
├─────────────────────────────────────────────────┤
│ driver_at24cxx_read_test.c (测试层)             │
│  - 8 组 × 12 字节随机数据写入+回读+比对        │
├─────────────────────────────────────────────────┤
│ driver_at24cxx_basic.c (封装层)                 │
│  - 静态 handle + 简化 API                       │
├─────────────────────────────────────────────────┤
│ driver_at24cxx.c (驱动层，第三方库 LibDriver)   │
│  - 通用 EEPROM 驱动，支持全系列 AT24CXX        │
├─────────────────────────────────────────────────┤
│ driver_at24cxx_interface.c (平台适配层)         │
│  - 软 I2C 回调注入 + GPIO 配置                  │
├─────────────────────────────────────────────────┤
│ soft_i2c.c (传输层，复用 14 例)                 │
└─────────────────────────────────────────────────┘
```

## 与 14 例的关键差异

| 维度 | 14 AHT30 | 15 AT24CXX |
|------|----------|------------|
| 器件类型 | 传感器（只读） | 存储器（读写） |
| 地址宽度 | 固定 0x38 | 可配置（A2/A1/A0），8 位/16 位地址可选 |
| 数据流向 | MCU 只读 | MCU 读写 |
| 页边界 | 无 | 有（8 字节页，跨页回绕） |
| 写入延时 | 无 | 5ms 内部写入周期 |
| 驱动来源 | 手写 | 第三方库 LibDriver（23KB） |
| 依赖注入 | 宏切换（编译期） | 函数指针注入（运行时） |
| 复杂度 | 低（单次触发+读取） | 高（地址宽度适配+页写入+跨页处理） |

## 核心设计：三层依赖注入

本例最大的设计特点是**三层依赖注入**，与 14 例的"宏切换"方案形成对比：

### 14 例（宏切换）

```c
#if AHT30_USE_SOFT_I2C
// 软件 I2C 实现
#else
// 硬件 I2C 实现
#endif
```

编译期确定，零运行时开销，但切换需重新编译。

### 15 例（函数指针注入）

`at24cxx_handle_t` 结构体包含 8 个函数指针：

```c
typedef struct {
    uint8_t (*iic_init)(void);
    uint8_t (*iic_deinit)(void);
    uint8_t (*iic_read)(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len);
    uint8_t (*iic_write)(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len);
    uint8_t (*iic_read_address16)(uint8_t addr, uint16_t reg, uint8_t *buf, uint16_t len);
    uint8_t (*iic_write_address16)(uint8_t addr, uint16_t reg, uint8_t *buf, uint16_t len);
    void (*delay_ms)(uint32_t ms);
    void (*debug_print)(const char *const fmt, ...);
    uint32_t id;
    uint8_t inited;
} at24cxx_handle_t;
```

`driver_at24cxx_interface.c` 实现这些回调，通过宏注入到 handle：

```c
DRIVER_AT24CXX_LINK_INIT(&gs_handle, at24cxx_handle_t);
DRIVER_AT24CXX_LINK_IIC_INIT(&gs_handle, at24cxx_interface_iic_init);
DRIVER_AT24CXX_LINK_IIC_READ(&gs_handle, at24cxx_interface_iic_read);
DRIVER_AT24CXX_LINK_IIC_WRITE(&gs_handle, at24cxx_interface_iic_write);
DRIVER_AT24CXX_LINK_IIC_READ_ADDRESS16(&gs_handle, at24cxx_interface_iic_read_address16);
DRIVER_AT24CXX_LINK_IIC_WRITE_ADDRESS16(&gs_handle, at24cxx_interface_iic_write_address16);
DRIVER_AT24CXX_LINK_DELAY_MS(&gs_handle, at24cxx_interface_delay_ms);
DRIVER_AT24CXX_LINK_DEBUG_PRINT(&gs_handle, at24cxx_interface_debug_print);
```

宏展开后就是简单的赋值，如 `DRIVER_AT24CXX_LINK_IIC_READ(HANDLE, FUC)` → `(HANDLE)->iic_read = FUC`。

**注入时机**：`at24cxx_basic_init` 或 `at24cxx_read_test` 初始化时一次性注入，之后驱动层通过函数指针调用。

**注入优势**：
- 运行时可切换（如系统中有两条不同引脚的 I2C 总线）
- 同一个驱动可服务多个实例（每个实例独立的 handle）
- 驱动层完全不碰 HAL，可移植到任何平台

## 核心实现详解

### at24cxx_interface_iic_init——平台适配层初始化

**作用**：配置 GPIO 开漏上拉，初始化软 I2C 总线结构体，注入 GPIO 回调。

**形参**：无。

**输入/输出**：输入 = 无；输出 = GPIO 配置完成，软 I2C 总线就绪，返回 0 成功 / 1 失败。

**调用者**：`at24cxx_basic_init`、`at24cxx_read_test`，初始化时通过函数指针间接调用。

```c
uint8_t at24cxx_interface_iic_init(void)
{
    GPIO_InitTypeDef init = {0};

    at24cxx_enable_gpio_clock(AT24CXX_SCL_GPIO_Port);  // ① 使能 GPIO 时钟
    at24cxx_enable_gpio_clock(AT24CXX_SDA_GPIO_Port);

    init.Pin = AT24CXX_SCL_Pin;
    init.Mode = GPIO_MODE_OUTPUT_OD;     // ② 开漏输出
    init.Pull = GPIO_PULLUP;             // ③ 内部上拉
    init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(AT24CXX_SCL_GPIO_Port, &init);

    init.Pin = AT24CXX_SDA_Pin;
    HAL_GPIO_Init(AT24CXX_SDA_GPIO_Port, &init);

    HAL_GPIO_WritePin(AT24CXX_SCL_GPIO_Port, AT24CXX_SCL_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(AT24CXX_SDA_GPIO_Port, AT24CXX_SDA_Pin, GPIO_PIN_SET);

    // ④ 注入 GPIO 回调
    s_at24cxx_soft_i2c_bus.scl.write = at24cxx_scl_write;
    s_at24cxx_soft_i2c_bus.scl.read = at24cxx_scl_read;
    s_at24cxx_soft_i2c_bus.scl.ctx = NULL;
    s_at24cxx_soft_i2c_bus.sda.write = at24cxx_sda_write;
    s_at24cxx_soft_i2c_bus.sda.read = at24cxx_sda_read;
    s_at24cxx_soft_i2c_bus.sda.ctx = NULL;
    s_at24cxx_soft_i2c_bus.delay_fn = NULL;
    s_at24cxx_soft_i2c_bus.delay_ctx = NULL;
    s_at24cxx_soft_i2c_bus.delay_ticks = AT24CXX_SOFT_I2C_DELAY_TICKS;  // 120
    s_at24cxx_soft_i2c_bus.stretch_timeout_ticks = 0U;  // ⑤ EEPROM 不做时钟拉伸
    s_at24cxx_soft_i2c_bus.initialized = 0U;

    return (soft_i2c_bus_init(&s_at24cxx_soft_i2c_bus) == SOFT_I2C_STATUS_OK) ? 0U : 1U;
}
```

- **① `at24cxx_enable_gpio_clock`**：穷举 GPIOA~GPIOK 的时钟使能宏，用 `#if defined` 条件编译。这是 HAL 的坑——GPIO 时钟必须手动使能，否则读写寄存器无效果
- **② 开漏输出**：I2C 协议要求。开漏模式下，输出 0 拉低，输出 1 释放（由上拉电阻拉高）。多个设备可共享总线而不会短路
- **③ 内部上拉**：STM32 内部上拉电阻约 40kΩ，勉强可用。正式产品应外接 4.7kΩ 上拉
- **④ GPIO 回调注入**：`at24cxx_scl_write` 等函数包装 HAL GPIO 操作，注入到 soft_i2c 总线结构体。`ctx = NULL` 因为引脚端口/引脚号是编译期常量（`AT24CXX_SCL_GPIO_Port`），不需要运行时上下文
- **⑤ `stretch_timeout_ticks = 0U`**：EEPROM 不做时钟拉伸（与 AHT30 不同），设 0 让 soft_i2c 库使用默认超时值

### at24cxx_interface_write_with_prefix——带前缀的写操作

**作用**：将地址前缀（1 或 2 字节）和数据缓冲区拼接后，通过软 I2C 一次性发送。解决 EEPROM 写入需要"先发地址再发数据"的协议要求。

**形参**：`addr` — I2C 设备地址；`prefix` — 地址前缀字节；`prefix_len` — 前缀长度（1 或 2）；`buf` — 数据缓冲区；`len` — 数据长度。

**输入/输出**：输入 = 设备地址 + 地址前缀 + 数据；输出 = 拼接后的数据通过 I2C 发出，返回 0 成功 / 1 失败。

**调用者**：`at24cxx_interface_iic_write`（8 位地址）、`at24cxx_interface_iic_write_address16`（16 位地址）。

```c
static uint8_t at24cxx_interface_write_with_prefix(uint8_t addr,
                                                   const uint8_t *prefix,
                                                   size_t prefix_len,
                                                   const uint8_t *buf,
                                                   uint16_t len)
{
    size_t tx_len = prefix_len + (size_t)len;
    if (tx_len == 0U) return 0;

    uint8_t stack_buf[AT24CXX_INTERFACE_TX_STACK_BUFFER_SIZE];  // 66 字节栈缓冲
    uint8_t *tx_buf = stack_buf;

    if (tx_len > sizeof(stack_buf)) {          // ① 超大时走堆
        tx_buf = (uint8_t *)malloc(tx_len);
        if (tx_buf == NULL) return 1;
    }

    memcpy(tx_buf, prefix, prefix_len);        // ② 拷贝地址前缀
    memcpy(tx_buf + prefix_len, buf, len);     // ③ 拷贝数据

    soft_i2c_status_t status = soft_i2c_master_transmit(&s_at24cxx_soft_i2c_bus, addr, tx_buf, tx_len);

    if (tx_buf != stack_buf) free(tx_buf);     // ④ 释放堆内存
    return (status == SOFT_I2C_STATUS_OK) ? 0U : 1U;
}
```

- **① 66 字节栈缓冲**：AT24C02 页大小 8 字节 + 2 字节地址前缀 = 10 字节，66 字节足够覆盖所有 AT24CXX 的最大页写入（256 字节页 + 2 字节地址 = 258，但实际不会一次写这么多）
- **堆分配兜底**：`tx_len > 66` 时走 malloc。实际场景中几乎不会触发（EEPROM 页写入受页大小限制）
- **②③ 拼接而非两次发送**：I2C 写入要求地址和数据在同一个 START-STOP 事务内发送，不能分两次。拼接后一次 `master_transmit` 搞定

### at24cxx_interface_iic_read——8 位地址读取

**作用**：先写寄存器地址（伪写），再重复起始读取数据。这是 EEPROM 随机读取的标准时序。

**形参**：`addr` — I2C 设备地址；`reg` — EEPROM 内存地址（8 位）；`buf` — 输出缓冲区；`len` — 读取长度。

**输入/输出**：输入 = 设备地址 + 内存地址 + 缓冲区；输出 = 缓冲区被填入读取的数据，返回 0 成功 / 1 失败。

**调用者**：驱动层 `at24cxx_read`，通过 `handle->iic_read` 函数指针间接调用。

```c
uint8_t at24cxx_interface_iic_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len)
{
    uint8_t reg_buf[1];
    reg_buf[0] = reg;

    soft_i2c_status_t status = soft_i2c_master_write_read(&s_at24cxx_soft_i2c_bus,
                                                          addr, reg_buf, 1, buf, len);
    return (status == SOFT_I2C_STATUS_OK) ? 0U : 1U;
}
```

**I2C 时序**：`S [addr+W] ACK [reg] ACK Sr [addr+R] ACK [data0] ... [dataN] NACK P`

- **`soft_i2c_master_write_read`**：复用 14 例的组合事务函数，内部自动处理重复起始
- **`reg_buf[1]`**：EEPROM 的"寄存器地址"就是内存地址，1 字节（AT24C02 地址范围 0x00~0xFF）
- **为什么用 `write_read` 而非 `transmit` + `receive`**：EEPROM 随机读取必须先写地址再读数据，中间不能发 STOP（发 STOP 后 EEPROM 内部地址指针会复位）

### at24cxx_interface_iic_read_address16——16 位地址读取

**作用**：与 8 位地址版本同构，但地址拆分为 2 字节（大端序）。

```c
uint8_t at24cxx_interface_iic_read_address16(uint8_t addr, uint16_t reg, uint8_t *buf, uint16_t len)
{
    uint8_t reg_buf[2];
    reg_buf[0] = (uint8_t)(reg >> 8);    // 高字节先发
    reg_buf[1] = (uint8_t)(reg & 0xFFU); // 低字节后发

    soft_i2c_status_t status = soft_i2c_master_write_read(&s_at24cxx_soft_i2c_bus,
                                                          addr, reg_buf, 2, buf, len);
    return (status == SOFT_I2C_STATUS_OK) ? 0U : 1U;
}
```

- **大端序**：I2C 协议规定多字节地址先发高位。`reg = 0x01A0` → 发送 `{0x01, 0xA0}`
- **驱动层自动选择**：`at24cxx_set_type` 设置芯片型号后，驱动层根据型号自动调用 8 位或 16 位版本的读写函数

### at24cxx_read_test——读写校验测试

**作用**：对 EEPROM 进行 8 组 × 12 字节的随机数据写入+回读+比对，验证驱动正确性。

**形参**：`type` — 芯片型号（如 AT24C02）；`address` — 地址引脚配置（如 A000）。

**输入/输出**：输入 = 芯片型号 + 地址配置；输出 = 串口打印测试结果，返回 0 全部通过 / 1 失败。

**调用者**：`main`，上电后调一次。

```c
uint8_t at24cxx_read_test(at24cxx_t type, at24cxx_address_t address)
{
    // ... 初始化 handle、注入回调、打印芯片信息 ...

    inc = ((uint32_t)type + 1) / 8;  // ① 计算地址步进
    for (i = 0; i < 8; i++) {
        for (j = 0; j < 12; j++) {
            buf[j] = (uint8_t)(rand() % 256);  // ② 随机数据
        }

        at24cxx_write(&gs_handle, i*inc, buf, 12);      // ③ 写入
        at24cxx_read(&gs_handle, i*inc, buf_check, 12);  // ④ 回读

        for (j = 0; j < 12; j++) {
            if (buf[j] != buf_check[j]) return 1;        // ⑤ 比对
        }
        at24cxx_interface_debug_print("at24cxx: 0x%04X read write test passed.\n", i*inc);
    }
    return 0;
}
```

- **① `inc = (type+1)/8`**：AT24C02 容量 256 字节，`(256+1)/8 = 32`。8 组测试分布在 0、32、64、...、224 地址，均匀覆盖整个 EEPROM
- **② `rand() % 256`**：伪随机数据，测试写入后数据是否能正确保持。未设种子，每次上电序列相同
- **③④ 写入+回读**：EEPROM 的核心操作。写入后立即回读，验证内部写入周期完成
- **⑤ 逐字节比对**：任何不匹配立即报错退出，串口输出 `check error`

### at24cxx_basic_init——封装层初始化

**作用**：静态 handle + 简化 API。把依赖注入的样板代码封装成一个函数调用。

```c
uint8_t at24cxx_basic_init(at24cxx_t type, at24cxx_address_t address)
{
    DRIVER_AT24CXX_LINK_INIT(&gs_handle, at24cxx_handle_t);
    DRIVER_AT24CXX_LINK_IIC_INIT(&gs_handle, at24cxx_interface_iic_init);
    // ... 其他 7 个 LINK 宏 ...

    at24cxx_set_type(&gs_handle, type);
    at24cxx_set_addr_pin(&gs_handle, address);
    at24cxx_init(&gs_handle);
    return 0;
}
```

**设计意图**：调用方只需 `at24cxx_basic_init(AT24C02, AT24CXX_ADDRESS_A000)` 一行，不需要知道 handle 和依赖注入细节。`gs_handle` 是文件静态变量，整个模块共享一个实例。

## 设计问题与改进空间

1. **写入后无延时等待**：`at24cxx_write` 内部应该有 5ms 等待（EEPROM 内部写入周期），但当前实现未显式等待。如果驱动库内部没有处理，连续写入可能失败。需查看 `driver_at24cxx.c` 内部实现确认。

2. **`rand()` 未设种子**：默认种子为 1，每次上电测试序列相同。可加 `srand(HAL_GetTick())` 用系统时间做种子。

3. **`at24cxx_enable_gpio_clock` 的 #if 穷举**：为每个 GPIO 端口写一个 `#if defined` 分支，可移植但冗长（11 个端口 × 5 行 = 55 行）。可用宏数组或直接依赖 CubeMX 初始化。

4. **页边界未处理**：测试代码每次写 12 字节，如果起始地址不在页边界，可能跨页回绕导致数据覆盖。驱动库应该内部处理分页，但需验证。

5. **14 例 vs 15 例的驱动风格对比**：14 例手写驱动，轻量可控；15 例用第三方库 LibDriver，功能全面但代码量大（23KB）。学习阶段手写更有价值，工程阶段用库更高效。

## 关联笔记

- [[14_rocketpi_i2c_aht30|14 I2C AHT30]]：同为 I2C 器件，传感器 vs 存储器对比
- [[14_rocketpi_i2c_aht30_1|软 I2C 位操作详解]]：soft_i2c 库的完整实现，15 例复用
- [[34_rocketpi_w25qxx|34 SPI Flash]]：对比 SPI vs I2C 存储器
