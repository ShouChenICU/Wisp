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
  （GPIO39）、触摸 RST（GPIO40）、功放使能（GPIO46）、**面板 SPI 的 CS（GPIO12，
  `board_devices.yaml` 的 `io_spi_config.cs_gpio_num`——Task 18b 再审查 N7）**这四个必须
  在睡眠期间保持电平的输出如果浮空：面板/触摸可能被意外复位（配合上面的 `DcsSleep` 会
  直接抵消它要避免的那次重新初始化），CS 浮空还可能让面板在浅睡眠期间误采样到总线上的
  随机电平、把无关的 SPI 数据当成发给自己的命令，功放浮空可能有爆音或漏电风险。
  `boards/waveshare_amoled_216/board.cpp` 的 `hold_outputs_during_light_sleep()`（在板级
  `init()` 的最后调用）对这四个引脚分别调用 `gpio_sleep_sel_dis()`，让它们在浅睡眠期间
  继续保持配置好的电平/方向。**待验证**：真机上连续经历多次自动浅睡眠/唤醒循环（例如
  待机 10 分钟以上，确保期间发生过几十次自动浅睡眠-唤醒）后，确认面板画面正常（没有
  花屏、没有被意外复位过的痕迹，例如亮度被重置——见下面 I3 的说明）、触摸依旧能正常
  响应、扬声器没有异常噗声或底噪——见下面"验收第 5 条"的验证步骤。
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
  设计；下面的分类是 Task 18b 再审查 N3/fix round 2 第二批附加范围整理后的最新状态，
  修正了早先"ButtonService 50 ms 是系统里最短的周期性唤醒源"这条结论——那条结论当时
  漏算了自检屏幕自己的采样任务，见下面第二条）：
  - **已在待机期间完全暂停、不再唤醒 CPU 的**：
    - 自检屏幕的采样任务（`SelfCheck::sample_loop()`，原本以
      `kSampleTaskPeriodMs` = 20 ms 的周期读取 IMU，附带每 `kSlowSampleIntervalMs`
      = 1000 ms 读一次电源状态/RTC）订阅了 `power/state`，进入待机后阻塞在
      `standby_wake_`（有 1 s 的兜底超时，防止漏收唤醒事件），完全不再轮询——这才是
      修正前文档漏记的、实际上系统里最快的周期性唤醒源（20 ms，比 ButtonService 的
      50 ms 还快）。`PowerManager::exit_standby()` 唤醒时立即 `give()` 该信号量恢复
      采样。
    - `wisp::hal::Imu::set_power(false)`（Task 18b fix round 2 第一批附加范围）：
      `PowerManager::enter_standby()` 关闭 QMI8658 的加速度计/陀螺仪使能位
      （`CTRL7`），待机期间芯片本身也不再采样，`exit_standby()` 时恢复到之前选定的
      ODR。
    - `EspSdStorage` 的 SD 卡探测 worker（`components/wisp_port_esp/dev/esp_storage.cpp`）
      原本固定以 `kWorkerPollMs` = 100 ms 的粒度轮询（`Mounted` 态下每 ~1 s 探测一次卡
      是否还在，`Unmounted` 态下每 2 s 重试一次 mount，但循环本身每 100 ms 就醒一次
      检查状态/`worker_running_`，即以 10 Hz 唤醒 CPU）。Task 18b fix round 2 第二批
      附加范围给它加了 `set_standby()`（经 `StorageManager::set_standby()` 转发，
      `PowerManager` 在 `enter_standby()`/`exit_standby()` 时调用）：待机期间这个
      worker 完全阻塞在自己的信号量上（同样有 1 s 兜底超时），不再探测/重试；
      `online()`/`capacity()` 不受影响，一直返回缓存的上一次状态，不会在待机期间
      卡住或报错。
  - **待机期间仍按原周期继续唤醒 CPU（有意保留，见下面的说明）**：
    - `wisp::os::Timer` 在 ESP 口径下全部跑在同一个 esp_timer 任务上：`App` 的
      `wisp.storage`（2000 ms 一次，`StorageManager::refresh()`）和 `wisp.power`
      （1000 ms 一次，`PowerManager::tick()`）即便在待机期间也会继续按周期唤醒。
      这是有意的裁定，不是遗漏：`2026-11-01-foundation-design.md` 的统一外设省电
      设计明确要求"StorageManager 的 2 s 状态刷新保持不变，但不触发 SD 检测"——
      `refresh()` 本身只读一次 `sd_dev_->online()` 缓存标志（上面已暂停的是真正的
      SD 探测本身，不是这次 `refresh()` 调用），改动它会违反这条裁定；`wisp.power`
      的 `tick()` 在 `standby_` 为真时第一行就直接返回（见
      `components/wisp_app/src/power_manager.cpp`），待机期间这个 1 Hz 定时器
      本身唤醒 CPU 的开销已经是这几个后台源里最小的一个，进一步拉长它的周期对实测
      待机电流的收益预计微乎其微，在"最小改动"的原则下没有跟着改。
    - 自检屏幕的性能上报定时器（`"wisp.perf"`，`kPerfReportMs` = 5000 ms，
      `SelfCheck::emit_perf_line()`）**不会**被上面的 `power/state` 订阅暂停——它和
      采样任务是两个独立的 `wisp::os::Timer`/任务。待机期间它仍然每 5 s 唤醒一次、
      照常打印一行 `WISP_PERF`，只是其中的 `fps`/`frames`/`flush_ms` 会是 0（因为
      `UiRuntime` 暂停后没有任何帧被渲染）。这属于预期行为，不是 bug：待机期间的
      `WISP_PERF` 日志本身就是用来确认"确实没有渲染"的信号之一；真机验收时如果
      发现待机日志里 `WISP_PERF` 的 `fps` 不是 0，才说明 N1/M1 的暂停渲染修复出现
      了回归。
    - `core::ButtonService` 待机时以 50 ms 轮询一次按键（`PowerManager::enter_standby()`
      放宽的间隔）；`ui::UiRuntime` 暂停后（Task 18b 审查 M1，fix round 1）**不再**自己
      周期性醒来——它的待机循环现在无限等待 `post()` 任务，没有事可做时完全不会主动醒来。
  - 经过以上两批修复，`core::ButtonService` 的 **50 ms** 轮询现在才真正是系统里最短的
    周期性唤醒源（自检采样任务的 20 ms 已被消除）。以上都只是"周期性唤醒 CPU 一下就
    继续睡"，不会阻止自动浅睡眠本身发生（没有常驻的 `ESP_PM_NO_LIGHT_SLEEP`/
    `ESP_PM_CPU_FREQ_MAX` 锁），但会把浅睡眠的最长连续时长限制在 `ButtonService` 的
    50 ms 量级，略微拉高平均功耗。真机测得的待机电流如果明显高于预期，这些仍是优先
    排查的来源。

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

另外几项验证：
1. 待机若干分钟（确保发生过自动浅睡眠）后，用音量键或电源键唤醒，确认触摸依旧正常
   响应、扬声器没有异常噗声或持续底噪——验证 I4 的 `gpio_sleep_sel_dis()` 处理是否真的
   覆盖了面板 RST、触摸 RST、功放使能、面板 CS（Task 18b 再审查 N7）这四个引脚。
2. 自动熄屏后短按电源键唤醒，确认屏幕不会先闪亮一下又熄灭；电源键熄屏后用音量键唤醒，
   确认下一次电源键短按能正常关屏——验证 C1 的修复（PowerManager 是屏幕状态的唯一真相
   来源）在真机上同样成立。
3. 开着电源键确认对话框（长按电源键）的状态下，等待自动熄屏触发，再分别用短按/长按
   电源键唤醒——确认两种按法都能唤醒屏幕（对话框可能仍留在界面上），不会因为"唤醒"
   和"确认对话框"两套逻辑抢着处理同一次按键而出现异常——验证 N2 的修复
   （`PowerKeyController::on_short()`/`on_long()` 现在总是先检查"屏幕是否已经是
   关的"，唤醒优先于确认对话框逻辑）。
4. IMU 掉电/恢复（Task 18b fix round 2 第一批附加范围）：待机前读一次自检屏幕上的
   IMU 数值，进入待机，唤醒后确认 IMU 数值恢复正常更新（不是卡在掉电前的最后一次
   读数，也不是全零/明显错误的数值）——验证 `qmi8658_set_power()` 唤醒时正确恢复了
   之前选定的 ODR，不是恢复成了驱动的默认值。
5. SD 卡探测暂停（Task 18b fix round 2 第二批附加范围）：插着 SD 卡待机若干分钟后
   唤醒，确认卡仍然在线（`online()` 没有被误判为掉线）；另外可以在待机期间物理
   拔掉/插入 SD 卡，确认热插拔检测在**唤醒后**才会被重新注意到（待机期间 worker
   已暂停，不会在待机过程中探测到这次插拔）——这是预期行为，不是 bug。
6. 待机电流的进一步拆解（可选，仅测量，不需要改代码）：如果上面测得的待机电流仍
   明显偏高，可以尝试在待机期间额外把触摸芯片 CST9217 的复位引脚（GPIO40）拉低
   （而不是像现在这样保持上电前的电平），单独测量这一项操作前后的电流差异，评估
   "触摸芯片复位期间功耗"是否值得作为后续一个单独的省电项——这只是一次性的测量
   方法，不代表现在的代码需要据此改动。）
