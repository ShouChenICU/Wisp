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

## 待机与省电（Task 18b，spec 6.7）

- **屏幕睡眠（CO5300）**：`esp_lcd_panel_disp_on_off()`/`esp_lcd_panel_disp_sleep()` 都已被
  `espressif__esp_lcd_co5300` 实现（`esp_lcd_co5300_spi.c`）。但本板 `board_devices.yaml` 给
  `display_lcd` 配了 `reset_gpio_num: 39`（屏幕确实接了硬件 RST），这会让驱动在
  `disp_sleep(true)` 时**额外**发送 `DSTBON`（深度待机），而不只是 `SLPIN`：
  - 进入待机：`SLPIN` + 120 ms 稳定（`CO5300_SLEEP_SETTLE_TIME_MS`）+ `DSTBON`，耗时
    约 130 ms；
  - **唤醒（关键问题）**：由于进入的是深度待机，`disp_sleep(false)` 会先调用
    `panel_co5300_reset()`（置位 RST：10 ms + 释放：150 ms = 160 ms），而
    `panel_co5300_reset()` 自身会把 `needs_init` 置为 `true`——这会强制接下来
    **重新跑一遍完整初始化序列** `panel_co5300_init()`，而不是走更快的
    `SLPOUT` + 120 ms 路径。本板 `boards/waveshare_amoled_216/setup_device.c` 的
    `s_lcd_init_cmds[]` 首尾两条命令（`0x11` SLPOUT、`0x29` DISPON）各带 **600 ms**
    的延时，二者相加已经 1200 ms，加上前面 reset 的 160 ms，**唤醒一次面板预计要
    ~1.36 s**，远超验收第 5 条"按电源键后 300 ms 内亮屏"的目标。
  - 这是 `esp_lcd_panel_disp_sleep()` 公共接口本身没有更细粒度控制导致的：只要
    `reset_gpio_num >= 0`，驱动就会在 `disp_sleep(true)` 里自动走深度待机分支，调用方
    （`EspDisplay::set_power()`）无法单独跳过。按 Task 18b 的任务说明，本次实现仍然按
    要求依次调用 `esp_lcd_panel_disp_on_off()`/`esp_lcd_panel_disp_sleep()`（见
    `components/wisp_port_esp/dev/esp_display.cpp`），**没有**绕开这个问题——这需要用户
    决策（例如：改为只发送裸的 `SLPIN`/`SLPOUT` DCS 命令，像 `set_brightness()` 已经做的
    那样绕开 `esp_lcd_panel_disp_sleep()` 和它的深度待机分支，但这是对上游驱动行为的
    针对性重新实现，不在本任务范围内直接做出决定）。真机验收第 5 条需要实测这个唤醒
    耗时，预期会明显超过 300 ms。
- **触摸睡眠（CST9217）**：`waveshare__esp_lcd_touch_cst9217` 驱动（见
  `esp_lcd_touch_cst9217.c`）只给 `esp_lcd_touch_s` 的 `read_data`/`get_xy`/`del` 三个
  字段赋值，`enter_sleep`/`exit_sleep` 始终是空指针。`esp_lcd_touch_enter_sleep()`/
  `exit_sleep()` 本身在字段为空时会 `ESP_LOGE` 后返回 `ESP_FAIL`——为了避免每次待机
  切换都打一条错误日志，`EspTouch::set_power()` 改为直接检查这两个函数指针，为空就直接
  返回 `ErrorCode::Unsupported`，完全不调用这两个函数（见
  `components/wisp_port_esp/dev/esp_touch.cpp`）。也就是说本板触摸芯片**没有**硬件睡眠：
  待机时触摸确实不会再产生触点（因为 `UiRuntime` 暂停后不再调用 `Touch::read()`），但
  CST9217 本身的功耗在待机期间不会下降。
- **USB-Serial-JTAG 与自动浅睡眠**：本板用 USB-Serial-JTAG 做控制台（没有外置
  UART 转 USB 芯片），而 ESP32-S3 上 `SOC_USB_SERIAL_JTAG_SUPPORT_LIGHT_SLEEP` 未定义
  （`components/soc/esp32s3/include/soc/soc_caps.h`：这个宏目前只在还没落地的
  `esp32h2`/`esp32c6` 等型号的 Kconfig 注释里出现，ESP32-S3 完全不支持"浅睡眠期间保持
  最小 USB 通信"）。`esp_driver_usb_serial_jtag`/Kconfig 的 `USJ_NO_AUTO_LS_ON_CONNECTION`
  选项正是为这种芯片准备的：开启后，只要 USB-Serial-JTAG 端口处于连接状态，系统会自动
  持有一个 `ESP_PM_NO_LIGHT_SLEEP` 锁，阻止自动浅睡眠；拔掉 USB 数据线（或主机侧没有打开
  对应的虚拟串口）之后锁被释放，自动浅睡眠才能真正发生。`firmware/sdkconfig.defaults`
  已经加上 `CONFIG_PM_ENABLE=y`、`CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`、
  `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y` 三项——结论是：**接着 USB 调试时不需要、也不会
  进入浅睡眠（串口监视器仍然能用）；只用电池供电（USB 数据线真正拔开，不只是没打开串口
  监视器）时才会真正睡下去**。验收第 5 条的电流测量必须真的拔掉 USB 数据线（哪怕用
  USB 供电、只是不接数据线也不行，这个锁看的是端口连接状态，不是供电来源）。
- **后台仍在运行、会限制浅睡眠深度/频率的任务**（Task 18b 任务说明要求记录，不要求重新
  设计）：
  - `EspSdStorage` 的 SD 卡探测 worker（`components/wisp_port_esp/dev/esp_storage.cpp`）
    固定以 `kWorkerPollMs` = 100 ms 的粒度轮询（`Mounted` 态下每 ~1 s 探测一次卡是否还在，
    `Unmounted` 态下每 2 s 重试一次 mount，但循环本身每 100 ms 就醒一次检查状态/
    `worker_running_`），即待机时这个任务仍然会以 10 Hz 唤醒 CPU；
  - `wisp::os::Timer` 在 ESP 口径下全部跑在同一个 esp_timer 任务上：`App` 的
    `wisp.storage`（2000 ms 一次，`StorageManager::refresh()`）和新增的 `wisp.power`
    （1000 ms 一次，`PowerManager::tick()`）即便在待机期间也会继续按周期唤醒；
  - 以上都只是"周期性唤醒 CPU 一下就继续睡"，不会阻止自动浅睡眠本身发生（没有常驻的
    `ESP_PM_NO_LIGHT_SLEEP`/`ESP_PM_CPU_FREQ_MAX` 锁），但会把浅睡眠的最长连续时长限制
    在大约 100 ms 量级，略微拉高平均功耗。真机测得的待机电流如果明显高于预期，这些是
    优先排查的来源。

## 性能基准

（真机验收后填写：滚动自检列表时的 fps / cpu / flush_ms。）

## 验收第 5 条：待机耗电实测

（真机验收后填写。测量方法：拔掉 USB 数据线，只用电池供电，在电池正极与电池之间串联
一个电流表（或用带电流检测的电源分析仪）；分别记录"亮屏静止"和"短按电源键熄屏待机"
下的平均电流，待机电流应不超过亮屏的 1/10；另外记录从短按电源键唤醒到屏幕实际点亮的
耗时——见上面"待机与省电"一节，CO5300 这块板子的深度待机唤醒路径预期在 1 s 以上，
大概率无法满足 300 ms 的目标，需要实测确认。）
