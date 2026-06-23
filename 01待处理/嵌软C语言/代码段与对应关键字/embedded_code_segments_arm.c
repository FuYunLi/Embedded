/**
 * @file    embedded_code_segments_arm.c
 * @brief   嵌入式开发全代码类型与存储段映射参考源码（32位 ARM Cortex-M 版）
 * @details 本文件以 ARM Cortex-M 体系为主视角，使用 C99/C11 标准与 <stdint.h>
 * 类型， 覆盖嵌入式开发中所有代码类型，用 /**/
注释标注每个元素所属的存储段、
*关键字作用及资源占用估算。适用于 GCC ARM / Keil MDK -
    ARM / IAR 环境。 * * 存储段速查： *.text     段  → Flash（函数机器码 + 内联常量 +
    rodata） *.rodata 段 → Flash（const 全局变量、字符串字面量） *.data 段   → Flash（存初始值） +
    RAM（运行时副本） *.bss                                             段    → RAM（零初始化 /
        未初始化的全局静态变量） *stack 区   → RAM（局部变量、函数调用帧，向下生长） *heap 区    → RAM（malloc /
        free 动态分配，向上生长） **资源估算基准：ARM                                      Cortex -
    M4，RAM 64KB，Flash 256KB * 指针 4 字节，int 4 字节，对齐按 4 字节 * /

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ========================================================================
 * 第一部分：类型定义工具（不占存储空间）
 * ======================================================================== */

/* --- #define 宏定义 ---
 * 存储段：无（预处理阶段纯文本替换，不生成任何机器码）
 * Flash 占用：0 字节
 * RAM 占用：0 字节
 */
#define PI                3.14159265358979323846           /* 浮点常量替换，0B Flash + 0B RAM */
#define MAX_BUF_SIZE      64                               /* 整型常量替换，0B Flash + 0B RAM */
#define APP_VERSION       "1.0.0"                          /* 字符串常量替换，0B Flash + 0B RAM */
#define SET_BIT(reg, bit) ((reg) |= (1UL << (bit)))        /* 宏函数：置位，0B Flash + 0B RAM */
#define CLR_BIT(reg, bit) ((reg) &= ~(1UL << (bit)))       /* 宏函数：清位，0B Flash + 0B RAM */
#define GET_BIT(reg, bit) (((reg) >> (bit)) & 1UL)         /* 宏函数：读位，0B Flash + 0B RAM */
#define ARRAY_SIZE(arr)   (sizeof(arr) / sizeof((arr)[0])) /* 宏函数：数组元素数，0B Flash + 0B RAM */
#define MIN(a, b)         (((a) < (b)) ? (a) : (b))        /* 宏函数：最小值，0B Flash + 0B RAM */
#define MAX(a, b)         (((a) > (b)) ? (a) : (b))        /* 宏函数：最大值，0B Flash + 0B RAM */

/* --- 编译器属性宏（GCC ARM / Clang）---
 * 用于将变量强制分配到指定段，替代 C51 的 code/data/xdata 关键字
 */
#define SECTION_RODATA __attribute__((section(".rodata"))) /* 强制放入只读段（Flash） */
#define SECTION_NOINIT __attribute__((section(".noinit"))) /* 不初始化段（RAM，上电值不确定） */
#define ALIGNED(n)     __attribute__((aligned(n)))         /* 对齐属性 */
#define PACKED         __attribute__((packed))             /* 紧凑对齐（取消填充） */
#define WEAK           __attribute__((weak))               /* 弱符号 */
#define USED           __attribute__((used))               /* 强制保留（防止优化删除） */

        /* --- enum 枚举类型 ---
         * 存储段：枚举常量不占空间；枚举变量占 RAM
         * 枚举常量本身：0B Flash + 0B RAM
         * 枚举变量：4 字节 RAM（ARM 中 enum 变量为 int，4B）
         * C23 支持指定底层类型以节省空间：enum WeekDay : uint8_t
         */
        enum WeekDay {
            WEEK_MON = 1, /* 枚举常量，值=1，0B Flash + 0B RAM */
            WEEK_TUE,     /* 枚举常量，值=2，0B Flash + 0B RAM */
            WEEK_WED,     /* 枚举常量，值=3，0B Flash + 0B RAM */
            WEEK_THU,     /* 枚举常量，值=4，0B Flash + 0B RAM */
            WEEK_FRI,     /* 枚举常量，值=5，0B Flash + 0B RAM */
            WEEK_SAT,     /* 枚举常量，值=6，0B Flash + 0B RAM */
            WEEK_SUN      /* 枚举常量，值=7，0B Flash + 0B RAM */
        };

enum DeviceMode
{
    MODE_IDLE = 0,  /* 枚举常量，值=0，0B Flash + 0B RAM */
    MODE_RUN,       /* 枚举常量，值=1，0B Flash + 0B RAM */
    MODE_STOP,      /* 枚举常量，值=2，0B Flash + 0B RAM */
    MODE_ERROR = 99 /* 枚举常量，值=99，0B Flash + 0B RAM */
};

/* ========================================================================
 * 第二部分：全局变量（.data 段 / .bss 段）
 * ======================================================================== */

/* --- 普通全局变量（已初始化，非零初值）---
 * 存储段：.data
 * Flash 占用：每个变量占其字节数（存初始值）
 * RAM 占用：每个变量占其字节数（运行时副本）
 */
uint8_t  g_count       = 100;        /* .data段，1B Flash + 1B RAM（实际对齐后 RAM 可能占 4B） */
uint16_t g_timer_ms    = 1000;       /* .data段，2B Flash + 2B RAM（对齐后 RAM 可能占 4B） */
uint32_t g_timestamp   = 0x12345678; /* .data段，4B Flash + 4B RAM */
int8_t   g_temperature = -5;         /* .data段，1B Flash + 1B RAM（对齐后 RAM 可能占 4B） */
float    g_kp          = 2.5f;       /* .data段，4B Flash + 4B RAM */
float    g_ki          = 0.1f;       /* .data段，4B Flash + 4B RAM */

/* --- 普通全局变量（零初始化/未初始化）---
 * 存储段：.bss
 * Flash 占用：0 字节（启动代码自动清零，无需存初始值）
 * RAM 占用：每个变量占其字节数
 */
uint8_t  g_rx_index;     /* .bss段，0B Flash + 1B RAM（对齐后可能占 4B） */
uint16_t g_adc_value;    /* .bss段，0B Flash + 2B RAM（对齐后可能占 4B） */
uint32_t g_total_count;  /* .bss段，0B Flash + 4B RAM */
uint8_t  g_error_flag;   /* .bss段，0B Flash + 1B RAM（对齐后可能占 4B） */
bool     g_system_ready; /* .bss段，0B Flash + 1B RAM（C99 bool，对齐后可能占 4B） */

/* --- const 修饰的全局变量（ARM 中存入 .rodata，即 Flash）---
 * 存储段：.rodata（ARM 中 const 全局变量默认存入 Flash 只读段）
 * Flash 占用：每个变量占其字节数
 * RAM 占用：0 字节
 * 只读性质：物理只读（Flash 硬件不可写）+ 语法只读（编译器禁止写入）
 *
 * 与 C51 的关键区别：
 *   C51 中 const 全局变量仍在 RAM（rwdata段），需 code 关键字才能放 Flash
 *   ARM 中 const 全局变量默认在 Flash（.rodata段），无需额外修饰符
 */
const uint8_t  g_max_retry  = 3;          /* .rodata段，1B Flash + 0B RAM，物理+语法只读 */
const uint16_t g_baud_rate  = 115200;     /* .rodata段，2B Flash + 0B RAM，物理+语法只读 */
const float    g_temp_upper = 35.0f;      /* .rodata段，4B Flash + 0B RAM，物理+语法只读 */
const float    g_temp_lower = 10.0f;      /* .rodata段，4B Flash + 0B RAM，物理+语法只读 */
const uint32_t g_device_id  = 0xDEADBEEF; /* .rodata段，4B Flash + 0B RAM，物理+语法只读 */

/* --- SECTION_RODATA 显式指定只读段 ---
 * 效果与 const 全局变量相同，但语义更明确
 * 适用于需要强制确保数据在 Flash 中的场景
 */
SECTION_RODATA const uint8_t g_hw_revision = 0x0A; /* .rodata段，1B Flash + 0B RAM，显式指定 */

/* --- volatile 修饰的全局变量 ---
 * 存储段：.data 或 .bss
 * volatile 不改变存储位置，仅告诉编译器不要优化对该变量的访问
 * 典型用途：硬件寄存器映射、中断与主循环共享的标志位
 */
volatile uint8_t  g_uart_rx_done; /* .bss段，0B Flash + 1B RAM，禁止优化读取 */
volatile uint32_t g_systick_ms;   /* .bss段，0B Flash + 4B RAM，禁止优化读取 */
volatile bool     g_timer_flag;   /* .bss段，0B Flash + 1B RAM，中断置位主循环检测 */

/* --- static 修饰的全局变量 ---
 * 存储段：.data（非零初值）或 .bss（零初值/未初始化）
 * Flash 占用：同普通全局变量
 * RAM 占用：同普通全局变量
 * 作用域变化：仅当前 .c 文件可见，其他文件不可通过 extern 访问
 */
static uint8_t  s_module_state = 1;   /* .data段，1B Flash + 1B RAM，文件内可见 */
static uint16_t s_interval_ms  = 500; /* .data段，2B Flash + 2B RAM，文件内可见 */
static uint32_t s_init_done;          /* .bss段，0B Flash + 4B RAM，文件内可见 */

/* --- static + const 组合 ---
 * 存储段：.rodata（const 全局变量在 ARM 中默认存 Flash）
 * 作用域：仅当前文件 + 物理只读
 */
static const uint8_t s_lookup_table[4] = { 0x01, 0x02, 0x04,
                                           0x08 }; /* .rodata段，4B Flash + 0B RAM，文件内可见+物理只读 */

/* --- SECTION_NOINIT 不初始化段 ---
 * 存储段：.noinit（RAM 中不被启动代码清零的区域）
 * Flash 占用：0 字节
 * RAM 占用：变量字节数
 * 用途：看门狗复位后需要保留的数据（上电值不确定，软复位值保留）
 */
SECTION_NOINIT uint32_t g_reset_reason; /* .noinit段，0B Flash + 4B RAM，上电值不确定 */

/* ========================================================================
 * 第三部分：全局数组与结构体
 * ======================================================================== */

/* --- 全局数组（已初始化，非零初值）---
 * 存储段：.data
 * Flash 占用：数组总字节数
 * RAM 占用：数组总字节数
 */
uint8_t g_tx_buffer[MAX_BUF_SIZE] = { 0xAA, 0xBB }; /* .data段，64B Flash + 64B RAM */

/* --- 全局数组（零初始化）---
 * 存储段：.bss
 * Flash 占用：0 字节
 * RAM 占用：数组总字节数
 */
uint8_t g_rx_buffer[MAX_BUF_SIZE] = { 0 }; /* .bss段，0B Flash + 64B RAM */

/* --- const 全局数组（查表数据，ARM 中存 Flash）---
 * 存储段：.rodata
 * Flash 占用：数组总字节数
 * RAM 占用：0 字节
 * ARM 中 const 全局数组自动存入 Flash，等效于 C51 的 code 修饰
 */
const uint16_t g_crc_table[16] = { 0x0000, 0x1021, 0x2042, 0x3063, /* .rodata段，32B Flash + 0B RAM */
                                   0x4084, 0x50A5, 0x60C6, 0x70E7, 0x8108, 0x9129,
                                   0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF };

const uint8_t g_days_per_month[12] = { 31, 28, 31, 30, 31, 30, /* .rodata段，12B Flash + 0B RAM */
                                       31, 31, 30, 31, 30, 31 };

const uint8_t g_led_font[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, /* .rodata段，10B Flash + 0B RAM */
                                 0x6D, 0x7D, 0x07, 0x7F, 0x6F };

/* --- 结构体类型定义（typedef）---
 * 存储段：类型定义本身不占空间
 * ARM 中结构体成员按 4 字节对齐（除非指定 PACKED）
 */
typedef struct
{
    uint8_t  id;         /* 成员：1B + 1B padding */
    uint8_t  cmd;        /* 成员：1B + 1B padding */
    uint16_t length;     /* 成员：2B */
    uint8_t  payload[8]; /* 成员：8B */
    uint8_t  checksum;   /* 成员：1B + 3B padding */
} Packet_t;              /* 结构体总大小：16B（含对齐填充），0B Flash + 0B RAM */

/* --- PACKED 紧凑结构体（取消对齐填充）---
 * 适用于通信协议帧，与线上格式一一对应
 * 代价：非对齐访问可能降低性能（Cortex-M3/M4 支持，M0 可能异常）
 */
typedef struct PACKED
{
    uint8_t  id;         /* 成员：1B */
    uint8_t  cmd;        /* 成员：1B */
    uint16_t length;     /* 成员：2B */
    uint8_t  payload[8]; /* 成员：8B */
    uint8_t  checksum;   /* 成员：1B */
} PacketRaw_t;           /* 结构体总大小：13B（无填充），0B Flash + 0B RAM */

/* --- 全局结构体变量（已初始化）---
 * 存储段：.data
 */
Packet_t g_default_packet = {
    0x01,
    0x10,
    0x0008, /* .data段，16B Flash + 16B RAM（含对齐填充） */
    { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 },
    0x00
};

/* --- 全局结构体变量（零初始化）---
 * 存储段：.bss
 */
Packet_t g_rx_packet; /* .bss段，0B Flash + 16B RAM（含对齐填充） */

/* --- 枚举变量 ---
 * 存储段：.data 或 .bss
 * ARM 中 enum 变量为 int 类型，占 4 字节
 */
enum DeviceMode g_current_mode = MODE_IDLE; /* .data段，4B Flash + 4B RAM */
enum WeekDay    g_alarm_day;                /* .bss段，0B Flash + 4B RAM */

/* ========================================================================
 * 第四部分：硬件寄存器映射（volatile + 结构体指针）
 * ======================================================================== */

/* --- 寄存器映射结构体 ---
 * 存储段：类型定义不占空间
 * 通过指针访问固定地址的硬件寄存器
 * volatile 确保每次访问都真正读写硬件
 */
typedef struct
{
    volatile uint32_t MODER;   /* 端口模式寄存器，偏移 0x00 */
    volatile uint32_t OTYPER;  /* 输出类型寄存器，偏移 0x04 */
    volatile uint32_t OSPEEDR; /* 输出速度寄存器，偏移 0x08 */
    volatile uint32_t PUPDR;   /* 上拉下拉寄存器，偏移 0x0C */
    volatile uint32_t IDR;     /* 输入数据寄存器，偏移 0x10 */
    volatile uint32_t ODR;     /* 输出数据寄存器，偏移 0x14 */
    volatile uint32_t BSRR;    /* 置位/复位寄存器，偏移 0x18 */
    volatile uint32_t LCKR;    /* 配置锁定寄存器，偏移 0x1C */
    volatile uint32_t AFRL;    /* 复用功能低位寄存器，偏移 0x20 */
    volatile uint32_t AFRH;    /* 复用功能高位寄存器，偏移 0x24 */
} GPIO_TypeDef;                /* 结构体总大小：40B，0B Flash + 0B RAM（仅类型定义） */

/* --- 外设基地址宏与指针强制转换 ---
 * 存储段：不占空间（宏 + 编译期常量折叠）
 * 将固定地址映射为结构体指针，实现寄存器按名访问
 */
#define GPIOA_BASE (0x40020000UL)
#define GPIOB_BASE (0x40020400UL)
#define GPIOC_BASE (0x40020800UL)

#define GPIOA ((GPIO_TypeDef *)GPIOA_BASE) /* 0B Flash + 0B RAM */
#define GPIOB ((GPIO_TypeDef *)GPIOB_BASE) /* 0B Flash + 0B RAM */
#define GPIOC ((GPIO_TypeDef *)GPIOC_BASE) /* 0B Flash + 0B RAM */

/* ========================================================================
 * 第五部分：函数（.text 段）
 * ======================================================================== */

/* --- 普通全局函数 ---
 * 存储段：.text（函数体的机器码存储在 Flash 中）
 * Flash 占用：函数体机器码大小（视指令数量而定）
 * RAM 占用：0 字节（函数本身不占 RAM，调用时临时占用栈）
 * 作用域：全项目可见（其他文件声明后可调用）
 */

/**
 * @brief   获取指定月份的天数
 * @param   month 月份（1~12）
 * @return  该月天数，0 表示月份非法
 * 存储段：.text
 * Flash 估算：约 20~30B（Thumb-2 指令密度高）
 * RAM 估算：调用时栈占用约 8B（r0 + lr + 对齐）
 */
uint8_t get_days_in_month(uint8_t month)
{
    if (month < 1 || month > 12)
    {
        return 0;
    }
    return g_days_per_month[month - 1];
}

/**
 * @brief   判断温度是否在正常范围
 * @param   temp 当前温度值
 * @return  0=低于下限, 1=正常, 2=高于上限
 * 存储段：.text
 * Flash 估算：约 30~40B（VFP 硬件浮点指令）
 * RAM 估算：调用时栈占用约 8B（s0/s1 + lr）
 */
uint8_t check_temperature(float temp)
{
    if (temp > g_temp_upper)
    {
        return 2;
    }
    if (temp < g_temp_lower)
    {
        return 0;
    }
    return 1;
}

/**
 * @brief   计算 CRC16-CCITT 校验值
 * @param   data 数据指针
 * @param   len  数据长度
 * @return  CRC16 值
 * 存储段：.text
 * Flash 估算：约 50~70B
 * RAM 估算：调用时栈占用约 16B（r0~r3 + 局部变量 + lr）
 */
uint16_t calculate_crc(const uint8_t *data, uint8_t len)
{
    uint16_t crc = 0x0000;
    uint8_t  i;

    for (i = 0; i < len; i++)
    {
        crc = (uint16_t)((crc << 4) ^ g_crc_table[(crc >> 12) ^ (data[i] >> 4)]);
        crc = (uint16_t)((crc << 4) ^ g_crc_table[(crc >> 12) ^ (data[i] & 0x0F)]);
    }
    return crc;
}

/**
 * @brief   字节序转换（大端转小端，32位）
 * @param   val 大端存储的 32 位值
 * @return  小端存储的 32 位值
 * 存储段：.text
 * Flash 估算：约 10~16B（REV 指令一条搞定）
 * RAM 估算：调用时栈占用约 8B
 */
uint32_t swap_endian_u32(uint32_t val)
{
    return __REV(val); /* ARM 专用字节反转指令，单周期 */
}

/**
 * @brief   字节序转换（大端转小端，16位）
 * @param   val 大端存储的 16 位值
 * @return  小端存储的 16 位值
 * 存储段：.text
 * Flash 估算：约 10~16B（REV16 指令一条搞定）
 * RAM 估算：调用时栈占用约 8B
 */
uint16_t swap_endian_u16(uint16_t val)
{
    return __REV16(val); /* ARM 专用半字反转指令，单周期 */
}

/* --- static 全局函数 ---
 * 存储段：.text（与普通函数相同，机器码在 Flash 中）
 * Flash 占用：同普通函数
 * RAM 占用：0 字节
 * 作用域：仅当前 .c 文件可见
 */

/**
 * @brief   微秒级延时（内部使用，SysTick 实现）
 * @param   us 延时微秒数
 * 存储段：.text
 * Flash 估算：约 20~30B
 * RAM 估算：调用时栈占用约 8B
 */
static void delay_us(uint32_t us)
{
    uint32_t start = g_systick_ms;
    while ((g_systick_ms - start) < (us / 1000))
    {
        /* 等待 SysTick 计数到达 */
    }
}

/* --- WEAK 弱函数 ---
 * 存储段：.text
 * 允许其他文件定义同名函数覆盖此默认实现
 * 典型用途：中断回调、默认空处理
 */

/**
 * @brief   串口接收完成回调（弱定义，可被用户覆盖）
 * 存储段：.text
 * Flash 估算：约 4~8B（仅一条 BX lr 返回指令）
 * RAM 估算：0B
 */
WEAK void uart_rx_callback(const uint8_t *data, uint16_t len)
{
    (void)data;
    (void)len;
    /* 默认空实现，用户可在其他文件中定义同名函数覆盖 */
}

/* --- inline / static inline 内联函数 ---
 * 存储段：调用处展开（无函数调用开销），若编译器决定不内联则存 .text
 * Flash 占用：每次调用处各占一份展开代码（可能增大 Flash）
 * RAM 占用：0 字节（省去栈帧分配）
 * 适用于：短小频繁调用的函数
 */

/**
 * @brief   限制值在指定范围内
 * @param   val 输入值
 * @param   min 最小值
 * @param   max 最大值
 * @return  限制后的值
 * 存储段：内联展开到调用处
 * Flash 估算：约 8~12B/调用点
 * RAM 估算：0B（无栈帧）
 */
static inline int32_t clamp(int32_t val, int32_t min, int32_t max)
{
    if (val < min)
        return min;
    if (val > max)
        return max;
    return val;
}

/* ========================================================================
 * 第六部分：局部变量与静态局部变量（栈区 / .data/.bss 段）
 * ======================================================================== */

/**
 * @brief   演示局部变量与静态局部变量的区别
 * 存储段：.text（函数体）
 * Flash 估算：约 20~30B
 * RAM 估算：调用时栈占用约 8B（n + 对齐 + lr）
 *
 * 局部变量 n：
 *   存储段：栈（Stack）
 *   Flash 占用：0B
 *   RAM 占用：1B（临时，函数返回后释放）
 *   每次调用重新初始化为 0
 *
 * 静态局部变量 s_counter：
 *   存储段：.bss（零初始化的静态变量）
 *   Flash 占用：0B
 *   RAM 占用：4B（持久，ARM 中 int 为 4B）
 *   仅首次初始化为 0，值在多次调用间保持
 */
void counter_demo(void)
{
    uint8_t n = 0; /* 栈区，1B RAM（临时），每次调用初值=0 */

    static uint32_t s_counter = 0; /* .bss段，4B RAM（持久），首次初始化后保持 */

    n++;
    s_counter++;
}

/**
 * @brief   数据处理函数（演示大数组避免放栈中）
 * 存储段：.text（函数体）
 *
 * 危险写法（不推荐）：
 *   uint8_t temp[256];  → 栈区，256B RAM（临时），可能栈溢出
 *
 * 安全写法（推荐）：
 *   static uint8_t temp[256]; → .bss段，256B RAM（持久），不占栈空间
 */
void process_data(void)
{
    /* 静态局部变量：大数组放全局数据区，避免栈溢出 */
    static uint8_t s_temp_buf[256]; /* .bss段，256B RAM（持久），0B Flash */

    /* 普通局部变量：小变量放栈中，函数返回自动释放 */
    uint32_t i;            /* 栈区，4B RAM（临时） */
    uint8_t  checksum = 0; /* 栈区，1B RAM（临时） */

    for (i = 0; i < 256; i++)
    {
        checksum += s_temp_buf[i];
    }
}

/* ========================================================================
 * 第七部分：指针与动态内存（栈区 / heap 区）
 * ======================================================================== */

/* --- 全局指针变量 ---
 * 存储段：.data 或 .bss（指针本身存储位置）
 * 指针指向的内存：取决于分配方式（栈/堆/全局区）
 */
uint8_t    *g_data_ptr;                  /* .bss段，4B RAM（指针本身），指向的内存位置未知 */
const char *g_version_str = APP_VERSION; /* .data段，4B Flash + 4B RAM，指向 .rodata 中的字符串 */

/* --- 字符串字面量 ---
 * 存储段：.rodata（字符串字面量存放在 Flash 只读段）
 * Flash 占用：字符串长度 + 1（'\0' 终止符）
 * RAM 占用：0 字节
 */
const char *g_error_msg = "System Error!"; /* .rodata段，13B Flash + 0B RAM（字符串本身） */
/* 注意：指针 g_error_msg 本身在 .data 段占 4B Flash + 4B RAM */

/**
 * @brief   演示指针与动态内存
 * 存储段：.text（函数体）
 * Flash 估算：约 40~60B
 * RAM 估算：调用时栈占用约 16B（指针变量 + 局部变量 + lr）
 *
 * 局部指针变量 ptr：
 *   存储段：栈（Stack）
 *   Flash 占用：0B
 *   RAM 占用：4B（临时，函数返回后释放）
 *
 * 动态分配的内存（malloc）：
 *   存储段：堆（Heap）
 *   Flash 占用：0B
 *   RAM 占用：64B（持久，直到 free 释放）
 */
void pointer_demo(void)
{
    uint8_t *ptr; /* 栈区，4B RAM（指针变量本身，临时） */
    uint32_t i;   /* 栈区，4B RAM（临时） */

    ptr = (uint8_t *)malloc(64); /* 堆区，64B RAM（动态分配，需手动释放） */
    if (ptr == NULL)
    {
        return; /* 分配失败处理 */
    }

    for (i = 0; i < 64; i++)
    {
        ptr[i] = (uint8_t)i; /* 使用堆内存 */
    }

    free(ptr); /* 释放堆内存，避免内存泄漏 */
}

/* ========================================================================
 * 第八部分：函数指针与回调（.text 段 + .data/.bss 段）
 * ======================================================================== */

/* --- 函数指针类型定义 ---
 * 存储段：类型定义不占空间
 */
typedef void (*callback_t)(uint8_t data); /* 函数指针类型，参数为 uint8_t，返回 void */

/* --- 全局函数指针变量 ---
 * 存储段：.data 或 .bss（指针本身存储位置）
 * 指向的函数：.text 段（Flash 中）
 */
callback_t g_uart_rx_callback = NULL; /* .bss段，4B RAM（指针本身），指向 .text 段中的函数 */

/* --- 回调注册函数 ---
 * 存储段：.text
 */
void register_uart_rx_callback(callback_t cb)
{
    g_uart_rx_callback = cb;
}

/* --- 回调触发函数（通常在中断中调用）---
 * 存储段：.text
 */
void trigger_uart_rx_callback(uint8_t data)
{
    if (g_uart_rx_callback != NULL)
    {
        g_uart_rx_callback(data);
    }
}

/* ========================================================================
 * 第九部分：中断服务程序（.text 段）
 * ======================================================================== */

/* --- 中断服务程序（ISR）---
 * 存储段：.text（函数体在 Flash 中）
 * Flash 占用：函数体机器码大小
 * RAM 占用：0 字节（调用时临时占用栈）
 * 特点：无参数、无返回值、不能被普通代码调用
 *
 * 注意：以下为示例，实际中断函数名需与启动文件中的向量表匹配
 */

/**
 * @brief   SysTick 中断服务程序（示例）
 * 存储段：.text
 * Flash 估算：约 10~20B
 * RAM 估算：调用时栈占用约 16B（寄存器保护 + lr）
 */
void SysTick_Handler(void)
{
    g_systick_ms++; /* 系统滴答计数器递增 */
}

/**
 * @brief   外部中断服务程序（示例）
 * 存储段：.text
 * Flash 估算：约 20~30B
 * RAM 估算：调用时栈占用约 16B
 */
void EXTI0_IRQHandler(void)
{
    if (g_uart_rx_callback != NULL)
    {
        g_uart_rx_callback(0x55); /* 触发回调 */
    }
    g_timer_flag = true; /* 设置标志位，通知主循环 */
}

/* ========================================================================
 * 第十部分：联合体与位域（.data/.bss 段）
 * ======================================================================== */

/* --- 联合体类型定义 ---
 * 存储段：类型定义不占空间
 * 所有成员共享同一块内存，大小等于最大成员
 */
typedef union
{
    uint32_t word;    /* 32位字访问 */
    uint16_t half[2]; /* 16位半字访问 */
    uint8_t  byte[4]; /* 8位字节访问 */
} DataUnion_t;        /* 联合体总大小：4B，0B Flash + 0B RAM */

/* --- 全局联合体变量 ---
 * 存储段：.bss（未初始化）
 */
DataUnion_t g_data_union; /* .bss段，0B Flash + 4B RAM */

/* --- 位域结构体 ---
 * 存储段：类型定义不占空间
 * 注意：位域的布局与编译器相关，跨平台需谨慎
 */
typedef struct
{
    uint32_t enable : 1;    /* 位 0：使能位 */
    uint32_t mode : 2;      /* 位 1~2：模式选择 */
    uint32_t speed : 3;     /* 位 3~5：速度等级 */
    uint32_t reserved : 26; /* 位 6~31：保留 */
} ConfigBits_t;             /* 结构体总大小：4B，0B Flash + 0B RAM */

/* --- 全局位域变量 ---
 * 存储段：.data（已初始化）
 */
ConfigBits_t g_config = { .enable = 1, .mode = 2, .speed = 5, .reserved = 0 }; /* .data段，4B Flash + 4B RAM */

/* ========================================================================
 * 第十一部分：编译器内置函数与优化提示
 * ======================================================================== */

/**
 * @brief   使用编译器内置函数优化
 * 存储段：.text
 * Flash 估算：约 5~10B（内置函数通常映射为单条指令）
 */
void builtin_demo(void)
{
    uint32_t val = 0x12345678;

    /* 字节序反转（ARM 内置函数） */
    uint32_t swapped = __REV(val); /* 单周期指令 */

    /* 计算前导零数量（Count Leading Zeros） */
    uint32_t lz = __CLZ(val); /* 单周期指令，用于快速计算对齐 */

    /* 饱和加法（防止溢出） */
    uint32_t result = __SSAT(val, 16); /* 有符号饱和到 16 位 */

    (void)swapped;
    (void)lz;
    (void)result;
}

/* ========================================================================
 * 第十二部分：主函数与初始化流程
 * ======================================================================== */

/**
 * @brief   系统初始化（由启动代码调用）
 * 存储段：.text
 * Flash 估算：约 30~50B
 * RAM 估算：调用时栈占用约 8B
 */
void SystemInit(void)
{
    /* 硬件初始化（通常由启动代码调用） */
    g_systick_ms   = 0;
    g_system_ready = false;
    g_current_mode = MODE_IDLE;
}

/**
 * @brief   主函数
 * 存储段：.text
 * Flash 估算：视具体逻辑而定
 * RAM 估算：调用时栈占用视局部变量而定
 */
int main(void)
{
    /* 局部变量：存储在栈区 */
    uint32_t loop_count = 0; /* 栈区，4B RAM（临时） */
    uint8_t  rx_byte;        /* 栈区，1B RAM（临时） */

    /* 系统初始化 */
    SystemInit();

    /* 主循环 */
    while (1)
    {
        loop_count++;

        /* 示例：检查温度 */
        if (check_temperature(25.0f) == 1)
        {
            g_error_flag = 0;
        }

        /* 示例：处理数据 */
        if (g_uart_rx_done)
        {
            rx_byte        = g_rx_buffer[0];
            g_uart_rx_done = false;
        }

        /* 简单延时 */
        delay_us(1000);
    }

    return 0; /* 永不到达（嵌入式系统通常无返回） */
}

/* ========================================================================
 * 附录：存储段总结
 * ========================================================================
 *
 * 1. .text 段（Flash）
 *    - 所有函数的机器码
 *    - 内联常量（如 switch-case 跳转表）
 *    - 字符串字面量（部分编译器）
 *
 * 2. .rodata 段（Flash）
 *    - const 全局变量（ARM 中默认）
 *    - const 全局数组
 *    - 字符串字面量（部分编译器）
 *
 * 3. .data 段（Flash + RAM）
 *    - 非零初值的全局变量
 *    - 非零初值的静态变量（含 static 局部变量）
 *    - Flash 存初始值，RAM 存运行时副本
 *
 * 4. .bss 段（RAM）
 *    - 零初值或未初始化的全局变量
 *    - 零初值或未初始化的静态变量（含 static 局部变量）
 *    - 启动代码自动清零，不占 Flash
 *
 * 5. stack 区（RAM）
 *    - 局部变量（非 static）
 *    - 函数参数
 *    - 函数返回地址
 *    - 寄存器保护
 *    - 向下生长
 *
 * 6. heap 区（RAM）
 *    - malloc 分配的内存
 *    - 向上生长
 *    - 需手动 free 释放
 *
 * 7. 关键字对存储段的影响
 *    - const：ARM 中全局变量存 .rodata（Flash），局部变量仍在栈
 *    - static：改变作用域，不改变存储段位置
 *    - volatile：不改变存储段，禁止编译器优化访问
 *    - __attribute__((section("name")))：强制指定存储段
 *
 * 8. ARM 与 C51 存储段差异
 *    - C51：code(data in Flash)、data(内部 RAM)、xdata(外部
 * RAM)、pdata(分页外部 RAM)
 *    - ARM：.text/.rodata(Flash)、.data/.bss(RAM)、stack/heap(RAM)
 *    - ARM 中 const 全局变量默认在 Flash，无需额外修饰符
 *    - ARM 中无 xdata/pdata 概念，统一通过地址映射访问
 */