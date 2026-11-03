# Waveshare ESP32-S3-Touch-AMOLED-2.16

板名（`WISP_BOARD` / bmgr）：`waveshare_amoled_216`。板级代码：`boards/waveshare_amoled_216/`。

资料来源：MyOS 项目（MIT，作者 ShouChen，只读参考，**以其代码为准，它的文档有误**）、Waveshare 官方 BSP、ESP-Claw 中同款板的板级定义（Apache-2.0）。

## 主控与存储

- ESP32-S3R8：16 MB QIO flash，8 MB octal PSRAM（80 MHz）
- 分区表见 `boards/waveshare_amoled_216/partitions.csv`（Flash、PSRAM、控制台等板级 sdkconfig 见同目录的 `sdkconfig.defaults.board`）：

  | 分区 | 偏移 | 大小 |
  |---|---|---|
  | `nvs` | 0x9000 | 24 KB |
  | `otadata` | 0xF000 | 8 KB |
  | `phy_init` | 0x11000 | 4 KB |
  | `ota_0` | 0x20000 | 5 MB |
  | `ota_1` | 0x520000 | 5 MB |
  | `storage`（LittleFS） | 0xA20000 | 5.875 MB |

  网页资源包链接在应用镜像里，没有 `assets` 分区；`nvs` 和 `storage` 保持了早先分区表里的偏移和大小，所以换成这张表时设置和数据都保留。从旧分区表（两个 4 MB 应用槽加 2 MB 的 `assets` 分区）换过来时，以及第一次用上打开了回滚功能的引导程序时，都必须用 USB 完整烧录一次（`idf.py -p <PORT> flash`，写入引导程序、分区表、`otadata` 和 `ota_0`），之后才能在网页上传固件。
- 镜像大小（ESP-IDF v6.0.3，`idf.py size`）：发布版 3,169,136 B（`ota_0` 的 60.4%），调试版（`sdkconfig.debug`）3,467,968 B（66.1%）；预算为单个 OTA 槽的 80%。

## 外设与引脚

| 外设 | 芯片 | 接口与引脚 | 地址（7 位） |
|---|---|---|---|
| 屏幕 | CO5300，480×480 AMOLED，圆角半径 40 | QSPI：CS 12、SCLK 38、D0–D3 = 4/5/6/7、RST 39 | — |
| 触摸 | **CST9217**（MyOS 文档误写为 CST9220） | I2C；INT 11、RST 40 | 0x5A |
| 电源管理 | AXP2101 | I2C；PWR 键状态由 GPIO16 读出（**按下时为高电平**） | 0x34 |
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
- GPIO16 反映 PWR 键的状态：松开时被驱动为低电平，按住时为高电平（实测，官方资料只把它标为 `SYS_OUT`）。它不是 AXP2101 的中断输出，按普通按键读取即可，短按/长按由 `ButtonService` 计时判断，不需要读 AXP2101 的 IRQ 寄存器。实测时 AXP2101 的 PWRON 长按 IRQ 也恰好在按住 1 s 时置位，与 GPIO16 的电平一致。
- 按住 PWR 键数秒（AXP2101 的 OFFLEVEL）会触发芯片的硬件强制关机，与软件无关。
- 网页上传固件的已知限制（httpd 单任务，接受现状，见 `docs/architecture.md`"真机的网络"）：超过 8 MiB 的文件得到的是连接重置而不是 413（被提前拒绝时剩余部分超过 8 MiB 的上传都是如此）；两个浏览器同时上传时，第二个要等第一个传完才得到 409 `busy`；上传期间日志页暂停推送。

## 真机验收清单

汇总各子任务实现过程中记录下来的"需要用户在真机上执行"的手动验证项，以及 spec 第 10 节的验收标准。本板尚未上过真机，以下全部待测；"结果"栏留空，验收时填写实测结果（通过/不通过 + 简短说明），不通过的项回到对应实现修复。

每项前标注的来源，对应该检查来自哪部分实现；标 "spec 10.n" 的是设计文档第 10 节的验收标准原文。

| # | 项目（来自） | 怎么测 | 预期结果 | 结果 |
|---|---|---|---|---|
| 1 | 启动耗时（spec 10.1） | `idf.py -p <PORT> flash monitor`，从上电到自检界面出现用肉眼或串口时间戳计时；同时看 `WISP_BOOT_REPORT` 的 `boot_ms` | `boot_ms` < 3000 | |
| 2 | 启动报告/能力清单格式（spec 10.4，wisp_core::BootReport/DeviceRegistry::manifest()） | 串口查看 `WISP_BOOT_REPORT <json>`、`WISP_MANIFEST <json>` 两行 | 均为合法单行 JSON；设备状态符合预期（没插 SD 卡时 `storage.sd` 报缺失是正常的） |  |
| 3 | IMU 无磁力计（spec 10.4） | 查看 `WISP_MANIFEST` 中 `imu.main.caps` | `magnetometer` 为 `false` |  |
| 4 | Display 能力清单（屏幕驱动：esp_display） | 查看 `WISP_MANIFEST` 中 `display.main` | `model` 为 `CO5300`；`caps` 含 `width:480 height:480 shape:"rounded_rect" corner_radius:40 panel:"amoled" brightness:true area_align:2`；`safe_area` 为 `{12,12,467,467}` |  |
| 5 | Touch 型号（触摸驱动：bmgr dev_lcd_touch） | 查看 `WISP_MANIFEST` 中 `touch.main` | `model` 为 `CST9217` |  |
| 6 | 屏幕颜色（屏幕驱动：esp_display 字节序） | 自检界面显示纯色色块或背景，目视核对纯红、纯蓝 | 红蓝不互换、不发花；如互换说明字节序判断有误 |  |
| 7 | 四角不裁切（spec 8，10.2） | 目视自检界面四角 | 内容不被圆角裁掉 |  |
| 8 | 触摸坐标方向（触摸驱动：esp_display_touch 坐标映射） | 手指依次点触屏幕四角 | 左上角坐标接近 (0,0)，右下角接近 (479,479)；不镜像、不 XY 互换 |  |
| 9 | 局部刷新无错位（屏幕驱动：esp_display flush()/area 对齐） | 上下滚动自检设备列表 | 不出现条纹或错位 |  |
| 10 | 亮度初始化（屏幕驱动：esp_display set_brightness()） | 对比刚上电瞬间与自检界面稳定后的亮度 | 启动后设为 200，应比初始化时的 0xFF 略暗 |  |
| 11 | IMU 跟随倾斜（spec 10.2） | 倾斜设备，看自检界面小球；水平放置看 `az` | 小球平稳跟随倾斜；水平时 `az` 接近 1.0g |  |
| 12 | 陀螺仪数值合理（drivers/qmi8658，新写寄存器配置，重点核实） | 静止与转动设备，看自检界面 `gx/gy/gz` | 静止时接近 0 dps；转动时方向与幅度符合直觉；若轴向颠倒或量程异常，检查 `drivers/qmi8658/qmi8658.c` 的 CTRL3 FS/ODR 编码 |  |
| 13 | 电量与充电状态更新（spec 10.2，drivers/axp2101） | 看自检界面电量百分比和"充电中/未充电"状态；插拔 USB 电源；也可以看串口上 `power_monitor` 的 `power.charging.changed` 日志行（见下面第 35 项） | 百分比在合理范围；插拔后充电状态在 1 秒内更新 |  |
| 14 | 三个按键事件（spec 10.2，wisp_core::ButtonService） | 分别短按/长按 power、vol_up、vol_down | 自检界面均有对应事件提示 |  |
| 15 | 电源键短按（spec 10.3） | 亮屏时短按电源键，再短按一次 | 第一次灭屏（进入待机），第二次重新点亮 |  |
| 16 | 电源键长按关机（spec 10.3，drivers/axp2101 power_off，wisp_app::PowerKeyController） | 亮屏时按住电源键约 1 秒（不松手） | 按住时即弹出关机确认框；开机时为开机而按住的那一次不会触发；确认后整机断电（用电流表或指示灯判断，不能只看屏幕熄灭） |  |
| 17 | 关机状态开机（spec 10.3） | 关机状态下长按电源键 | 重新开机 |  |
| 18 | RTC 持续计时（spec 10.2，drivers/pcf85063） | 看启动日志是否有 "system clock set from clock.main RTC"；断电（拔 USB 和电池）重启后再看一次时间。如果 RTC 从未设置过（芯片第一次通电），`get_unix()` 会失败，日志只有一条 `WLOG_W`，系统时间不会被设置——此时需要临时在 `board.cpp` 的 `init_clock` 成功之后插入一行 `clock.value()->set_unix(<当前 UTC 时间戳>)` 写入一次当前时间，重新编译烧录后再验证本项，验证完成后删除这段临时代码 | 日志出现 "system clock set from clock.main RTC"；重启后时间连续，不跳回 1970 或固定值 |  |
| 19 | 充电电流曲线（drivers/axp2101 充电参数配置） | 用一块电量明显偏低的电池，经 USB 功率计供电，观察充电过程电流；同时看启动日志是否出现 `power.main: charging configuration failed: ...`（`board.cpp` 的 `WLOG_W`） | 恒流阶段稳定在约 400mA；接近 4.2V 时电流渐降；充满后 `charging` 翻 `false`，电流降到维持量级，不持续大电流过充；不出现 `charging configuration failed` 日志（出现则说明 `axp2101_charging_config_status()` 返回非 `ESP_OK`，充电参数可能停留在芯片出厂默认值） |  |
| 20 | 音频/存储设备状态（wisp_port_esp::EspAudio/EspStorage） | 看 `WISP_BOOT_REPORT` 中 `audio_out.main`/`audio_in.main`/`storage.int`/`storage.sd` | 前三者为 `ok`；`storage.sd` 视是否插卡为 `ok` 或 `missing`（"no card"） |  |
| 21 | 开机无爆音（屏幕驱动上电顺序，wisp_port_esp::EspAudio 初始化） | 接好喇叭，听开机瞬间 | 自检界面出现前无可闻的啪声/爆音 |  |
| 22 | 录音回放（spec 10.2，wisp_app::loopback） | 自检界面触发"录音回放"按钮 | 录 3 秒后播放，音量合适，声音正确，不失真、不卡顿、不静音 |  |
| 23 | 存储容量与 SD 热插拔（spec 10.2，wisp_port_esp::EspStorage/sd_retry） | 看自检界面内置 Flash/SD 卡容量；运行中插入/拔出 SD 卡 | 容量数字正确；插入约 2 秒内上线，不需要重启；拔出后几秒内被检测为离线 |  |
| 24 | NVS 设置持久化（wisp_port_esp::NvsSettingsBackend） | 改一项会在启动时读取的设置（如亮度），整机断电重启（不是软重启） | 修改值在重启后仍生效 |  |
| 25 | 内部存储降级路径（可选，wisp_port_esp::EspStorage 降级路径） | 如方便的话，人为让 `storage.int` 挂载失败 | 系统仍以降级/只读模式启动，不中止 |  |
| 26 | 待机进入/退出与唤醒（spec 6.7/10.3，wisp_app::PowerManager/PowerKeyController） | 亮屏时短按电源键进入待机；按任意键唤醒；再测试：电源键确认框打开时等待自动熄屏触发，分别用短按/长按唤醒 | 待机后任意键均可唤醒；确认框场景下两种按法都能唤醒（对话框可能仍留在界面上），不会因为"唤醒"和"确认框"逻辑冲突而异常 |  |
| 27 | 唤醒延迟（spec 10.5） | 从按电源键到面板实际重新点亮计时（秒表或逻辑分析仪） | ≤ 300 ms（见下方"待机与省电"一节的 `DcsSleep` 估算，约 200 ms，仅为估算） |  |
| 28 | 待机电流（spec 10.5） | 见下方"验收第 5 条"的详细测量方法 | 待机平均电流 ≤ 亮屏静止平均电流的 1/10 |  |
| 29 | 多次浅睡眠后无异常（wisp_app::PowerManager 轻睡眠） | 待机 10 分钟以上（确保发生过多次自动浅睡眠）后唤醒 | 面板无花屏、亮度未被意外重置；触摸正常响应；扬声器无异常噗声或底噪（见下方"验收第 5 条"第 1 条） |  |
| 30 | IMU 掉电恢复（wisp_app::PowerManager 待机，drivers/qmi8658） | 待机前记一次 IMU 读数，唤醒后再看 | 读数恢复正常更新，不是待机前最后一次读数，也不是全零/明显错误值（见"验收第 5 条"第 4 条） |  |
| 31 | SD 探测暂停（wisp_port_esp::sd_retry 待机期间暂停探测） | 插卡待机若干分钟后唤醒；可选在待机期间物理插拔 SD 卡 | 唤醒后 `online()` 仍正确；待机期间的插拔只在唤醒后才被发现（预期行为，不是 bug，见"验收第 5 条"第 5 条） |  |
| 32 | 触摸复位引脚功耗（可选测量，待机功耗评估） | 待机期间尝试拉低触摸复位引脚（GPIO40），对比前后电流 | 仅用于评估是否值得做后续省电项，不代表现在代码需要改动（见"验收第 5 条"第 6 条） |  |
| 33 | UI 帧率基准（spec 10 性能基准） | 手指持续滚动自检列表 10 秒，看串口 `WISP_PERF` | fps ≥ 30；把 fps/cpu/flush_ms 三个数值记入下方"性能基准"一节 |  |
| 34 | DFS 不拖累帧率（firmware/main.cpp apply_pm_config，DFS/light sleep） | 对比开启/关闭 `CONFIG_PM_ENABLE` 前后的 `WISP_PERF` | 开启 DFS 后帧率仍不低于上一项的基准 |  |
| 35 | `power.charging.changed` 事件（spec 6.3，wisp_core::PowerMonitor） | 插拔 USB 电源，看串口上 `power_monitor` 的 `power.charging.changed: charging=... usb_present=... battery_percent=...` 日志行 | 插拔后 1 秒内出现该日志行，`charging`/`usb_present` 的值与实际状态一致 |  |
| 36 | `power.battery.low` 事件（spec 6.3，wisp_core::PowerMonitor） | 电量明显偏低时观察串口日志；子项目 1 还没有 CLI/REST 能改 Settings，想快速触发可以临时在 `components/wisp_app/src/app.cpp` 里 `core::Settings power_settings(*impl_->platform.settings, "power");`（约第 102 行）之后加一行 `power_settings.set("low_batt_pct", int64_t{100});`，重新编译固件验证完后删掉这行（不要提交） | 只出现一次 `power_monitor` 的 `power.battery.low: battery_percent=... (threshold=...)` 日志行（不持续重复）；电量回升超过阈值+5 后再降回阈值以下会再出现一次；没有接电池、只接 USB 供电时不出现该日志 |  |
| 37 | 新分区表首次完整烧录（connectivity spec 3.2） | 在已经有设置和数据（配过 Wi-Fi、配对过浏览器、改过亮度、`/int` 里有文件）的板子上执行 `idf.py -p <PORT> flash` | 正常启动；串口没有分区相关报错；Wi-Fi、配对、亮度等设置和 `/int` 数据都还在；管理后台"系统"页的固件卡片显示版本、`waveshare_amoled_216` 和运行分区 `ota_0` |  |
| 38 | 网页上传并确认（connectivity spec 6.5） | 改一点代码再构建，在"系统"页上传 `firmware/build/wisp.bin`；设备上弹出确认框后点"是" | 进度条走完后网页提示"请在设备上确认"；确认框显示旧版本 → 新版本；点"是"后设备重启，网页提示正在运行新版本；固件卡片的运行分区变成 `ota_1`；串口没有回滚日志 |  |
| 39 | 网页上传后拒绝 | 再上传一次，设备上点"否"；再上传一次，2 分钟内不操作 | 两次网页都提示"已在设备上拒绝"；设备不重启，版本与运行分区不变 |  |
| 40 | 拒绝其他板子的镜像 | 用 `-DWISP_BOARD=<另一块板>` 构建（或上传任何非 Wisp 的 ESP-IDF 应用 `.bin`）后上传 | 网页立即提示"这个固件是为其他开发板或芯片构建的"（400 `wrong_board`），设备上不弹确认框，Flash 未被写入（再上传一次正确镜像仍然正常） |  |
| 41 | 启动失败自动回滚（`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`） | 临时在 `firmware/main/main.cpp` 的 `app_main()` 开头加一行 `abort();` 构建（不要提交），上传并在设备上确认 | 新镜像崩溃复位后，引导程序回到旧镜像；网页提示"设备重启后运行的仍是旧版本，已回滚"；之后上传正常镜像仍能装上 |  |
| 42 | 发布版帧率（全局 `-Os`、渲染路径 `-O2`） | 用默认（发布版）配置构建烧录，重复第 33 项 | `WISP_PERF` 的 fps ≥ 30，不低于第 33 项记下的基准。达不到时先试把 `esp_driver_spi`（`esp_lcd_panel_io` 下面的 QSPI 传输层，目前按 `-Os` 编译）加进 `firmware/CMakeLists.txt` 的 `-O2` 匹配规则 |  |
| 43 | 上传期间的日志推送 | 上传固件时打开"日志"页 | 上传期间日志暂停推送（httpd 只有一个任务，被上传请求占着），上传结束后恢复推送，WebSocket 不断开 |  |

## 待机与省电（spec 6.7）

- **屏幕睡眠（CO5300）——改用标准 MIPI DCS 睡眠，不走厂商深度待机（经用户确认采用）**：
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
  - **亮度丢失**：无论哪种睡眠方式，唤醒后面板的亮度寄存器都可能被重置（本板
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
- **自动浅睡眠期间 GPIO 会被隔离**：`CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`
  在 ESP32-S3 上会自动选中 `CONFIG_PM_SLP_DISABLE_GPIO`（`ESP_SLEEP_GPIO_RESET_WORKAROUND`
  的 Kconfig 默认依赖；生成的 `firmware/sdkconfig` 里也确实是 `y`）——每次自动浅睡眠时，
  **所有** GPIO 的方向、电平和上下拉都会被禁用（浮空），直到浅睡眠结束才恢复。面板 RST
  （GPIO39）、触摸 RST（GPIO40）、功放使能（GPIO46）、**面板 SPI 的 CS（GPIO12，
  `board_devices.yaml` 的 `io_spi_config.cs_gpio_num`）**这四个必须
  在睡眠期间保持电平的输出如果浮空：面板/触摸可能被意外复位（配合上面的 `DcsSleep` 会
  直接抵消它要避免的那次重新初始化），CS 浮空还可能让面板在浅睡眠期间误采样到总线上的
  随机电平、把无关的 SPI 数据当成发给自己的命令，功放浮空可能有爆音或漏电风险。
  `boards/waveshare_amoled_216/board.cpp` 的 `hold_outputs_during_light_sleep()`（在板级
  `init()` 的最后调用）对这四个引脚分别调用 `gpio_sleep_sel_dis()`，让它们在浅睡眠期间
  继续保持配置好的电平/方向。**待验证**：真机上连续经历多次自动浅睡眠/唤醒循环（例如
  待机 10 分钟以上，确保期间发生过几十次自动浅睡眠-唤醒）后，确认面板画面正常（没有
  花屏、没有被意外复位过的痕迹，例如亮度被重置——见上面"亮度丢失"一条）、触摸依旧能正常
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
- **后台仍在运行、会限制浅睡眠深度/频率的任务**（下面按"待机期间的行为"分类；记录现状，
  不代表现在就要再去改动）：
  - **已在待机期间完全暂停、只在真正被 `give()` 唤醒时才醒的**：
    - 自检屏幕采样任务的 IMU 读取分支（`SelfCheck::sample_loop()`，原本以
      `kSampleTaskPeriodMs` = 20 ms 的周期读取 IMU）：订阅了 `power.state`，进入待机
      后完全停止读 IMU——这是系统里最快的周期性唤醒源（20 ms，比 ButtonService 的
      50 ms 还快），待机时完全消除。`PowerManager::exit_standby()` 唤醒时立即
      `give()` 恢复采样。电源状态/RTC 的"慢采样"分支**不**完全停止，见下一条分类。
    - `wisp::hal::Imu::set_power(false)`：`PowerManager::enter_standby()` 关闭
      QMI8658 的加速度计/陀螺仪使能位（`CTRL7`），待机期间芯片本身也不再采样，
      `exit_standby()` 时恢复到之前选定的 ODR。
    - `EspSdStorage` 的 SD 卡探测 worker（`components/wisp_port_esp/dev/esp_storage.cpp`）
      原本固定以 `kWorkerPollMs` = 100 ms 的粒度轮询（`Mounted` 态下每 ~1 s 探测一次卡
      是否还在，`Unmounted` 态下每 2 s 重试一次 mount，但循环本身每 100 ms 就醒一次
      检查状态/`worker_running_`，即以 10 Hz 唤醒 CPU）。待机期间关闭的方式是给它加了
      `set_standby()`（经 `StorageManager::set_standby()` 转发，`PowerManager` 在
      `enter_standby()`/`exit_standby()` 时调用）：worker 完全阻塞在自己的信号量上
      （无超时的 `os::kForever` 等待——`set_standby(false)` 和析构都保证一定会
      `give()`，不存在漏收信号的情形），不再探测/重试；`online()`/`capacity()` 不受
      影响，一直返回缓存的上一次状态，不会在待机期间卡住或报错。`Mounted` 态下探测
      间隔之间的等待也会检查待机标志，一旦在等待期间进入待机就直接跳过这次探测，不会
      多发一次 CMD13。
  - **降频到约 30 s 一次，而不是完全停止（按 spec 6.7 字面要求）**：
    - 自检屏幕采样任务的电源状态/RTC"慢采样"分支（原本以 `kSlowSampleIntervalMs`
      = 1000 ms 读一次）：待机期间改为每 `kStandbySlowSampleMs` = 30000 ms 读一次
      （阻塞在 `standby_wake_` 上，超时就读一次，被 `give()` 唤醒就直接跳过），退出
      待机时同样立即 `give()` 恢复到原来 1 s 一次的节奏。这个分支读到的值只用于自检
      界面显示（界面本身已经暂停渲染）。
    - `wisp::core::PowerMonitor` 自己的采样定时器（`"wisp.pwrmon"`，见 spec 6.3/6.7）：
      亮屏时 1 s、待机时 30 s 轮询一次 `power.main`（每次最多 5 次 I2C 读；
      `low_batt_pct` 阈值做了缓存，每 20 次采样（或每次 `start()`）才重读 1 次
      NVS，不是每次采样都读），发布 `power.charging.changed`/
      `power.battery.low`。这是一个真正的消费者：`power.battery.low` 在电量降到
      Settings `power`/`low_batt_pct`（默认 15）以下时发布一次（带滞后重新武装），
      两个事件发布时都会打一行 `WLOG_I`，串口上可以直接观察到。
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
      `SelfCheck::emit_perf_line()`）**不会**被上面的 `power.state` 订阅暂停——它和
      采样任务是两个独立的 `wisp::os::Timer`/任务。待机期间它仍然每 5 s 唤醒一次、
      照常打印一行 `WISP_PERF`，只是其中的 `fps`/`frames`/`flush_ms` 会是 0（因为
      `UiRuntime` 暂停后没有任何帧被渲染）。这属于预期行为，不是 bug：待机期间的
      `WISP_PERF` 日志本身就是用来确认"确实没有渲染"的信号之一；真机验收时如果
      发现待机日志里 `WISP_PERF` 的 `fps` 不是 0，才说明暂停渲染这个处理出现了回归。
    - `core::ButtonService` 待机时以 50 ms 轮询一次按键（`PowerManager::enter_standby()`
      放宽的间隔）；`ui::UiRuntime` 暂停后**不再**自己周期性醒来——它的待机循环现在
      无限等待 `post()` 任务，没有事可做时完全不会主动醒来。
  - 目前，`core::ButtonService` 的 **50 ms** 轮询是系统里最短的周期性唤醒源（自检
    采样任务的 IMU 分支 20 ms 已被消除；电源/RTC 慢采样分支和 `wisp.pwrmon` 都降到
    了 30 s；SD worker 待机期间完全不主动醒来）。以上都只是"周期性唤醒 CPU 一下就
    继续睡"，不会阻止自动浅睡眠本身发生（没有常驻的 `ESP_PM_NO_LIGHT_SLEEP`/
    `ESP_PM_CPU_FREQ_MAX` 锁），但会把浅睡眠的最长连续时长限制在 `ButtonService` 的
    50 ms 量级，略微拉高平均功耗。真机测得的待机电流如果明显高于预期，这些仍是优先
    排查的来源。

## 性能基准

（真机验收后填写：滚动自检列表时的 fps / cpu / flush_ms。另外对比开启动态调频（DFS，
`firmware/sdkconfig.defaults` 的 `CONFIG_PM_ENABLE`）前后的 `WISP_PERF`，确认 DFS 本身
没有把渲染路径的帧率拖到 spec/计划约定的基准以下。）

## 验收第 5 条：待机耗电实测

（真机验收后填写。测量方法：拔掉 USB 数据线，只用电池供电，在电池正极与电池之间串联
一个电流表（或用带电流检测的电源分析仪）；分别记录"亮屏静止"和"短按电源键熄屏待机"
下的平均电流，待机电流应不超过亮屏的 1/10；另外记录从短按电源键唤醒到屏幕实际点亮的
耗时——见上面"待机与省电"一节，改用 `DcsSleep` 后估算约 200 ms，应能满足 300 ms 的
目标，但这只是估算，需要实测确认。

另外几项验证：
1. 待机若干分钟（确保发生过自动浅睡眠）后，用音量键或电源键唤醒，确认触摸依旧正常
   响应、扬声器没有异常噗声或持续底噪——验证 `gpio_sleep_sel_dis()` 处理是否真的
   覆盖了面板 RST、触摸 RST、功放使能、面板 CS 这四个引脚。
2. 自动熄屏后短按电源键唤醒，确认屏幕不会先闪亮一下又熄灭；电源键熄屏后用音量键唤醒，
   确认下一次电源键短按能正常关屏——验证 PowerManager 是屏幕状态的唯一真相来源，在
   真机上同样成立。
3. 开着电源键确认对话框（长按电源键）的状态下，等待自动熄屏触发，再分别用短按/长按
   电源键唤醒——确认两种按法都能唤醒屏幕（对话框可能仍留在界面上），不会因为"唤醒"
   和"确认对话框"两套逻辑抢着处理同一次按键而出现异常（`PowerKeyController::
   on_short()`/`on_long()` 总是先检查"屏幕是否已经是关的"，唤醒优先于确认对话框
   逻辑）。
4. IMU 掉电/恢复：待机前读一次自检屏幕上的 IMU 数值，进入待机，唤醒后确认 IMU 数值
   恢复正常更新（不是卡在掉电前的最后一次读数，也不是全零/明显错误的数值）——验证
   `qmi8658_set_power()` 唤醒时正确恢复了之前选定的 ODR，不是恢复成了驱动的默认值。
5. SD 卡探测暂停：插着 SD 卡待机若干分钟后唤醒，确认卡仍然在线（`online()` 没有被
   误判为掉线）；另外可以在待机期间物理拔掉/插入 SD 卡，确认热插拔检测在**唤醒后**
   才会被重新注意到（待机期间 worker 已暂停，不会在待机过程中探测到这次插拔）——这是
   预期行为，不是 bug。
6. 待机电流的进一步拆解（可选，仅测量，不需要改代码）：如果上面测得的待机电流仍
   明显偏高，可以尝试在待机期间额外把触摸芯片 CST9217 的复位引脚（GPIO40）拉低
   （而不是像现在这样保持上电前的电平），单独测量这一项操作前后的电流差异，评估
   "触摸芯片复位期间功耗"是否值得作为后续一个单独的省电项——这只是一次性的测量
   方法，不代表现在的代码需要据此改动。）
