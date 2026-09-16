---
status: done
created: 2026-09-16
tags:
  - c/wifi
  - c/tcp
  - c/at-command
  - embedded/networking
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/38_rocketpi_esp8266_tcp"
  - "[[37_rocketpi_esp8266]] ESP8266 AT 指令基础（38 例的基础）"
  - "[[08_rocketpi_uart_control_led]] UART 命令解析（对比 TCP 数据解析）"
---

# 38 ESP8266 TCP 客户端

## 一句话定性

在 37 例 ESP8266 AT 指令基础上，实现 TCP 客户端连接远程服务器，接收并解析 `+IPD` 数据推送，是嵌入式系统通过 WiFi 进行 TCP 网络通信的基础。

## 与 37 例的关键差异

| 维度 | 37 AT 指令基础 | 38 TCP 客户端 |
|------|--------------|--------------|
| 网络层 | WiFi 连接 + MQTT | WiFi 连接 + TCP |
| 数据接收 | MQTT 消息事件 | +IPD 异步数据推送 |
| 连接方式 | AT+MQTTCONN | AT+CIPSTART TCP |
| 主循环 | 事件轮询打印 | +IPD 解析 + CLOSED 检测 |
| UART | USART1 | USART6（不同引脚） |

## 代码架构

```
main.c (11KB)
├── esp8266_tcp_test_run：WiFi 连接 + TCP 建立
├── esp8266_tcp_test_poll：主循环事件轮询
│   ├── +IPD 事件 → esp8266_tcp_print_ipd_payload
│   └── CLOSED 事件 → 标记连接断开
└── esp8266_tcp_print_ipd_payload：解析 +IPD 数据格式
```

## 核心实现

### TCP 连接流程

```c
static void esp8266_tcp_test_run(void)
{
    esp8266_at_init();
    esp8266_at_disable_echo(true);
    esp8266_at_set_wifi_mode(1, false);
    esp8266_at_connect_ap("SSID", "PASSWORD", 20000, false);

    // ① 单连接模式
    esp8266_at_send_command(CIPMUX, SET, "0", ...);

    // ② 建立 TCP 连接
    snprintf(start_args, sizeof(start_args), "\"TCP\",\"%s\",%u", host, port);
    esp8266_at_send_command(CIPSTART, SET, start_args, timeout*5, false);

    s_esp8266_tcp_link_ready = true;
}
```

- **① CIPMUX=0**：单连接模式（ESP8266 支持多连接，但单连接更简单）
- **② CIPSTART**：建立 TCP 连接到 `192.168.1.77:8899`，超时 5 倍默认值

### +IPD 数据解析

```c
static void esp8266_tcp_print_ipd_payload(const esp8266_at_event_t *event)
{
    const char *raw_line = event->raw_line;

    // ③ 解析 +IPD 格式
    if (sscanf(raw_line, "+IPD,%u,%u:%n", &channel, &payload_length, &payload_offset) == 2) {
        // 带通道号：+IPD,<channel>,<length>:<data>
    } else if (sscanf(raw_line, "+IPD,%u:%n", &payload_length, &payload_offset) == 1) {
        // 无通道号：+IPD,<length>:<data>
    }

    // ④ 提取 payload
    payload = raw_line + payload_offset;
    memcpy(buffer, payload, copy_len);

    printf("[ESP8266][TCP RX][id=%u len=%u] %s\r\n", channel, payload_length, buffer);
}
```

- **③ +IPD 格式**：ESP8266 收到 TCP 数据时推送 `+IPD,<len>:<data>` 或 `+IPD,<id>,<len>:<data>`
- **④ payload 提取**：`sscanf` 的 `%n` 记录偏移量，`raw_line + payload_offset` 直接指向数据起始位置

### 主循环事件轮询

```c
static void esp8266_tcp_test_poll(void)
{
    esp8266_at_poll();  // 从 UART 读取数据，解析 AT 响应

    esp8266_at_event_t event;
    while (esp8266_at_fetch_event(&event)) {
        if (strncmp(event.raw_line, "+IPD", 4) == 0) {
            esp8266_tcp_print_ipd_payload(&event);  // 解析 TCP 数据
        }
        if (strstr(event.raw_line, "CLOSED") != NULL) {
            s_esp8266_tcp_link_ready = false;  // 连接断开
        }
    }
}
```

- **+IPD 事件**：ESP8266 收到 TCP 数据时主动推送，驱动层解析为事件
- **CLOSED 事件**：TCP 连接被对端关闭时推送，标记连接状态

### USART6 替代 USART1

38 例使用 USART6 连接 ESP8266（37 例用 USART1），引脚分配不同。USART6 在 APB2 总线上（84MHz），USART1 也在 APB2，性能相同。

## 设计问题与改进空间

1. **单连接模式限制**：`CIPMUX=0` 只能维持一个 TCP 连接。多连接场景需 `CIPMUX=1` + 连接 ID 管理。

2. **无数据发送**：38 例只接收不发送。可加 `AT+CIPSEND=<len>` 发送数据到服务器。

3. **+IPD 解析的健壮性**：`sscanf` 对格式异常的 +IPD 行可能解析失败。已加 fallback（找冒号分隔），但仍有边界情况。

4. **连接断开无重连**：CLOSED 事件后标记 `link_ready=false`，但没有自动重连逻辑。可加重连状态机。

5. **主循环 10ms 延时**：`HAL_Delay(10)` 控制轮询频率。对 TCP 数据接收来说足够快，但可改为事件驱动（UART 中断触发轮询）。

## 关联笔记

- [[37_rocketpi_esp8266|37 ESP8266 AT 指令]]：ESP8266 基础，38 例复用驱动
- [[08_rocketpi_uart_control_led|08 UART 命令控制 LED]]：UART 文本协议解析，对比 +IPD 解析
- [[22_rocketpi_irda|22 红外遥控 NEC]]：另一种协议解析（红外 vs TCP）
