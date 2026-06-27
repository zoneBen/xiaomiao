# ESP32 小喵掌机 - 完整示例

## 功能说明

- ST7789 SPI TFT LCD 屏幕驱动 (240x240)
- LVGL 图形库集成
- 6个按键检测（上/下/左/右/A/B）
- 按键按下时触发无源蜂鸣器发声（不同按键对应不同音调）
- 串口输出按键信息

## 硬件连接

### TFT LCD (ST7789)
- SCLK: GPIO18
- MOSI: GPIO23
- CS: GPIO5
- DC: GPIO4
- RES: GPIO19

### 蜂鸣器
- GPIO14 (PWM 驱动)

### 按键
- 上: GPIO2
- 下: GPIO13
- 左: GPIO27
- 右: GPIO35
- A: GPIO34
- B: GPIO12

## 编译说明

1. 设置目标芯片:
```bash
idf.py set-target esp32
```

2. 编译项目 (会自动下载 LVGL 依赖):
```bash
idf.py build
```

3. 烧录到设备:
```bash
idf.py -p COMx flash monitor
```
(将 COMx 替换为实际串口号)

## 按键音调

- 上键: C5 (523Hz)
- 下键: D5 (587Hz)
- 左键: E5 (659Hz)
- 右键: F5 (698Hz)
- A键: G5 (784Hz)
- B键: A5 (880Hz)

## 参考

本项目参考 ESP-IDF 官方示例:
`examples/peripherals/lcd/spi_lcd_touch`
