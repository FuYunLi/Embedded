---
status: done
created: 2026-09-12
tags:
  - embedded/ymodem
  - c/implementation
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/10_rocketpi_uart_ymodem/ymodem.h"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/10_rocketpi_uart_ymodem/ymodem.c"
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/10_rocketpi_uart_ymodem/ymodem_port.c"
---

# Ymodem 完整实现精读：从协议常量到收发状态机

> [[10_rocketpi_uart_ymodem|10 主笔记]] 给了架构和流程全景，[[10_rocketpi_uart_ymodem_1|依赖注入]] 讲了接口设计。这篇**逐函数拆解 ymodem.c 的全部实现细节**，目标是让只有 C 语言基础的读者能依据这篇笔记把 Ymodem 协议引擎完整复刻出来。ymodem_port.c 的平台适配在 _1 已讲，本篇不重复。

## 文件总览

ymodem.c 约 450 行有效代码，结构如下：

```
CRC 工具（crc16_ccitt）                    ~15 行
日志/进度钩子包装（ylog/yprog/y_putc 等）  ~10 行
进度显示（human_bytes/prog_init/prog_tick） ~60 行
发送端核心（send_block/send_block0/ymd_send_multi） ~120 行
接收端核心（read_packet_rest/ymd_recv_multi）       ~180 行
文件大小提示（全局变量 + getter/setter）            ~10 行
```

下面按调用关系从底层到顶层逐个讲解。

## 全局变量：文件大小提示

```c
static int64_t g_store_file_size_hint = -1;

void ymodem_store_set_file_size_hint(int64_t sz){ g_store_file_size_hint = sz; }
int64_t ymodem_store_get_file_size_hint(void){ return g_store_file_size_hint; }
```

**作用**：Block 0 里解析出的文件大小，需要传给存储层（ymodem_port.c 的 flash_open_write 用来判断 Flash 容量是否够）。ymodem.c 和 ymodem_port.c 是独立编译的两个文件，不能直接共享变量——用全局变量 + getter/setter 函数做跨文件通信。`-1` 表示"未知大小"。

**调用链**：`ymd_recv_multi` 解析 Block 0 得到 fsz → `ymodem_store_set_file_size_hint(fsz)` → `flash_open_write` 内部调 `ymodem_store_get_file_size_hint()` 读取。

## CRC16-CCITT：逐位实现

```c
static uint16_t crc16_ccitt(const uint8_t* q, int len) {
    uint16_t crc = 0;
    while (len-- > 0) {
        crc ^= (uint16_t)(*q++) << 8;          // ① 当前字节左移 8 位，与 CRC 高 8 位异或
        for (int i=0; i<8; i++)                  // ② 逐位处理
            crc = (crc & 0x8000) ? (crc<<1) ^ 0x1021 : (crc<<1);  // ③
    }
    return crc;
}
```

**形参**：`q` — 数据缓冲区；`len` — 数据字节数。

**返回值**：16 位 CRC 值。

**逐行拆解**：

① `crc ^= (uint16_t)(*q++) << 8`：把当前字节放到 CRC 的高 8 位（左移 8 位），然后与 CRC 异或。`*q++` 是"先取值再推进指针"——C 的后置自增在表达式里取旧值，表达式结束后指针才前进。

② `for (int i=0; i<8; i++)`：每个字节处理 8 位（一个字节 8 bit）。

③ `(crc & 0x8000) ? (crc<<1) ^ 0x1021 : (crc<<1)`：检查 CRC 最高位（bit 15）——
- 最高位为 1：左移 1 位后异或多项式 0x1021（CRC-CCITT 标准多项式）
- 最高位为 0：只左移 1 位

这就是 CRC 的数学定义：把数据当作一个长二进制数，除以多项式 0x1021，余数就是 CRC。逐位实现就是手动做这个除法——每看一位，决定是"带余数进位"还是"纯进位"。

**为什么初始值是 0**：CRC-CCITT 的标准初始值就是 0x0000（另一种常见变体 CRC-16 初始值是 0xFFFF，不同标准不同）。

**工程优化方向**：当前逐位循环每字节 8 次迭代。用 256 项查找表（预计算每个字节值的 CRC 结果）可以一次处理一整字节，速度提升 8 倍，代价是 512 字节 Flash 存查找表。

## I/O 包装函数：统一调用接口

```c
static int y_putc (const YContext* c, uint8_t b){ return c->port->putc(c->port, b); }
static int y_getc (const YContext* c, uint8_t* b, int ms){ return c->port->getc(c->port, b, ms); }
static int y_write(const YContext* c, const void* p, int n){ return c->port->write(c->port, p, n); }
static int y_readx(const YContext* c, void* p, int n, int ms){ return c->port->read_exact(c->port, p, n, ms); }
```

**作用**：把 `c->port->putc(c->port, b)` 这种长调用简化为 `y_putc(c, b)`。协议引擎内部全部用这四个包装函数，不直接写 `c->port->xxx`——**如果将来接口变了（比如加一个缓冲层），只改这四个函数**。

**返回值约定**：putc/getc 返回 1 表示成功、0 表示超时、-1 表示错误；write/readx 返回实际读写的字节数，-1 表示错误。

## drain_input：清空残留数据

```c
static void drain_input(YContext* ctx){
    uint8_t ch;
    while (y_getc(ctx, &ch, 1) == 1) { /* 1ms 轮询直到没有可读数据 */ }
}
```

**作用**：用 1ms 超时不断读字节，读到没有数据为止。在发送一个块之后调用——**发送方可能收到接收方延迟到达的 ACK/C（上一个块的应答），如果不清理，这些残留字节会被误读为下一个块的应答**。

**调用时机**：`send_block0` 成功后、每个文件发送完成后、会话结束时。

## 进度显示（prog_init / prog_tick / prog_done_line）

```c
typedef struct {
    const char* prefix; const char* name;
    long long total; long long done;
    uint32_t t0_ms; uint32_t last_ms;
    size_t last_print_len;
    const YTimer* timer;
} Prog;
```

**作用**：跟踪传输进度（已发/总量/速度/ETA），定时打印进度条。`prog_tick` 内部用 `timer->now_ms()` 判断是否过了 200ms——**限频打印**，避免每个块都输出一行刷屏。

**`human_bytes`**：把字节数格式化成人可读的单位（B/KB/MB/GB），如 1234 → "1.21 KB"。

**当前例程中进度打印被注释掉了**（`/* printf("%s", line); */`）——因为 MCU 的 printf 走串口，进度输出会和 Ymodem 协议数据混在同一条串口上，干扰传输。实际使用时可以用 RTT 或第二个 UART 输出进度。

## 发送端核心：send_block

```c
static int send_block(const YContext* c, uint8_t seq, const uint8_t* payload, int plen){
    uint8_t code = (plen==YMD_BLK1K) ? YMD_STX : YMD_SOH;   // ① 选帧头
    uint8_t hdr[3] = { code, seq, (uint8_t)(~seq) };          // ② 帧头+序号+取反
    uint16_t crc = crc16_ccitt(payload, plen);                 // ③ 计算 CRC
    uint8_t crc2[2] = { (uint8_t)(crc>>8), (uint8_t)(crc&0xFF) }; // ④ CRC 大端拆两字节

    for(int r=0; r<c->cfg.retry_max; ++r){                     // ⑤ 重试循环
        if (y_write(c, hdr, 3)!=3) return -1;                  // ⑥ 发帧头 3 字节
        if (y_write(c, payload, plen)!=plen) return -2;        // ⑦ 发数据
        if (y_write(c, crc2, 2)!=2) return -3;                 // ⑧ 发 CRC 2 字节
        uint8_t ch;
        if (y_getc(c, &ch, c->cfg.rx_timeout_ms)==1){          // ⑨ 等应答
            if (ch==YMD_ACK) return 0;                          //    ACK → 成功
            if (ch==YMD_CAN) return -10;                        //    CAN → 对方取消
            /* NAK 或其他 → 继续重试 */
        }
    }
    return -4;                                                  // ⑩ 超过重试次数
}
```

**形参**：`c` — 上下文；`seq` — 序号（0~255 循环）；`payload` — 数据缓冲区；`plen` — 数据长度（128 或 1024）。

**返回值**：0=成功，负数=失败（-1~-3 写入错误，-4 重试耗尽，-10 对方取消）。

**逐行拆解**：

① 帧头字节选择：`plen==1024` 用 STX（0x02），否则用 SOH（0x01）。Ymodem 规范：128 字节包用 SOH，1K 包用 STX。

② 帧头三字节：`{帧头, 序号, ~序号}`。`~seq` 是按位取反——`seq=1` 则 `~seq=0xFE`，接收方验证 `seq + ~seq == 0xFF`。

④ CRC 大端拆分：`(crc>>8)` 取高字节先发，`(crc&0xFF)` 取低字节后发——**网络字节序（大端）是 CRC 传输的惯例**。

⑤ 重试循环：最多 `retry_max` 次。每次发完帧头+数据+CRC 后等应答。

⑨ 等应答：`y_getc` 超时 `rx_timeout_ms`（默认 3000ms）。收到 ACK 返回 0（成功），收到 CAN 返回 -10（对方取消），收到 NAK 或其他继续重试。**超时（返回 0 不是 1）也继续重试**。

## 发送端核心：send_block0

```c
static int send_block0(const YContext* c, const char* filename, int64_t fsz){
    uint8_t buf[YMD_BLK128]={0}; int p=0;
    if (filename && *filename){
        int n=(int)strlen(filename); if(n>YMD_BLK128-2) n=YMD_BLK128-2;
        memcpy(buf+p, filename, n); p+=n; buf[p++]=0;           // 文件名 + \0
        char sz[32]; if (fsz<0) fsz=0; snprintf(sz,sizeof(sz),"%lld",(long long)fsz);
        int m=(int)strlen(sz); if(p+m+1>YMD_BLK128) m=YMD_BLK128-p-1;
        memcpy(buf+p, sz, m); p+=m; buf[p++]=0;                 // 大小字符串 + \0
    } else {
        buf[0]=0;                                                 // 空文件名 = 会话结束
    }
    return send_block(c, 0x00, buf, YMD_BLK128);                 // 序号固定 0x00
}
```

**作用**：组装 Block 0 的 128 字节数据并发送。

**数据格式**：`文件名\0大小字符串\0`（剩余填 0）。例如 `firmware.bin\012345\0` + 108 个 0x00。

**边界保护**：文件名长度超过 `YMD_BLK128-2`（126）则截断；大小字符串放不下也截断。`snprintf` 的返回值不直接用——通过 `strlen(sz)` 重新测量实际长度，更安全。

**空文件名**：`filename` 为 NULL 或空字符串时，`buf[0]=0`——接收方看到第一个字节是 0 就知道"没有更多文件了"。

**序号固定 0x00**：Block 0 的序号永远是 0，这是 Ymodem 规范约定。

## 发送端核心：ymd_send_multi（完整发送状态机）

```c
int ymd_send_multi(YContext* ctx, const char* const* files, int nfiles){
    // ① 参数检查
    if (!ctx || !ctx->port || !ctx->timer || !ctx->store) return -100;

    // ② 握手：等接收方发 'C'
    uint8_t ch=0; uint32_t t0=ctx->timer->now_ms();
    while ((int)(ctx->timer->now_ms()-t0) < ctx->cfg.hs_total_ms){
        if (y_getc(ctx,&ch,1000)==1 && ch==YMD_CHC) break;
    }
    if (ch!=YMD_CHC){ ylog(ctx,"[TX] no 'C' from receiver"); return -101; }

    // ③ 分配发送缓冲区
    uint8_t* blk=(uint8_t*)malloc(YMD_BLK1K); if(!blk) return -102;

    // ④ 逐文件发送
    for(int i=0; i<nfiles; i++){
        const char* path=files[i];
        void* f=ctx->store->open_read(path);        // 打开文件
        if(!f){ /* 报错，free(blk)，return -103 */ }

        // ⑤ 获取文件大小
        int64_t fsz=-1;
        if (ctx->store->size) fsz=ctx->store->size(f);
        if (fsz<0 && ctx->store->seek && ctx->store->tell){
            ctx->store->seek(f,0,2); fsz=ctx->store->tell(f); ctx->store->seek(f,0,0);
        }

        // ⑥ 提取文件名（去掉路径前缀）
        const char* base=path; for(const char* p=path;*p;++p){ if(*p=='/'||*p=='\\') base=p+1; }

        // ⑦ 发送 Block 0（文件元信息）
        if (send_block0(ctx,base,fsz)!=0){ /* 报错，return -104 */ }
        drain_input(ctx);   // 清理 Block 0 应答的残留

        // ⑧ 逐块发送数据
        uint8_t seq=1; int plen= ctx->cfg.packet_prefer_1k? YMD_BLK1K:YMD_BLK128;
        uint64_t done=0;
        for(;;){
            int r=ctx->store->read(f, blk, plen);     // 读一块数据
            if (r<0){ /* 报错，return -105 */ }
            if (r==0) break;                            // 文件读完
            if (r<plen) memset(blk+r, 0x1A, plen-r);   // 不足一块，0x1A 填充
            int rc=send_block(ctx, seq, blk, plen);     // 发送
            if (rc!=0){ /* 报错，return -106 */ }
            seq++; done+=(uint32_t)r;
            // 进度更新...
        }

        // ⑨ EOT 双次握手
        y_putc(ctx, YMD_EOT);                          // 第一个 EOT
        // 等 NAK（忽略其他）
        { /* 超时循环等 NAK，没等到 return -107 */ }
        y_putc(ctx, YMD_EOT);                          // 第二个 EOT
        // 等 ACK（忽略其他）
        { /* 超时循环等 ACK，没等到 return -108 */ }

        drain_input(ctx);
        ctx->store->close_read(f);                     // 关闭文件
    }

    // ⑩ 发送空 Block 0 结束会话
    if (send_block0(ctx,"",0)!=0){ /* 报错，return -109 */ }
    drain_input(ctx);
    free(blk);
    return 0;
}
```

**整体流程**：握手 → 逐文件循环（Block0 → 数据块循环 → EOT 双次） → 空 Block0 结束。

**关键细节**：

② 握手：循环等 `hs_total_ms`（默认 20 秒），每次 1 秒超时读一个字节，收到 `'C'` 就 break。超时未收到返回 -101。**`'C'` 是接收方告诉发送方"我准备好了，用 CRC16 校验"**。

⑤ 获取文件大小：优先用 `store->size()`（直接返回）；如果不支持 size，用 seek(0, SEEK_END) + tell() + seek(0, SEEK_SET) 间接获取——**先跳到末尾取位置，再跳回来**。

⑥ 文件名提取：从完整路径中找最后一个 `/` 或 `\`，取后面的部分作为文件名。`base` 初始值是 `path`（没找到分隔符时整个路径当文件名）。

⑧ 数据块循环：`read(f, blk, plen)` 读一块 → 不足一块用 `0x1A` 填充 → `send_block` 发送 → 序号递增。**`0x1A`（Ctrl+Z）是 CP/M 时代的文件结束标记**，接收方用文件大小截断多余填充。

⑨ EOT 双次：第一个 EOT 后等 NAK（接收方确认"我知道你要结束了"），第二个 EOT 后等 ACK（确认"结束"）。两步之间如果收到 CAN 则对方取消。**等应答时忽略非目标字节**（用 while 循环+超时，只 break 在目标字节上）。

## 接收端核心：read_packet_rest

```c
static int read_packet_rest(const YContext* c, uint8_t code, uint8_t* buf, int* payload, int ms){
    if (code==YMD_SOH) *payload=YMD_BLK128;
    else if (code==YMD_STX) *payload=YMD_BLK1K;
    else { *payload=0; return 0; }
    int tsz = 2 + *payload + 2;    // 序号(1B) + ~序号(1B) + 数据 + CRC(2B)
    int n = y_readx(c, buf, tsz, ms);
    return (n==tsz) ? 0 : -1;
}
```

**作用**：帧头字节（SOH/STX）已经由调用方读走了，本函数读剩下的部分：序号+取反+数据+CRC。

**形参**：`code` — 已读到的帧头字节；`buf` — 接收缓冲区（写入从序号开始的剩余数据）；`payload` — 输出参数，本帧的数据长度（128 或 1024）；`ms` — 超时。

**返回值**：0=成功读满，-1=超时或读取不足。

**`tsz` 的计算**：`2 + *payload + 2` = 序号和取反（2B）+ 数据（128 或 1024B）+ CRC（2B）。总帧长 = 帧头（1B，已读）+ tsz。

## 接收端核心：ymd_recv_multi（完整接收状态机）

这是全文件最长的函数（约 180 行），用两层嵌套 `for(;;)` 循环实现：外层循环处理多个文件，内层循环处理一个文件的多个数据块。

### 外层循环：文件级

```c
for(;;){
    // ① 发 'C' 握手（每次新文件或会话开始时）
    int tail=(file_cnt>0);   // 已经收过文件 = 尾部等待
    int total_wait_ms = tail? (rx_timeout_ms+1000) : hs_total_ms;

    uint32_t t0=ctx->timer->now_ms(); int got0=0;
    for(;;){
        y_putc(ctx, YMD_CHC);                          // 发 'C'
        if (y_getc(ctx,&code,1000)==1){
            if (code==YMD_SOH || code==YMD_STX){ got0=1; break; }  // 收到帧头
            if (code==YMD_CAN){ return -201; }          // 对方取消
        }
        if (超时){ if(tail) return 0; else return -202; }  // 尾部超时=正常结束
    }
```

**握手逻辑**：每隔 1 秒发一次 `'C'`，等帧头（SOH/STX）到来。第一个文件等 `hs_total_ms`（20 秒），后续文件等 `rx_timeout_ms+1000`（4 秒）——**第一个文件要给人时间在 PC 上选文件，后续文件间隔应该很短**。超时且已收过文件（`tail`）→ 正常结束（return 0），超时且没收到任何文件 → 错误（return -202）。

### 收 Block 0 并解析文件信息

```c
    // ② 读 Block 0 剩余部分
    read_packet_rest(ctx, code, pkt, &payload, rx_timeout_ms);

    // ③ 校验：序号取反 + CRC
    uint8_t seq=pkt[0], inv=pkt[1];
    if ((uint8_t)(seq+inv)!=0xFF || seq!=0x00) return -204;   // 序号必须是 0x00
    uint16_t rxcrc = ((uint16_t)pkt[2+payload]<<8) | pkt[2+payload+1];
    if (rxcrc!=crc16_ccitt(pkt+2, payload)) return -205;      // CRC 校验

    // ④ 提取文件名和大小
    const char* name = (const char*)(pkt+2);                    // 数据区开头是文件名
    const char* sizeStr = name + (int)strlen(name) + 1;         // 跳过 \0 后是大小字符串
    int64_t fsz = (int64_t)strtoll(sizeStr, NULL, 10);          // 字符串转整数

    // ⑤ 空文件名 = 会话结束
    if (!*fname){ y_putc(ctx,YMD_ACK); break; }

    // ⑥ 通知存储层文件大小，打开写入
    ymodem_store_set_file_size_hint(fsz);
    void* fw = ctx->store->open_write(out_dir, fname);
    if (!fw){ y_putc(ctx,YMD_CAN); return -206; }

    // ⑦ ACK + 'C'（告诉发送方"Block 0 收到了，可以开始发数据"）
    y_putc(ctx, YMD_ACK);
    y_putc(ctx, YMD_CHC);
```

**③ CRC 校验的数据范围**：`pkt+2` 开始、长度 `payload`（128）——跳过序号和取反的 2 字节，只对数据区做 CRC。CRC 存在数据区之后的 2 字节里，大端格式。

**④ 数据区内部格式**：`文件名\0大小字符串\0`（剩余是 0 填充）。`name` 指向 pkt+2（数据区起始），`sizeStr` 跳过文件名的 `\0` 指向大小字符串。`strtoll` 把字符串转 int64。

### 内层循环：数据块级

```c
    for(;;){
        // ⑧ 等帧头
        if (y_getc(ctx, &code, rx_timeout_ms)!=1){ return -207; }  // 超时

        if (code==YMD_SOH || code==YMD_STX){
            // ⑨ 读剩余部分 + 校验
            read_packet_rest(ctx, code, pkt, &payload, rx_timeout_ms);
            uint8_t s1=pkt[0], s2=pkt[1];
            if ((uint8_t)(s1+s2)!=0xFF){ y_putc(ctx,YMD_NAK); continue; }  // 序号错→NAK+重收
            uint16_t c = ((uint16_t)pkt[2+payload]<<8) | pkt[2+payload+1];
            if (c!=crc16_ccitt(pkt+2, payload)){ y_putc(ctx,YMD_NAK); continue; }  // CRC错→NAK+重收

            // ⑩ 写入存储
            int wlen=payload;
            if (fsz>=0 && (int64_t)wlen>(fsz-(int64_t)done)) wlen=(int)(fsz-(int64_t)done);  // 截断尾包
            if (wlen>0){
                ctx->store->write(fw, pkt+2, wlen);
                done+=(uint32_t)wlen;
            }
            y_putc(ctx, YMD_ACK);   // 确认

        } else if (code==YMD_EOT){
            // ⑪ EOT 双次握手
            y_putc(ctx, YMD_NAK);   // 第一个 EOT → 回 NAK
            // 等第二个 EOT
            { /* 超时循环等 EOT，没等到 return -210 */ }
            y_putc(ctx, YMD_ACK);   // 第二个 EOT → 回 ACK
            y_putc(ctx, YMD_CHC);   // 发 'C' 准备收下一个文件

            ctx->store->close_write(fw, 1);   // 正常关闭（ok=1）
            file_cnt++;
            break;   // 跳出内层循环，回到外层收下一个文件

        } else if (code==YMD_CAN){
            ctx->store->close_write(fw, 0);   // 异常关闭（ok=0）
            return -211;

        } else {
            y_putc(ctx, YMD_NAK);   // 未知字节 → NAK 让对方重发
        }
    }
}
```

**⑨ 序号校验失败的处理**：回 NAK + `continue`——**不 return，不 break**，继续等下一个帧头。发送方收到 NAK 会重发这个块。CRC 校验失败同理。

**⑩ 尾包截断**：最后一块可能不满 128/1024 字节（用 0x1A 填充），但文件实际大小已知（fsz），所以 `wlen = min(payload, fsz-done)`——**只写实际字节数，不写填充**。

**⑪ EOT 双次握手（接收方视角）**：收到第一个 EOT → 回 NAK（"我确认你要结束"）→ 等第二个 EOT → 回 ACK（"确认结束"）→ 发 'C'（"准备收下一个文件"）。如果等第二个 EOT 超时，说明协议异常。

## 错误码汇总

| 范围 | 含义 |
|---|---|
| -100~-109 | 发送端错误（参数/握手/分配/读取/发送/EOT/结束） |
| -200~-211 | 接收端错误（参数/握手/超时/序号/CRC/写入/EOT/取消） |
| 0 | 成功 |

负数十位数表示阶段（10=发送，20=接收），个位数表示具体错误。这个编码方式让调用方可以快速定位是哪个阶段出了问题。

## 整体状态机总结

**发送方**：
```
等'C' → 发 Block0 → 等 ACK+'C' → [读数据→发块→等ACK]循环 → EOT→NAK→EOT→ACK → [下一个文件...] → 空 Block0 → 结束
```

**接收方**：
```
发'C' → 收 Block0 → 校验 → ACK+'C' → [收块→校验→ACK/NAK]循环 → 收 EOT→NAK→收 EOT→ACK+'C' → [下一个文件...] → 收空 Block0 → ACK → 结束
```

每一步都有超时处理、错误码返回、存储层关闭——**没有静默忽略的异常路径**。

## 关联笔记

- [[10_rocketpi_uart_ymodem]] — 主笔记：架构全景与协议流程
- [[10_rocketpi_uart_ymodem_1|依赖注入详解]] — YPort/YTimer/YStore 接口设计
- [[10_rocketpi_uart_ymodem_2|Ymodem 与文件传输概念]] — 文件语义层讨论
- [[08_rocketpi_uart_control_led_3|08 函数逐类精读]] — 对比：文本解析器的逐字符纪律 vs 二进制协议的定长块纪律
