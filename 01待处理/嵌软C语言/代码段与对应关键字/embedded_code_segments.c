/**
 * @file    embedded_code_segments.c
 * @brief   嵌入式开发全代码类型与存储段映射参考源码
 * @details 本文件以 C51/8051 体系为主视角，覆盖嵌入式开发中所有代码类型，
 *          用 /**/ 注释标注每个元素所属的存储段、关键字作用及资源占用估算。
 *          适用于 Keil C51 环境，32 位平台请参考段尾的移植说明。
 *
 * 存储段速查：
 *   code   段 → Flash（函数机器码）
 *   rodata 段 → Flash（const/code 修饰的只读数据）
 *   rwdata 段 → Flash（存初始值）+ RAM（运行时副本）
 *   bss    段 → RAM（零初始化/未初始化的全局静态变量）
 *   stack  区 → RAM（局部变量、函数调用帧）
 *   heap   区 → RAM（动态分配，C51 极少使用）
 *
 * 资源估算基准：C51/8051，RAM 256 字节，Flash 视型号而定
 */

/* ========================================================================
 * 第一部分：类型定义工具（不占存储空间）
 * ======================================================================== */

/* --- #define 宏定义 ---
 * 存储段：无（预处理阶段纯文本替换，不生成任何机器码）
 * Flash 占用：0 字节
 * RAM 占用：0 字节
 */
#define PI              3.1415926f      /* 浮点常量替换，0B Flash + 0B RAM */
#define MAX_BUF_SIZE    64              /* 整型常量替换，0B Flash + 0B RAM */
#define LED_PORT        P1              /* 端口映射替换，0B Flash + 0B RAM */
#define SET_BIT(reg, bit)      ((reg) |= (1 << (bit)))   /* 宏函数：置位，0B Flash + 0B RAM */
#define CLR_BIT(reg, bit)      ((reg) &= ~(1 << (bit)))  /* 宏函数：清位，0B Flash + 0B RAM */
#define GET_BIT(reg, bit)      (((reg) >> (bit)) & 1)    /* 宏函数：读位，0B Flash + 0B RAM */

/* --- typedef 类型别名 ---
 * 存储段：无（编译期类型别名，不生成存储分配）
 * Flash 占用：0 字节
 * RAM 占用：0 字节
 * 跨平台移植时只需修改此处即可适配不同 MCU 的字长
 */
typedef unsigned char      U8;    /* 8位无符号，0B Flash + 0B RAM */
typedef signed char        S8;    /* 8位有符号，0B Flash + 0B RAM */
typedef unsigned int       U16;   /* 16位无符号（C51=2字节，ARM=4字节），0B Flash + 0B RAM */
typedef signed int         S16;   /* 16位有符号，0B Flash + 0B RAM */
typedef unsigned long      U32;   /* 32位无符号，0B Flash + 0B RAM */
typedef signed long        S32;   /* 32位有符号，0B Flash + 0B RAM */
typedef float              F32;   /* 单精度浮点（4字节），0B Flash + 0B RAM */

/* --- enum 枚举类型 ---
 * 存储段：枚举常量不占空间；枚举变量占 RAM
 * 枚举常量本身：0B Flash + 0B RAM
 * 枚举变量：1~2 字节 RAM（C51 中 enum 变量通常为 1 字节）
 */
enum WeekDay
{
    WEEK_MON = 1,   /* 枚举常量，值=1，0B Flash + 0B RAM */
    WEEK_TUE,       /* 枚举常量，值=2，0B Flash + 0B RAM */
    WEEK_WED,       /* 枚举常量，值=3，0B Flash + 0B RAM */
    WEEK_THU,       /* 枚举常量，值=4，0B Flash + 0B RAM */
    WEEK_FRI,       /* 枚举常量，值=5，0B Flash + 0B RAM */
    WEEK_SAT,       /* 枚举常量，值=6，0B Flash + 0B RAM */
    WEEK_SUN        /* 枚举常量，值=7，0B Flash + 0B RAM */
};

enum DeviceMode
{
    MODE_IDLE  = 0,     /* 枚举常量，值=0，0B Flash + 0B RAM */
    MODE_RUN,           /* 枚举常量，值=1，0B Flash + 0B RAM */
    MODE_STOP,          /* 枚举常量，值=2，0B Flash + 0B RAM */
    MODE_ERROR = 99     /* 枚举常量，值=99，0B Flash + 0B RAM */
};

/* ========================================================================
 * 第二部分：全局变量（rwdata 段 / bss 段）
 * ======================================================================== */

/* --- 普通全局变量（已初始化，非零初值）---
 * 存储段：rwdata
 * Flash 占用：每个变量占其字节数（存初始值）
 * RAM 占用：每个变量占其字节数（运行时副本）
 */
U8  g_count       = 100;     /* rwdata段，1B Flash + 1B RAM */
U16 g_timer_ms    = 1000;    /* rwdata段，2B Flash + 2B RAM */
U32 g_timestamp   = 0x12345678;  /* rwdata段，4B Flash + 4B RAM */
S8  g_temperature = -5;      /* rwdata段，1B Flash + 1B RAM */

/* --- 普通全局变量（零初始化 / 未初始化）---
 * 存储段：bss
 * Flash 占用：0 字节（启动代码自动清零，无需存初始值）
 * RAM 占用：每个变量占其字节数
 */
U8  g_rx_index;              /* bss段，0B Flash + 1B RAM */
U16 g_adc_value;             /* bss段，0B Flash + 2B RAM */
U32 g_total_count;           /* bss段，0B Flash + 4B RAM */
U8  g_error_flag;            /* bss段，0B Flash + 1B RAM */

/* --- const 修饰的全局变量 ---
 * 存储段：rwdata（const 不改变存储位置，仍在 RAM）
 * Flash 占用：每个变量占其字节数（存初始值）
 * RAM 占用：每个变量占其字节数
 * 只读性质：语法约束，编译器禁止写入，物理上仍可写
 */
const U8  g_max_retry     = 3;       /* rwdata段，1B Flash + 1B RAM，语法只读 */
const U16 g_baud_rate     = 9600;    /* rwdata段，2B Flash + 2B RAM，语法只读 */
const F32 g_temp_upper    = 35.0f;   /* rwdata段，4B Flash + 4B RAM，语法只读 */
const F32 g_temp_lower    = 10.0f;   /* rwdata段，4B Flash + 4B RAM，语法只读 */

/* --- code 修饰的全局变量（C51 专属）---
 * 存储段：rodata（强制存储在 ROM/Flash 中）
 * Flash 占用：每个变量占其字节数
 * RAM 占用：0 字节
 * 只读性质：物理只读，硬件层面不可写
 */
code U8  g_device_addr  = 0x50;      /* rodata段，1B Flash + 0B RAM，物理只读 */
code U16 g_vendor_id    = 0x1234;    /* rodata段，2B Flash + 0B RAM，物理只读 */

/* code 修饰的查表数组 —— 节省 RAM 的典型应用 */
code U8 g_days_per_month[12] = {
    31, 28, 31, 30, 31, 30,     /* rodata段，12B Flash + 0B RAM，物理只读 */
    31, 31, 30, 31, 30, 31
};

/* code 修饰的 LED 字模表 */
code U8 g_led_font[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66,   /* rodata段，10B Flash + 0B RAM，物理只读 */
    0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

/* --- static 修饰的全局变量 ---
 * 存储段：rwdata（非零初值）或 bss（零初值/未初始化）
 * Flash 占用：同普通全局变量
 * RAM 占用：同普通全局变量
 * 作用域变化：仅当前 .c 文件可见，其他文件不可通过 extern 访问
 */
static U8  s_module_state = 1;       /* rwdata段，1B Flash + 1B RAM，文件内可见 */
static U16 s_interval_ms  = 500;     /* rwdata段，2B Flash + 2B RAM，文件内可见 */
static U8  s_init_done;              /* bss段，0B Flash + 1B RAM，文件内可见 */

/* --- static + const 组合 ---
 * 存储段：rwdata（const 不改变位置）
 * 作用域：仅当前文件 + 语法只读
 */
static const U8 s_lookup_table[4] = {0x01, 0x02, 0x04, 0x08};
                                     /* rwdata段，4B Flash + 4B RAM，文件内可见+语法只读 */

/* --- static + code 组合 ---
 * 存储段：rodata
 * 作用域：仅当前文件 + 物理只读
 */
static code U8 s_hw_version[4] = {'V', '1', '.', '0'};
                                     /* rodata段，4B Flash + 0B RAM，文件内可见+物理只读 */

/* ========================================================================
 * 第三部分：全局数组与结构体
 * ======================================================================== */

/* --- 全局数组（已初始化，非零初值）---
 * 存储段：rwdata
 * Flash 占用：数组总字节数
 * RAM 占用：数组总字节数
 */
U8 g_tx_buffer[MAX_BUF_SIZE] = {0xAA, 0xBB};  /* rwdata段，64B Flash + 64B RAM */

/* --- 全局数组（零初始化）---
 * 存储段：bss
 * Flash 占用：0 字节
 * RAM 占用：数组总字节数
 */
U8 g_rx_buffer[MAX_BUF_SIZE] = {0};  /* bss段，0B Flash + 64B RAM */

/* --- code 修饰的全局数组（查表数据）---
 * 存储段：rodata
 * Flash 占用：数组总字节数
 * RAM 占用：0 字节
 * 适用场景：运行期间不修改的固定数据表
 */
code U16 g_crc_table[16] = {
    0x0000, 0x1021, 0x2042, 0x3063,  /* rodata段，32B Flash + 0B RAM */
    0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B,
    0xC18C, 0xD1AD, 0xE1CE, 0xF1EF
};

/* --- 结构体类型定义（typedef）---
 * 存储段：类型定义本身不占空间
 */
typedef struct
{
    U8  id;           /* 成员：1B */
    U8  cmd;          /* 成员：1B */
    U16 length;       /* 成员：2B */
    U8  payload[8];   /* 成员：8B */
    U8  checksum;     /* 成员：1B */
} Packet_t;          /* 结构体总大小：13B，0B Flash + 0B RAM（仅类型定义） */

/* --- 全局结构体变量（已初始化）---
 * 存储段：rwdata
 */
Packet_t g_default_packet = {
    0x01, 0x10, 0x0008,           /* rwdata段，13B Flash + 13B RAM */
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07},
    0x00
};

/* --- 全局结构体变量（零初始化）---
 * 存储段：bss
 */
Packet_t g_rx_packet;              /* bss段，0B Flash + 13B RAM */

/* --- 枚举变量 ---
 * 存储段：rwdata 或 bss
 * C51 中 enum 变量通常为 1 字节
 */
enum DeviceMode g_current_mode = MODE_IDLE;  /* rwdata段，1B Flash + 1B RAM */
enum WeekDay    g_alarm_day;                 /* bss段，0B Flash + 1B RAM */

/* ========================================================================
 * 第四部分：函数（code 段）
 * ======================================================================== */

/* --- 普通全局函数 ---
 * 存储段：code（函数体的机器码存储在 Flash 中）
 * Flash 占用：函数体机器码大小（视指令数量而定）
 * RAM 占用：0 字节（函数本身不占 RAM，调用时临时占用栈）
 * 作用域：全项目可见（其他文件声明后可调用）
 */

/**
 * @brief   获取指定月份的天数
 * @param   month 月份（1~12）
 * @return  该月天数，0 表示月份非法
 * 存储段：code
 * Flash 估算：约 30~50B（含查表跳转和边界检查）
 * RAM 估算：调用时栈占用约 2B（参数+返回地址）
 */
U8 get_days_in_month(U8 month)
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
 * 存储段：code
 * Flash 估算：约 40~60B（浮点比较指令较多）
 * RAM 估算：调用时栈占用约 4B（float 参数+返回地址）
 */
U8 check_temperature(F32 temp)
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
 * @brief   计算 CRC 校验值
 * @param   data 数据指针
 * @param   len  数据长度
 * @return  CRC16 值
 * 存储段：code
 * Flash 估算：约 60~80B
 * RAM 估算：调用时栈占用约 4B（指针+长度+局部变量+返回地址）
 */
U16 calculate_crc(const U8 *data, U8 len)
{
    U16 crc = 0x0000;
    U8 i;

    for (i = 0; i < len; i++)
    {
        crc = (crc << 4) ^ g_crc_table[(crc >> 12) ^ (data[i] >> 4)];
        crc = (crc << 4) ^ g_crc_table[(crc >> 12) ^ (data[i] & 0x0F)];
    }
    return crc;
}

/* --- static 全局函数 ---
 * 存储段：code（与普通函数相同，机器码在 Flash 中）
 * Flash 占用：同普通函数
 * RAM 占用：0 字节
 * 作用域：仅当前 .c 文件可见
 */

/**
 * @brief   微秒级延时（内部使用）
 * @param   us 延时微秒数
 * 存储段：code
 * Flash 估算：约 15~20B（简单循环）
 * RAM 估算：调用时栈占用约 2B
 */
static void delay_us(U16 us)
{
    while (us--)
    {
        /* 空循环，约 1us @ 12MHz */
    }
}

/**
 * @brief   字节序转换（大端转小端）
 * @param   val 大端存储的 16 位值
 * @return  小端存储的 16 位值
 * 存储段：code
 * Flash 估算：约 10~15B
 * RAM 估算：调用时栈占用约 2B
 */
static U16 swap_endian_U16(U16 val)
{
    return (U16)((val << 8) | (val >> 8));
}

/* ========================================================================
 * 第五部分：局部变量与静态局部变量（栈区 / rwdata/bss 段）
 * ======================================================================== */

/**
 * @brief   演示局部变量与静态局部变量的区别
 * 存储段：code（函数体）
 * Flash 估算：约 30~40B
 * RAM 估算：调用时栈占用约 3B（n + 返回地址）
 *
 * 局部变量 n：
 *   存储段：栈（Stack）
 *   Flash 占用：0B
 *   RAM 占用：1B（临时，函数返回后释放）
 *   每次调用重新初始化为 0
 *
 * 静态局部变量 s_counter：
 *   存储段：bss（零初始化的静态变量）
 *   Flash 占用：0B
 *   RAM 占用：1B（持久，程序全程存在）
 *   仅首次初始化为 0，值在多次调用间保持
 */
void counter_demo(void)
{
    U8 n = 0;                   /* 栈区，1B RAM（临时），每次调用初值=0 */
    static U8 s_counter = 0;    /* bss段，1B RAM（持久），首次初始化后保持 */

    n++;
    s_counter++;
}

/**
 * @brief   数据处理函数（演示大数组避免放栈中）
 * 存储段：code（函数体）
 *
 * 危险写法（不推荐）：
 *   U8 temp[100];  → 栈区，100B RAM（临时），C51 栈仅约 32B 可用，必溢出
 *
 * 安全写法（推荐）：
 *   static U8 temp[100]; → bss段，100B RAM（持久），不占栈空间
 */
void process_data(void)
{
    /* 静态局部变量：大数组放全局数据区，避免栈溢出 */
    static U8 s_temp_buf[100];  /* bss段，100B RAM（持久），0B Flash */

    /* 普通局部变量：小变量放栈中，函数返回自动释放 */
    U8 i;                       /* 栈区，1B RAM（临时） */
    U8 checksum = 0;            /* 栈区，1B RAM（临时） */

    for (i = 0; i < 100; i++)
    {
        checksum += s_temp_buf[i];
    }
}

/* ========================================================================
 * 第六部分：指针类型
 * ======================================================================== */

/* --- 全局指针变量 ---
 * C51 中指针占用 1~3 字节 RAM（1B=通用指针idata/data, 2B=code/xdata, 3B=通用远指针）
 * 以下以通用指针（3B）为例
 */
U8 *g_p_tx;                     /* bss段，3B RAM（C51通用指针），0B Flash */
const U8 *g_p_const_data;       /* bss段，3B RAM，0B Flash，指向只读数据 */
code U8 *g_p_code_data;         /* bss段，3B RAM，0B Flash，指向 code 区数据 */

/* --- 函数指针 ---
 * 存储段：bss 或 rwdata
 * 占用：2~3B RAM（C51 函数指针）
 */
typedef void (*EventHandler)(U8 event);  /* 函数指针类型定义，0B Flash + 0B RAM */
static EventHandler s_handler;           /* bss段，2~3B RAM */

/* ========================================================================
 * 第七部分：资源占用汇总
 * ======================================================================== */

/*
 * Flash 占用估算（code + rodata + rwdata）：
 * ┌─────────────────────────────┬──────────┐
 * │ 段                          │ 估算大小  │
 * ├─────────────────────────────┼──────────┤
 * │ code（函数机器码）           │ ~300B    │
 * │ rodata（code修饰的变量/数组）│ ~53B     │
 * │   g_device_addr             │   1B     │
 * │   g_vendor_id               │   2B     │
 * │   g_days_per_month          │  12B     │
 * │   g_led_font                │  10B     │
 * │   s_hw_version              │   4B     │
 * │   g_crc_table               │  32B     │
 * │ rwdata（非零初值全局变量）   │ ~98B     │
 * │   g_count                   │   1B     │
 * │   g_timer_ms                │   2B     │
 * │   g_timestamp               │   4B     │
 * │   g_temperature             │   1B     │
 * │   g_max_retry               │   1B     │
 * │   g_baud_rate               │   2B     │
 * │   g_temp_upper              │   4B     │
 * │   g_temp_lower              │   4B     │
 * │   s_module_state            │   1B     │
 * │   s_interval_ms             │   2B     │
 * │   s_lookup_table            │   4B     │
 * │   g_tx_buffer               │  64B     │
 * │   g_default_packet          │  13B     │
 * │   g_current_mode            │   1B     │
 * ├─────────────────────────────┼──────────┤
 * │ Flash 合计                  │ ~451B    │
 * └─────────────────────────────┴──────────┘
 *
 * RAM 占用估算（rwdata + bss + stack + heap）：
 * ┌─────────────────────────────┬──────────┐
 * │ 段                          │ 估算大小  │
 * ├─────────────────────────────┼──────────┤
 * │ rwdata（非零初值全局变量）   │ ~98B     │
 * │   （同 Flash 中 rwdata 列表）│          │
 * │ bss（零初值/未初始化全局变量）│ ~200B    │
 * │   g_rx_index                │   1B     │
 * │   g_adc_value               │   2B     │
 * │   g_total_count             │   4B     │
 * │   g_error_flag              │   1B     │
 * │   s_init_done               │   1B     │
 * │   g_rx_buffer               │  64B     │
 * │   g_rx_packet               │  13B     │
 * │   g_alarm_day               │   1B     │
 * │   s_counter (静态局部)      │   1B     │
 * │   s_temp_buf (静态局部)     │ 100B     │
 * │   g_p_tx                    │   3B     │
 * │   g_p_const_data            │   3B     │
 * │   g_p_code_data             │   3B     │
 * │   s_handler                 │   3B     │
 * │ stack（栈，运行时临时占用）  │ ~20B     │
 * │   局部变量 + 函数调用帧      │          │
 * │ heap（堆，本文件未使用）     │   0B     │
 * ├─────────────────────────────┼──────────┤
 * │ RAM 合计                    │ ~318B    │
 * └─────────────────────────────┴──────────┘
 *
 * 注意：C51 的 8051 内核仅有 128B 内部 RAM（data区）+ 128B 特殊功能寄存器区，
 * 上述 bss+rwdata 约 298B 需使用 xdata（外部 RAM）或 pdata 分页访问。
 * 实际项目中应尽量使用 code 关键字将只读数据移至 Flash 以节省 RAM。
 */

/* ========================================================================
 * 第八部分：32 位平台移植说明
 * ======================================================================== */

/*
 * 移植到 ARM Cortex-M 等 32 位平台时的主要变化：
 *
 * 1. 类型大小变化：
 *    unsigned int  → 4 字节（C51 为 2 字节）
 *    unsigned long → 4 字节（与 int 相同）
 *    指针          → 4 字节（C51 为 1~3 字节）
 *
 * 2. 关键字变化：
 *    code 关键字不可用 → 改用 const（ARM 中 const 数据默认存 Flash）
 *    ARM 中 const 全局变量存储在 Flash（rodata 段），自动节省 RAM
 *
 * 3. 存储模型简化：
 *    ARM 采用统一寻址，不再有 data/idata/xdata/code 等存储区修饰符
 *    所有地址空间统一为 32 位线性地址
 *
 * 4. typedef 的跨平台价值：
 *    只需修改 typedef 定义即可适配：
 *      C51:  typedef unsigned int   U16;  // 2字节
 *      ARM:  typedef unsigned int   U16;  // 仍为2字节需改为 uint16_t
 *    推荐使用 <stdint.h> 中的 uint8_t / uint16_t / uint32_t
 *
 * 5. 字节序变化：
 *    C51（8051）为大端，ARM Cortex-M 为小端
 *    多字节通信协议需注意字节序转换
 */
