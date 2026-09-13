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
  - "[[14_rocketpi_i2c_aht30_1]] 软 I2C 位操作详解"
---

# 15 I2C EEPROM AT24CXX

## 一句话定性

AT24CXX 是 I2C 接口的 EEPROM（电可擦除可编程只读存储器），掉电不丢失数据，用于存储配置参数、校准数据等小量关键数据。

## 同类产品定位

- **同系列**：AT24C01(128B)/AT24C02(256B)/AT24C04(512B)/AT24C08(1KB)/AT24C16(2KB)/AT24C32(4KB)/AT24C64(8KB)/AT24C128(16KB)/AT24C256(32KB)/AT24C512(64KB)/AT24CM01(128KB)/AT24CM02(256KB)——容量递增，引脚兼容，驱动统一
- **竞品**：FM24xx（铁电 RAM，写入无限次但贵 5~10 倍）、W25Qxx（SPI Flash，容量大但擦写粒度 4KB，不适合小量频繁写入）
- **本例选型理由**：I2C 接口、容量可选（本例 AT24C02 = 256B）、写入次数 100 万次、数据保持 100 年、成本低（~¥0.5）

## 硬件连接与地址

- I2C 地址：0x50（7 位），HAL 左移后 0xA0。地址低 3 位由 A2/A1/A0 引脚决定，可挂 8 片同型号
- 引脚分配：PB8=SCL，PB9=SDA（开漏上拉）
- 本例使用软件 I2C（复用 14 例的 soft_i2c 库）

## 通信协议要点

- **地址宽度**：AT24C01/02/04/08/16 用 8 位地址（1 字节）；AT24C32/64/128/256/512/CM01/CM02 用 16 位地址（2 字节，大端序）
- **页写入**：AT24C02 页大小 8 字节，写入不能跨页（页边界自动回绕到页首）
- **写入延时**：内部写入周期最大 5ms，写入后需等待（ACK 轮询或固定延时）才能进行下一次操作
- **随机读取**：写地址（伪写）→ 重复起始 → 读数据。顺序读取：持续发 ACK 读取，地址自动递增

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
│  - 静态 handle + 简化 API（init/read/write）    │
├─────────────────────────────────────────────────┤
│ driver_at24cxx.c (驱动层，第三方库 LibDriver)   │
│  - 通用 EEPROM 驱动，支持全系列 AT24CXX        │
│  - 地址宽度适配、页写入分片、写入等待           │
├─────────────────────────────────────────────────┤
│ driver_at24cxx_interface.c (平台适配层)         │
│  - 软 I2C 回调注入 + GPIO 配置                  │
│  - 8 位/16 位地址读写的桥接实现                 │
├─────────────────────────────────────────────────┤
│ soft_i2c.c (传输层，复用 14 例)                 │
└─────────────────────────────────────────────────┘
```

## 与 14 例（AHT30）的关键差异

| 维度 | 14 AHT30 | 15 AT24CXX |
|------|----------|------------|
| 器件类型 | 传感器（只读） | 存储器（读写） |
| 地址宽度 | 固定 0x38 | 可配置（A2/A1/A0） |
| 数据流向 | MCU 只读 | MCU 读写 |
| 页边界 | 无 | 有（8 字节页，跨页回绕） |
| 写入延时 | 无 | 5ms 内部写入周期 |
| 驱动来源 | 手写 | 第三方库 LibDriver |
| 复杂度 | 低（单次触发+读取） | 高（地址宽度适配+页写入+跨页处理） |
| 依赖注入方式 | 宏切换（编译期） | 函数指针注入（运行时） |

## 核心设计：三层依赖注入

本例最大的设计特点是**三层依赖注入**，与 14 例的"宏切换"方案形成对比。

### 14 例方案：宏切换

```c
// driver_aht30_config.h
#define AHT30_USE_SOFT_I2C 1  // 编译期选择

// driver_aht30.c
#if AHT30_USE_SOFT_I2C
static HAL_StatusTypeDef aht30_bus_transmit(...) { /* 软件 I2C */ }
#else
static HAL_StatusTypeDef aht30_bus_transmit(...) { /* 硬件 I2C */ }
#endif
```

**特点**：零运行时开销，但切换需重新编译。

### 15 例方案：函数指针注入

```c
// at24cxx_handle_t 结构体包含 8 个函数指针
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

`driver_at24cxx_interface.c` 实现这些回调，注入到 handle：

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

**宏注入 vs 函数指针注入**：
- 宏注入：编译期确定，零运行时开销，但切换需重新编译，不支持多实例
- 函数指针注入：运行时可切换（如多条 I2C 总线挂不同器件），支持多实例，但每次调用多一次间接跳转（~2 时钟周期）

## 核心实现详解

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

    uint8_t stack_buf[66];  // AT24CXX_INTERFACE_TX_STACK_BUFFER_SIZE
    uint8_t *tx_buf = stack_buf;

    if (tx_len > sizeof(stack_buf)) {        // ① 超大时走堆
        tx_buf = (uint8_t *)malloc(tx_len);
        if (tx_buf == NULL) return 1;
    }

    if ((prefix_len > 0U) && (prefix != NULL)) {
        memcpy(tx_buf, prefix, prefix_len);   // ② 拷贝地址前缀
    }
    if ((len > 0U) && (buf != NULL)) {
        memcpy(tx_buf + prefix_len, buf, len); // ③ 拷贝数据
    }

    soft_i2c_status_t status = soft_i2c_master_transmit(
        &s_at24cxx_soft_i2c_bus, addr, tx_buf, tx_len);  // ④ 一次性发送

    if (tx_buf != stack_buf) free(tx_buf);    // ⑤ 释放堆内存
    return (status == SOFT_I2C_STATUS_OK) ? 0U : 1U;
}
```

- **① 66 字节栈缓冲**：AT24C02 页大小 8 字节 + 2 字节地址前缀 = 10 字节，66 字节足够覆盖所有 AT24CXX 的最大页写入
- **②③ 拼接而非两次发送**：I2C 写入要求地址和数据在同一个 START-STOP 事务内发送，不能分两次 `soft_i2c_master_transmit`
- **④ 复用 soft_i2c 库**：底层传输完全复用 14 例的 `soft_i2c_master_transmit`
- **⑤ 堆释放**：只有 `tx_len > 66` 时才 malloc，此时必须 free。`tx_buf != stack_buf` 判断避免对栈数组 free

### at24cxx_interface_iic_read——8 位地址读取

**作用**：先写寄存器地址（伪写），再重复起始读取数据。这是 EEPROM 随机读取的标准时序。

**形参**：`addr` — I2C 设备地址；`reg` — EEPROM 内存地址（1 字节）；`buf` — 输出缓冲区；`len` — 读取长度。

**输入/输出**：输入 = 设备地址 + 内存地址 + 长度；输出 = 数据写入 buf，返回 0/1。

**调用者**：`at24cxx_read`（驱动层内部）。

```c
uint8_t at24cxx_interface_iic_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len)
{
    if (len == 0U) return 0;
    if (buf == NULL) return 1;

    uint8_t reg_buf[1];
    reg_buf[0] = reg;

    soft_i2c_status_t status = soft_i2c_master_write_read(
        &s_at24cxx_soft_i2c_bus, addr, reg_buf, 1, buf, len);
    return (status == SOFT_I2C_STATUS_OK) ? 0U : 1U;
}
```

**I2C 时序**：`S [addr+W] ACK [reg_addr] ACK Sr [addr+R] ACK [data0] ... [dataN] NACK P`

- **`soft_i2c_master_write_read`**：复用 14 例的组合事务函数，内部自动处理重复起始
- **`reg_buf[1]`**：EEPROM 的"寄存器地址"就是内存地址，1 字节（AT24C02 地址范围 0x00~0xFF）
- **NULL 和 len=0 防御**：避免无意义的 I2C 通信

### at24cxx_interface_iic_read_address16——16 位地址读取

**作用**：与 8 位地址版本同构，区别是内存地址用 2 字节大端序发送。

```c
uint8_t at24cxx_interface_iic_read_address16(uint8_t addr, uint16_t reg, uint8_t *buf, uint16_t len)
{
    uint8_t reg_buf[2];
    reg_buf[0] = (uint8_t)(reg >> 8);     // 高字节先发
    reg_buf[1] = (uint8_t)(reg & 0xFFU);  // 低字节后发

    soft_i2c_status_t status = soft_i2c_master_write_read(
        &s_at24cxx_soft_i2c_bus, addr, reg_buf, 2, buf, len);
    return (status == SOFT_I2C_STATUS_OK) ? 0U : 1U;
}
```

**16 位地址拆分**：大端序（MSB 先发），与 I2C 协议一致。AT24C32/64/128/256/512/CM01/CM02 使用 16 位地址，支持更大容量。

### at24cxx_interface_iic_write——8 位地址写入

**作用**：调用 `write_with_prefix`，将 1 字节内存地址作为前缀拼接到数据前面发送。

```c
uint8_t at24cxx_interface_iic_write(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len)
{
    if (len == 0U) return 0;
    if (buf == NULL) return 1;

    uint8_t prefix[1];
    prefix[0] = reg;

    return at24cxx_interface_write_with_prefix(addr, prefix, 1, buf, len);
}
```

**I2C 时序**：`S [addr+W] ACK [reg_addr] ACK [data0] ACK ... [dataN] ACK P`

### at24cxx_interface_iic_write_address16——16 位地址写入

```c
uint8_t at24cxx_interface_iic_write_address16(uint8_t addr, uint16_t reg, uint8_t *buf, uint16_t len)
{
    uint8_t prefix[2];
    prefix[0] = (uint8_t)(reg >> 8);
    prefix[1] = (uint8_t)(reg & 0xFFU);

    return at24cxx_interface_write_with_prefix(addr, prefix, 2, buf, len);
}
```

### at24cxx_interface_iic_init——软 I2C 总线初始化

**作用**：配置 GPIO 开漏上拉、初始化软 I2C 总线结构体、注入 GPIO 回调。

```c
uint8_t at24cxx_interface_iic_init(void)
{
    GPIO_InitTypeDef init = {0};

    at24cxx_enable_gpio_clock(AT24CXX_SCL_GPIO_Port);  // ① 使能 GPIO 时钟
    at24cxx_enable_gpio_clock(AT24CXX_SDA_GPIO_Port);

    init.Pin = AT24CXX_SCL_Pin;
    init.Mode = GPIO_MODE_OUTPUT_OD;   // ② 开漏输出
    init.Pull = GPIO_PULLUP;           // ③ 内部上拉
    init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(AT24CXX_SCL_GPIO_Port, &init);

    init.Pin = AT24CXX_SDA_Pin;
    HAL_GPIO_Init(AT24CXX_SDA_GPIO_Port, &init);

    HAL_GPIO_WritePin(AT24CXX_SCL_GPIO_Port, AT24CXX_SCL_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(AT24CXX_SDA_GPIO_Port, AT24CXX_SDA_Pin, GPIO_PIN_SET);

    // ④ 注入 GPIO 回调
    s_at24cxx_soft_i2c_bus.scl.write = at24cxx_scl_write;
    s_at24cxx_soft_i2c_bus.scl.read = at24cxx_scl_read;
    s_at24cxx_soft_i2c_bus.scl.ctx = NULL;  // 本例 GPIO 端口固定，无需 ctx
    s_at24cxx_soft_i2c_bus.sda.write = at24cxx_sda_write;
    s_at24cxx_soft_i2c_bus.sda.read = at24cxx_sda_read;
    s_at24cxx_soft_i2c_bus.sda.ctx = NULL;
    s_at24cxx_soft_i2c_bus.delay_fn = NULL;  // 用默认 NOP 循环
    s_at24cxx_soft_i2c_bus.delay_ticks = 120U;
    s_at24cxx_soft_i2c_bus.stretch_timeout_ticks = 0U;  // ⑤ EEPROM 不做时钟拉伸

    return (soft_i2c_bus_init(&s_at24cxx_soft_i2c_bus) == SOFT_I2C_STATUS_OK) ? 0U : 1U;
}
```

- **② 开漏输出**：I2C 协议要求。开漏模式下，输出 0 拉低，输出 1 释放（由上拉电阻拉高）。多个设备可以共享总线而不会短路
- **③ 内部上拉**：STM32 内部上拉电阻约 40kΩ，I2C 标准要求 4.7kΩ 以下。**短距离低速通信可以凑合，长距离或高速应外接 4.7kΩ 上拉**
- **④ ctx = NULL**：与 14 例不同，本例 GPIO 端口固定（AT24CXX_SCL_GPIO_Port），不需要通过 ctx 动态指定。14 例的 ctx 指向 `aht30_gpio_pin_t {port, pin}`，是为了支持多实例
- **⑤ EEPROM 不做时钟拉伸**：`stretch_timeout_ticks = 0U`。EEPROM 的写入延时（5ms）通过 ACK 轮询或固定延时处理，不在 SCL 线上拉伸

### at24cxx_read_test——读写校验测试

**作用**：对 EEPROM 进行 8 组 × 12 字节的随机数据写入+回读+比对，验证驱动正确性。

**形参**：`type` — 芯片型号（如 AT24C02）；`address` — 地址引脚配置（A2/A1/A0）。

**输入/输出**：输入 = 芯片型号 + 地址配置；输出 = 串口打印测试结果，返回 0 成功 / 1 失败。

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
            if (buf[j] != buf_check[j]) return 1;        // ⑤ 逐字节比对
        }
    }
    return 0;
}
```

- **① `inc = (type+1)/8`**：AT24C02 容量 256 字节，`(256+1)/8 = 32`。8 组测试分布在 0、32、64、...、224 地址，覆盖整个 EEPROM 空间
- **② `rand() % 256`**：伪随机数据。默认种子为 1，每次上电测试序列相同。可加 `srand(HAL_GetTick())` 用系统时间做种子
- **③④ 写入+回读**：EEPROM 的核心操作。写入后立即回读，验证内部写入周期完成（驱动库内部应有 ACK 轮询或延时等待）
- **⑤ 逐字节比对**：任何不匹配立即报错退出。比对失败通常意味着：I2C 通信错误、写入未完成、或 EEPROM 损坏

### at24cxx_basic_init——封装层初始化

**作用**：将"注入回调 + 设置型号 + 设置地址 + 初始化"四步封装为一个函数调用，简化上层使用。

```c
uint8_t at24cxx_basic_init(at24cxx_t type, at24cxx_address_t address)
{
    DRIVER_AT24CXX_LINK_INIT(&gs_handle, at24cxx_handle_t);
    DRIVER_AT24CXX_LINK_IIC_INIT(&gs_handle, at24cxx_interface_iic_init);
    // ... 注入 8 个回调 ...

    at24cxx_set_type(&gs_handle, type);       // 设置芯片型号（决定地址宽度等）
    at24cxx_set_addr_pin(&gs_handle, address); // 设置地址引脚配置
    at24cxx_init(&gs_handle);                  // 初始化驱动
}
```

**DRIVER_AT24CXX_LINK_* 宏展开**：`(HANDLE)->iic_init = FUC`，就是简单的函数指针赋值。宏存在的意义是提供类型安全的命名规范，而非复杂的代码生成。

## 设计问题与改进空间

1. **写入后无显式延时**：测试代码 `at24cxx_write` 后立即 `at24cxx_read`，如果驱动库内部没有 ACK 轮询或 5ms 延时，连续写入可能失败。需验证 LibDriver 的 `at24cxx_write` 内部实现。

2. **`rand()` 未设种子**：默认种子为 1，每次上电测试序列相同。可加 `srand(HAL_GetTick())` 用系统时间做种子，增加测试覆盖度。

3. **`at24cxx_enable_gpio_clock` 的 #if 穷举**：为每个 GPIO 端口写一个 `#if defined` 分支，可移植但冗长（11 个端口 × 5 行 = 55 行）。可用宏数组或直接依赖 CubeMX 初始化。

4. **页边界风险**：测试代码每次写 12 字节，如果起始地址不在页边界（如地址 6），会跨页回绕（8 字节页：6~7 写在第一页，8~13 回绕到页首 0~5）。驱动库应内部处理分页，但需验证。

5. **14 例 vs 15 例的驱动风格**：14 例手写驱动，轻量可控（~5KB）；15 例用第三方库 LibDriver，功能全面但代码量大（~23KB）。学习阶段手写更有价值（理解协议细节），工程阶段用库更高效（功能完备、测试充分）。

## 关联笔记

- [[14_rocketpi_i2c_aht30|14 I2C AHT30]]：同为 I2C 器件，传感器 vs 存储器对比
- [[14_rocketpi_i2c_aht30_1|软 I2C 位操作详解]]：soft_i2c 库的完整实现，15 例复用
- [[34_rocketpi_w25qxx|34 SPI Flash]]：对比 SPI vs I2C 存储器
