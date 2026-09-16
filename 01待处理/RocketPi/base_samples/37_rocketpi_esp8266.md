---
status: done
created: 2026-09-16
tags:
  - c/wifi
  - c/at-command
  - c/mqtt
  - embedded/networking
  - rocketpi/base_samples
references:
  - "[[Embedded_Code]] 01待处理/rocketpi/base_samples/37_rocketpi_esp8266"
  - "[[08_rocketpi_uart_control_led]] UART 命令解析（对比 AT 命令协议）"
  - "[[13_rocketpi_uart_radar]] UART 协议驱动（对比 ESP8266 AT 驱动）"
---

# 37 ESP8266 WiFi 模块 AT 指令

## 一句话定性

ESP8266 是 WiFi 模块，通过 UART AT 指令与 MCU 通信，实现 WiFi 连接、TCP/IP 网络通信和 MQTT 消息发布，是嵌入式系统接入物联网的基础。

## 同类产品定位

- **ESP8266**：最常用的 WiFi 模块，AT 指令集，成本 ~¥5
- **ESP32**：升级版，集成蓝牙，性能更强
- **RTL8720DN**：双频 WiFi，更稳定
- **本例选型理由**：ESP8266 最普及、AT 指令简单、MQTT 支持完善

## 硬件连接

- UART：USART1（或 USART2）连接 ESP8266 的 TX/RX
- 波特率：115200bps（默认）
- 引脚分配：PA9=TX, PA10=RX（USART1）

## 通信协议要点

- **AT 指令**：文本协议，`AT+CMD\r\n` 发送，`OK\r\n`/`ERROR\r\n` 响应
- **WiFi 连接**：`AT+CWJAP="SSID","PASSWORD"`
- **MQTT**：`AT+MQTTUSERCFG` → `AT+MQTTCONN` → `AT+MQTTSUB` → `AT+MQTTPUB`
- **事件驱动**：ESP8266 主动推送事件（`+IPD`、`WIFI CONNECTED` 等）

## 代码架构

```
┌─────────────────────────────────────────────────┐
│ main.c (应用层)                                 │
│  - esp8266_at_test_run()：完整测试流程          │
├─────────────────────────────────────────────────┤
│ driver_esp8266_at_test.c (测试层)               │
│  - WiFi 连接 + MQTT 发布/订阅/断开             │
│  - 事件轮询与打印                               │
├─────────────────────────────────────────────────┤
│ driver_esp8266_at.c (驱动层，54KB)             │
│  - AT 指令解析状态机                            │
│  - WiFi/MQTT/TCP 命令封装                      │
│  - 事件队列管理                                 │
├─────────────────────────────────────────────────┤
│ driver_esp8266_at_interface.c (平台适配层)      │
│  - UART 收发 + 延时 + 调试打印                 │
└─────────────────────────────────────────────────┘
```

## 核心设计：AT 指令协议

### AT 指令格式

```
发送：AT+CMD=<参数>\r\n
响应：OK\r\n（成功）或 ERROR\r\n（失败）
数据：+IPD,<len>,<data>（异步数据推送）
```

### 测试流程

```c
void esp8266_at_test_run(void)
{
    // ① 初始化 + 复位
    esp8266_at_init();
    esp8266_at_reset(2000);

    // ② 关回显 + 基本 AT 测试
    esp8266_at_disable_echo(true);
    esp8266_at_send_command(ESP8266_AT_CMD_AT, ...);

    // ③ WiFi 连接
    esp8266_at_set_wifi_mode(1, false);  // Station 模式
    esp8266_at_connect_ap("SSID", "PASSWORD", 20000, false);
    esp8266_at_query_ip_info(&ip_info);  // 查询 IP

    // ④ MQTT 连接
    esp8266_at_mqtt_user_config(0, "client_id", "user", "pass");
    esp8266_at_mqtt_connect(0, "broker.emqx.io", 1883, 120, true);

    // ⑤ MQTT 发布/订阅
    esp8266_at_send_command(MQTTSUB, SET, "0,\"/test/esp8266\",1", ...);
    esp8266_at_mqtt_publish(0, "/test/esp8266", "hello from rocketpi", 1, false);

    // ⑥ 清理
    esp8266_at_mqtt_disconnect(0);
    esp8266_at_disconnect_ap();
}
```

### 事件驱动模型

```c
void esp8266_at_test_poll(void)
{
    esp8266_at_poll();  // 从 UART 读取数据，解析 AT 响应

    esp8266_at_event_t event;
    while (esp8266_at_fetch_event(&event)) {
        // 打印事件类型和参数
        esp8266_at_test_print_event(&event);
    }
}
```

- **`esp8266_at_poll`**：从 UART 接收缓冲区读取数据，解析 AT 响应和异步事件
- **`esp8266_at_fetch_event`**：从事件队列取出已解析的事件
- **事件类型**：OK/ERROR/FAIL/BUSY/PROMPT/SEND_OK/SEND_FAIL/INFO/INDICATION/RESPONSE

### WiFi 连接流程

```
AT+RST           → 复位模块
ATE0             → 关回显
AT               → 测试通信
AT+CWMODE=1      → Station 模式
AT+CWJAP="SSID","PASS"  → 连接 AP（超时 20s）
AT+CIPSTA?       → 查询 IP 地址
```

### MQTT 流程

```
AT+MQTTUSERCFG=0,"client_id","user","pass"  → 配置 MQTT
AT+MQTTCONN=0,"broker.emqx.io",1883,120     → 连接 Broker
AT+MQTTSUB=0,"/test/esp8266",1              → 订阅主题
AT+MQTTPUB=0,"/test/esp8266","hello",1      → 发布消息
AT+MQTTUNSUB=0,"/test/esp8266"              → 取消订阅
AT+MQTTCLEAN=0                              → 断开 MQTT
```

## 设计问题与改进空间

1. **WiFi 密码硬编码**：`ESP8266_AT_TEST_WIFI_SSID` 和 `PASSWORD` 在编译时确定。可改为运行时配置（从 Flash 读取或串口输入）。

2. **MQTT 无持久订阅**：测试流程订阅→发布→取消订阅→断开。实际应用应保持订阅，持续接收消息。

3. **事件队列大小有限**：ESP8266 可能推送大量事件（如 WiFi 断连、MQTT 消息），队列溢出会丢失事件。可加事件优先级或丢弃策略。

4. **阻塞式命令**：`esp8266_at_send_command` 等待 OK/ERROR 响应，超时 5~20 秒。期间 MCU 不能做其他事。可改为非阻塞式（命令发送后返回，轮询检查结果）。

5. **与 08 例（UART 命令）对比**：08 例是 MCU 接收命令、解析执行；37 例是 MCU 发送命令、解析响应。两者都是 UART 文本协议，但方向相反。

## 关联笔记

- [[08_rocketpi_uart_control_led|08 UART 命令控制 LED]]：UART 文本协议解析，对比 AT 指令
- [[13_rocketpi_uart_radar|13 UART 雷达]]：UART 二进制协议，对比 AT 文本协议
- [[38_rocketpi_esp8266_tcp|38 ESP8266 TCP]]：ESP8266 TCP 通信，37 例的扩展
