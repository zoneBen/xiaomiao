# XC_BOT 小船 WiFi 遥控器（ESP32 小喵掌机）

“遥控”分支把原来的一机多棋固件改造成 **XC_BOT 小船遥控器**：掌机作为 WiFi STA 连接船载 AP，通过 UDP 向船端发送推进器、灯光和档位命令。

- 主控：ESP32（无 PSRAM），ESP-IDF **5.5.2**
- 显示：ST7789 SPI TFT，逻辑分辨率 **160x128**，RGB565
- 按键：上/下/左/右/A/B，低电平有效
- 蜂鸣器：GPIO14，LEDC PWM（仅遥控器本地提示音）
- 界面：中文（内置 16x16 点阵字库），船体/推进器/船灯/档位全部用绘图代码绘制

> 船端固件不在本仓库，需要按下面的 UDP 协议实现接收端。

## 操作

| 操作 | 功能 |
|---|---|
| 开机 | 自动连接 NVS 中记录的上一艘船；没有记录则停在控制界面显示“未连接” |
| 控制界面按住 **A+B** 700ms | 停止推进器并进入 WiFi 搜索界面 |
| 搜索界面 **上/下** | 选择 `XC_BOT-` 开头的船 |
| 搜索界面 **A** | 用内置密码 `BOT88888888` 连接；无结果时重新扫描 |
| 搜索界面 **B** | 返回控制界面 |
| 连接中 **B** | 取消连接，返回搜索界面 |
| **上**（按住） | 双推进器 100%（前进） |
| **左**（按住） | 左推进器 0%，右推进器 100%（左转） |
| **右**（按住） | 左推进器 100%，右推进器 0%（右转） |
| **下**（按住） | 双推进器 0% + 刹车标志 |
| 松开方向键 | 双推进器停止，滑行 |
| **A**（单击，松开时生效） | 开关船上的灯 |
| **B**（单击，松开时生效） | 循环档位 `1 → 2 → 3 → 4 → 5 → 1`（默认 1 档） |

同时按 A+B 时不会误触发灯/档位切换；A/B 在松开时生效，组合键会抑制本次单键动作。方向键是按住持续、松开停止。

状态栏显示 `已连接 / 连接中 / 未连接`；未连接时底部提示红字“按住 A+B 搜索”。搜索列表只显示 SSID 以 `XC_BOT-` 开头的 AP，按信号强度排序。

## WiFi 流程

1. `esp_wifi` 以 STA 模式启动，设置国家码 `CN`、关闭省电（低延迟）。
2. A+B 进入搜索：`esp_wifi_scan_start()`，`WIFI_EVENT_SCAN_DONE` 后过滤 `XC_BOT-`、按 SSID 去重、按 RSSI 降序。
3. A 连接：密码固定为 `BOT88888888`，`threshold.authmode = WIFI_AUTH_WPA2_PSK`。
4. `IP_EVENT_STA_GOT_IP` 后保存 SSID 到 NVS（namespace `boat_cfg`，key `last_ssid`），打开 UDP socket，返回控制界面。
5. 断开后最多自动重连 3 次（间隔 1s）；连接超时 12s；认证失败立即提示“密码错误”。

## UDP 协议（船端需实现）

船端的完整实现要求（AP 配置、UDP 接收、失控保护、验收清单和 ESP-IDF 骨架）见
[`docs/boat-side-requirements.md`](docs/boat-side-requirements.md)。

- 船端开 SoftAP：SSID 前缀 `XC_BOT-`，密码 `BOT88888888`，建议 AP IP `192.168.4.1`。
- 遥控器加入该 AP 后，向 **DHCP 网关 IP 的 UDP 8888 端口**发送命令，频率 **20Hz**（50ms）。
- 若网关为 0，固件回退到 `192.168.4.1`（见 `main/boat_protocol.h` 的 `BOAT_UDP_TARGET_IP`）。

### 命令包（8 字节，全部 `uint8_t`，与端序无关）

| 偏移 | 字段 | 说明 |
|---|---|---|
| 0 | `magic0` | `'X'` (0x58) |
| 1 | `magic1` | `'C'` (0x43) |
| 2 | `version` | 1 |
| 3 | `seq` | 0–255 循环 |
| 4 | `left` | 左推进器 0–100 |
| 5 | `right` | 右推进器 0–100 |
| 6 | `flags` | bit0 灯，bit1 保留（必须 0），bit2 刹车，bit3–5 档位（1–5） |
| 7 | `checksum` | byte0..6 按位异或 |

```c
typedef struct __attribute__((packed)) {
    uint8_t magic0, magic1, version, seq;
    uint8_t left, right, flags, checksum;
} boat_packet_t;   // sizeof == 8
```

以 `seq=0` 为例：

| 场景 | left | right | flags | 完整报文 |
|---|---|---|---|---|
| 3 档前进 | 100 | 100 | `3<<3 = 0x18` | `58 43 01 00 64 64 18 02` |
| 5 档前进 + 灯 | 100 | 100 | `0x01 \| 5<<3 = 0x29` | `58 43 01 00 64 64 29 33` |
| 1 档后退 | 0 | 0 | `0x04 \| 1<<3 = 0x0C` | `58 43 01 00 00 00 0C 16` |
| 旧遥控器前进 | 100 | 100 | `0x00` | `58 43 01 00 64 64 00 1A` |

最后一个字节是 XOR 校验：`byte0 ^ ... ^ byte6`。

### 船端处理建议

```c
// 1. 收到 UDP 包后校验：长度 8、magic == 'X','C'、version == 1、checksum 正确
// 2. left/right 映射到两路电机（0..100 可先当作启停），档位 gear 用于缩放推力
// 3. flags bit0 控制灯，bit2 执行刹车，bit3..5 为 1..5 档；bit1 保留为 0
// 4. 失败安全：连续 500ms 未收到有效包 -> 两路推进器停止（不关心档位）
```

- `left=right=0, brake=1` 表示主动刹车。
- `left=right=0, brake=0` 表示滑行停止。
- 遥控器进入搜索/连接界面时会立即补发停止包；断线时靠船端 500ms 失效保护停车。

按键到推进器的优先级：后 > 左右同按 > 仅左 > 仅右 > 仅前进 > 无方向键；左右同按视为停车+刹车。

## 构建与烧录

```powershell
& D:\ESP32\Espressif\frameworks\esp-idf-v5.5.2\export.ps1
cd D:\works\esp32\xiaomiao-01
idf.py fullclean
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

- 本分支的 `sdkconfig.defaults` 只要求 WiFi，Bluetooth 组件因 `MINIMAL_BUILD` 和未加入 `REQUIRES` 而不会参与编译。如果本地已有旧的被忽略文件 `sdkconfig`/`sdkconfig.old`，请删除后再执行 `set-target`，避免旧配置残留。
- `main/CMakeLists.txt` 依赖：`esp_wifi esp_netif esp_event lwip nvs_flash driver esp_lcd tft_display`。
- 如果在 Git Bash 里启动 PowerShell，先执行 `Remove-Item Env:MSYSTEM` 再调用 `export.ps1`，否则脚本会误判为 MSys 环境并拒绝激活。

## 中文界面与字库

所有中文文案集中在 `main/boat_strings.h`。字库由脚本从该文件提取汉字，用 Windows 自带 SimHei 16px 生成 16x16 1bpp 点阵：

```bash
python tools/gen_cn_font.py
```

生成 `components/tft_display/font_cn.c/.h`。修改任何中文文案后必须重新运行脚本，否则新字会显示为空心方框。生成文件已提交，正常构建不依赖 Python。

## PC 端 UDP 收包测试

用 Windows 移动热点或另一台 ESP32 建测试 AP：

- SSID：`XC_BOT-TEST`
- 密码：`BOT88888888`

PC 上运行：

```bash
python tools/udp_listen.py --port 8888
```

应能看到约 20 包/秒，`seq` 递增；按住方向键时 `L/R` 变化，A 键切换 `flags` bit0，B 键切换 bit3–5（档位 1–5），下键时 bit2（刹车）置位，bit1 应始终为 0。

## 测试

推进器映射和 UDP 包校验有主机侧单元测试（不需要开发板）：

```bash
gcc -std=c11 -Wall -Wextra -Werror -I main test/test_boat_logic.c -o test/test_boat_logic.exe
./test/test_boat_logic.exe
```

字库生成脚本会校验每个汉字都能渲染且非空；修改文案后运行 `python tools/gen_cn_font.py` 即可同时完成覆盖检查。

## 目录

```text
main/
  xiaomiao-01.c    app_main
  input.c/.h       按键扫描与防抖
  boat_link.c/.h   WiFi 扫描/连接、重连、20Hz 发送任务
  boat_storage.c/.h NVS 保存/读取上次 SSID
  boat_udp.c/.h    UDP socket 与命令包发送
  boat_protocol.h  UDP 命令包定义
  boat_control.h   方向键到推进器的纯逻辑映射（可主机测试）
  boat_ui.c/.h     屏幕状态机、按键语义、30FPS UI 任务
  boat_draw.c/.h   船体/推进器/灯/档位/列表绘制
  boat_strings.h   全部中文 UI 文案
  sound.c/.h       本地提示音
components/tft_display/
  tft_display.c/.h ST7789 + 精灵缓冲 + ASCII 字模
  font_cn.c/.h     生成的 16x16 中文字库 + UTF-8 绘制
tools/
  gen_cn_font.py   字库生成
  udp_listen.py    UDP 调试工具
test/
  test_boat_logic.c 推进器映射 / 协议包主机测试
docs/
  boat-side-requirements.md 船端 WiFi/UDP/失控保护实现要求
```

## 硬件参考

引脚与硬件限制见 [硬件信息.txt](硬件信息.txt)。TFT 与 SD 卡共用 SPI2（18/23/19），本固件不使用 SD 卡。
