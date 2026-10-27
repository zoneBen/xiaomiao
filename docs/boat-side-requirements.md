# 船端（XC_BOT）实现要求

本文档定义“遥控”分支遥控器对船端固件的要求。遥控器只负责连接船载 WiFi AP 并发送 UDP 命令；船端需要提供 AP、接收命令、驱动双推进器、灯和蜂鸣器，并实现失控保护。

协议的唯一权威定义在 [`main/boat_protocol.h`](../main/boat_protocol.h)，本文档与代码不一致时以代码为准。

## 1. WiFi AP 要求

船端必须作为 **2.4GHz SoftAP** 工作，遥控器作为 STA 连接。

| 项目 | 要求 |
|---|---|
| SSID | 必须以 `XC_BOT-` 开头，区分大小写，例如 `XC_BOT-A1B2` |
| 密码 | 固定为 `BOT88888888` |
| 认证方式 | WPA2-PSK（遥控器 `threshold.authmode = WIFI_AUTH_WPA2_PSK`） |
| 频段/信道 | 2.4GHz；建议信道 1–13（遥控器扫描时国家码为 CN） |
| DHCP | 必须开启 DHCP 服务，给遥控器分配 IP |
| AP IP / 网关 | 遥控器把命令发往自己拿到的 **DHCP 网关**。ESP-IDF 默认 SoftAP 为 `192.168.4.1`，推荐保持默认 |
| 连接数 | 建议 `max_connection = 1`（只允许一个遥控器） |

注意：

- 遥控器发送目标是 **DHCP 网关**，因此船端必须是遥控器所连 AP 本身。如果船端只是接入一个外部路由器，命令会发到路由器而不是船，需要修改遥控器协议。
- 不要启用 Captive Portal / 强制门户，遥控器不会处理 HTTP 重定向。
- 必须在 AP 启动后监听 UDP，不能依赖互联网连接。

## 2. UDP 命令通道

| 项目 | 要求 |
|---|---|
| 协议 | UDP |
| 端口 | **8888** |
| 方向 | 遥控器 → 船端，单播；船端无需回复 |
| 频率 | **20Hz**（每 50ms 一包），空闲时也持续发送停止命令作为心跳 |
| 包长 | 固定 **8 字节** |
| 字节序 | 全部为单字节字段，与端序无关 |

船端应绑定 `0.0.0.0:8888`，推荐使用非阻塞 socket 或 `SO_RCVTIMEO`，不要永久阻塞在 `recvfrom()` 里，否则无法执行失控保护。

## 3. 命令包格式

```c
typedef struct __attribute__((packed)) {
    uint8_t magic0;    // 'X' = 0x58
    uint8_t magic1;    // 'C' = 0x43
    uint8_t version;   // 1
    uint8_t seq;       // 0..255 循环
    uint8_t left;      // 左推进器 0..100
    uint8_t right;     // 右推进器 0..100
    uint8_t flags;     // bit0 灯, bit1 蜂鸣器, bit2 刹车
    uint8_t checksum;  // byte0..byte6 按位异或
} boat_packet_t;       // sizeof == 8
```

| 偏移 | 字段 | 说明 |
|---|---|---|
| 0 | `magic0` | 固定 `'X'` (0x58) |
| 1 | `magic1` | 固定 `'C'` (0x43) |
| 2 | `version` | 当前为 1；不兼容的版本应丢弃 |
| 3 | `seq` | 0–255 循环，可用于丢包统计 |
| 4 | `left` | 左推进器 0–100；当前遥控器只发 0 或 100 |
| 5 | `right` | 右推进器 0–100；当前遥控器只发 0 或 100 |
| 6 | `flags` | bit0=灯，bit1=蜂鸣器，bit2=刹车 |
| 7 | `checksum` | `byte[0] ^ byte[1] ^ ... ^ byte[6]` |

### 校验示例

```c
static bool boat_packet_valid(const uint8_t* data, size_t len)
{
    if (len != 8) {
        return false;
    }
    if (data[0] != 'X' || data[1] != 'C' || data[2] != 1) {
        return false;
    }

    uint8_t sum = 0;
    for (int i = 0; i < 7; i++) {
        sum ^= data[i];
    }
    return sum == data[7];
}
```

校验失败、长度不符、magic/version 不符的包必须 **丢弃**，并且 **不能刷新失控保护计时器**。

## 4. 推进器与方向约定

- `left` / `right` 指站在船尾、面向船头时的左/右推进器。
- 数值 0–100 表示推力百分比；当前遥控器只使用 0 和 100，船端可先按开关量处理，也可映射到 PWM。
- 如果船体接线导致左右相反，请在船端固件里交换两路输出，不要修改遥控器。

遥控器的按键到命令映射如下（同时按下时优先级从高到低）：

| 遥控器操作 | left | right | 刹车 bit |
|---|---|---|---|
| 按住前进 | 100 | 100 | 0 |
| 按住左转 | 0 | 100 | 0 |
| 按住右转 | 100 | 0 | 0 |
| 按住后退 | 0 | 0 | **1** |
| 左右同时按住 | 0 | 0 | **1** |
| 松开所有方向键 | 0 | 0 | 0 |

- **刹车 bit=1**：船端应执行主动刹车（例如短促反转、电机短接制动或按船体结构选择的安全停车方式），而不是仅滑行。
- **0/0 且刹车 bit=0**：滑行停止，不主动制动。
- 后退优先级最高；左右同时按住按停车+刹车处理。

## 5. 灯与蜂鸣器

`flags` 的 bit0/bit1 表示船端期望的灯和蜂鸣器状态：

- 遥控器每次发心跳都会带上当前状态，船端可以每包都应用，也可以只在状态变化时应用。
- A 键切换灯，B 键切换船端蜂鸣器；两者互相独立，并会跨方向操作保持。
- 建议船端上电默认灯和蜂鸣器关闭。
- 如果船端没有蜂鸣器，可忽略 bit1；如果没有灯，可忽略 bit0，但不要因此丢弃整包。

## 6. 失控保护（必须实现）

这是安全要求，不是可选项：

1. 船端维护 `last_valid_packet_ms`，只在收到**校验通过**的包时刷新。
2. 如果从未收到过有效包，或 `now - last_valid_packet_ms > 500ms`：
   - **立即停止左右推进器**（输出 0）；
   - 建议同时关闭灯和蜂鸣器，或保持最后状态，由船端自行决定；
   - 在恢复收到有效包之前，不允许电机重新转动。
3. 失控检查应放在独立的控制循环/定时器中，不能只依赖 UDP 接收回调。
4. 遥控器进入搜索界面时会主动发送停止包；但在连接切换、WiFi 断线、遥控器死机、超出范围等情况下，船端不能只依赖停止包，必须由 500ms 超时兜底。

参考逻辑：

```c
static int64_t last_valid_ms = 0;
static bool has_valid_packet = false;

void control_loop(void)
{
    int64_t now = esp_timer_get_time() / 1000;

    if (!has_valid_packet || (now - last_valid_ms) > 500) {
        set_thruster(0, 0);
        // 可选：关闭灯/蜂鸣器
        return;
    }

    // 应用最近一次有效包中的 left/right/flags
}
```

## 7. 接收与处理流程

推荐顺序：

1. 启动 SoftAP（SSID `XC_BOT-*`、密码 `BOT88888888`、WPA2）。
2. 创建 UDP socket，绑定 `0.0.0.0:8888`。
3. 收到数据后：
   - 长度必须为 8；
   - 校验 magic/version/checksum；
   - 校验通过才刷新 `last_valid_ms`，更新左右推力、灯、蜂鸣器、刹车状态；
   - 校验失败直接丢弃，不改变任何输出，也不刷新超时。
4. 控制循环每 10–50ms 运行一次：
   - 先执行 500ms 失控保护；
   - 否则按最新命令驱动电机、灯、蜂鸣器；
   - 刹车 bit 置位时执行主动制动。
5. 可选：用 `seq` 统计丢包率并打印日志，便于调试。

## 8. ESP-IDF 参考骨架

以下仅示意 AP + UDP 接收 + 超时保护的结构，电机、灯、蜂鸣器的具体 GPIO/PWM 请按船体硬件实现。

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include <string.h>

#define BOAT_UDP_PORT 8888

static volatile int64_t s_last_valid_ms = 0;
static volatile bool s_has_valid_packet = false;
static volatile uint8_t s_left = 0, s_right = 0, s_flags = 0;

static bool packet_valid(const uint8_t* d, size_t len)
{
    if (len != 8 || d[0] != 'X' || d[1] != 'C' || d[2] != 1) return false;
    uint8_t sum = 0;
    for (int i = 0; i < 7; i++) sum ^= d[i];
    return sum == d[7];
}

static void udp_task(void* arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(BOAT_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(sock, (struct sockaddr*)&addr, sizeof(addr));

    uint8_t buf[32];
    while (1) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr*)&from, &from_len);
        if (len > 0 && packet_valid(buf, (size_t)len)) {
            s_left = buf[4];
            s_right = buf[5];
            s_flags = buf[6];
            s_last_valid_ms = esp_timer_get_time() / 1000;
            s_has_valid_packet = true;
        }
    }
}

static void control_task(void* arg)
{
    while (1) {
        int64_t now = esp_timer_get_time() / 1000;
        if (!s_has_valid_packet || (now - s_last_valid_ms) > 500) {
            set_thruster(0, 0);          // 必须停车
        } else {
            set_thruster(s_left, s_right);
            set_light(s_flags & 0x01);
            set_buzzer(s_flags & 0x02);
            // s_flags & 0x04 -> 执行主动刹车
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void boat_ap_init(void)
{
    wifi_config_t ap = {0};
    strcpy((char*)ap.ap.ssid, "XC_BOT-A1B2");
    strcpy((char*)ap.ap.password, "BOT88888888");
    ap.ap.channel = 6;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 1;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();

    xTaskCreate(udp_task, "udp", 4096, NULL, 5, NULL);
    xTaskCreate(control_task, "ctrl", 4096, NULL, 4, NULL);
}
```

> 实际工程中请在 `app_main` 里先初始化 NVS、`esp_netif`、默认事件循环，并检查各 API 返回值。以上代码省略了错误处理。

## 9. 验收清单

船端完成后至少验证以下项目：

- [ ] 手机/电脑能看到 SSID 以 `XC_BOT-` 开头、密码 `BOT88888888` 可连接。
- [ ] 遥控器 A+B 搜索列表能看到该船；A 连接成功后返回控制界面。
- [ ] 用 `python tools/udp_listen.py --port 8888`（PC 建热点时）能看到约 20 包/秒，`seq` 递增、checksum 正确。
- [ ] 前进：`L=100 R=100`，两推进器转。
- [ ] 左转：`L=0 R=100`；右转：`L=100 R=0`。
- [ ] 后退：`L=0 R=0` 且 brake bit=1，执行刹车。
- [ ] 松开方向键：`L=0 R=0`，滑行停止。
- [ ] A 键改变 flags bit0，船灯开关；B 键改变 bit1，船蜂鸣器开关。
- [ ] 关闭遥控器/断开 WiFi：船在 **500ms 内**停车。
- [ ] 发送错误校验/错误 magic 的包：船不改变输出，且不刷新失控计时。
- [ ] 遥控器重启后能自动重连上次的船。

## 10. 调试工具

- 遥控器端协议定义：[`main/boat_protocol.h`](../main/boat_protocol.h)
- PC 端收包工具：[`tools/udp_listen.py`](../tools/udp_listen.py)
- 遥控器操作与构建说明：[`README.md`](../README.md)
