# Waveshare ESP32-S3-Touch-AMOLED-2.16

板名（`WISP_BOARD` / bmgr）：`waveshare_esp32_s3_touch_amoled_2_16`。板级代码：`boards/waveshare_esp32_s3_touch_amoled_2_16/`。

资料来源：MyOS 项目（MIT，作者 ShouChen，只读参考，**以其代码为准，它的文档有误**）、Waveshare 官方 BSP、ESP-Claw 中同款板的板级定义（Apache-2.0）、[官方产品页](https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-2.16/)。

## 主控与存储

- ESP32-S3R8：16 MB QIO flash，8 MB octal PSRAM（80 MHz）
- 分区表见 `boards/waveshare_esp32_s3_touch_amoled_2_16/partitions.csv`（Flash、PSRAM、控制台等板级 sdkconfig 见同目录的 `sdkconfig.defaults.board`）：

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
| 屏幕 | CO5300，480×480 AMOLED，圆角半径 70（实测，见下方"已知勘误与注意事项"） | QSPI：CS 12、SCLK 38、D0–D3 = 4/5/6/7、RST 39 | — |
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
- 屏幕的局部刷新区域需要按 2 像素对齐（`area_align = 2`）。
- 面板实际圆角半径比厂商资料给的 40 大得多：在自检界面之外另写一个校准场景，在屏幕四角画出半径 40、50、60、70、80 的四分之一圆弧（圆心在 `(R,R)`），逐一目视核对对角线方向上哪一段还能看到、哪一段已经被实际的圆角裁掉。结果是 60 px 的弧在对角线方向完全不可见，70 px 的弧只有一部分可见，由此反推可见边界在对角线上离角大约 20 px，对应半径约 70（`board.cpp` 的 `kCornerRadius`）。按厂商数据表的 40 来算安全区会裁掉文字或控件的角，新板子务必用这种画弧比对的方法实测，不要直接用数据表的值（见 `docs/porting-guide.md`）。
- AXP2101 初始化时不要改动任何电源轨的电压或开关。
- GPIO16 反映 PWR 键的状态：松开时被驱动为低电平，按住时为高电平（实测，官方资料只把它标为 `SYS_OUT`）。它不是 AXP2101 的中断输出，按普通按键读取即可，短按/长按由 `ButtonService` 计时判断，不需要读 AXP2101 的 IRQ 寄存器。实测时 AXP2101 的 PWRON 长按 IRQ 也恰好在按住 1 s 时置位，与 GPIO16 的电平一致。
- 按住 PWR 键数秒（AXP2101 的 OFFLEVEL）会触发芯片的硬件强制关机，与软件无关。
- 网页上传固件的已知限制（httpd 单任务，接受现状，见 `docs/architecture.md`"真机的网络"）：超过 8 MiB 的文件得到的是连接重置而不是 413（被提前拒绝时剩余部分超过 8 MiB 的上传都是如此）；两个浏览器同时上传时，第二个要等第一个传完才得到 409 `busy`；上传期间日志页暂停推送。

## 真机验收清单

汇总各子任务实现过程中记录下来的"需要用户在真机上执行"的手动验证项，以及 spec 第 10 节的验收标准。本板已上真机，未填结果的项待测；"结果"栏留空，验收时填写实测结果（通过/不通过 + 简短说明），不通过的项回到对应实现修复。

每项前标注的来源，对应该检查来自哪部分实现；标 "spec 10.n" 的是设计文档第 10 节的验收标准原文。

| # | 项目（来自） | 怎么测 | 预期结果 | 结果 |
|---|---|---|---|---|
| 1 | 启动耗时（spec 10.1） | `idf.py -p <PORT> flash monitor`，从上电到自检界面出现用肉眼或串口时间戳计时；同时看 `WISP_BOOT_REPORT` 的 `boot_ms` | `boot_ms` < 3000 | |
| 2 | 启动报告/能力清单格式（spec 10.4，wisp_core::BootReport/DeviceRegistry::manifest()） | 串口查看 `WISP_BOOT_REPORT <json>`、`WISP_MANIFEST <json>` 两行 | 均为合法单行 JSON；设备状态符合预期（没插 SD 卡时 `storage.sd` 报缺失是正常的） |  |
| 3 | IMU 无磁力计（spec 10.4） | 查看 `WISP_MANIFEST` 中 `imu.main.caps` | `magnetometer` 为 `false` |  |
| 4 | Display 能力清单（屏幕驱动：esp_display） | 查看 `WISP_MANIFEST` 中 `display.main` | `model` 为 `CO5300`；`caps` 含 `width:480 height:480 shape:"rounded_rect" corner_radius:70 panel:"amoled" brightness:true area_align:2 rgb565_be:true`；`safe_area` 为 `{24,24,455,455}` |  |
| 5 | Touch 型号（触摸驱动：bmgr dev_lcd_touch） | 查看 `WISP_MANIFEST` 中 `touch.main` | `model` 为 `CST9217` |  |
| 6 | 屏幕颜色（字节序：esp_display 发布 caps `rgb565_be`，wisp_ui 据此直接按高字节在前渲染） | 自检界面显示纯色色块或背景，目视核对纯红、纯蓝 | 红蓝不互换、不发花；如互换说明字节序判断有误 |  |
| 7 | 四角不裁切（spec 8，10.2） | 目视自检界面四角 | 内容不被圆角裁掉 |  |
| 8 | 触摸坐标方向（触摸驱动：esp_display_touch 坐标映射） | 手指依次点触屏幕四角 | 左上角坐标接近 (0,0)，右下角接近 (479,479)；不镜像、不 XY 互换 |  |
| 9 | 局部刷新无错位（屏幕驱动：esp_display flush_async()/area 对齐，两块缓冲轮流传输） | 上下快速滚动自检设备列表 | 不出现条纹、错位或某一段显示成上一帧/另一段的内容 |  |
| 10 | 亮度初始化（屏幕驱动：esp_display set_brightness()） | 对比刚上电瞬间与自检界面稳定后的亮度 | 启动后设为 200，应比初始化时的 0xFF 略暗 |  |
| 11 | IMU 跟随倾斜（spec 10.2） | 倾斜设备，看自检界面小球；水平放置看 `az` | 小球平稳跟随倾斜；水平时 `az` 接近 1.0g |  |
| 12 | 陀螺仪数值合理（drivers/qmi8658，新写寄存器配置，重点核实） | 静止与转动设备，看自检界面 `gx/gy/gz` | 静止时接近 0 dps；转动时方向与幅度符合直觉；若轴向颠倒或量程异常，检查 `drivers/qmi8658/qmi8658.c` 的 CTRL3 FS/ODR 编码 |  |
| 13 | 电量与充电状态更新（spec 10.2，drivers/axp2101） | 看自检界面电量百分比和"充电中/未充电"状态；插拔 USB 电源；也可以看串口上 `power_monitor` 的 `power.charging.changed` 日志行（见下面第 35 项） | 百分比在合理范围；插拔后充电状态在 1 秒内更新 |  |
| 14 | 三个按键事件（spec 10.2，wisp_core::ButtonService） | 分别短按/长按 power、vol_up、vol_down | 自检界面均有对应事件提示 |  |
| 15 | 电源键短按（spec 10.3） | 亮屏时短按电源键，再短按一次 | 第一次灭屏（进入待机），第二次重新点亮 |  |
| 16 | 电源键长按关机（spec 10.3，drivers/axp2101 power_off，wisp_app::PowerKeyController） | 亮屏时按住电源键约 1 秒（不松手） | 按住时即弹出关机确认框；开机时为开机而按住的那一次不会触发；确认后整机断电（用电流表或指示灯判断，不能只看屏幕熄灭） |  |
| 17 | 关机状态开机（spec 10.3） | 关机状态下长按电源键 | 重新开机 |  |
| 18 | RTC 持续计时（spec 10.2，drivers/pcf85063） | 看启动日志是否有 "system clock set from clock.main RTC"；断电（拔 USB 和电池）重启后再看一次时间。如果 RTC 从未设置过（芯片第一次通电），`get_unix()` 会失败，日志只有一条 `WLOG_W`，系统时间不会被设置——此时先连上 Wi-Fi：网络校时（连接 spec 5.6）成功后日志出现 `time: synced from <服务器>: unix <秒数>`，同时写入 RTC，再重启验证本项 | 日志出现 "system clock set from clock.main RTC"；重启后时间连续，不跳回 1970 或固定值 |  |
| 19 | 充电电流曲线（drivers/axp2101 充电参数配置） | 用一块电量明显偏低的电池，经 USB 功率计供电，观察充电过程电流；同时看启动日志是否出现 `power.main: charging configuration failed: ...`（`board.cpp` 的 `WLOG_W`） | 恒流阶段稳定在约 400mA；接近 4.2V 时电流渐降；充满后 `charging` 翻 `false`，电流降到维持量级，不持续大电流过充；不出现 `charging configuration failed` 日志（出现则说明 `axp2101_charging_config_status()` 返回非 `ESP_OK`，充电参数可能停留在芯片出厂默认值） |  |
| 20 | 音频/存储设备状态（wisp_port_esp::EspAudio/EspStorage） | 看 `WISP_BOOT_REPORT` 中 `audio_out.main`/`audio_in.main`/`storage.int`/`storage.sd` | 前三者为 `ok`；`storage.sd` 视是否插卡为 `ok` 或 `missing`（"no card"） |  |
| 21 | 开机无爆音（屏幕驱动上电顺序，wisp_port_esp::EspAudio 初始化） | 接好喇叭，听开机瞬间 | 自检界面出现前无可闻的啪声/爆音 |  |
| 22 | 录音回放（spec 10.2，wisp_app::loopback） | 自检界面点"Mic test"按钮（中文界面为"录音回放"） | 录 3 秒后播放，音量合适，声音正确，不失真、不卡顿、不静音 |  |
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
| 33 | UI 帧率基准（spec 10 性能基准） | 手指持续滚动自检列表 10 秒，看串口 `WISP_PERF` | 目标 fps ≥ 30。验收线（用户 2026-11-04 裁定，见 foundation spec 第 10 节"已记录的偏差"）：不低于下方"性能基准"一节记下的基准：关热点 22.7 fps，开热点（配网卡片显示时）也不低于关热点的 22.7 fps（2026-11-05 改，28.2 不再作为验收线，原因见该节末尾）；把 fps/cpu/flush_ms 三个数值记入该节 |  |
| 34 | DFS 不拖累帧率（firmware/main.cpp apply_pm_config，DFS/light sleep） | 对比开启/关闭 `CONFIG_PM_ENABLE` 前后的 `WISP_PERF` | 开启 DFS 后帧率仍不低于上一项的基准 |  |
| 35 | `power.charging.changed` 事件（spec 6.3，wisp_core::PowerMonitor） | 插拔 USB 电源，看串口上 `power_monitor` 的 `power.charging.changed: charging=... usb_present=... battery_percent=...` 日志行 | 插拔后 1 秒内出现该日志行，`charging`/`usb_present` 的值与实际状态一致 |  |
| 36 | `power.battery.low` 事件（spec 6.3，wisp_core::PowerMonitor） | 电量明显偏低时观察串口日志；子项目 1 还没有 CLI/REST 能改 Settings，想快速触发可以临时在 `components/wisp_app/src/app.cpp` 里 `core::Settings power_settings(*impl_->platform.settings, "power");`（约第 102 行）之后加一行 `power_settings.set("low_batt_pct", int64_t{100});`，重新编译固件验证完后删掉这行（不要提交） | 只出现一次 `power_monitor` 的 `power.battery.low: battery_percent=... (threshold=...)` 日志行（不持续重复）；电量回升超过阈值+5 后再降回阈值以下会再出现一次；没有接电池、只接 USB 供电时不出现该日志 |  |
| 37 | 新分区表首次完整烧录（connectivity spec 3.2） | 在已经有设置和数据（配过 Wi-Fi、配对过浏览器、改过亮度、`/int` 里有文件）的板子上执行 `idf.py -p <PORT> flash` | 正常启动；串口没有分区相关报错；Wi-Fi、配对、亮度等设置和 `/int` 数据都还在；管理后台"系统"页的固件卡片显示版本、`waveshare_esp32_s3_touch_amoled_2_16` 和运行分区 `ota_0` |  |
| 38 | 网页上传并确认（connectivity spec 6.5） | 改一点代码再构建，在"系统"页上传 `firmware/build/wisp.bin`；设备上弹出确认框后点"是" | 进度条走完后网页提示"请在设备上确认"；确认框显示旧版本 → 新版本；点"是"后设备重启，网页提示正在运行新版本；固件卡片的运行分区变成 `ota_1`；串口没有回滚日志 |  |
| 39 | 网页上传后拒绝 | 再上传一次，设备上点"否"；再上传一次，2 分钟内不操作 | 两次网页都提示"已在设备上拒绝"；设备不重启，版本与运行分区不变 |  |
| 40 | 拒绝其他板子的镜像 | 用 `-DWISP_BOARD=<另一块板>` 构建（或上传任何非 Wisp 的 ESP-IDF 应用 `.bin`）后上传 | 网页立即提示"这个固件是为其他开发板或芯片构建的"（400 `wrong_board`），设备上不弹确认框，Flash 未被写入（再上传一次正确镜像仍然正常） |  |
| 41 | 启动失败自动回滚（`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`） | 临时在 `firmware/main/main.cpp` 的 `app_main()` 开头加一行 `abort();` 构建（不要提交），上传并在设备上确认 | 新镜像崩溃复位后，引导程序回到旧镜像；网页提示"设备重启后运行的仍是旧版本，已回滚"；之后上传正常镜像仍能装上 |  |
| 42 | 发布版帧率（全局 `-Os`、渲染路径 `-O2`） | 用默认（发布版）配置构建烧录，重复第 33 项 | `WISP_PERF` 的 fps 不低于第 33 项的验收线（关热点、开热点都是 22.7 fps；目标仍是 30）。`esp_driver_spi` 改用 `-O2` 已实测无收益（见该节），不用再试 |  |
| 43 | 上传期间的日志推送 | 上传固件时打开"日志"页 | 上传期间日志暂停推送（httpd 只有一个任务，被上传请求占着），上传结束后恢复推送，WebSocket 不断开 |  |
| 44 | 内部 RAM 最坏情况（AGENTS.md 第 5 节的预算） | 设备连上家里的 Wi-Fi 后在串口执行 `wifi ap on`，手机连上热点打开设置页；电脑经 STA 地址打开管理页的"日志"页（WebSocket），在"模型"页对一个真实的 HTTPS 端点做连通性测试，再上传一次固件；全程每隔几秒执行一次 `mem` | 日志里 `admin server listening on port 8900` 和 `captive redirect listening on port 80` 都出现，后面的 `internal RAM: ...` 行不是警告；`mem` 的 `internal free` 始终 ≥ 40 KB、`largest` ≥ 16 KB（`min` 是各内存区各自最低点之和，偏低，只能当下限看）；`wisp.ws`、`wisp.dns` 和端口 80 的 `httpd` 的栈在 `psram`，端口 8900 的 `httpd` 的栈在 `internal`；整个过程 STA 不掉线、日志里没有反复重连（RX BA 窗口只有 10，换路由器时也看一眼） | |
| 45 | 不透明卡片的遮挡裁剪（`wisp/ui/overlay.hpp`） | `wifi ap on` 让配网卡片显示，手指上下滚动自检列表；再执行 `wifi ap off` 关掉卡片 | 滚动时卡片周围和卡片上没有残影、撕裂或缺块；关掉卡片后，卡片原来位置下面的列表内容完整、与周围一致 | 用户 2026-11-04 确认开热点滚动无残影（之后修过渲染期间的探测和隐藏图层的处理，建议复看一次） |
| 46 | 配网二维码（按屏幕像素格式存，`net_cards.cpp` 的 `flatten_qr()`） | `wifi ap on` 后用手机相机扫配网卡片上的二维码 | 黑白没有反，手机能识别并提示连接热点 | 用户 2026-11-04 确认能扫 |
| 47 | 默认字体的 8 位字形缓存（`components/wisp_ui/src/a8_font.cpp`） | 看自检界面和配网卡片上的中、日、英文文字 | 与改动前一样：没有缺字、错字、发虚或错位 |  |
| 48 | 开 XIP 后的待机与唤醒（`CONFIG_SPIRAM_XIP_FROM_PSRAM`） | 开 XIP 的固件上重复第 26–29 项（待机进入与唤醒、唤醒延迟、待机电流、多次浅睡眠） | 结果与第 26–29 项的预期一致；浅睡眠唤醒后没有花屏、复位或卡死 |  |
| 49 | 网络校时（连接 spec 5.6，`net::TimeSyncService`、`wisp_port_esp` 的 `EspSntp`） | 连上 Wi-Fi 后看串口日志和自检界面"电源与时钟"卡片；再用 `mem` 看 `wisp.ntp` 任务；在管理后台"系统"页点"立即校时"；把 `ntp_servers` 改成 `down.invalid,ntp.aliyun.com` 再校时一次 | 拿到 IP 后几秒内出现 `time: synced from pool.ntp.org`（或回落到 `ntp.aliyun.com`）；卡片显示"Synced HH:MM"（本地时间）；`wisp.ntp` 栈在 PSRAM 且 high-water mark 余量足够（4096 字节栈），按实测调整 `NetWiring::kNtpStackBytes`；空闲时内部 RAM 不低于预算；第二次校时回落到 `ntp.aliyun.com` |  |
| 50 | 两种安装方式（scene spec 9.4.1） | 网页"场景"页上传 `counter` 的 zip；`python3 tools/wisp_scene.py --host <IP>:8900 push scenes/examples/todo --open`；从自检界面"场景"区块打开两者 | 都安装成功，推送输出显示声明的域名；列表点击即打开 | 2026-11-05：通过。API 推送与网页 zip 上传都安装成功（日志 `admin: scene upload started` 后场景正常安装/打开/关闭）。 |
| 51 | 示例功能与断电保留（scene spec 9.4.2） | 逐个打开 7 个示例；在待办加 3 条、记账加 2 条后拔电池和 USB 断电重启 | 功能正常；重启后数据仍在 |  |
| 52 | 返回手势与错误面板（scene spec 9.4.3） | 从圆角屏左边缘 16 px 以内起、向右滑动超过 24 px（任意角度，`ui::SystemLayer::kBackZoneWidth`/`kBackTriggerDx`，见 `components/wisp_ui/include/wisp/ui/system_layer.hpp`）；推送 `tests/scene/fixtures/broken` 并打开，点"返回"，再打开点"重载" | 手势可用；错误面板显示文件与行号（`main.lua:5`），返回与重载正常 |  |
| 53 | 长列表帧率与内存（scene spec 9.4.4） | 待办放 200 条，手指持续滚动 10 s，看 `WISP_PERF`；同时执行 `mem`；记下日志里 `slow message` 的最大耗时（GC 停顿） | fps 不低于自检界面基准（下方"性能基准"一节，关热点、开热点都是 22.7）；`internal free` ≥ 60 KB、`largest` ≥ 20 KB（`ram_budget.hpp` 的 `kIdleInternalFreeMin`/`kIdleInternalLargestMin`） | 2026-11-05：todo200 持续滚动 fps 22–26，偶尔掉到 14；`internal free` ≥ 84 KB，高于 60 KB 下限；`slow message open` 115–196 ms。短暂掉到 14 fps 低于基准，本次没有继续排查。 |
| 54 | 熄屏恢复与待机耗电（scene spec 9.4.5） | 番茄钟和时钟运行时熄屏 3 分钟再亮屏；按"验收第 5 条：待机耗电实测"一节的方法测待机电流，对比无场景时 | 剩余时间与表盘正确，`minute` 只补一次；待机电流与无场景时相当 |  |
| 55 | 未声明域名被拒（scene spec 9.4.6） | 临时把天气场景的 URL 改成未声明的主机后推送打开 | 回调收到 `permission_denied`，没有出站连接 |  |
| 56 | LVGL 堆在 PSRAM 后的帧率 | 不开场景，在最终固件上重复"UI 帧率基准"一行，不开热点与开热点各测一次（LVGL 堆改到 PSRAM 后已经在真机上测过一次，见下方"性能基准"一节的"LVGL 堆"小节） | `WISP_PERF` 的 fps 不开热点 ≥ 22.7、开热点也 ≥ 22.7（下方"性能基准"一节；开热点原来的 28.2 不再作为验收线，原因见该节末尾）；低于时停下来报告，不要自行改回 | 2026-11-05：不开热点 22–24 fps，达标（基准 22.7）；开热点 23–24 fps，当时按 28.2 的基准记为低于基准、待排查。验收线改为开热点 ≥ 22.7 之后，这个结果达标。 |
| 57 | 内部 RAM 最坏情况复测（mbedTLS 的分配全部改到 PSRAM 之后，`firmware/sdkconfig.defaults` 的 `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`；UI 任务栈也已从 8 KB 加到 10 KB） | 烧录包含这项改动的固件（改 `sdkconfig.defaults` 后要先删掉 `firmware/sdkconfig` 再构建，否则不生效；构建后 `firmware/sdkconfig` 里应是 `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`）。按第 44 项的同一场景操作：STA 连家里的 Wi-Fi，`wifi ap on`，手机连热点打开设置页，电脑经 STA 打开管理页"日志"页（WebSocket）；然后同时进行：打开天气场景（HTTPS）、在"模型"页对真实 HTTPS 端点做连通性测试、上传一次固件。全程每隔 1–2 秒执行一次 `mem` | 每次 `mem` 的 `internal free` 都 ≥ 40 KB（`kWorstInternalFreeMin`）、`largest` ≥ 16 KB；记下所有采样里最低的 `internal free` 和最后的 `min`。`internal free` 是同一时刻的快照，以它为准；`min` 是各内存区各自最低点之和，偏低，只作参考。若采样都 ≥ 40 KB 但 `min` < 40 KB，记下两者，不算不达标（需要精确最低点时，可以临时在固件里用 `heap_caps_monitor_local_minimum_free_size_start/stop(MALLOC_CAP_INTERNAL)` 包住这段操作）。同时确认：天气场景和连通性测试都成功（握手变慢但不超时），STA 不掉线。结果低于 40 KB 时停下来报告 | 待测 |

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
    `boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp` 的 `kDisplaySleepOutSettleMs` 开启）：完全不调用
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
  `boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp` 的 `hold_outputs_during_light_sleep()`（在板级
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

## 内部 RAM

预算和规则见 AGENTS.md 第 5 节。以下数据用串口命令 `mem` 实测（ESP-IDF v6.0.3，发布版）。`largest` 是 IDF 报告的可分配最大块；`min` 是各内存区各自最低点之和，偏低，只能当下限看。

**问题**：没有保存任何网络时执行 `wifi ap on`，端口 80 的重定向能起来，端口 8900 的管理服务器却起不来（`httpd_start: ESP_ERR_HTTPD_TASK`）。当时内部 RAM 空闲 13.5 KB，最大块 7.7 KB，放不下 10 KB 的 httpd 栈。启动后内部堆的主要占用：

| 占用 | 大小 |
|---|---|
| LVGL 局部刷新缓冲 2 × 480 × 48 × 2 B（`Mem::Dma`） | 90 KB |
| Wi-Fi 静态 RX/TX 缓冲各 16 个（IDF 在开了 PSRAM 时的默认值） | 54 KB |
| `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` 预留池（只给 DMA 和任务栈用，并没有浪费） | 32 KB |
| 任务栈（`wisp_ui` 8 KB，`wisp.console`/`wisp.net`/`wisp.bus` 各 6 KB，`esp_timer`/`wifi` 各 6.5 KB 等） | 约 75 KB |

当时 `hal::Display::flush()` 要等传输完成才返回，LVGL 不可能一边往第二块缓冲画、一边传第一块，所以第二块缓冲（46 KB）完全没有用。（后来 flush 改成异步，又换回两块各 24 行的缓冲，总量仍是 46 KB，见下方"性能基准"。）

**改动**：LVGL 只用一块 48 行的缓冲；Wi-Fi 静态 RX/TX 缓冲改为 10/10、RX BA 窗口改为 10（`firmware/sdkconfig.defaults`）；`wisp.ws`、`wisp.dns` 和端口 80 的 httpd 不碰 flash，栈改放 PSRAM。

**哪些任务的栈能放 PSRAM**（规则见 AGENTS.md 第 5 节）：ESP-IDF 做 flash 操作时会停掉 cache，并断言当前任务的栈在内部 RAM（`spi_flash/cache_utils.c`），读和写都一样，经 NVS、LittleFS、`esp_partition_read` 间接调用也一样。本板开了 XIP（`CONFIG_SPIRAM_XIP_FROM_PSRAM`）之后，`esp_flash` 的读写改为只拿一把互斥锁、不再停 cache（`spi_flash/spi_flash_os_func_app.c` 的 `SPI_FLASH_CACHE_NO_DISABLE`），但映射和解除映射 flash（`esp_partition_mmap`、OTA 收尾时校验镜像）仍然冻结 cache 并做同样的栈检查（`esp_mm/esp_cache_utils.c`）。所以规则不因 XIP 放宽：XIP 只是板级配置，规则对所有板子都成立。PSRAM 栈只在 `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` 打开时使用（`firmware/sdkconfig.defaults` 固定为 y），关掉时自动退回内部栈。

已核对的三个任务：`wisp.ws` 只把排队的帧经 `httpd_queue_work()` 交给管理 httpd 去发；`wisp.dns` 只收发 UDP；端口 80 的 httpd 只用内存里的热点状态拼一个 302。三者都不读 Settings、不读文件，日志只进内存环形缓冲和串口。网页资源包链接在应用镜像里，经 cache 读取（开 XIP 后在 PSRAM 里），不触发 flash 操作，而且只由端口 8900 的管理 httpd 提供。

停掉任一个 httpd 实例（关热点、STA 断开）时，httpd 任务用 `vTaskDeleteWithCaps()` 删除自己，这需要临时创建一个内部栈的辅助任务（约 2 KB），创建失败就 abort（ESP-IDF 的 `idf_additions.c`）。最坏情况 40 KB 的下限已经包含这部分余量。

| 状态（空闲，服务都已启动） | 改动前 | 改动后 |
|---|---|---|
| 启动后，无网络 | 36.3 KB（最大块 21.5 KB） | 105.0 KB（31.7 KB） |
| 只开热点（两个 httpd） | 13.5 KB（7.7 KB），管理服务器起不来 | 75.9 KB（31.7 KB） |
| 只连 STA（管理 httpd + mDNS） | 62.6 KB¹ | 85.1 KB（31.7 KB） |
| STA + 热点 | 41.1 KB¹（26.6 KB） | 70.5–71.9 KB（31.7 KB） |

¹ 只把 LVGL 改成一块缓冲之后测得。

STA + 热点下各种操作期间的内部 RAM 最低值（`min` 下限，改动后）：配对 58 KB；扫描 58 KB；WebSocket 日志流加 TLS 握手 51 KB；WebSocket 加 OTA 上传 3.18 MB 约 60 KB；手机侧设置流程（302 跳转、`setup.html`、`/pair/ap`、扫描、WebSocket）51 KB。TLS 只测到证书校验失败为止：固件只开了 TLS 1.2，被拒绝的证书在 ECDHE 和签名校验之前就会中断握手，完整握手估计还要再多用几 KB，仍然高于 40 KB 的预算。真实端点的连通性测试见上面清单第 44 项。

代价：OTA 上传 3.18 MB 从 15.1 s 变成 17.5 s（RX 缓冲少了）。用程序滚动自检列表（每帧 6 px），改动前后都是 8–9 fps，原因与本改动无关，见下方"性能基准"。RX BA 窗口 10 低于 IDF 对开了 PSRAM 的板子建议的 16（`CONFIG_ESP_WIFI_RX_BA_WIN` 的帮助说 16 对吞吐和与各种 AP 的兼容性更好）；目前只在电脑网卡开的热点上测过，个别路由器可能在窗口小时吞吐骤降或反复重连，见清单第 44 项。

**改成异步双缓冲、XIP 之后**（下方"性能基准"的最终配置）：启动后 103.3 KB（最大块 31.7 KB），只开热点 73.5–74.0 KB（31.7 KB），关掉热点回到 101.5 KB；滚动期间 102–105 KB（`WISP_PERF` 的 `heap_internal_free`）。比上表的"改动后"少约 2 KB：`LV_OBJ_STYLE_CACHE` 约 0.8 KB，XIP 约 0.6 KB，剩下约 0.6 KB 没有细分。热点打开过程中的 `min` 下限是 40.0 KB（改之前 41.7 KB）。STA + 热点没有重测（不连真实网络），按上表推算约 68–70 KB。

**加上渲染优化之后**（二维码按屏幕格式存、跳过被卡片挡住的区域、8 位字形缓存、SIMD；字形缓存和二维码在 PSRAM）：启动后 103.1 KB（最大块 31.7 KB，`min` 56.3 KB），只开热点 76.9–79.6 KB（31.7 KB），热点打开过程中的 `min` 下限 40.9–41.6 KB。

**接入场景运行时之后**（2026-11-04，固件 e9e11a7，只连 STA、空闲、未打开场景）：内部 RAM 空闲 92.6 KB（`min` 58.3 KB），接入前 97.5 KB（`min` 62.8 KB），净少 4.9 KB（`min` 净少 4.5 KB）。这是两项改动的净值：`os::Queue` 的槽位同时移进了 PSRAM（事件总线 64 × 80 B、UI 投递队列 32 × 16 B，约 5.6 KB），扣除后场景子系统本身约占 10 KB 内部 RAM，划分见场景 spec 2.3。新任务的栈：`wisp.fs` 4093 B，放内部 RAM（它读写 flash），high-water mark 剩 1532 B；`wisp.shttp` 8181 B 和 `wisp.scene` 131057 B，都在 PSRAM。最坏情况（STA + 热点、两个 httpd、WebSocket 日志、TLS 握手、OTA 上传同时进行）没有在这一版本上重测。

**`wisp.fs` 栈从 4 KB 加到 6 KB**（2026-11-05，固件 5fab9ac 加临时 12 KB 栈实测）：装了一个场景后，开机列出场景时在这个线程上解析各场景的 manifest，4 KB 栈溢出，表现为开机约 3 秒就重启，反复循环（日志里是 `stack overflow in task wisp.fs`，或者溢出写坏了旁边的队列，变成 `Interrupt wdt timeout`）。装上 7 个示例场景后实测：开机列出场景时用了 4.3 KB，安装并打开 ledger、weather 后峰值 4.5 KB，所以定为 6 KB，留约 1.7 KB 余量。同一次测量里，安装场景加 weather 的 HTTPS 请求期间，内部 RAM `min` 降到 39.7 KB（当时 `wisp.fs` 用的是 12 KB 临时栈；改成 6 KB 后相当于约 45.7 KB）。

**STA 和热点互斥能省多少**：只开热点比 STA + 热点只多 5.4 KB（mDNS 任务栈 4 KB 加 DHCP 客户端和关联状态），只连 STA 比 STA + 热点多 14.6 KB。热点部分本身：softAP 驱动、netif 和 DHCP 服务器约 10.6 KB；端口 80 的 httpd 约 3 KB（栈已移到 PSRAM，移之前是 7.1 KB）；DNS 任务 0.4 KB（移之前是 3.4 KB）。配网时热点不能不开，所以互斥只能把峰值降低约 5 KB。

### 场景运行时

真机验收清单第 53 项之外，另外交给用户在真机上执行的实测步骤（场景 spec 2.3）：分别在以下四种状态下执行一次串口命令 `mem`，记录 `internal free`/`largest`/`min`，以及 `wisp.scene`、`wisp.fs`、`wisp.shttp`、`wisp.ntp` 四个任务的栈大小与 high-water mark：

(a) 开机空闲、未打开任何场景；
(b) 打开待办场景并滚动 200 条；
(c) 天气场景发出 HTTPS 请求期间；
(d) STA + 热点 + 网页日志页 + 上传一个场景 zip 时。

(b)–(d) 相对 (a) 的内部 RAM 增量，对照 spec 2.3 给出的 ≤ 2 KB 预计，以及上方"接入场景运行时之后"一段已经测过的净值（接入场景运行时本身净少约 4.9 KB，因为 `os::Queue` 槽位同时移进了 PSRAM）。测完后把三个在 PSRAM 上的任务按 high-water mark 加余量调整 `*StackBytes`/`stack_bytes`，`wisp.fs` 已按同样方法从 4 KB 调到现在的 6144 B（见上方"`wisp.fs` 栈从 4 KB 加到 6 KB"一段）；调整后同步改 `docs/architecture.md`。

四个任务的预期落点和配置栈大小（按代码里的 `task_config()`/`TaskConfig` 核对，`flash_access = true` 的任务栈在内部 RAM，`false` 的在 PSRAM）：

| 任务 | 预期所在 | 配置栈大小 | 依据 |
|---|---|---|---|
| `wisp.scene` | PSRAM | 131072 B | `SceneRuntime::task_config()`（`components/wisp_scene/src/scene_runtime.cpp`）的 `flash_access = false`；大小是 `SceneRuntimeConfig::stack_bytes`（`components/wisp_scene/include/wisp/scene/scene_runtime.hpp`），按 Lua `LUAI_MAXCCALLS` 的 `string.gsub` 递归深度定的 |
| `wisp.fs` | 内部 RAM | 6144 B | `StorageWorker::task_config()`（`components/wisp_core/src/storage_worker.cpp`）的 `flash_access = true`（读写 flash）；大小是 `StorageWorkerConfig::stack_bytes`（`components/wisp_core/include/wisp/core/storage_worker.hpp`） |
| `wisp.shttp` | PSRAM | 8192 B | `SceneHttp::task_config()`（`components/wisp_net/src/scene_http.cpp`）的 `flash_access = false` |
| `wisp.ntp` | PSRAM | 4096 B | `NetWiring` 里 `os::Task::spawn()` 的 `flash_access = false`（`components/wisp_app/src/net_wiring.cpp`）；大小是 `kNtpStackBytes`（`components/wisp_app/src/net_wiring.hpp`） |

实测结果（2026-11-05，真机验收，固件 `0.1.0-387-gac2c5d0-dirty`；(a)–(d) 四种状态下各任务的栈大小与 high-water mark，B；额外记了 `wisp_ui`/`httpd`/`wisp.ws` 三个任务，因为这次顺带测到了 `wisp_ui` 的新低点）：

| 任务 | 配置栈大小 | (a) HWM | (b) HWM | (c) HWM | (d) HWM |
|---|---|---|---|---|---|
| `wisp.scene` | 131072 | 130140 | 124892 | 124892 | 124892 |
| `wisp.fs` | 6144 | 1752 | 1688 | 1688 | 1688 |
| `wisp.shttp` | 8192 | 7344 | 7344 | 3664 | 3664 |
| `wisp.ntp` | 4096 | 1620 | 1620 | 1620 | 1620 |
| `wisp_ui` | 8192（测量时）¹ | 1120 | 480 | 432 | 432 |
| `httpd`（端口 8900，管理服务器） | 10240 | 9064 | 9064 | 9064 | 4280 |
| `wisp.ws` | 4096 | 3264 | 3264 | 3264 | 2208 |

¹ 这次测量用的还是改动前的 8192 字节栈。(c)/(d) 里 `wisp_ui` 的 high-water mark 只剩 432 B，是本次验收测到的全场最低点；8 KB 栈几乎打满，按此把 `UiConfig::task_stack`（`components/wisp_ui/include/wisp/ui/ui_runtime.hpp`）从 8192 调到 10240，多占内部 RAM 2 KB。(a) 空闲时 internal free 88899 B，扣掉这 2 KB 后约 86851 B，仍远高于 `kIdleInternalFreeMin`（60 KB）下限，不需要跟着调整 idle 预算。

内部 RAM（B）：

| 状态 | internal free | largest | min |
|---|---|---|---|
| (a) 空闲，未打开场景 | 88899 | 31744 | 54656 |
| (b) 待办场景，滚动 200 条 | 88615 | 31744 | 48924 |
| (c) 天气场景，HTTPS 请求期间 | 88851 | 31744 | 39772 |
| (d) STA + 热点 + 日志页 + 上传场景 zip | 86287 | 31744 | 33772 |

(b)/(c) 相对 (a) 的瞬时 `internal free` 降幅分别为 284 B、48 B，都在 spec 2.3 给出的 ≤ 2 KB 以内；(d) 降幅约 2.6 KB，略高一些，但 (d) 叠加了热点、两个 httpd 和一次上传，开销不只是场景子系统本身的。

`largest` 全程保持 31744 B，高于 `components/wisp_app/include/wisp/app/ram_budget.hpp` 的 `kWorstInternalLargestMin`（16 × 1024 = 16384 B）和 `kIdleInternalLargestMin`（20 × 1024 = 20480 B），两项都达标。(a) 的 `internal free` 88899 B 高于 `kIdleInternalFreeMin`（60 × 1024 = 61440 B），达标。

`min` 是开机以来的历史最低点：这次验收触到的内部 RAM 历史最低点是 33772 B（TLS 连通性测试叠加热点上传前后出现，一直保持到测试结束），对照 `kWorstInternalFreeMin`（40 × 1024 = 40960 B）：33772 < 40960，**不达标**，需要跟进（日志显示这个低点是在热点打开、WebSocket 日志连上之后、上传之前的区间内出现的，具体触发点还没有继续拆开排查）。

跟进（2026-11-05，未上机）：TLS 会话的大部分内部 RAM 来自 mbedTLS 小于 16 KB 的分配（4 KB 发送缓冲、SSL 上下文、握手状态、解析后的证书链、大数临时变量），按默认 `malloc` 规则都落在内部 RAM，估计握手期间 15–20 KB；TLS 闸门只串行化握手，场景的 HTTPS 会话和连通性测试的握手可以重叠。现在 `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` 把 mbedTLS 的分配全部放进 PSRAM（理由和代价见 `firmware/sdkconfig.defaults` 中该项的注释）。另外这次测量之后 UI 任务栈从 8 KB 加到了 10 KB，内部 RAM 常驻多占 2 KB。需要按清单第 57 项在真机上复测，复测前不能算达标。

## 性能基准

ESP-IDF v6.0.3，发布版（全局 `-Os`，LVGL、esp_lcd 和 wisp_ui 的渲染路径 `-O2`），CPU 240 MHz，QSPI 40 MHz。"程序滚动"是临时加在固件里的测试命令（没有提交），在 UI 线程里每帧把自检列表滚动 6 px，往返 10 s：

- **raw**：`lv_obj_scroll_by_raw()`，前后各发一次 `SCROLL_BEGIN`/`SCROLL_END`，和手指拖动时 LVGL 的做法一样。
- **scroll_by**：每帧调用一次 `lv_obj_scroll_by()`。每次调用都会发一对 `SCROLL_BEGIN`/`SCROLL_END`，切换 `LV_STATE_SCROLLED` 状态，触发整棵树重新布局，每帧多出约 11 ms。手指滚动不会这样，所以以 raw 为准。

fps 按 LVGL 的 `REFR_READY` 事件计数，`WISP_PERF` 的数值与之一致。"热点"指 `wifi ap on`（只开热点，不连任何网络），这时配网卡片（带二维码）盖在列表上面。

| 配置 | raw fps | raw fps（热点） | scroll_by fps | `WISP_PERF`（滚动中） | 启动后内部 RAM 空闲 |
|---|---|---|---|---|---|
| 最初：同步 flush，一块 48 行缓冲，CPU 交换字节 | 10.6 | — | 8.6–8.8 | fps 9，cpu 100，flush_ms 4.2–4.4 | 105.2 KB |
| 异步 flush，两块 24 行缓冲，按面板字节序渲染，XIP，`LV_OBJ_STYLE_CACHE` | 19.8–20.4 | 11.8–12.2 | 14.8 | fps 20，cpu 100，flush_ms 0.22–0.23 | 103.3 KB |
| **现在**：再加上二维码按屏幕格式存、跳过被不透明卡片挡住的区域、默认字体缓存 8 位字形、S3 SIMD 填充/拷贝、内联的带遮罩填充 | **22.7** | **28.2** | 15.8 | fps 22–23（热点 28），cpu 97–100，flush_ms 0.24–0.27 | 103.1 KB |
| LVGL 堆改到 PSRAM（`LV_STDLIB_CUSTOM`，`wisp/ui/lv_heap.hpp`）；STA 连着家里的 Wi-Fi，"热点"一列是 STA + 热点 | 21.8 | 27.7 | — | fps 22（热点 28），cpu 97–100，flush_ms 0.23–0.27 | 98.8–99.8 KB |

静止时自检界面仍然是 10 fps（它每 100 ms 刷新约 7 个标签）：`WISP_PERF` cpu 22–23、flush_ms 0.70–0.87；开热点后 cpu 24–25、flush_ms 0.22–0.25。LVGL 堆改到 PSRAM 之后（STA 连着网络）：cpu 22–24，开热点后 25–26，UI 任务真实占用的 CPU 不变（见下方"LVGL 堆"）。`boot_ms` 1826–1828。XIP 要在启动时把镜像拷进 PSRAM，app 开始运行的时刻从 960 ms 推迟到 1042 ms（ESP 日志里 `Pro cpu start user code` 的时间戳），还占用 3.1 MB PSRAM。默认字体的 8 位字形缓存在 PSRAM 里，上限 512 KB（`components/wisp_ui/src/a8_font.hpp`）。

**静止 cpu 对代码布局敏感**：链接时排在 LVGL 前面的组件（`wisp_port_esp`、`main`、`wisp_app`、`wisp_admin`、`wisp_ui`）多出 1–3 KB 代码，就会把整个 LVGL 挪位，16 KB 指令 cache 里的冲突随之改变。同一界面、同样的缓冲，实测静止 cpu 在 24 和 33 之间跳动，fps 不变。所以改了这些组件要重测 `WISP_PERF`；不在渲染路径上的新代码优先放进链接在 LVGL 之后的组件（如 `wisp_hal`、`wisp_core`）。

**每项改动的收益**（raw fps，关热点 / 开热点）：

| 改动 | 关热点 | 开热点 | 说明 |
|---|---|---|---|
| 二维码按屏幕格式存（`net_cards.cpp` 的 `flatten_qr()`） | 19.9 | 11.8 → 14.5 | `lv_qrcode` 存的是 1 位索引图，每次重画卡片都要逐像素查调色板 |
| 跳过被不透明卡片挡住的区域（`wisp/ui/overlay.hpp`） | 19.8 | 14.5 → 27.2 | 列表在卡片下面滚动时，LVGL 会把整块失效区（包括卡片）重画；现在只重画卡片外的部分，每帧失效像素从约 28 万降到约 12 万 |
| 默认字体缓存 8 位字形（`a8_font.cpp`） | 19.8 → 22.0 | — | LVGL 对 4 位字形每画一次就展开一次；缓存后走 LVGL 的静态 A8 位图路径 |
| S3 SIMD 纯色填充和 16 位拷贝（`components/wisp_port_esp/lv_blend/esp32s3/`） | 22.0–22.1 | 27.9 | 这个界面上在噪声范围内；只在 ESP32-S3 构建时启用 |
| 带遮罩的纯色填充改为内联（`lv_blend_mask_rgb565.h`） | 22.9 | 28.2 | 字形和圆角边缘的混合，结果与 LVGL 逐位一致 |

这些是逐项改动后各测一次的数，同一固件重复测量的波动约 ±0.3 fps；最终基准以上面总表的 22.7 / 28.2 为准（"内联"一行的 22.9 与最终的 22.7 之差在波动范围内，不是后续改动造成的回退）。

**每帧的时间花在哪里**（现在的配置，关热点，raw 滚动）：

- **周期**：每帧约 44 ms，其中渲染（`REFR_START` 到 `REFR_READY`）平均 40.2 ms、最长约 52 ms，约 9.6 M 周期。滚动时整个列表区域每帧都要重画（每帧失效约 28 万像素，传输 480 × 480）。
- **CPU 时间分布**（UI 核上的 PC 采样）：文字排版和字形查找 20%，像素混合和填充 17%，对象树遍历和刷新调度 13%，样式查找 12%，ROM 里的代码 11%（大部分在临界区退出 `vPortExitCritical` 之后，即 LVGL 内存分配和 SPI 提交时关中断的那一段），事件分发 8%，SPI 提交 6%，矩形、圆角和阴影 4%，其他 10%。阴影只占 0.4%，圆角约 1.5%。
- **流水线**：每帧执行约 430 万条指令（IPC 约 0.45）。约 32% 的周期卡在取指上（16 KB 指令 cache 的缺失，代码从 PSRAM 取），数据 cache 缺失约 1%。
- **传输**：40 MHz QSPI 实测约 17.9 MB/s，480 × 480 RGB565 整帧在线上约 25.7 ms。每段还要先发 CASET/RASET，提交一段约 0.15–0.25 ms（每帧 20 段，共 3–5 ms）。整帧刷新的链路上限约 36 fps。传输已经和下一段的渲染重叠。
- **CPU 频率**：开着 DFS（`CONFIG_PM_ENABLE`），渲染期间一直是 240 MHz；滚动时持有 `ESP_PM_CPU_FREQ_MAX` 锁，结果没有变化。所以 DFS 不拖帧率（第 34 项）。
- **flash 和 PSRAM**：QIO 80 MHz flash，octal 80 MHz PSRAM。渲染循环频繁 miss 16 KB 指令 cache，所以把代码放进 PSRAM 运行（XIP，`CONFIG_SPIRAM_XIP_FROM_PSRAM`）收益最大：同样的代码从 14.9 fps 升到 20.2 fps。

**试过的其他方案**（raw fps；内部 RAM 是带临时测试代码时测的启动后空闲值，比正式固件低 2–3 KB）：

| 方案 | raw fps | 内部 RAM | 结论 |
|---|---|---|---|
| 异步 2 × 24 行，不开 XIP | 14.2–14.9 | 100.8 KB | 不开 XIP 时的基础收益 |
| 异步 2 × 48 行 | 19.1 | 55.2 KB | 超出预算 |
| 异步 2 × 48 行 + XIP | 25.5 | 约 55 KB（推算） | 超出预算 |
| 2 × 24 行 + XIP + 32 KB 指令 cache | 23.4 | 83.3 KB | 多占 16.7 KB，热点 + STA 的最坏情况会跌破 40 KB |
| 2 × 48 行 + XIP + 32 KB 指令 cache | 28.1 | 35.7 KB | 最接近 30，但启动后就低于 60 KB 的预算 |
| 关掉 `LV_OBJ_STYLE_CACHE` | 19.2 | 多 0.8 KB | 保留 style cache（+1 fps） |
| LVGL 两个软件渲染单元（`LV_USE_OS LV_OS_FREERTOS`、`LV_DRAW_SW_DRAW_UNIT_CNT 2`） | 16.7 | 80.2 KB | 更慢，还多占内部 RAM |
| `esp_driver_spi` 改用 `-O2`（第 42 项） | 20.1 | 不变 | 无收益，未采用 |
| 绘制缓冲放 PSRAM（只渲染，不传输） | 24 行每帧 46.5 ms，240 行每帧 35.4 ms | 省 46 KB | 渲染更慢。从 PSRAM 直接 DMA 还要 esp_lcd 的 `psram_dma_direct`，bmgr 0.7.5 只在 IDF ≥ 6.1 时生成这个字段 |
| UI 任务固定在 core 1 | 20.1 | 不变 | 无收益（UI 原本就没和别的任务抢 CPU） |
| LVGL 的 `src/draw/sw/**`（29 个文件）用 `-O3`（在 SIMD 之前测） | 19.4–19.9（对照 19.8–20.0） | 不变 | 无收益；app 大 37 KB |
| 整个 LVGL 用 `-O3`（现在的配置上测） | 23.1（对照 22.7） | 不变 | 不到 1 fps，未采用；app 大 65 KB，指令数少约 4%，但取指停顿从 31.6% 升到 33.2%，抵掉了大部分收益 |
| 简化自检界面样式 | — | — | 没有改：按钮阴影（主题默认，50% 灰）和圆角都看得见，去掉会改变外观，而且合计不到 2%；没有渐变、半透明图层和变换 |
| LVGL 热点函数放进 IRAM | — | — | 没有做：IRAM 和 DRAM 共用同一块 SRAM，热点集合要占 10 KB 以上，而开热点时 `min` 下限只比 40 KB 高约 1 KB |

没有用 `-Ofast`/`-ffast-math`：LVGL 的软件渲染主要是整数运算，fast-math 只影响浮点，却可能改变变换和渐变里浮点计算的语义（NaN、舍入、结合律），收益很小。

**为什么到不了 30 fps**：在预算内（启动后 ≥ 60 KB，最坏情况 ≥ 40 KB），每帧光渲染就要约 40 ms，CPU 时间分散在文字排版、样式查找、对象遍历和混合上，没有一个能单独砍掉 25% 的热点。约三分之一的周期花在等取指上。要达到 30 fps，需要以下其中之一：

- 在内部 RAM 预算放宽后改用 32 KB 指令 cache 加更大的缓冲；
- 等 bmgr 支持 `psram_dma_direct`，把缓冲放进 PSRAM，腾出内部 RAM 给 32 KB 指令 cache；
- 减少滚动区域里的对象和标签数（界面设计层面的取舍）。

开热点时曾经到 28.2 fps：那是旧版自检界面上测的（`b16bfc3`，只开热点、不连 STA，用上面的程序滚动），配网卡片挡住了一半列表，每帧要画的像素更少，并不是开热点本身让渲染变快。之后自检界面改版了两次（内容和分区卡片都变了），安全区变小（圆角半径 70，安全区 `{24,24,455,455}`），配网卡片也变大了（大号对话框按钮），卡片挡住的比例不再相同，所以 28.2 和现在的界面没有可比性。2026-11-05 起开热点的验收线改为不低于关热点的基准 22.7 fps（第 33、42、53、56 项）。要恢复一个开热点的专门基准，需要在当前界面上用同样的方法（程序 raw 滚动、关掉性能浮层、卡片显示）在同一个固件上先后测关热点和开热点，再把两个数一起记进上面的总表。那次 23–24 fps 与关热点的数相同，可能是卡片的遮挡裁剪没有生效，例如性能浮层（LVGL sysmon 标签）开着：它是系统层上没有标记的可见对象，任何碰到它的失效区域都会关掉裁剪（`components/wisp_ui/src/overlay.cpp`）。

### LVGL 堆

LVGL 的堆放在 PSRAM（`LV_STDLIB_CUSTOM`，`components/wisp_ui/src/lv_mem_wisp.cpp`），启动后内部 RAM 比用 CLIB `malloc` 时多约 17 KB，STA + 热点的 `min` 从 39.7 KB 升到 56.8–57.2 KB（正式固件实测）。没有 PSRAM 或 PSRAM 用完时退回内部 RAM，这部分用 `wisp::ui::lv_heap_internal_bytes()` 单独计数。

改到 PSRAM 后曾测到静止 cpu 高约 11 个点、滚动帧率低 0.16–0.47 fps。复测发现这两个数都不是堆造成的：

- **`WISP_PERF` 的 cpu 不是 CPU 时间。** 它是 `lv_timer_handler()` 里度过的墙钟时间，包括等 flush 等阻塞。同一个固件每次启动都不一样，启动后就固定下来：用 CLIB 堆的固件静止时 8 次启动在 23 到 29 之间，而用 FreeRTOS 运行时统计量的 `wisp_ui` 任务 CPU 每次都是 11.6–11.9%。另外，滚动测试结束时列表停的位置取决于帧数，停在不同位置时静止重画的标签不同，"滚动之后"的静止 cpu 就不可比。把列表滚回顶部再测，所有方案的 UI 任务 CPU 都一样（见下表）。
- **帧率差在代码布局的噪声以内。** 同一个固件反复测，raw fps 相差不到 0.05；但只改测试命令、重新链接一次，同样用 CLIB 堆的固件就从 22.31 变成 21.64（热点 28.51 → 27.97）。代码从 PSRAM 执行，约三分之一的周期在等取指（见上文），函数位置一变指令 cache 的冲突就变。几种堆方案之间的差别（±0.35 fps）都比这小，而且换一次链接排序就会倒过来。

测法：临时的程序滚动命令（同上，每帧 6 px，30 s），外加一条用 `uxTaskGetSystemState()` 统计 12 s 内各任务 CPU 时间的命令（临时打开 `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`，所以下表的内部 RAM 比正式固件低约 9 KB）。STA 一直连着家里的 Wi-Fi，"热点"指 STA + 热点。每个方案测两遍，同一组的固件除堆以外完全相同：

| LVGL 堆 | raw fps | raw fps（热点） | UI 任务 CPU：静止 / 静止（热点） | 启动后内部 RAM | STA + 热点 `min` | 启动后 PSRAM |
|---|---|---|---|---|---|---|
| CLIB `malloc`（改之前） | 21.64 / 21.64 | 27.97 / 27.96 | 11.9% / 15.0–15.2% | 72.3–72.9 KB | 39.8 KB | 5,106 KB |
| PSRAM（现在） | 21.84 / 21.80 | 27.68 / 27.67 | 12.1–12.2% / 15.3–15.4% | 89.5–90.0 KB | 46.2–46.6 KB | 5,084 KB |
| LVGL 自带的 TLSF，512 KB 池在 PSRAM | 21.99 / 21.98 | 27.86 / 27.87 | 11.7–12.0% / 15.2% | 89.3–89.8 KB | 47.0 KB | 4,580 KB |

另一组固件上试过的混合方案（这一组 CLIB 是 22.31 / 28.51，PSRAM 是 21.83–21.88 / 27.63–27.76）：

| 方案 | raw fps | raw fps（热点） | 启动后内部 RAM | STA + 热点 `min` |
|---|---|---|---|---|
| 渲染期间（`RENDER_START` 到 `RENDER_READY`）≤ 512 字节的块放内部 RAM | 21.78–21.79 | 27.61–27.62 | 89.4–90.0 KB | 46.6–46.8 KB |
| ≤ 256 字节的块放内部 RAM | 21.90–21.92 | 27.79–27.81 | 71.9–72.0 KB | 39.7 KB |
| ≤ 16 KB 的块放内部 RAM（等于 CLIB 的规则，单次） | 22.08 | 28.04 | 67.4 KB | 39.7 KB |

结论：保留 PSRAM 堆。TLSF 不更快，而且 LVGL 9.5 不会自己扩池（`LV_MEM_POOL_EXPAND_SIZE` 只放宽单个池和单块的上限），固定的池装不下 2000 个控件的场景（主机上约 300 字节/控件），截图用的 690–920 KB 绘制缓冲也要另外处理。把块放回内部 RAM 的方案要么没有收益，要么把 17 KB 的收益吃回去。

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
