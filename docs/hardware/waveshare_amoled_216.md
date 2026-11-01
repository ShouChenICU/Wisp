# Waveshare ESP32-S3-Touch-AMOLED-2.16

板名（`WISP_BOARD` / bmgr）：`waveshare_amoled_216`。板级代码：`boards/waveshare_amoled_216/`。

资料来源：用户自己的项目 `~/Projects/MyOS`（MIT，只读参考，**以其代码为准，它的文档有误**）、Waveshare 官方 BSP、ESP-Claw 中同款板的板级定义（Apache-2.0）。

## 主控与存储

- ESP32-S3R8：16 MB QIO flash，8 MB octal PSRAM（80 MHz）
- 分区表见 `firmware/partitions.csv`

## 外设与引脚

| 外设 | 芯片 | 接口与引脚 | 地址（7 位） |
|---|---|---|---|
| 屏幕 | CO5300，480×480 AMOLED，圆角半径 40 | QSPI：CS 12、SCLK 38、D0–D3 = 4/5/6/7、RST 39 | — |
| 触摸 | **CST9217**（MyOS 文档误写为 CST9220） | I2C；INT 11、RST 40 | 0x5A |
| 电源管理 | AXP2101 | I2C；电源键接 GPIO16（按下时为低电平） | 0x34 |
| IMU | QMI8658（无磁力计） | I2C；INT1 17、INT2 21 | 0x6B |
| RTC | PCF85063 | I2C；INT 13 | 0x51 |
| 音频 DAC | ES8311 | I2S + I2C | 0x18 |
| 音频 ADC | ES7210 | I2S + I2C | 0x40 |
| 功放 | — | PA_EN GPIO46 | — |
| SD 卡 | — | **1-bit SDMMC**：CLK 2、CMD 1、D0 3 | — |
| 按键 | — | 音量+ GPIO18；音量−/BOOT GPIO0（均为低电平有效） | — |

- I2C 总线：SDA 15、SCL 14，400 kHz，所有 I2C 器件共用这一条总线。
- I2S：MCLK 42、BCLK 9、LRCK/WS 45、DOUT 8、DIN 10。

## 已知勘误与注意事项

- 触摸芯片实际为 CST9217，驱动使用 `waveshare/esp_lcd_touch_cst9217`。
- SD 卡走 SDMMC 而不是 SPI。
- 屏幕的局部刷新区域需要按 2 像素对齐（`area_align = 2`）；四角留出 ≥ 40 px 的安全边距。
- AXP2101 初始化时不要改动任何电源轨的电压或开关。

## 性能基准

（真机验收后填写：滚动自检列表时的 fps / cpu / flush_ms。）
