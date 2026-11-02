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

- **屏幕睡眠（CO5300）——改用标准 MIPI DCS 睡眠，不走厂商深度待机（Task 18b 审查 I2，
  用户已拍板）**：
  - 最初的实现直接依次调用 `esp_lcd_panel_disp_on_off()`/`esp_lcd_panel_disp_sleep()`。
    但本板 `board_devices.yaml` 给 `display_lcd` 配了 `reset_gpio_num: 39`（屏幕确实接了
    硬件 RST），这会让 `espressif__esp_lcd_co5300` 驱动（`esp_lcd_co5300_spi.c`）在
    `disp_sleep(true)` 时**额外**发送 `DSTBON`（深度待机）；唤醒时 `disp_sleep(false)`
    会先硬复位面板（`panel_co5300_reset()`，10 ms + 150 ms）并把 `needs_init` 置 `true`，
    强制重新跑一遍完整初始化序列。本板 `setup_device.c` 的 `s_lcd_init_cmds[]` 首尾两条
    命令（`0x11` SLPOUT、`0x29` DISPON）各带 600 ms 延时，合计（160 + 600 + 600 =
    1360 ms）预计唤醒一次面板要 **~1.36 s**，远超验收第 5 条"按电源键后 300 ms 内亮屏"
    的目标；而且硬复位会清空 GRAM，初始化序列第 45 行就发 DISPON（之后还要等 600 ms），
    期间会先显示未初始化的 GRAM（花屏/黑屏）。
  - 现改为 `EspDisplayConfig::PanelSleep::DcsSleep`（`components/wisp_port_esp/dev/esp_display.cpp`，
    `boards/waveshare_amoled_216/board.cpp` 的 `kDisplaySleepOutSettleMs` 开启）：完全不调用
    `esp_lcd_panel_disp_sleep()`/`disp_on_off()`，改为用 `esp_lcd_panel_io_tx_param()`（和
    `set_brightness()` 同一条路径）直接发送标准 MIPI DCS 命令——入睡：`DISPOFF`(0x28) +
    `SLPIN`(0x10)；唤醒：`SLPOUT`(0x11) + 等待 `sleep_out_settle_ms`（本板配置 120 ms，
    MIPI DCS 规范对这一等待的下限）+ `DISPON`(0x29)。这是标准 DCS 命令，不涉及任何
    CO5300 私有内容。普通睡眠不清空 GRAM，唤醒后立即显示上次画面，随后正常重绘，不会
    触发硬复位。
  - **唤醒耗时估算（未经真机验证）**：按键轮询延迟（待机时 50 ms）+ `sleep_out_settle_ms`
    （120 ms）+ 少量命令传输时间，预计在 **200 ms 左右**，应能满足 300 ms 的目标——这是
    估算，**没有在真机上测过**，见下面"验收第 5 条"的真机验证步骤。
  - 一旦选了 `DcsSleep`，`EspDisplay` 就再也不会调用 `esp_lcd_panel_disp_sleep()`/
    `disp_on_off()`（会和它们内部的 `sleep_mode` 记录状态冲突）；深度待机（DSTB）留给
    spec 1.6 的"深度待机"一级，不在子项目 1 范围内。
  - **亮度丢失（I3）**：无论哪种睡眠方式，唤醒后面板的亮度寄存器都可能被重置（本板
    init 序列里 `0x51` 被写成 `0xFF`，`DcsSleep` 路径虽然不走这段 init，但为了稳妥仍然
    保留这层防护）。`EspDisplay::set_brightness()` 现在会记住最后一次设置的值，
    `set_power(true)` 成功后自动重发一次该亮度，用户设置不会在每次待机后被重置为
    最大亮度。
- **触摸睡眠（CST9217）**：`waveshare__esp_lcd_touch_cst9217` 驱动（见
  `esp_lcd_touch_cst9217.c`）只给 `esp_lcd_touch_s` 的 `read_data`/`get_xy`/`del` 三个
  字段赋值，`enter_sleep`/`exit_sleep` 始终是空指针。`esp_lcd_touch_enter_sleep()`/
  `exit_sleep()` 本身在字段为空时会 `ESP_LOGE` 后返回 `ESP_FAIL`——为了避免每次待机
  切换都打一条错误日志，`EspTouch::set_power()` 改为直接检查这两个函数指针，为空就直接
  返回 `ErrorCode::Unsupported`，完全不调用这两个函数（见
  `components/wisp_port_esp/dev/esp_touch.cpp`）。也就是说本板触摸芯片**没有**硬件睡眠：
  待机时触摸确实不会再产生触点（因为 `UiRuntime` 暂停后不再调用 `Touch::read()`），但
  CST9217 本身的功耗在待机期间不会下降。
- **自动浅睡眠期间 GPIO 会被隔离（Task 18b 审查 I4）**：`CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`
  在 ESP32-S3 上会自动选中 `CONFIG_PM_SLP_DISABLE_GPIO`（`ESP_SLEEP_GPIO_RESET_WORKAROUND`
  的 Kconfig 默认依赖；生成的 `firmware/sdkconfig` 里也确实是 `y`）——每次自动浅睡眠时，
  **所有** GPIO 的方向、电平和上下拉都会被禁用（浮空），直到浅睡眠结束才恢复。面板 RST
  （GPIO39）、触摸 RST（GPIO40）、功放使能（GPIO46）这三个必须在睡眠期间保持电平的输出
  如果浮空：面板/触摸可能被意外复位（配合上面的 `DcsSleep` 会直接抵消它要避免的那次
  重新初始化），功放浮空可能有爆音或漏电风险。`boards/waveshare_amoled_216/board.cpp` 的
  `hold_outputs_during_light_sleep()`（在板级 `init()` 的最后调用）对这三个引脚分别调用
  `gpio_sleep_sel_dis()`，让它们在浅睡眠期间继续保持配置好的电平/方向。**待验证**：真机
  上待机若干分钟（确保至少发生过几次自动浅睡眠）后用音量键唤醒，确认触摸依旧能正常响应、
  扬声器没有异常噗声或底噪——见下面"验收第 5 条"的验证步骤。
- **USB-Serial-JTAG 与自动浅睡眠**：本板用 USB-Serial-JTAG 做控制台（没有外置
  UART 转 USB 芯片），而 ESP32-S3 上 `SOC_USB_SERIAL_JTAG_SUPPORT_LIGHT_SLEEP` 未定义
  （`components/soc/esp32s3/include/soc/soc_caps.h`：这个宏目前只在还没落地的
  `esp32h2`/`esp32c6` 等型号的 Kconfig 注释里出现，ESP32-S3 完全不支持"浅睡眠期间保持
  最小 USB 通信"）。`esp_driver_usb_serial_jtag`/Kconfig 的 `USJ_NO_AUTO_LS_ON_CONNECTION`
  选项正是为这种芯片准备的：开启后，只要 USB-Serial-JTAG 端口处于连接状态（主机侧完成了
  USB 枚举，靠 SOF 信号判断——和有没有打开虚拟串口监视器无关），系统会自动持有一个
  `ESP_PM_NO_LIGHT_SLEEP` 锁，阻止自动浅睡眠；拔掉 USB 数据线之后锁被释放，自动浅睡眠
  才能真正发生。`firmware/sdkconfig.defaults` 已经加上 `CONFIG_PM_ENABLE=y`、
  `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`、`CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y` 三项——
  结论是：**接着 USB 调试时不需要、也不会进入浅睡眠（串口监视器开不开都一样，不影响
  这把锁）；只用电池供电、或者只接 USB 供电但不接数据线（哪怕是只充电的 USB 线，没有
  数据引脚）时，没有 SOF、锁会被释放，才会真正睡下去**。验收第 5 条的电流测量必须真的
  让数据线处于未连接状态（断开数据线本身，不是"只是没打开串口监视器"）。
- **后台仍在运行、会限制浅睡眠深度/频率的任务**（Task 18b 任务说明要求记录，不要求重新
  设计）：
  - `EspSdStorage` 的 SD 卡探测 worker（`components/wisp_port_esp/dev/esp_storage.cpp`）
    固定以 `kWorkerPollMs` = 100 ms 的粒度轮询（`Mounted` 态下每 ~1 s 探测一次卡是否还在，
    `Unmounted` 态下每 2 s 重试一次 mount，但循环本身每 100 ms 就醒一次检查状态/
    `worker_running_`），即待机时这个任务仍然会以 10 Hz 唤醒 CPU；
  - `wisp::os::Timer` 在 ESP 口径下全部跑在同一个 esp_timer 任务上：`App` 的
    `wisp.storage`（2000 ms 一次，`StorageManager::refresh()`）和 `wisp.power`
    （1000 ms 一次，`PowerManager::tick()`）即便在待机期间也会继续按周期唤醒；
  - `core::ButtonService` 待机时以 50 ms 轮询一次按键（`PowerManager::enter_standby()`
    放宽的间隔）；`ui::UiRuntime` 暂停后（Task 18b 审查 M1，fix round 1）**不再**自己
    周期性醒来——它的待机循环现在无限等待 `post()` 任务，没有事可做时完全不会主动醒来，
    真正做到 spec 6.7 要求的"待机时没有周期性的 UI 定时器，也没有高频轮询"。
  - 以上都只是"周期性唤醒 CPU 一下就继续睡"，不会阻止自动浅睡眠本身发生（没有常驻的
    `ESP_PM_NO_LIGHT_SLEEP`/`ESP_PM_CPU_FREQ_MAX` 锁），但会把浅睡眠的最长连续时长限制
    在 `ButtonService` 的 **50 ms** 量级（目前系统里最短的周期性唤醒源），略微拉高平均
    功耗。真机测得的待机电流如果明显高于预期，这些是优先排查的来源。

## 性能基准

（真机验收后填写：滚动自检列表时的 fps / cpu / flush_ms。另外对比开启动态调频（DFS，
`firmware/sdkconfig.defaults` 的 `CONFIG_PM_ENABLE`）前后的 `WISP_PERF`，确认 DFS 本身
没有把渲染路径的帧率拖到 spec/计划约定的基准以下——Task 18b 审查 M3。）

## 验收第 5 条：待机耗电实测

（真机验收后填写。测量方法：拔掉 USB 数据线，只用电池供电，在电池正极与电池之间串联
一个电流表（或用带电流检测的电源分析仪）；分别记录"亮屏静止"和"短按电源键熄屏待机"
下的平均电流，待机电流应不超过亮屏的 1/10；另外记录从短按电源键唤醒到屏幕实际点亮的
耗时——见上面"待机与省电"一节，改用 `DcsSleep` 后估算约 200 ms，应能满足 300 ms 的
目标，但这只是估算，需要实测确认。

另外两项验证：
1. 待机若干分钟（确保发生过自动浅睡眠）后，用音量键或电源键唤醒，确认触摸依旧正常
   响应、扬声器没有异常噗声或持续底噪——验证 I4 的 `gpio_sleep_sel_dis()` 处理是否真的
   覆盖了面板 RST、触摸 RST、功放使能这三个引脚。
2. 自动熄屏后短按电源键唤醒，确认屏幕不会先闪亮一下又熄灭；电源键熄屏后用音量键唤醒，
   确认下一次电源键短按能正常关屏——验证 C1 的修复（PowerManager 是屏幕状态的唯一真相
   来源）在真机上同样成立。）
