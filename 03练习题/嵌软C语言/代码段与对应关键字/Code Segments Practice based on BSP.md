---
status: todo
created: 2026-06-23
tags:
  - c-lang/memory-segment
  - todo
  - practice
source_project: SkyStar_BSP_HAL
references:
  - "[[代码段与对应关键字综合笔记]]"
  - "[[embedded_code_segments_arm]]"
---

# Code Segments Practice — 基于 SkyStar BSP

> 本练习集以 **SkyStar BSP V2** 真实工程代码为素材，考察嵌入式 C 语言中变量、函数、关键字与存储段的对应关系。
>
> **题目结构**：
> - **Part A**：带参考解析的概念分析题（Obsidian 折叠展开）
> - **Part B**：纯实操题（无答案，给出参考代码位置）
>   - B-1：直接用 BSP 源码回答
>   - B-2：基于 BSP 创新需求出题

---

## Part A：概念分析题（含参考解析）

> 点击 ▶ 展开查看解析

---

### A01 — static const 映射表的存储段分析

**来源**：`port_gpio.c` 第 20 行

```c
static const port_gpio_map_t gpio_mapping[] = {
    [PORT_GPIO_LED_CORE] = {GPIOB, GPIO_PIN_8},
    [PORT_GPIO_KEY1]     = {GPIOA, GPIO_PIN_0},
    /* ... 共 13 项 ... */
};
```

**问题**：

1. `gpio_mapping` 存储在哪个段？Flash 还是 RAM？为什么？
2. `static` 和 `const` 分别起了什么作用？去掉其中一个会有什么后果？
3. 估算 `gpio_mapping` 占用的 Flash 空间（ARM Cortex-M4，4 字节对齐）

??? note "参考解析"

    1. **存储段**：`.rodata`（Flash）。ARM 中 `const` 全局变量默认存入只读段，`static` 不改变存储位置，仅限制作用域。
    
    2. **关键字作用**：
       - `const`：语法只读 + 物理只读（Flash 硬件不可写），编译器禁止写入，同时让数据进入 `.rodata` 而非 `.data`
       - `static`：限制作用域为当前 `.c` 文件，其他文件不可通过 `extern` 访问
       - 去掉 `const`：数据移入 `.data` 段，占用 Flash + RAM 双份空间，且运行时可被误写
       - 去掉 `static`：其他文件可 `extern` 引用，增加耦合度，且符号表暴露
    
    3. **资源估算**：`port_gpio_map_t` 含 `GPIO_TypeDef*`（4B）+ `uint16_t`（2B）+ padding（2B）= 8B/项。13 项 × 8B = **104B Flash**，0B RAM。

---

### A02 — volatile 中断标志的必要性

**来源**：`dev_ft6336.c` 第 11 行

```c
static volatile bool g_touch_interrupt_fired = false;

static void ft6336_exti_callback(void)
{
    g_touch_interrupt_fired = true;
}
```

**问题**：

1. `g_touch_interrupt_fired` 存储在哪个段？`volatile` 是否改变存储位置？
2. 如果去掉 `volatile`，在 `O2` 优化下主循环 `while (!g_touch_interrupt_fired)` 会发生什么？
3. 为什么这里用 `static` 而不是普通全局变量？

??? note "参考解析"

    1. **存储段**：`.bss`（零初始化的静态全局变量）。`volatile` 不改变存储位置，仅告诉编译器每次访问都必须真正读写内存，不能缓存到寄存器或优化掉。
    
    2. **去掉 volatile 的后果**：编译器在 `O2` 下会将 `g_touch_interrupt_fired` 的值缓存到寄存器，`while` 循环变成死循环（永远读寄存器中的旧值 0），即使中断已将其置为 `true`，主循环也看不到。这是嵌入式开发中最经典的 bug 之一。
    
    3. **static 的好处**：该标志仅在本文件中使用，`static` 限制作用域避免命名冲突和外部误操作。如果其他模块也需要检测触摸中断，应通过函数接口暴露而非直接暴露变量。

---

### A03 — volatile DMA 传输状态与竞态

**来源**：`port_uart.c` 第 45-47 行

```c
volatile bool     tx_busy;
volatile uint16_t tx_dma_len;
```

**问题**：

1. `tx_busy` 和 `tx_dma_len` 是结构体成员，它们所在的结构体实例 `s_ctx` 存储在哪个段？
2. 为什么结构体成员需要 `volatile`？DMA 传输完成中断修改、主循环读取，这属于什么场景？
3. 如果 `tx_busy` 不加 `volatile`，在 `port_uart_tx_wait` 的 `while (ctx->tx_busy)` 中会发生什么？

??? note "参考解析"

    1. **存储段**：`s_ctx` 是 `static uart_context_t s_ctx[PORT_UART_MAX]`，未初始化，存于 `.bss` 段。结构体成员的 `volatile` 不影响结构体本身的存储位置。
    
    2. **volatile 必要性**：这是典型的"中断/主循环共享变量"场景。DMA 传输完成中断（`HAL_UART_TxCpltCallback`）会清零 `tx_busy`，主循环在 `port_uart_tx_wait` 中轮询该标志。`volatile` 确保主循环每次都从内存读取最新值，而不是使用寄存器中的过期缓存。
    
    3. **后果**：编译器可能将 `tx_busy` 的值缓存到寄存器，`while` 循环永远看不到中断清零操作，导致 `port_uart_tx_wait` 永不返回（死等）。同理，`tx_dma_len` 不加 `volatile` 可能导致回调中读取到旧值，跳过错误的 `lwrb_skip` 长度。

---

### A04 — 函数指针回调表的存储分析

**来源**：`port_gpio.c` 第 35 行 + `port_gpio.h` 第 68 行

```c
/* port_gpio.h */
typedef void (*port_exti_callback_t)(void);

/* port_gpio.c */
static port_exti_callback_t exti_callbacks[PORT_GPIO_MAX] = {NULL};
```

**问题**：

1. `exti_callbacks` 存储在哪个段？为什么？
2. `exti_callbacks[i]` 中存储的函数指针指向哪里？被指向的函数在哪个段？
3. 注册回调 `exti_callbacks[pin_id] = cb;` 执行后，`cb` 指针的值存储在 Flash 还是 RAM？

??? note "参考解析"

    1. **存储段**：`.bss`。`exti_callbacks` 是 `static` 局部数组，初始化为 `{NULL}`（零初始化），所以存入 `.bss` 段，0B Flash + `PORT_GPIO_MAX × 4B` RAM。
    
    2. **指针指向**：`exti_callbacks[i]` 存储的是函数指针（4B），指向 `.text` 段（Flash）中的函数机器码。例如 `ft6336_exti_callback` 的代码存在 Flash 中。
    
    3. **赋值后存储位置**：`cb` 指针的值（即函数地址）写入 `exti_callbacks[pin_id]`，该数组在 RAM 中，所以指针值存在 **RAM**。这是运行时动态绑定的典型模式——函数代码在 Flash，但函数指针在 RAM，运行时修改指针即可切换回调目标。

---

### A05 — #define 宏 vs const 变量 vs enum

**来源**：`dev_st7789.c` 第 35-52 行 vs `port_uart.h` 第 20 行

```c
/* dev_st7789.c — 宏定义方式 */
#define ST7789_CMD_NOP              0x00
#define ST7789_CMD_SOFT_RESET       0x01
#define ST7789_CMD_DISPLAY_ON       0x29

/* port_uart.h — 枚举方式 */
typedef enum {
    PORT_UART_1 = 0,
    PORT_UART_MAX
} port_uart_id_t;
```

**问题**：

1. `ST7789_CMD_NOP` 和 `PORT_UART_1` 各占多少 Flash 和 RAM？
2. 如果把 ST7789 命令改为 `const uint8_t` 全局数组，Flash/RAM 占用如何变化？
3. 三种方式（宏、const 变量、枚举）各适合什么场景？

??? note "参考解析"

    1. **占用**：
       - `ST7789_CMD_NOP`：宏定义，预处理阶段纯文本替换，**0B Flash + 0B RAM**
       - `PORT_UART_1`：枚举常量，编译期常量折叠，**0B Flash + 0B RAM**
    
    2. **改为 const 全局数组**：例如 `const uint8_t st7789_cmds[] = {0x00, 0x01, ..., 0x29}`，则存入 `.rodata` 段，**N 字节 Flash + 0B RAM**。比宏多占 Flash，但可以在运行时通过指针间接访问（如查表法）。
    
    3. **适用场景**：
       - **宏**：简单常量替换、条件编译、代码片段（如 `LCD_CS_SELECT()`），零开销
       - **const 变量**：需要取地址、运行时查表、字符串常量，有类型检查
       - **枚举**：有限集合的命名常量，调试器可显示符号名，`switch` 缺省警告

---

### A06 — static 全局函数的作用域与链接

**来源**：`soft_i2c.c` 全文约 10 个 static 函数

```c
static inline void s_delay(uint32_t us) { ... }
static int s_sda_read(soft_i2c_t *i2c) { ... }
static int s_write_bit(soft_i2c_t *i2c, int bit) { ... }
/* ... 共 10+ 个 static 函数 ... */
```

**问题**：

1. 这些 `static` 函数的机器码存储在哪个段？`static` 是否影响函数的存储位置？
2. 如果去掉 `static`，其他文件能直接调用 `s_write_bit` 吗？这有什么风险？
3. `static inline` 和 `static` 在编译器行为上有什么区别？

??? note "参考解析"

    1. **存储段**：`.text`（Flash）。`static` 不改变函数机器码的存储位置，仅限制符号的链接可见性为当前翻译单元。
    
    2. **去掉 static 的风险**：
       - 其他文件可 `extern` 声明后直接调用，破坏封装性
       - 多个 `.c` 文件定义同名函数时产生链接冲突
       - 编译器无法确定函数不被外部调用，可能无法内联优化
       - 符号表暴露，增加最终二进制体积
    
    3. **区别**：
       - `static`：限制链接可见性，编译器自由选择内联或不内联
       - `static inline`：建议编译器内联展开（无函数调用开销），若编译器决定不内联则生成一个 `static` 副本。`inline` 本身不是强制指令，最终决定权在编译器

---

### A07 — 模块私有数组的资源估算

**来源**：`bsp_uart.c` 第 20-27 行

```c
static uint8_t s_rx_dma_buf[RX_DMA_BUF_SIZE];   /* 64B */
static uint8_t s_rx_rb_buf[RX_RB_BUF_SIZE];      /* 1024B */
static uint8_t s_tx_rb_buf[TX_RB_BUF_SIZE];      /* 2048B */
static lwrb_t  s_rx_rb;
static lwrb_t  s_tx_rb;
```

**问题**：

1. 这 5 个变量各在哪个段？总 Flash 和 RAM 占用分别是多少？
2. 如果把 `s_rx_dma_buf` 改为非 `static` 全局变量，存储段会变吗？
3. `lwrb_t` 通常包含指针和长度字段（约 16B），估算 `s_rx_rb` 和 `s_tx_rb` 的 RAM 占用

??? note "参考解析"

    1. **存储段分析**：
       - `s_rx_dma_buf[64]`：零初始化 → `.bss`，0B Flash + 64B RAM
       - `s_rx_rb_buf[1024]`：零初始化 → `.bss`，0B Flash + 1024B RAM
       - `s_tx_rb_buf[2048]`：零初始化 → `.bss`，0B Flash + 2048B RAM
       - `s_rx_rb`：零初始化 → `.bss`，0B Flash + ~16B RAM
       - `s_tx_rb`：零初始化 → `.bss`，0B Flash + ~16B RAM
       - **总计**：0B Flash + ~3168B RAM（约 3.1KB）
    
    2. **去掉 static**：存储段不变，仍在 `.bss`。`static` 只影响链接可见性，不影响存储位置。但符号会暴露到全局，增加命名冲突风险。
    
    3. **lwrb_t 估算**：`lwrb_t` 通常包含 `buf` 指针（4B）、`size`（4B）、`r` 读指针（4B）、`w` 写指针（4B）= 16B。两个共 **32B RAM**。

---

### A08 — const 全局变量的只读性质

**来源**：`dev_ws2812.c` 第 10-14 行

```c
const dev_ws2812_rgb_t DEV_WS2812_COLOR_RED   = {255, 0, 0};
const dev_ws2812_rgb_t DEV_WS2812_COLOR_GREEN = {0, 255, 0};
const dev_ws2812_rgb_t DEV_WS2812_COLOR_BLUE  = {0, 0, 255};
const dev_ws2812_rgb_t DEV_WS2812_COLOR_BLACK = {0, 0, 0};
const dev_ws2812_rgb_t DEV_WS2812_COLOR_WHITE = {255, 255, 255};
```

**问题**：

1. 这 5 个 `const` 全局变量存储在哪个段？总 Flash 占用多少？
2. 尝试在代码中写 `DEV_WS2812_COLOR_RED.r = 128;`，编译和运行分别会发生什么？
3. 为什么不用 `#define` 定义这些颜色？

??? note "参考解析"

    1. **存储段**：`.rodata`（Flash）。`dev_ws2812_rgb_t` 含 3 个 `uint8_t`（3B），对齐后每项占 4B。5 项 × 4B = **20B Flash + 0B RAM**。
    
    2. **写入尝试**：
       - **编译期**：编译器直接报错，`const` 变量不可赋值
       - **如果强转绕过**（如 `*(uint8_t*)&DEV_WS2812_COLOR_RED.r = 128`）：在 ARM 上触发 **HardFault**，因为 `.rodata` 段映射到 Flash 硬件，Flash 不可随机写入
    
    3. **不用宏的原因**：`dev_ws2812_rgb_t` 是结构体，宏无法定义结构体常量；且 `const` 变量有类型检查，调试时可查看符号名，宏只是文本替换无类型安全。

---

### A09 — 临界区保护与变量一致性

**来源**：`dev_pca9555.c` 第 189-198 行

```c
uint32_t primask = port_enter_critical();
if (state == DEV_PCA9555_SET)
{
    dev->shadow_output[port] |= (1 << pin);
}
else
{
    dev->shadow_output[port] &= ~(1 << pin);
}
port_exit_critical(primask);
```

**问题**：

1. `port_enter_critical` 返回的 `primask` 存储在哪个段？为什么需要保存并恢复？
2. 如果不使用临界区保护，在什么场景下 `shadow_output` 会被破坏？
3. `port_enter_critical` / `port_exit_critical` 的机器码本身在哪个段？

??? note "参考解析"

    1. **primask 存储**：`primask` 是 `port_enter_critical` 的返回值，存储在**栈区**（局部变量），4B RAM（临时）。需要保存是因为可能存在临界区嵌套——外层已经关中断时，内层退出不应开中断，必须恢复原始 PRIMASK 值。
    
    2. **无保护的破坏场景**：主循环正在执行 `shadow_output[port] |= (1 << pin)`（读-改-写三步操作），如果此时中断中也修改了同一个 `shadow_output[port]`，则主循环的写回会覆盖中断的修改，导致数据丢失。这是经典的"读-改-写竞态"。
    
    3. **函数机器码**：`.text` 段（Flash）。这两个函数非常短小（各约 4-6 条 Thumb-2 指令），Flash 占用约 8-12B。

---

### A10 — static 局部变量 vs 普通局部变量

**来源**：`port_dwt.c` 第 11 行

```c
static uint8_t dwt_status = 0;
```

以及 `bsp_backlight.c` 第 14 行

```c
static uint8_t s_current_brightness = BACKLIGHT_DEFAULT_BRIGHTNESS;
```

**问题**：

1. `dwt_status` 和 `s_current_brightness` 分别存储在哪个段？为什么？
2. 如果将 `dwt_status` 改为函数内局部变量 `uint8_t dwt_status = 0;`，行为有什么变化？
3. `s_current_brightness` 的初始值来自 `BACKLIGHT_DEFAULT_BRIGHTNESS` 宏，这个初始值存储在哪里？

??? note "参考解析"

    1. **存储段**：
       - `dwt_status = 0`：零初始化的 static 全局变量 → `.bss`，0B Flash + 1B RAM
       - `s_current_brightness = BACKLIGHT_DEFAULT_BRIGHTNESS`（假设值为 50）：非零初值的 static 全局变量 → `.data`，1B Flash + 1B RAM
    
    2. **改为局部变量**：每次函数调用时在栈上重新分配并初始化为 0，函数返回后释放。`port_dwt_init` 中的状态判断逻辑将失效——每次调用都看到初始值 0，无法记录"已初始化"状态。
    
    3. **初始值存储**：`BACKLIGHT_DEFAULT_BRIGHTNESS` 是宏，编译期替换为常量。`s_current_brightness` 的初始值作为 `.data` 段的一部分存储在 **Flash** 中（启动时由初始化代码复制到 RAM）。

---

### A11 — 结构体对齐与 sizeof 分析

**来源**：`port_uart.c` 第 29-62 行

```c
typedef struct
{
    UART_HandleTypeDef *huart;       /* 4B */
    port_uart_id_t      id;          /* 4B (enum = int) */
    uint8_t  *rx_dma_buf;            /* 4B */
    uint16_t  rx_dma_size;           /* 2B */
    uint16_t  dma_write_pos;         /* 2B */
    lwrb_t   *rx_rb;                 /* 4B */
    lwrb_t           *tx_rb;         /* 4B */
    volatile bool     tx_busy;       /* 1B + 3B padding */
    volatile uint16_t tx_dma_len;    /* 2B + 2B padding */
    port_async_cb_t on_tx_complete;  /* 4B */
    port_async_cb_t on_error;        /* 4B */
    void (*on_rx_data)(port_uart_id_t, uint16_t, void*); /* 4B */
    void *user_ctx;                  /* 4B */
    port_async_cb_t  one_shot_cb;    /* 4B */
    void            *one_shot_ctx;   /* 4B */
    volatile bool initialized;       /* 1B + 3B padding */
    volatile bool rx_enabled;        /* 1B + 3B padding */
} uart_context_t;
```

**问题**：

1. 估算 `uart_context_t` 的 `sizeof`（ARM Cortex-M4，4 字节对齐）
2. `volatile bool tx_busy` 和 `bool tx_busy` 在 `sizeof` 上有区别吗？
3. 如果将两个 `volatile bool` 连续声明（`tx_busy` 和 `initialized`），能否减少 padding？

??? note "参考解析"

    1. **sizeof 估算**：按成员逐一排列，对齐到 4 字节边界：
       - `huart` 4B + `id` 4B + `rx_dma_buf` 4B + `rx_dma_size` 2B + `dma_write_pos` 2B = 16B
       - `rx_rb` 4B + `tx_rb` 4B + `tx_busy` 1B + 3B padding = 12B
       - `tx_dma_len` 2B + 2B padding + `on_tx_complete` 4B + `on_error` 4B = 12B
       - `on_rx_data` 4B + `user_ctx` 4B + `one_shot_cb` 4B + `one_shot_ctx` 4B = 16B
       - `initialized` 1B + 3B padding + `rx_enabled` 1B + 3B padding = 8B
       - **总计约 64B**
    
    2. **volatile 不影响 sizeof**：`volatile` 是类型限定符，只影响编译器的访问行为（禁止优化），不影响类型的大小或对齐。`volatile bool` 和 `bool` 都是 1 字节。
    
    3. **重排减少 padding**：如果将 `tx_busy`、`initialized`、`rx_enabled` 三个 `bool` 连续排列，3 × 1B = 3B + 1B padding = 4B，比原来 1+3+1+3=8B 节省 4B。这是嵌入式开发中常见的结构体优化手法。

---

### A12 — extern 全局实例的存储段

**来源**：`dev_pca9555.c` 第 26 行

```c
extern dev_pca9555_t g_pca_led;
```

**问题**：

1. `extern` 声明本身占不占 Flash/RAM？
2. `g_pca_led` 的实际定义可能在另一个 `.c` 文件中，它的存储段由什么决定？
3. 如果 `g_pca_led` 的定义为 `dev_pca9555_t g_pca_led = {...}`（非零初值），它存哪个段？

??? note "参考解析"

    1. **extern 声明**：不占任何存储空间。`extern` 仅告诉编译器"这个符号在别处定义"，链接时解析地址。声明不分配内存。
    
    2. **存储段由定义决定**：存储段取决于定义处的初始化方式：
       - 非零初值 → `.data`
       - 零初值/未初始化 → `.bss`
       - `const` 修饰 → `.rodata`
    
    3. **非零初值**：`.data` 段。Flash 存初始值，RAM 存运行时副本。`dev_pca9555_t` 含影子寄存器数组等成员，sizeof 可能在 20-30B 左右，因此 Flash + RAM 各占 20-30B。

---

### A13 — 宏函数的零开销原理

**来源**：`soft_i2c.c` 第 11-14 行

```c
#define SDA_HIGH(i2c) port_gpio_write((i2c)->sda, PORT_GPIO_HIGH)
#define SDA_LOW(i2c)  port_gpio_write((i2c)->sda, PORT_GPIO_LOW)
#define SCL_HIGH(i2c) port_gpio_write((i2c)->scl, PORT_GPIO_HIGH)
#define SCL_LOW(i2c)  port_gpio_write((i2c)->scl, PORT_GPIO_LOW)
```

**问题**：

1. 这 4 个宏展开后，是否产生额外的函数调用开销？
2. 为什么不用 `static inline` 函数替代？两者在此场景下有什么区别？
3. 宏的参数 `i2c` 没加括号会有什么隐患？

??? note "参考解析"

    1. **无额外开销**：宏在预处理阶段纯文本替换，展开后直接调用 `port_gpio_write`，不产生额外的函数调用层级。宏本身 0B Flash + 0B RAM。
    
    2. **inline 替代的区别**：
       - `static inline` 也能达到类似效果，且有类型检查
       - 但宏可以更灵活地做"代码片段"拼接（如 `SDA_HIGH(i2c); SCL_HIGH(i2c);` 一行写完）
       - 宏的缺点：无类型检查、参数可能被多次求值、调试时看不到宏内部
       - 此场景下两者性能等价，`static inline` 更安全
    
    3. **括号隐患**：如果宏写成 `#define SDA_HIGH(i2c) port_gpio_write(i2c->sda, ...)`，当调用 `SDA_HIGH(ptr + 1)` 时展开为 `port_gpio_write(ptr + 1->sda, ...)`，`->` 优先级高于 `+`，导致语法错误或逻辑错误。加括号 `(i2c)->sda` 确保先求值参数。

---

### A14 — DMA 缓冲区的存储要求

**来源**：`dev_ws2812.c` 第 49-50 行

```c
static uint32_t s_dma_buffer[WS2812_DMA_BUF_LEN] = {0};
```

其中 `WS2812_DMA_BUF_LEN = 300 + 72 + 200 = 572`

**问题**：

1. `s_dma_buffer` 存储在哪个段？总 RAM 占用多少？
2. DMA 控制器访问的地址是虚拟地址还是物理地址？对缓冲区的对齐有什么要求？
3. 如果把 `s_dma_buffer` 放在栈上（局部变量），会有什么风险？

??? note "参考解析"

    1. **存储段**：零初始化的 static 数组 → `.bss`，0B Flash + 572 × 4B = **2288B RAM**（约 2.2KB）。
    
    2. **DMA 地址与对齐**：DMA 访问的是物理地址（ARM Cortex-M 无 MMU，地址直接对应）。STM32 的 DMA 对源/目标地址有对齐要求：32 位传输需 4 字节对齐，16 位需 2 字节对齐。`uint32_t` 数组天然 4 字节对齐，满足要求。
    
    3. **栈上风险**：
       - 2.2KB 的局部数组直接压栈，极易导致**栈溢出**（Cortex-M4 默认栈可能只有 1-4KB）
       - 函数返回后栈空间释放，DMA 仍在传输，访问已释放的栈空间导致数据错乱
       - 嵌入式开发铁律：**DMA 缓冲区必须是 static 或全局变量，绝不能是局部变量**

---

### A15 — 字符串字面量的存储

**来源**：`bsp_board.c` 第 40-55 行

```c
const char *bsp_status_str(bsp_status_t status)
{
    switch (status)
    {
    case BSP_OK:     return "OK";
    case BSP_ERROR:  return "ERROR";
    case BSP_EINVAL: return "EINVAL";
    /* ... */
    default:         return "UNKNOWN";
    }
}
```

**问题**：

1. `"OK"`、`"ERROR"` 等字符串字面量存储在哪个段？
2. 返回的 `const char *` 指针本身存储在哪里？
3. 如果两个不同的函数都返回 `"OK"`，编译器会存几份？

??? note "参考解析"

    1. **存储段**：`.rodata`（Flash）。字符串字面量在 ARM 中默认存入只读段，内容不可修改。
    
    2. **指针存储**：`return "OK"` 返回的是指向 `.rodata` 段中字符串的指针，该指针值通过寄存器（`r0`）返回，不占额外存储。调用者接收后可存入栈上的局部变量。
    
    3. **字符串合并**：大多数编译器（GCC/Clang）会启用字符串常量合并（string pooling），相同的字符串字面量只存一份。`"OK"` 无论在多少个函数中使用，Flash 中只占 3B（'O'、'K'、'\0'）。

---

### A16 — PWM 映射表的 static const 分析

**来源**：`port_pwm.c` 第 17 行

```c
static const port_pwm_map_t pwm_mapping[PORT_PWM_MAX] = {
    [PORT_PWM_BUZZER] = {&htim13, TIM_CHANNEL_1},
    [PORT_PWM_WS2812] = {&htim5,  TIM_CHANNEL_4},
    [PORT_PWM_LCD_BL] = {&htim10, TIM_CHANNEL_1}
};
```

**问题**：

1. `pwm_mapping` 存储在哪个段？`&htim13` 等指针值在编译期还是运行时确定？
2. 为什么这里用指定下标初始化（`[PORT_PWM_BUZZER] = ...`）而不是顺序初始化？
3. 估算 `pwm_mapping` 的 Flash 占用

??? note "参考解析"

    1. **存储段**：`.rodata`（Flash）。`static const` 全局数组。`&htim13` 等是链接期确定的地址常量，编译器在编译期生成重定位条目，链接时填入最终地址。启动代码将初始值从 Flash 复制到 RAM（但 `.rodata` 段不会被复制，直接在 Flash 中读取）。
    
    2. **指定下标初始化**：C99 的 designated initializer，让枚举值与数组下标显式对应，即使枚举值不连续也能正确映射。比顺序初始化更清晰、更安全，新增/删除项时不会错位。
    
    3. **Flash 占用**：`port_pwm_map_t` 含 `TIM_HandleTypeDef*`（4B）+ `uint32_t`（4B）= 8B/项。3 项 × 8B = **24B Flash**。

---

### A17 — 条件编译与存储段的关系

**来源**：`dev_buzzer.c` 第 12-16 行

```c
#if BUZZER_TYPE == BUZZER_PASSIVE
#define BUZZER_MAX_VOLUME   100U
#define BUZZER_MAX_DUTY     500U
static uint16_t s_buzzer_freq = BUZZER_DEFAULT_FREQ;
static uint8_t  s_buzzer_vol  = BUZZER_DEFAULT_VOL;
#endif
```

**问题**：

1. 如果 `BUZZER_TYPE == BUZZER_ACTIVE`，`s_buzzer_freq` 和 `s_buzzer_vol` 是否占用存储空间？
2. 条件编译中的 `static` 变量，在未选中的分支中是否参与链接？
3. 这种条件编译方式对 Flash/RAM 优化有什么意义？

??? note "参考解析"

    1. **不占空间**：`#if` 条件为假时，`s_buzzer_freq` 和 `s_buzzer_vol` 的定义根本不会被编译器看到，不生成任何代码或数据，0B Flash + 0B RAM。
    
    2. **不参与链接**：条件编译是预处理阶段的行为，未选中的代码在编译前就被移除，不存在符号，不参与链接。
    
    3. **优化意义**：有源蜂鸣器只需 GPIO 开关，不需要 PWM 频率/占空比变量，条件编译让未使用的代码和数据完全不占空间。在资源紧张的 MCU 上，这种"编译期裁剪"比运行时判断更高效——零开销。

---

### A18 — HAL 回调函数的弱符号机制

**来源**：`port_spi.c` 第 325-385 行

```c
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi) { ... }
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi) { ... }
```

**问题**：

1. HAL 库中这些回调函数的默认实现使用了什么关键字？它存储在哪个段？
2. 用户重定义的回调函数与默认实现，在链接时如何选择？
3. 如果多个 `.c` 文件都定义了 `HAL_SPI_TxCpltCallback`，会发生什么？

??? note "参考解析"

    1. **弱符号**：HAL 库中回调的默认实现使用 `__attribute__((weak))`（即 `WEAK` 宏），定义为空函数。存储在 `.text` 段（Flash），通常只有一条 `BX lr` 返回指令（4B Flash）。
    
    2. **链接选择**：链接器在强符号和弱符号同名时，优先选择强符号。用户定义的回调是强符号，覆盖 HAL 的弱定义。最终二进制中只有一份实现。
    
    3. **多重定义**：多个强符号同名会导致**链接错误**（multiple definition）。这是回调函数只能在一个 `.c` 文件中实现的原因。BSP 中 `port_spi.c` 统一处理所有 SPI 回调，再通过 `get_id_by_handle` 分发到对应通道。

---

### A19 — 影子寄存器模式的存储分析

**来源**：`dev_pca9555.h` 结构体定义

```c
typedef struct {
    port_i2c_id_t i2c_id;
    uint8_t       dev_addr;
    uint8_t       shadow_output[2];  /* 输出影子寄存器 */
    uint8_t       shadow_config[2];  /* 配置影子寄存器 */
} dev_pca9555_t;
```

**问题**：

1. 如果 `g_pca_led` 是全局实例，`shadow_output` 数组存储在哪个段？
2. 影子寄存器模式相比"每次操作都直接读写硬件寄存器"，在 RAM 和 I2C 总线占用上有什么优势？
3. `shadow_output[2]` 和直接声明两个 `uint8_t` 成员，在内存布局上有什么区别？

??? note "参考解析"

    1. **存储段**：取决于 `g_pca_led` 的初始化方式。如果非零初值 → `.data`；零初值 → `.bss`。`shadow_output` 作为结构体成员，跟随结构体实例的存储段。
    
    2. **影子寄存器优势**：
       - **RAM 优势**：仅 2B 本地缓存，避免每次读-改-写都需要先 I2C 读回当前值
       - **I2C 总线优势**：单次 I2C 写操作替代"读回 → 修改 → 写入"三次操作，减少总线占用
       - **一致性**：本地影子保证读-改-写原子性（配合临界区）
    
    3. **布局区别**：`shadow_output[2]` 保证两个字节在内存中连续排列，可用 `port_i2c_mem_write` 一次性写入 2 字节。两个独立 `uint8_t` 成员可能因对齐产生 padding，且无法一次写入。

---

### A20 — printf 重定向与代码段

**来源**：`bsp_board.c` 第 55-63 行

```c
#if defined(__ARMCC_VERSION)
int fputc(int ch, FILE *f)
{
    (void)f;
    uint8_t data = (uint8_t)ch;
    bsp_uart_write(&data, 1U);
    return ch;
}
#endif
```

**问题**：

1. `fputc` 函数存储在哪个段？条件编译 `#if defined(__ARMCC_VERSION)` 的作用是什么？
2. `data` 局部变量存储在哪里？函数返回后还存在吗？
3. 如果用 GCC 编译器，`fputc` 的定义会被编译吗？需要什么替代方案？

??? note "参考解析"

    1. **存储段**：`.text`（Flash）。条件编译确保仅在 Keil ARM Compiler 下编译此 `fputc` 重定向，因为不同编译器的 printf 重定向机制不同。
    
    2. **局部变量**：`data` 存储在**栈区**（1B RAM，临时），函数返回后栈空间释放，变量不存在。`ch` 参数也通过寄存器传递，不占额外 RAM。
    
    3. **GCC 替代**：GCC 使用 `_write()` 或 `_write_r()` 作为底层输出接口（newlib），`fputc` 重定向不生效。需要实现 `int _write(int fd, char *ptr, int len)` 来重定向。

---

## Part B：纯实操题

> 无参考答案，给出 BSP 源码位置供自行验证

---

### B-1：直接用 BSP 源码回答

---

#### B01 — port_gpio.c 全量存储段标注

**参考位置**：`BSP/Interface/port_gpio.c`

请为 `port_gpio.c` 中所有文件级变量标注存储段和资源占用：

```c
/* 请在下方填写 */
static const port_gpio_map_t gpio_mapping[] = { ... };  /* 存储段: ___  Flash: ___B  RAM: ___B */
static port_exti_callback_t exti_callbacks[PORT_GPIO_MAX] = {NULL};  /* 存储段: ___  Flash: ___B  RAM: ___B */
```

---

#### B02 — port_spi.c 上下文结构体资源估算

**参考位置**：`BSP/Interface/port_spi.c` 第 11-18 行

```c
typedef struct
{
    port_async_cb_t callback;
    void            *user_ctx;
    volatile bool   is_busy;
} port_spi_context_t;

static port_spi_context_t s_spi_contexts[PORT_SPI_MAX];
```

请回答：

1. `port_spi_context_t` 的 `sizeof` 是多少？
2. `s_spi_contexts` 在哪个段？总 RAM 占用多少？
3. `is_busy` 在 DMA 中断中被置为 `false`，在 `port_spi_wait_complete` 中被轮询——去掉 `volatile` 会怎样？

---

#### B03 — dev_key.c 硬件配置表分析

**参考位置**：`BSP/Driver/dev_key.c` 第 10-22 行

```c
static Button btn[DEV_KEY_MAX];

typedef struct {
    port_gpio_id_t pin_id;
    uint8_t        active_level;
} dev_key_map_t;

static const dev_key_map_t key_mapping[] = {
    [DEV_KEY1] = { PORT_GPIO_KEY1, 1 },
    [DEV_KEY2] = { PORT_GPIO_KEY2, 1 },
    [DEV_KEY3] = { PORT_GPIO_KEY3, 1 }
};
```

请回答：

1. `btn` 和 `key_mapping` 分别在哪个段？
2. `Button` 是 MultiButton 库的结构体（约 20-30B），估算 `btn` 的 RAM 占用
3. `key_mapping` 为什么用 `const`？去掉 `const` 后 Flash/RAM 如何变化？

---

#### B04 — bsp_shell.c 缓冲区存储分析

**参考位置**：`BSP/Board/bsp_shell.c` 第 12-13 行

```c
Shell shell;
static char shell_buffer[512];
```

请回答：

1. `shell` 和 `shell_buffer` 分别在哪个段？
2. `Shell` 结构体通常包含函数指针、缓冲区指针等（约 40-60B），估算总 RAM 占用
3. `shell` 没有 `static`，其他文件能直接访问吗？这有什么利弊？

---

#### B05 — port_i2c.c 映射表综合分析

**参考位置**：`BSP/Interface/port_i2c.c` 第 28-55 行

```c
static I2C_HandleTypeDef *hw_mapping[] = { ... };
static const port_i2c_hw_pin_t hw_pins[] = { ... };
static soft_i2c_t sw_mapping[] = { ... };
```

请回答：

1. 三个映射表分别在哪个段？为什么 `hw_pins` 是 `const` 而 `sw_mapping` 不是？
2. `soft_i2c_t` 含两个 `port_gpio_id_t`（enum）和一个 `uint32_t`，估算 `sw_mapping` 的 RAM 占用
3. `hw_mapping` 存储的是指针，指针指向的对象（如 `hi2c1`）在哪个段？

---

#### B06 — dev_w25q.c 宏定义与局部变量

**参考位置**：`BSP/Driver/dev_w25q.c` 第 10-20 行 + 第 150-160 行

```c
#define W25Q_SPI_BUS PORT_SPI_2
#define W25Q_CS_PIN  PORT_GPIO_W25Q_CS
#define W25X_WRITE_ENABLE    0x06
#define W25X_READ_DATA       0x03

/* ... */

static bsp_status_t w25q_page_program(uint32_t addr, const uint8_t *buf, uint32_t size)
{
    uint8_t cmd[4];
    /* ... */
}
```

请回答：

1. 所有 `#define` 宏占多少 Flash/RAM？
2. `cmd[4]` 局部数组存储在哪里？函数返回后还存在吗？
3. 如果把 `cmd[4]` 改为 `static uint8_t cmd[4]`，存储段和线程安全性如何变化？

---

#### B07 — port_uart.c 全量资源审计

**参考位置**：`BSP/Interface/port_uart.c`

请统计 `port_uart.c` 中所有文件级变量，填写下表：

| 变量 | 类型 | 存储段 | Flash | RAM |
|------|------|--------|-------|-----|
| `s_uart_map[]` | `static UART_HandleTypeDef*[]` | ___ | ___ | ___ |
| `s_ctx[]` | `static uart_context_t[]` | ___ | ___ | ___ |

并回答：`PORT_UART_MAX = 1` 时，`s_ctx` 的 RAM 占用是多少？

---

#### B08 — dev_st7789.c static 变量群分析

**参考位置**：`BSP/Driver/dev_st7789.c` 第 66-77 行

```c
static uint16_t s_lcd_width = ST7789_CFG_DEFAULT_WIDTH;
static uint16_t s_lcd_height = ST7789_CFG_DEFAULT_HEIGHT;
static uint8_t  s_lcd_rotation = 0;
static lcd_dma_ctx_t s_lcd_dma_ctx = {NULL, NULL};
```

请回答：

1. 4 个变量分别在哪个段？
2. `lcd_dma_ctx_t` 含一个函数指针和一个 `void*`（共 8B），估算 4 个变量总 Flash + RAM 占用
3. 屏幕旋转时修改 `s_lcd_rotation = 1`，这个值存在 Flash 还是 RAM？掉电能保持吗？

---

#### B09 — port_dwt.c 初始化状态变量

**参考位置**：`BSP/Interface/port_dwt.c` 第 11 行

```c
static uint8_t dwt_status = 0;
```

以及第 48 行

```c
uint32_t cycles = (SystemCoreClock / 1000000u) * us;
uint32_t start  = DWT->CYCCNT;
```

请回答：

1. `dwt_status` 在哪个段？运行时被赋值为 1 或 2 后，新值存在哪里？
2. `cycles` 和 `start` 在哪个段？`DWT->CYCCNT` 读取的是什么地址空间？
3. `DWT->CYCCNT` 是硬件寄存器，它的地址属于什么区域？与 `.data/.bss` 有什么区别？

---

#### B10 — bsp_uart.c 缓冲区与回调函数

**参考位置**：`BSP/Board/bsp_uart.c`

```c
static uint8_t s_rx_dma_buf[RX_DMA_BUF_SIZE];
static uint8_t s_rx_rb_buf[RX_RB_BUF_SIZE];
static uint8_t s_tx_rb_buf[TX_RB_BUF_SIZE];
static lwrb_t  s_rx_rb;
static lwrb_t  s_tx_rb;

static void uart_rx_data_cb(port_uart_id_t uart, uint16_t len, void *user_ctx);
static void uart_error_cb(uint8_t bus_id, bsp_status_t result, void *user_ctx);
```

请回答：

1. 5 个变量分别在哪个段？总 RAM 占用多少？
2. 两个 `static` 回调函数在哪个段？为什么用 `static`？
3. `port_uart_config_t cfg = { ... }` 是 `bsp_uart_init` 中的局部结构体，它在哪个段？

---

### B-2：基于 BSP 创新需求出题

---

#### B11 — 新增 SPI 传感器驱动

**场景**：需要在 BSP 中新增一个 SPI 接口的 IMU 传感器驱动（如 MPU6500），请设计变量声明。

要求：

1. 定义芯片命令宏（至少 5 个寄存器地址）
2. 定义 `static const` 设备配置映射表
3. 定义模块私有上下文结构体（含 SPI 通道 ID、CS 引脚、busy 标志）
4. 定义全局设备实例

请在下方代码框中编写，并标注每个元素的存储段：

```c
/* 请在下方编写你的实现 */



```

---

#### B12 — 新增 ADC 采样模块

**场景**：需要新增一个 ADC 多通道采样模块，支持 DMA 循环采集 4 路模拟信号。

要求：

1. 定义采样通道枚举
2. 定义 `static` DMA 缓冲区和结果数组
3. 定义 `volatile` 采样完成标志
4. 定义 `static` 通道映射表（ADC 通道号 → 逻辑 ID）

```c
/* 请在下方编写你的实现 */



```

---

#### B13 — 新增 EEPROM 参数存储模块

**场景**：基于已有的 `dev_w25q.c` 和 `port_i2c.c`，设计一个参数存储模块，将运行参数保存到 AT24C02（I2C EEPROM）。

要求：

1. 定义参数结构体（含校验和、版本号、业务参数）
2. 定义 `static` 参数缓存（RAM 中的影子副本）
3. 定义 `const` 默认参数模板
4. 定义读/写/校验函数声明

```c
/* 请在下方编写你的实现 */



```

---

#### B14 — 新增看门狗喂狗模块

**场景**：需要新增独立看门狗（IWDG）管理模块，支持多任务注册喂狗。

要求：

1. 定义喂狗任务注册结构体（含任务名、超时阈值、最后喂狗时间戳）
2. 定义 `static` 注册表数组（最多 8 个任务）
3. 定义 `volatile` 看门狗触发标志
4. 考虑：注册表是否需要 `SECTION_NOINIT`？为什么？

```c
/* 请在下方编写你的实现 */



```

---

#### B15 — 新增 RTC 闹钟回调模块

**场景**：需要基于 HAL RTC 实现闹钟功能，支持注册多个闹钟回调。

要求：

1. 定义闹钟时间结构体（时、分、秒、星期掩码）
2. 定义回调函数指针类型
3. 定义 `static` 闹钟注册表（含时间 + 回调）
4. 定义 `volatile` 闹钟触发标志（RTC 中断中置位，主循环中检测）

```c
/* 请在下方编写你的实现 */



```

---

#### B16 — 优化 port_uart.c 结构体布局

**场景**：A11 题分析了 `uart_context_t` 的 padding 浪费，请重新排列成员以最小化 `sizeof`。

要求：

1. 将所有 `bool` 和 `uint8_t` 成员集中排列
2. 将所有指针和 `uint32_t` 成员集中排列
3. 计算优化后的 `sizeof` 和节省的字节数

```c
/* 请在下方编写优化后的结构体定义 */



```

---

#### B17 — 新增 CAN 通信模块

**场景**：需要新增 CAN 总线通信模块，支持多帧发送队列和接收过滤。

要求：

1. 定义 CAN 帧 结构体（ID、DLC、数据、扩展帧标志）
2. 定义 `static` 发送队列（环形缓冲区）
3. 定义 `volatile` 接收完成标志
4. 定义 `static const` 波特率配置映射表
5. 定义接收回调函数指针类型

```c
/* 请在下方编写你的实现 */



```

---

#### B18 — 新增电源管理模块

**场景**：需要新增低功耗管理模块，支持多种休眠模式和唤醒源配置。

要求：

1. 定义休眠模式枚举（运行、睡眠、停机、待机）
2. 定义唤醒源枚举（RTC、外部中断、串口）
3. 定义 `static` 当前模式变量
4. 定义 `static const` 模式配置表（每种模式的唤醒源掩码、恢复函数指针）
5. 定义 `volatile` 唤醒原因标志

```c
/* 请在下方编写你的实现 */



```

---

#### B19 — 重构 dev_led.c 为多实例驱动

**场景**：当前 `dev_led.c` 只支持核心板一颗 LED，需要重构为支持多颗 LED（含 PCA9555 扩展的 LED）。

要求：

1. 定义 LED 驱动接口结构体（含 `init`、`set`、`toggle` 函数指针）
2. 定义 `static const` LED 实例注册表
3. 区分 GPIO 直驱 LED 和 PCA9555 扩展 LED 的实现
4. 所有实例的函数指针表应存储在哪个段？

```c
/* 请在下方编写你的实现 */



```

---

#### B20 — BSP 全局资源审计

**场景**：请对整个 BSP 的所有文件级变量进行一次完整的存储段审计。

要求：填写下表（仅统计 `.c` 文件中的文件级变量，不含 HAL 库变量）

| 文件 | 变量名 | 存储段 | Flash (B) | RAM (B) |
|------|--------|--------|-----------|---------|
| port_gpio.c | gpio_mapping[] | .rodata | 104 | 0 |
| port_gpio.c | exti_callbacks[] | .bss | 0 | ___ |
| port_uart.c | s_uart_map[] | ___ | ___ | ___ |
| port_uart.c | s_ctx[] | ___ | ___ | ___ |
| port_spi.c | s_spi_contexts[] | ___ | ___ | ___ |
| port_spi.c | hw_mapping[] | ___ | ___ | ___ |
| port_pwm.c | pwm_mapping[] | ___ | ___ | ___ |
| port_i2c.c | hw_mapping[] | ___ | ___ | ___ |
| port_i2c.c | hw_pins[] | ___ | ___ | ___ |
| port_i2c.c | sw_mapping[] | ___ | ___ | ___ |
| port_dwt.c | dwt_status | ___ | ___ | ___ |
| bsp_uart.c | s_rx_dma_buf[] | ___ | ___ | ___ |
| bsp_uart.c | s_rx_rb_buf[] | ___ | ___ | ___ |
| bsp_uart.c | s_tx_rb_buf[] | ___ | ___ | ___ |
| bsp_uart.c | s_rx_rb | ___ | ___ | ___ |
| bsp_uart.c | s_tx_rb | ___ | ___ | ___ |
| bsp_shell.c | shell | ___ | ___ | ___ |
| bsp_shell.c | shell_buffer[] | ___ | ___ | ___ |
| dev_st7789.c | s_lcd_width | ___ | ___ | ___ |
| dev_st7789.c | s_lcd_height | ___ | ___ | ___ |
| dev_st7789.c | s_lcd_rotation | ___ | ___ | ___ |
| dev_st7789.c | s_lcd_dma_ctx | ___ | ___ | ___ |
| dev_ws2812.c | DEV_WS2812_COLOR_* (5个) | ___ | ___ | ___ |
| dev_ws2812.c | s_bit_0_ccr | ___ | ___ | ___ |
| dev_ws2812.c | s_bit_1_ccr | ___ | ___ | ___ |
| dev_ws2812.c | s_dma_buffer[] | ___ | ___ | ___ |
| dev_ws2812.c | s_global_brightness | ___ | ___ | ___ |
| dev_buzzer.c | s_buzzer_freq | ___ | ___ | ___ |
| dev_buzzer.c | s_buzzer_vol | ___ | ___ | ___ |
| dev_ft6336.c | g_touch_interrupt_fired | ___ | ___ | ___ |
| dev_ft6336.c | touch_dev | ___ | ___ | ___ |
| dev_led.c | led_mapping[] | ___ | ___ | ___ |
| dev_key.c | btn[] | ___ | ___ | ___ |
| dev_key.c | key_mapping[] | ___ | ___ | ___ |
| bsp_backlight.c | s_current_brightness | ___ | ___ | ___ |
| bsp_board.c | (无文件级变量) | — | — | — |

**最终汇总**：

| 存储段 | 总 Flash (B) | 总 RAM (B) |
|--------|-------------|------------|
| .rodata | ___ | 0 |
| .data | ___ | ___ |
| .bss | 0 | ___ |
| **合计** | ___ | ___ |