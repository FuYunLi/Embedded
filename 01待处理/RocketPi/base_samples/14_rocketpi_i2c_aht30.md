---
status: done
created: 2026-09-13
tags:
  - c/i2c
  - c/soft-peripheral
  - embedded/sensor
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/14_rocketpi_i2c_aht30"
---

# 14 I2C 温湿度传感器 AHT30

## 一句话定性

AHT30 是 I2C 接口的数字温湿度传感器，测量环境温湿度，精度 ±0.3°C/±2%RH。

## 同类产品定位

- **同系列**：AHT10（早期，已停产）、AHT20（低成本替代，精度略低 ±0.5°C）
- **竞品**：DHT11/DHT22（单总线，精度 ±2°C/±5%RH，占一个定时器中断）、SHT30（工业级，精度 ±0.2°C，贵 2-3 倍）
- **本例选型理由**：I2C 接口（多从设备可挂同一总线）、精度够用、成本适中（~¥3）

## 硬件连接与地址

- I2C 地址：0x38（7 位），HAL 左移后 0x70
- 引脚分配：PB8=SCL，PB9=SDA（开漏上拉）
- 本例默认软件 I2C，`driver_aht30_config.h` 宏切换硬件 I2C

## 通信协议要点

- 触发测量：3 字节命令 {0xAC, 0x33, 0x00}
- 等待转换：80ms（典型值）
- 读取：6 字节原始数据，raw[0] bit7 忙标志
- 数据格式：湿度 20 位 + 温度 20 位，定点转浮点

## 代码架构

```
┌─────────────────────────────────────┐
│ main.c (应用层)                     │
│  - 初始化 + 主循环轮询 1s          │
├─────────────────────────────────────┤
│ driver_aht30_test.c (测试/日志层)   │
│  - aht30_test_log_measurement()    │
├─────────────────────────────────────┤
│ driver_aht30.c (驱动层)             │
│  - aht30_init / read_raw / read    │
│  - aht30_convert_samples           │
├─────────────────────────────────────┤
│ soft_i2c.c / HAL I2C (传输层)       │
│  - 依赖注入：GPIO 回调 + 延时回调  │
└─────────────────────────────────────┘
```

## 核心实现详解

### aht30_init——传感器初始化

**作用**：初始化传输层，等待上电稳定，发送软复位命令，完成传感器就绪。

**形参**：无。

**输入/输出**：输入 = 无；输出 = 传感器进入可测量状态，返回 HAL_OK 或错误码。

**调用者**：`main`，上电后调一次，在 MX_GPIO_Init / MX_I2C1_Init / MX_USART2_UART_Init 之后。

```c
HAL_StatusTypeDef aht30_init(void)
{
    HAL_StatusTypeDef status = aht30_bus_init();  // ① 初始化传输层
    if (status != HAL_OK) {
        return status;
    }

    HAL_Delay(AHT30_POWER_ON_DELAY);  // ② 上电等待 20ms
    status = aht30_soft_reset();       // ③ 发送软复位命令 0xBA
    HAL_Delay(AHT30_POST_RESET_DELAY); // ④ 复位后等待 20ms
    return status;
}
```

- **① `aht30_bus_init`**：软件 I2C 模式下配置 GPIO 开漏上拉、拉高 SCL/SDA、初始化总线结构体；硬件 I2C 模式下直接返回 HAL_OK（CubeMX 已初始化）
- **② 上电等待**：AHT30 数据手册要求上电后至少等 20ms 才能接收命令，否则首条命令可能丢失
- **③ 软复位**：发送 0xBA 命令，传感器内部状态机复位到空闲态，不触发测量
- **④ 复位后等待**：软复位需要 20ms 完成，期间传感器不响应总线

### aht30_read_raw——触发测量并读取原始数据

**作用**：发送触发测量命令，等待传感器内部 ADC 转换完成，读取 6 字节原始数据并检查忙标志。

**形参**：`raw[6]` — 6 字节输出缓冲区，调用方分配。

**输入/输出**：输入 = 空的 6 字节数组；输出 = 数组被填入原始数据（raw[0] 状态字节 + raw[1]~raw[5] 湿度温度混合），返回 HAL_OK/HAL_BUSY/HAL_ERROR。

**调用者**：`aht30_read`（转换层）、`aht30_test_log_raw`（调试层）。

```c
HAL_StatusTypeDef aht30_read_raw(uint8_t raw[6])
{
    uint8_t cmd[3] = {AHT30_CMD_TRIGGER, AHT30_CMD_CONFIG_0, AHT30_CMD_CONFIG_1};
    // ① 发送触发命令 {0xAC, 0x33, 0x00}
    HAL_StatusTypeDef status = aht30_bus_transmit(cmd, sizeof(cmd));
    if (status != HAL_OK) {
        return status;
    }

    HAL_Delay(AHT30_MEASUREMENT_DELAY);  // ② 等待转换 80ms

    status = aht30_bus_receive(raw, 6U);  // ③ 读取 6 字节
    if (status != HAL_OK) {
        return status;
    }

    if ((raw[0] & AHT30_STATUS_BUSY) != 0U) {  // ④ 检查忙标志
        return HAL_BUSY;
    }

    return HAL_OK;
}
```

- **① 命令含义**：0xAC = 触发测量，0x33 = 配置字节 0（数据手册规定），0x00 = 配置字节 1。三个字节必须连续发送，中间不能插入 STOP
- **② 80ms 等待**：数据手册典型值 80ms，最大值未明确标注。当前实现用阻塞延时，主循环 1s 周期足够覆盖
- **③ 读取时序**：发送读地址后，传感器先发 1 字节状态，再发 5 字节数据。I2C 协议保证字节间有 ACK
- **④ 忙标志**：raw[0] bit7 = 1 表示传感器仍在转换中，此时 raw[1]~raw[5] 无效。**正常流程不会触发此分支**（已等 80ms），保留是为了防御传感器异常
- **raw 数组布局**：raw[0]=状态，raw[1]=湿度高字节，raw[2]=湿度中字节，raw[3]=湿度低4位+温度高4位，raw[4]=温度中字节，raw[5]=温度低字节——**湿度和温度在 raw[3] 处交叉**，这是 AHT30 的数据格式特性

### aht30_convert_samples——原始数据转物理量

**作用**：将 6 字节原始数据中的 20 位湿度和 20 位温度提取出来，转换为浮点型的百分比和摄氏度。

**形参**：`raw[6]` — 已读取的原始数据；`temperature_c` — 温度输出指针；`humidity_pct` — 湿度输出指针。

**输入/输出**：输入 = 6 字节原始数据；输出 = 温度（-50~+150°C）和湿度（0~100% RH）写入调用方变量。

**调用者**：`aht30_read`，在 read_raw 成功后调用。

```c
static void aht30_convert_samples(const uint8_t raw[6], float *temperature_c, float *humidity_pct)
{
    uint32_t raw_humidity = ((uint32_t)raw[1] << 12)
                          | ((uint32_t)raw[2] << 4)
                          | (uint32_t)(raw[3] >> 4);

    uint32_t raw_temperature = (((uint32_t)raw[3] & 0x0FU) << 16)
                             | ((uint32_t)raw[4] << 8)
                             | (uint32_t)raw[5];

    *humidity_pct = (raw_humidity * 100.0f) / 1048576.0f;
    *temperature_c = (raw_temperature * 200.0f) / 1048576.0f - 50.0f;
}
```

**湿度 20 位拼接**：

```
raw[1] = 0x6B  →  0x6B000  (<<12)
raw[2] = 0x9F  →  0x009F0  (<<4)
raw[3] = 0x3C  →  0x00003  (>>4，取高4位)
                  --------
                  0x6B9F3  = 440819
湿度 = 440819 * 100 / 1048576 = 42.04% RH
```

- **`(uint32_t)raw[1] << 12` 强转必要性**：raw[1] 是 uint8_t，不强转则 `raw[1] << 12` 在 int（32 位有符号）上计算，若 raw[1] 的 bit7 为 1，结果的 bit19 可能触发符号扩展问题。强转到 uint32_t 确保无符号运算
- **`raw[3] >> 4` 取高 4 位**：raw[3] 同时承载湿度低 4 位和温度高 4 位，右移 4 位丢弃温度部分，保留湿度部分
- **1048576 = 2^20**：20 位原始值的满量程。除以它得到 0.0~1.0 的归一化值，再乘以 100 得百分比

**温度 20 位拼接**：

```
raw[3] = 0x3C  →  0x30000  (&0x0F <<16，取低4位)
raw[4] = 0x1A  →  0x01A00  (<<8)
raw[5] = 0x2B  →  0x0002B  (不移位)
                  --------
                  0x31A2B  = 203307
温度 = 203307 * 200 / 1048576 - 50 = -11.16°C
```

- **`& 0x0FU`**：raw[3] 的低 4 位属于温度，高 4 位属于湿度，必须用掩码分离
- **`* 200.0f / 1048576.0f - 50.0f`**：数据手册公式。满量程对应 200°C 范围，偏移 -50°C（原始值 0 = -50°C）

### aht30_read——高层读取接口

**作用**：触发测量、读取原始数据、转换为物理量，一步到位的高层接口。

**形参**：`temperature_c` — 温度输出指针；`humidity_pct` — 湿度输出指针。

**输入/输出**：输入 = 两个浮点指针；输出 = 温度和湿度被写入，返回状态码。

**调用者**：`aht30_test_log_measurement`、`main` 主循环。

```c
HAL_StatusTypeDef aht30_read(float *temperature_c, float *humidity_pct)
{
    if ((temperature_c == NULL) || (humidity_pct == NULL)) {
        return HAL_ERROR;
    }

    uint8_t raw[6] = {0};
    HAL_StatusTypeDef status = aht30_read_raw(raw);
    if (status != HAL_OK) {
        return status;
    }

    aht30_convert_samples(raw, temperature_c, humidity_pct);
    return HAL_OK;
}
```

**NULL 防御放最前面**：convert_samples 会解引用这两个指针，若为 NULL 则崩溃。防御在 read_raw 之前，避免无意义的 I2C 通信。

### aht30_test_log_measurement——日志格式化输出

**作用**：读取一次温湿度，将浮点值转换为定点整数打印，避免 printf 浮点库的体积开销。

**形参**：无。

**输入/输出**：输入 = 调用 aht30_read 获取温度湿度；输出 = 串口打印如 `AHT30 -> T=23.5C  RH=45.2%`。

**调用者**：`main` 主循环，每 1s 调一次。

```c
HAL_StatusTypeDef aht30_test_log_measurement(void)
{
    float temperature = 0.0f;
    float humidity = 0.0f;
    HAL_StatusTypeDef status = aht30_read(&temperature, &humidity);
    if (status == HAL_OK) {
        int16_t temp10 = (int16_t)(temperature * 10.0f);
        uint16_t hum10 = (uint16_t)(humidity * 10.0f);
        printf("AHT30 -> T=%d.%01dC  RH=%d.%01d%%\r\n",
               temp10 / 10, abs(temp10 % 10),
               hum10 / 10, hum10 % 10);
    } else if (status == HAL_BUSY) {
        printf("AHT30 measurement busy\r\n");
    } else {
        aht30_test_print_status("read", status);
    }
    return status;
}
```

- **`temperature * 10.0f` 再取整**：浮点转定点，保留 1 位小数。23.56°C → temp10=235 → `23.5`
- **`abs(temp10 % 10)`**：负温度时 `temp10 = -116`，`-116 % 10 = -6`，取绝对值得 `6`，打印 `-11.6`
- **`%%` 转义**：printf 中 `%` 是格式符前缀，要打印字面 `%` 必须写 `%%`
- **不用 `%f`**：嵌入式 printf 的 `%f` 默认链接浮点库，增加 5~10KB Flash。定点整数打印零额外开销

## 软/硬 I2C 双模切换

`driver_aht30_config.h` 用一个宏控制传输层实现：

```c
#ifndef AHT30_USE_SOFT_I2C
#define AHT30_USE_SOFT_I2C 1  // 默认软件 I2C，设为 0 切换硬件 I2C
#endif
```

`driver_aht30.c` 内部通过两个静态函数隔离差异：

```c
// 软件 I2C 模式
static HAL_StatusTypeDef aht30_bus_transmit(const uint8_t *data, size_t size)
{
    soft_i2c_status_t status = soft_i2c_master_transmit(&s_aht30_bus, AHT30_I2C_ADDRESS, data, size);
    if (status == SOFT_I2C_STATUS_TIMEOUT) return HAL_TIMEOUT;
    return (status == SOFT_I2C_STATUS_OK) ? HAL_OK : HAL_ERROR;
}

// 硬件 I2C 模式
static HAL_StatusTypeDef aht30_bus_transmit(const uint8_t *data, size_t size)
{
    return HAL_I2C_Master_Transmit(&AHT30_I2C_HANDLE, AHT30_I2C_ADDRESS, (uint8_t *)data, size, HAL_MAX_DELAY);
}
```

驱动层（`aht30_init/read_raw/read`）完全不感知底层实现，只调用 `aht30_bus_transmit/receive`。**宏切换在编译期完成，无运行时开销**。

## 设计问题与改进空间

1. **软 I2C 延时精度**：`delay_ticks` 用 NOP 循环，精度受编译器优化影响（-O0 比 -O2 慢 3~5 倍）。可改用 DWT 周期计数器（Cortex-M4 支持 `DWT->CYCCNT`），精度达 1/84MHz ≈ 12ns。

2. **AHT30 CRC 校验未实现**：数据手册有可选 CRC 校验（第 7 字节），当前代码未使用。在噪声环境（长导线、电机附近）可能读到错误数据。实现 CRC-8（多项式 0x31）约需 20 行代码。

3. **错误恢复机制**：`aht30_read_raw` 返回 `HAL_BUSY` 后，调用方需自行重试。可考虑在 `aht30_read` 内加重试逻辑（如最多重试 3 次，间隔 10ms）。

4. **`soft_i2c_bus_init` 防重入**：每次调用都会重新初始化总线结构体（拉高 SCL/SDA），若已在使用中可能产生毛刺。可加 `initialized` 检查跳过重复初始化。

## 关联笔记

- [[15_rocketpi_i2c_at24cxx|15 I2C EEPROM]]：同为 I2C 器件，EEPROM 存储器
- [[13_rocketpi_uart_radar|13 UART 雷达]]：对比 UART vs I2C 协议差异
- [[14_rocketpi_i2c_aht30_1|补充笔记：软 I2C 位操作详解]]：逐行解析 `soft_i2c.c`，相当于学一遍 I2C 协议实现
