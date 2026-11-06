# Wisp 子项目 3b-1：场景的设备侧接口 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让场景能在后台和待机中继续运行、发通知、读 IMU、存大文件、升级时迁移数据；设备侧有五级电源管理、唤醒锁登记、状态胶囊与下拉面板。

**Architecture:** `wisp_hal` 给每个设备加 `set_power(PowerLevel)` 和 caps `standby` 策略，`wisp_app::PowerManager` 按 `wisp_core::WakeLockRegistry` 在 active/dim/standby/deep_standby 之间切换，深度待机关 Wi-Fi、按锁开关自动浅睡（经 `Platform::set_light_sleep`），`net::SceneHttp` 在深度待机中把请求排进延迟队列，由 `net::NetworkWindow` 每 15 分钟借一把 `network` 锁开一次 Wi-Fi。`scene::SceneRuntime` 从"一个前台"扩展为"一个前台 + 最多 3 个后台"，所有 VM 共用 `VmPool` 3 MB 预算并按最久未在前台淘汰。`core::NotificationService`、`core::SensorService`、`core::OccupancyRegistry`、`core::SceneFiles`、`core::TonePlayer` 是新服务，Lua 绑定、REST 和 `ui::SystemLayer` 的胶囊/面板/横幅都只是它们的薄适配层，`app::NotificationRouter` 与 `app::StatusShell` 负责接线。

**Tech Stack:** C++20（`-fno-exceptions -fno-rtti`）、LVGL 9.5.0、Lua 5.5.1、ESP-IDF v6.0.3（`esp_pm` 自动浅睡、`esp_wifi_stop/start`）、GoogleTest、Preact + htm（网页）、Python 3 标准库（冒烟测试）。

**Spec:** `docs/superpowers/specs/2026-11-05-scene-device-design.md`（下文"spec N"均指它；"3a spec"指 `2026-11-04-scene-runtime-core-design.md`，"基础 spec"指 `2026-11-01-foundation-design.md`，"连接 spec"指 `2026-11-03-connectivity-admin-design.md`）

## Global Constraints

逐字摘自 AGENTS.md 的适用规则：

- **可移植层禁止 include 平台头文件**：`esp_*`、`freertos/*`、`driver/*`、IDF 的 `hal/`、`soc/*`、`rom/*`、`lwip/*`、`mbedtls/*`、`curl/*`、`sdkconfig.h`、`nvs.h`/`nvs_flash.h`、`sdmmc_cmd.h`、`SDL*`，以及平台层的 `wisp/port/*`。另外每一层只能依赖上面顺序中排在它之前的层，不得反向依赖。这条规则由 `tools/check_layering.py` 强制检查。分层：`wisp_base` → `wisp_os` → `wisp_hal` → `wisp_core` → `wisp_net` → `wisp_admin` → `wisp_ui` → `wisp_scene` → `wisp_app`。
- **板子无关**：可移植层和平台层里不得出现具体板子的引脚、芯片型号或常量。这些只能写在 `boards/` 和 `drivers/` 中，通过设备的 caps 对外暴露。
- **服务层约定**：Agent 工具、REST 接口、Lua 绑定都只是同一组服务 API 的薄适配层，不允许绕过服务层直接调用驱动。
- **屏幕形状**：系统界面（自检界面、`ui::SystemLayer` 的对话框、屏幕键盘、错误面板、返回手势区，以及本期的胶囊、面板、横幅、下拉手势区）必须按 Display caps 的 `shape` 和 `hal::safe_rect()`/`row_span()` 布局，保证内容可见。场景拥有整块屏幕，自己负责布局。
- **UI 单线程**：只有 UI 线程可以调用 LVGL，其他线程一律通过 `UiRuntime::post()` 把操作交给 UI 线程；脚本也不直接调用 LVGL。
- **设备模型**：设备注册在 `DeviceRegistry` 里，id 的格式为 `<类别>.<名字>`；设备能力通过 caps 声明；设备缺失或初始化失败时只降级运行，不得阻止启动。
- **存储**：数据分为 System / Library / Backup 三个逻辑区，所有持久化写入都走 `StorageManager::write_atomic`（追加日志与流式下载的例外见细化第 20 条）。
- **语言与错误处理**：C++20，`-fno-exceptions -fno-rtti`。不使用 `dynamic_cast` 和 `typeid`，错误一律通过 `wisp::Result<T>` 返回。
- **命名**：组件前缀 `wisp_`，命名空间 `wisp::<层>`，头文件路径 `wisp/<层>/<名>.hpp`，日志宏 `WLOG_D/I/W/E`。组件 CMake 用 `wisp_component(...)`。
- **格式与语言**：格式以 `.clang-format` 为准。代码标识符、注释和提交信息用英文；面向用户的界面文字可以用中文。
- **测试**：GoogleTest，坚持 TDD（先写失败的测试，再实现）。测试输出要干净，不能有告警。
- **配置键**：Settings 的 namespace 和 key 都不能超过 15 个字符。
- **内存分配**：大块缓冲用 `os::alloc(Mem::Large)`，DMA 缓冲用 `Mem::Dma`。内部 SRAM 只留给 DMA 缓冲、会访问内部 flash 的任务的栈、ISR/IRAM；其余超过几 KB 的一律放 PSRAM。小块 `malloc`/`std::string`/`std::vector` 默认落内部 RAM，常驻对象用 `os::make_large`，大容器用 `os::LargeVector`/`os::LargeString`。
- **任务**：新任务都要如实声明 `os::TaskConfig::flash_access`；栈大小按 `mem` 实测的 high-water mark 加余量。消耗内部 RAM 的改动必须上真机重新测量（`wisp/app/ram_budget.hpp`）。
- **注释**：只解释现在的代码为什么这样写；不写评审轮次、不引用 `.superpowers/`、不用"Task N"/"R2-N"式编号。`tools/ci.sh` 有 grep 守卫。
- 不得在代码、请求或日志中泄露用户的个人信息或密钥；通知正文不进事件和日志（spec 5.7）。
- 能在模拟器里验证的，就不要依赖真机。需要真机的步骤写清楚交给用户执行，不得声称已经验证过。改动渲染路径时对比 `WISP_PERF`。
- **分支与提交**：在 `feat/scene-device` 上开发；Conventional Commits；每个提交结尾附 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`。**不 push。** `README.md` 不改。

本计划的固定约束（spec 原文数值）：

- 后台场景最多 3 个；VM 总预算 3 MB（`scene::kVmBudgetBytes`），单个 VM 仍 2 MB，前台保证 2 MB；每条消息 200 ms 上限不变。
- 电源：`dim` 在距离灭屏 10 秒时进入，亮度降到 30%；`power/screen_off_s`（已有，默认 60）；`power/deep_after_s` 默认 300、0 表示不进入；`power/light_sleep` 布尔、默认开。
- 唤醒锁类型 `network`/`cpu`/`screen`；持有者 `system.<模块>` 或 `scene.<id>`；连续持有满 30 分钟提醒一次、之后每 30 分钟一次。
- 深度待机联网：最多每 15 分钟开一次 Wi-Fi；每场景排队最多 4 条（超出 `deferred_queue_full`）；排队超过 1 小时失败（`deferred_timeout`）；回调里 `res.deferred = true`。
- 通知：`title` 1–32 字节，`body` 0–200 字节，`id` 1–32 字节 `[a-z0-9_-]`，`sound` 为 `none`/`tone`/`alarm`（alarm 最长 30 秒），每场景最多 5 条未读，每分钟最多 10 条（超出 `rate_limited`），持久化最多 50 条、300 ms 合并；横幅 4 秒自动收起。
- 胶囊 72×24 px（实测最宽文字 `▶9+ 🔔9+` 需要 72 px）；下滑手势判定区 16 px、下移 24 px 触发；面板行高至少 48 px；界面文字中/日/英。
- 提示音：tone 约 150 ms，alarm 约 1 秒一段循环；`audio/notify_vol` 0–100、默认 60。
- 传感器：`accel`/`gyro`；采样率 10/25/50/100 Hz；每 100 ms 打包；`shake` 为 500 ms 内 3 次超阈值，`flip` 为 z 轴翻转后稳定 300 ms；单位 m/s²（gyro 为 °/s），`t` 为毫秒。
- `wisp.files`：文件名 `[A-Za-z0-9._-]`、1–64 字节、不以 `.` 开头、平铺；`read` 的 `len` ≤ 64 KB；单次 `write`/`append` ≤ 256 KB；SD 卡配额 64 MB，内部存储配额 512 KB。
- `schema_version` 为 1–1000 的整数；`migrate` 时限 2 秒。
- 任务配置：`wisp.imu` 优先级 3、`flash_access = false`、栈 4096 字节起（实测调整）；`wisp.tone` 优先级 3、`flash_access = false`、栈 4096 字节起。两者真机上栈在 PSRAM。
- 验证命令：主机 `cmake -S simulator -B build/sim -G Ninja && cmake --build build/sim && ctest --test-dir build/sim -R '<正则>' --output-on-failure`；提交前 `bash tools/ci.sh`；固件 `bash -c '. ~/esp/esp-idf-v6.0.3/export.sh >/dev/null && cd firmware && idf.py build'`。

## Review Focus

1. **关机、重启或断电时有场景在后台**：期望 `App::stop()` 关闭场景时不把 `scenes/background.json` 写成空表（否则下次开机什么都不恢复）；某个场景每次开机都恢复失败时只失败一次就从列表中移除，不会每次开机都拖慢启动。测试：Task 7 `SceneRestore.StopKeepsTheList`、`SceneRestore.FailedRestoreSkippedNotifiedAndDropped`。
2. **深度待机中路由器不在（Wi-Fi 永远连不上）**：期望联网窗口 60 秒后放弃、释放 `network` 锁、设备回到深度待机，请求继续排队直到 1 小时超时，而不是让 Wi-Fi 一直开着耗电。测试：Task 4 `NetworkWindow.GivesUpAfterConnectTimeout`。
3. **点击的通知属于已删除、打不开或出错的场景**：期望通知被移除、不崩溃、不卡在面板上；场景被删除时它的未读通知一并移除。测试：Task 8 `NotificationRouter.TapForMissingSceneJustDismisses`，Task 5 `Notifications.DismissSourceRemovesAll`。
4. **SD 卡在写文件或下载中途被拔出**：期望进行中的调用返回错误、下载留下的半截文件在下次统计时被清掉、之后的调用返回 `unavailable`、重新插卡后恢复，配额统计不漂移。测试：Task 12 `SceneFiles.SdRemovedMidDownload`。
5. **场景在后台和前台之间来回切时有 HTTP、文件、定时器或传感器回调在路上**：期望转入后台（同一实例）的回调照常执行，`wisp.scene.exit()` 或淘汰之后的回调一律丢弃、不访问已销毁的 VM。测试：Task 6 `SceneBackground.CallbacksSurviveBackgroundAndDieWithExit`。

---

## 依赖分支

本分支 `feat/scene-device` 从 `feat/scene-runtime-core` 的 `114cdfd` 切出（3a 已完成，未合入 `main`）。写计划时 `feat/scene-runtime-core` 又多了两个文档提交（`700e791` 场景运行时的架构与移植文档、`1b50b5e`），Task 15 改 `docs/architecture.md`、`docs/porting-guide.md` 之前先 `git merge feat/scene-runtime-core`（只合这两个文档提交，冲突按对方版本为底）。

矢量字体（`2026-11-05-vector-font-design.md`）在并行分支 `feat/vector-font` 上实施，本分支没有它。本期只经 `ui::font(px)` 和场景的 `font = "xxl"` 用大字，两者在矢量字体合入前后签名不变（细化第 24 条）。

已核实下列 3a 接口存在（`114cdfd`）：`scene::SceneRuntime`（`components/wisp_scene/include/wisp/scene/scene_runtime.hpp`，单前台，`Foreground` 结构、`UiSide`、`post_scene_message`）；`SceneInstance`（`src/scene_instance.hpp`）；`SceneApi`（`src/scene_api.hpp`）；`core::SceneHost`（`SceneState { Stopped, Running, Error }`）；`ui::SystemLayer`（被动边缘观察者包装指针读回调，`observe()`）；`app::PowerManager`（两态，按名字处理 `display`/`touch`/`imu`）；`Platform::set_standby`（真机在待机时打开自动浅睡，`firmware/main/main.cpp` 的 `apply_pm_config()`）；`net::WifiService::set_standby()` 与 `net/idle_off_min`；`net::SceneHttp`（单 worker、8 条队列）；`HttpClientRequest::on_body`（流式响应体）。

## 相对 spec 的细化（写计划时裁定，执行前请用户确认）

1. **浅睡的位置**：spec 4.1 规定 `standby` 保持 CPU 和 Wi-Fi，浅睡只在 `deep_standby` 中、没有 `cpu` 锁时打开。子项目 1 目前在 `standby` 就打开浅睡（`Platform::set_standby`），本期改为只在深度待机中打开：`Platform::set_standby(bool)` 改名为 `Platform::set_light_sleep(bool enable)`，由 `PowerManager` 按"处于 deep_standby、`power/light_sleep` 为真、没有 `cpu` 锁"调用。Task 1 的裁定为 B（不兼容）时，固件不设置这个钩子，深度待机只关 Wi-Fi，`GET /system/power` 的 `light_sleep` 为 `"unsupported"`，设置项保留。
2. **关 Wi-Fi**：`hal::Wifi` 新增 `set_radio(bool on)`（默认 `Unsupported`；ESP 为 `esp_wifi_stop()`/`esp_wifi_start()`）。`WifiService::set_standby()` 换成 `set_power_level(hal::PowerLevel, uint64_t now_ms)`：`DeepStandby` 时断开、关热点、关射频；离开时开射频并重连。原来的 `net/idle_off_min`（待机 10 分钟后断开）删除：它会在有 `network` 锁时也断网，与 spec 4.3 冲突，作用已由 `deep_after_s` 取代。
3. **联网窗口**：就是一把 `network` 锁（持有者 `system.netwindow`）。延迟队列非空，且距进入深度待机或上次窗口结束满 15 分钟时打开；`PowerManager` 看到锁回到 `standby`、开 Wi-Fi；连上后放行队列，等队列清空并等到一次 `system.time` 结果（最多 30 秒）后释放锁。连不上 60 秒放弃；窗口最长开 5 分钟。延迟队列放在 `SceneHttp` 里（`set_deferring()`），与 8 条的执行队列分开；排队的请求在提交时照常计入每分钟 30 次。
4. **系统锁清单**：`system.admin`（`network`，管理后台至少有一个 WebSocket 连接时，WebSocket 每 30 秒 ping、连续 3 轮无回应即关闭；另外每个经过鉴权的 REST 请求续一把 2 分钟的租约，原因 `"admin request"`，让不开 WebSocket 的页面在编辑期间也保持联网）、`system.upload`（`network`，场景 zip 上传进行中）、`system.ota`（`network` + `cpu`，固件上传与写入）、`system.ntp`（`network`，校时尝试进行中）、`system.netwindow`（`network`）、`system.hotspot`（`network`，配网热点开着时）、`system.tone`（`cpu`，提示音播放中）。原因文字用英文短语（`"admin session"` 等），只在 REST 与网页里显示；设备上的提醒通知按持有者显示本地化名称（第 14 条）。
5. **dim**：亮度取当前设置的 30%，至少 10；只有 `screen_off_s > 0` 时才有 dim。`dim` 不调用设备的 `set_power`，只改亮度。
6. **caps `standby` 由端口设备类声明**：`display`、`touch`、`imu` 为 `"off"`；按键为 `"wake_source"`；`audio_in`、`audio_out` 也为 `"off"`，待机时关掉各自的 I2S 通道（每个开着的通道持有一把 `i2s_driver` APB_FREQ_MAX 锁，会挡住所有自动浅睡，见 `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md` 的"浅睡兼容性（3b-1 spike）"一节）；其他设备不声明，`PowerManager` 跳过（存储、电源、时钟、Wi-Fi 由各自的服务处理）。ESP 触摸只有驱动实现了睡眠钩子时才声明 `"off"`，否则不声明、`set_power` 返回 `Unsupported`；本板的 CST9217 没有钩子，所以它没有 `standby`。`Display`/`Touch`/`Imu` 原有的 `set_power(bool)` 删除，改为覆盖基类的 `set_power(PowerLevel)`。`SensorService` 没有订阅者时调用 `imu->set_power(PowerLevel::Standby)`。AGENTS.md §3"设备模型"补一句：设备经 caps `standby` 声明待机策略，`PowerManager` 统一调度，不按名字处理设备。
7. **新设置进 `SystemSettings`**：`deep_after_s`（`power`/`deep_after_s`，0 或 60–3600，默认 300）、`light_sleep`（`power`/`light_sleep`，布尔，默认 true）、`notify_vol`（`audio`/`notify_vol`，0–100，默认 60）。`GET/PUT /settings` 与 `system.settings` 事件随之多三项，网页"系统"页加三个控件。
8. **多场景的名字**：`core::SceneState` 加 `Background`（`"background"`）；`"running"` 仍表示前台，`scene.state` 事件因此多一个 `background` 取值。Lua 侧 `wisp.scene.state()` 返回 `"foreground"`/`"background"`。新钩子沿用 `scene` 表：`scene.on_state(state)` 在每次进入前台或后台时调用（包括第一次打开和开机恢复，不包括关闭），`scene.on_notification(id)`，`scene.migrate(old)`。`POST /scenes/{id}/close` 保持 3a 的"返回"语义，新增的 `POST /scenes/{id}/exit` 是彻底关闭；网页的"关闭"按钮改调 `/exit`（spec 3.2 表"管理后台关闭"）。
9. **后台场景的设备事件**：声明了 `background` 的场景在后台和灭屏时照常收到 `minute`、`battery`、`lang`；没有声明的前台场景保持 3a 行为。
10. **切回前台的重建**：`SceneInstance::rebuild()` 对 `start()` 留在注册表里的根节点重新 `collect_tree()`，得到带当前值的 `Build`；`ui.ref` 句柄照常可用；列表的行由新视图经 `ListRange` 按需再建。每次重建以 Info 记 `scene.<id>` 的 `rebuilt in <ms> ms`，供 9.3 的真机测量。
11. **VM 预算**：`scene::VmPool` 计数所有 VM 的分配，超出 `kVmBudgetBytes` 时当前分配失败。淘汰条件：有前台场景（或正在打开一个）时，后台 VM 合计超过 `kVmBudgetBytes − VmLimits::memory_bytes`（即 1 MB）就按最久未在前台的顺序关闭后台场景；没有前台场景时不按内存淘汰。
12. **后台列表**：`<System>/scenes/background.json`，`{"ids": [...]}`，按最久未在前台在先的顺序，`write_atomic` 在 `wisp.fs` 上写。`SceneRuntime::stop()` 关闭场景时不改写它。开机恢复由 App 在 `start_runtime()` 之后、发布 `system.boot.completed` 时调用 `SceneRuntime::restore_background()`。
13. **通知的身份与文字**：一条通知由 `(source, id)` 标识；场景不给 `id` 时生成 `n<序号>`；系统通知的 id 为 `<code>:<arg>`（同一场景再次被淘汰会替换旧的那条）。系统通知只存 `code` + `arg`，显示时由 `wisp_ui` 按当前界面语言渲染；`notification.posted` 里系统通知的 `title` 是 `code`。场景传入不合法的参数（标题超长、`id` 非法、`sound` 未知）抛 Lua 错误，只有 `rate_limited` 作为返回值。持久化文件 `<System>/notifications.json`。前台场景自己发的通知只弹横幅，但照常发 `notification.posted`。场景被删除（`scene.state` 为 `deleted`）时移除它的全部未读通知。
14. **系统通知与持有者的文字**（`ui::StrId`，`{}` 由 `tr_fill` 填入）：

| StrId | zh | ja | en |
|---|---|---|---|
| `NoticeSystemTitle` | 系统 | システム | System |
| `NoticeEvictedMemory` | {}已因内存不足关闭 | {}はメモリ不足のため終了しました | {} was closed to free memory |
| `NoticeEvictedCount` | {}已关闭：后台场景已满 | {}を終了しました：バックグラウンドが上限です | {} was closed: too many background scenes |
| `NoticeSceneError` | {}出错已停止 | {}でエラーが発生し、停止しました | {} stopped after an error |
| `NoticeRestoreFailed` | {}开机恢复失败 | {}を起動時に復元できませんでした | {} could not be restored at startup |
| `NoticeWakeLock` | {}已让设备保持唤醒 30 分钟 | {}がデバイスを 30 分間起動したままにしています | {} has kept the device awake for 30 minutes |
| `NoticeBatteryLow` | 电量低：{}% | バッテリー残量低下：{}% | Battery low: {}% |
| `NoticeRelease` | 释放 | 解除 | Release |
| `HolderAdmin` | 管理后台 | 管理画面 | Admin page |
| `HolderUpload` | 上传 | アップロード | Upload |
| `HolderOta` | 固件更新 | ファームウェア更新 | Firmware update |
| `HolderTimeSync` | 校时 | 時刻同期 | Time sync |
| `HolderNetWindow` | 后台联网 | バックグラウンド通信 | Background network |
| `HolderHotspot` | 配网热点 | 設定用ホットスポット | Setup hotspot |
| `HolderTone` | 提示音 | 通知音 | Alert sound |
| `PanelBackground` | 后台运行 | バックグラウンド | In background |
| `PanelNotifications` | 通知 | 通知 | Notifications |
| `PanelClearAll` | 全部清除 | すべて消去 | Clear all |
| `PanelEmpty` | 没有通知 | 通知はありません | No notifications |
| `PanelJustNow` | 刚刚 | たった今 | just now |
| `PanelMinutes` | {} 分钟 | {} 分 | {} min |
| `PanelHours` | {} 小时 | {} 時間 | {} h |

  场景锁的持有者显示场景名。新字若不在 16 px 字体子集里（`tools/gen_cjk_font_test.py` 失败），换一个说法，不重新生成字体。
15. **图标**：现在没有任何地方解码场景的 `icon`。横幅与面板里每个场景显示一个圆形徽标：场景名的第一个字符，底色按 id 的哈希从主题色里取；系统通知用 `LV_SYMBOL_BELL`。真正的图标图片留给子项目 8。
16. **胶囊与下拉区**：`status_area` = `x1 = (width − 72) / 2`、`x2 = x1 + 71`、`y1 = safe.y1 + 4`、`y2 = y1 + 23`。480×480 时：rect `{204,4,275,27}`，rounded_rect 半径 70（参考板）`{204,28,275,51}`，rounded_rect 半径 40（模拟器默认）`{204,19,275,42}`，circle `{204,78,275,101}`。胶囊显示 `LV_SYMBOL_PLAY <后台数>` 和 `LV_SYMBOL_BELL <未读数>` 中不为 0 的部分，大于 9 的数显示为 `9+`；胶囊总是恰好占 `status_area`（16 px 字体下最宽的 `▶9+ 🔔9+` 实测需要 72 px，56 px 放不下）。下滑判定区（spec 5.6 已修订）：设按下点所在列最上面的可见行为 `top(x)`（对 `row_span` 二分求得，与 3a 的 `row_span` 二分写法相同），判定区为 `y ≤ top(x) + 15` 且 `(x, y)` 可见；向下移动 ≥ 24 px 触发，不限角度。
17. **横幅**：顶边 `safe.y1 + 4`，高 72 px，宽度取它所占各行 `row_span` 的公共可见宽度减 8 px、水平居中；不透明，标记为不透明覆盖物；4 秒收起；点击 = 点通知；上滑只收起横幅，通知仍留在未读列表。层级：横幅在胶囊之上、确认框之下（`SystemLayer::raise()` 的顺序）。
18. **提示音**：spec 5.4 写"端口提供 `play_tone(kind)`，内置两段 PCM"。改为可移植的 `core::TonePlayer`：经 `hal::AudioOut` 播放运行时合成的 PCM（16 kHz 单声道），不放 PCM 数据。`tone` 为 1 kHz、150 ms、首尾 5 ms 渐变；`alarm` 一段 1 秒（880 Hz 200 ms、静音 100 ms、880 Hz 200 ms、静音 500 ms），循环到 `silence()` 或 30 秒。音量由调用方传入（`notify_vol`，App 从 `system.settings` 缓存，`wisp.tone` 任务不读 NVS）。播放期间持有 `cpu` 锁 `system.tone`。没有 `audio_out.main` 或 `open()` 失败时静默返回。
19. **传感器**：`shake` 阈值 20 m/s²（约 2 g），每次越过阈值的上升沿算一次，500 ms 内 3 次触发，之后 1 秒不应期；`flip` 以 `az > +7` / `az < −7` m/s² 为朝上/朝下，切换到相反朝向并保持 300 ms 触发一次。每个订阅者（`scene.<id>`、`system.selfcheck`）各自以共享方式登记 `imu.main` 的占用，最后一个订阅取消时释放。自检界面的采样（`SelfCheckSampler`）改经 `SensorService` 读 IMU（accel + gyro，50 Hz），不再直接调用驱动。样本的 `t` 是 `os::now_ms()` 时间基准的毫秒。
20. **文件位置与写法**：SD 卡上的位置按 Library 区的实际根目录写成 `<sd>/wisp/library/scenes/<id>/files`（spec 7.2 的 `/<sd>/library` 是简写，基础 spec 7.2 的 Library 根在 `<sd>/wisp/library`）；内部存储为 `<data_root>/<id>/files`（真机即 `<int>/scenes/<id>/files`）。位置记录 `data/files_loc.json`（`{"location":"sd"|"int"}`，`write_atomic`）。位置按 manifest 的 `files`（`SceneManifest::files`，`FilesWant { Auto, Sd, Int }`，缺省 `Auto`，非法值安装时 `InvalidArgument`，信息含 `files`）在第一次写入时选定：`Auto` 有卡用 SD、否则内部；`Sd` 无卡返回 `unavailable`，不退回内部；`Int` 总是内部。`info()` 多返回 `want`。已有位置记录时一律以记录为准，与 `files` 声明不一致只记一条 `W` 日志。系统从不自动删除场景文件：扫描和插拔卡只统计用量，不删除目录（场景被删时卡不在，卡上留下的 `files/` 原样保留并记日志），重装同 id 的场景沿用卡上的 `files/`；只有下载协议的 `.dl`/`.done`、用户以 `data=1` 删除场景和场景自己的 `remove` 会删除文件。`files/` 随 `data=1` 删除，`data=0` 时与 `data/` 一样保留；代码包替换时 `files/` 与 `data/` 一起搬到新目录；`load()`、导出、`code_bytes` 都不含 `files/`。`append` 走 `StorageManager::append()`。下载用两阶段命名：写 `.<name>.dl`（可能写了一半，视为垃圾）→ 完成后改名为 `.<name>.done` → 删除目标 → 改名为 `<name>`；统计用量时丢弃 `.dl`、把目标缺失的 `.done` 恢复为目标。只有 2xx 的响应体写入文件；下载超时 5 分钟；下载与其他场景请求共用 `wisp.shttp` 一个 worker，串行执行。超过单次 256 KB 的 `write`/`append` 和超过 64 KB 的 `read` 返回 `too_large`，不存在的文件返回 `not_found`。`list()` 最多返回按名字排序的前 256 个文件（`SceneFilesConfig::max_list`），超出时 C++ 的 `SceneFileList::truncated` 为真、Lua 回调多一个参数 `cb(err, list, truncated)`；按目录逐项遍历（`StorageManager::visit_dir`）并只保留前 256 个，不在内部 RAM 里攒整个目录（spec 7.1 未写上限，此为裁定）。保留后缀 `.tmp`/`.new` 不分大小写拒绝（FAT 上 `x.TMP` 就是 `x.tmp`）。SD 卡上名字只差大小写时视为同一文件：写、追加、读、删、下载（含进行中检查）和统计都用卡上已有的拼写。没有位置记录、声明允许 SD（`auto`/`sd`）、卡在且 `<sd>/wisp/library/scenes/<id>/files` 已存在的已安装场景，在第一次任何调用（包括只读的 `info`/`list`/`read` 和 `scan`）时沿用卡上目录并写下记录。设备上带数据删除失败时，只有仓库已删掉 `scene.json`（`manifest(id)` 为 `NotFound`）才调用 `SceneFiles::remove_all()`。
21. **数据版本**：存在 `data/meta.json`（`{"schema_version": n}`）。从 `main.lua` 开始到迁移结束的写入就是 `SceneStore` 的一个批处理，成功时 `commit_migration(new_version)` 把整批和新版本写进同一个 `journal.json`，掉电后整体重放或整体不生效。没有 `meta.json` 的 3a 数据视为版本 1，`meta.json` 读不出版本时进入 `error`（`data version unreadable`）；全新的场景（没有任何数据文件）取 manifest 的值，随第一次写入数据才写 `meta.json`。降级安装进入 `error`，错误面板显示 `data version <n> newer than scene <m>`。
22. **REST 下载大文件**：`net::HttpResponse` 新增流式响应体 `stream`（两个服务器都实现：ESP 用 `httpd_resp_send_chunk`，模拟器分块写 socket），`GET /scenes/{id}/files/{name}` 用它，不把 64 MB 读进内存。
23. **错误字符串**：统一为 3a 的下划线风格（`not_in_background`、`deferred_queue_full`、`deferred_timeout`、`rate_limited`、`bad_name`、`quota_exceeded`、`too_large`、`not_found`、`unavailable`）；spec 已按此修订（用户 2026-11-05 批准）。
24. **大字**：水平仪示例的度数用 `font = "xxl"`（3a 为 Montserrat 48 的 ASCII 数字，矢量字体合入后自动变为矢量字），不依赖 `feat/vector-font`。
25. **`GET /system/power`**：`{"level": "active"|"dim"|"standby"|"deep_standby", "light_sleep": "on"|"off"|"unsupported", "deferred": int, "locks": [{"id": int, "holder": string, "kind": "network"|"cpu"|"screen", "reason": string, "held_ms": int}]}`。`GET /system/occupancy`：`{"items": [{"device": string, "holder": string, "mode": "shared"|"exclusive", "held_ms": int}]}`。
26. **冒烟测试可观察的日志**：`app::StatusShell` 在计数变化时以 Info 记 `status: background=<n> unread=<m>`，面板开关记 `panel: open`/`panel: closed`；`net::NetworkWindow` 记 `netwindow: open`/`netwindow: closed (<原因>)`；`PowerManager` 每次换级别记 `power: <level>`。

27. **通知的限额与保存**（执行中裁定）：每分钟 10 条按场景分别计数，是滑动的 60 秒窗口（被拒绝的不计，替换和前台场景的通知都计）；系统通知不限速。低电量通知只保留一条：新的 `power.battery.low` 先移除旧的 `battery_low:*`，`power.charging.changed` 变为充电时也移除。保存用一个常驻的 300 ms 周期定时器，有未保存的变化时启动、磁盘上已是最新时在回调里停下（`os::Timer` 在回调里停下后立即视为未启动，别的线程可以马上再启动它）；失败按 600 ms 起翻倍重试，连续 5 次失败后等下一次变化。

---

## 文件结构总览

```
components/
  wisp_hal/   include/wisp/hal/{device,display,touch,imu,wifi}.hpp  src/device.cpp        # PowerLevel、StandbyPolicy、set_power、set_radio
  wisp_core/  include/wisp/core/{wakelock,notification_service,tone_player,occupancy,sensor_service,
                                 gesture,scene_files}.hpp  src/...                           # 新服务
              (scene_manifest, scene_repository, scene_store, scene_host, storage_manager, system_settings: 改)
  wisp_net/   include/wisp/net/network_window.hpp  src/network_window.cpp                    # 联网窗口
              (scene_http, wifi_service, http: 改)
  wisp_admin/ src/{routes_power,routes_scene_files}.cpp  (routes_scenes, admin_server, ws_hub: 改)
  wisp_ui/    include/wisp/ui/{system_layer,system_notice}.hpp
              src/{system_layer,status_capsule,notification_panel,banner,system_notice}.cpp
  wisp_scene/ include/wisp/scene/{scene_runtime,vm,scene_schedule}.hpp
              src/{scene_runtime,scene_instance,scene_message,bind_scene,bind_power,bind_notify,
                   bind_sensor,bind_files,bind_http,bind_device}.cpp
  wisp_app/   src/{power_manager,notification_router,status_shell,scene_wiring,net_wiring,
                   self_check_sampler,app}.cpp
  wisp_port_esp/ dev/{esp_display,esp_touch,esp_imu,esp_button}.cpp  net/{esp_wifi,esp_http_server}.cpp
  wisp_port_sim/ dev/{sim_display,sim_touch,sim_imu,sim_button}.cpp  net/{sim_wifi,sim_http_server}.cpp
                 include/wisp/port/sim/imu_script.hpp  imu_script.cpp
firmware/main/main.cpp                       # set_light_sleep（按 Task 1 裁定）
simulator/{sim_options,main}.cpp             # --imu-script、debug 命令
web/app/pages/{scenes,system}.js  web/app/i18n/{zh,ja,en}.js
scenes/examples/{pomodoro,weather,level}/  tests/scene/fixtures/{migrate_v1,migrate_v2}/
tests/{hal,core,net,admin,ui,scene,app,sim}/...  tests/smoke/sim_smoke.py
docs/{architecture,porting-guide,scene-api}.md  docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md
```

---

### Task 1: 浅睡兼容性 spike（真机，一次性探针）

**目的**（spec 9.3、风险第 1 条）：在参考板上确认"关 Wi-Fi + 灭屏 + 自动浅睡"下 PSRAM、屏幕、触摸、定时器精度、USB 串口重连都正常，裁定后续任务是 A（深度待机打开浅睡）还是 B（深度待机只关 Wi-Fi）。探针代码只在一次性分支上，不合入；合入 `feat/scene-device` 的只有裁定记录。后续任务在两种结果下都能完成：区别只在 Task 3 Step 3 里 `firmware/main/main.cpp` 是否设置 `Platform::set_light_sleep`。

**Files:**
- 一次性分支 `spike/light-sleep`（从 `feat/scene-device` 切出，不合入）：Create `firmware/main/spike_light_sleep.cpp`、`firmware/sdkconfig.spike`；Modify `firmware/main/main.cpp`（注册控制台命令 `lsprobe`）、`firmware/main/CMakeLists.txt`（加入源文件）
- `feat/scene-device`：Modify `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`（新增"浅睡兼容性（3b-1 spike）"一节）、本计划（本任务下的"执行记录"）

**Interfaces:**
- Produces：裁定 **A** 或 **B**，写在本任务的执行记录和硬件文档里。Task 3 读它。

- [ ] **Step 1: 写探针（`spike/light-sleep` 分支）**

`firmware/sdkconfig.spike`：

```
CONFIG_PM_PROFILING=y
CONFIG_PM_LIGHT_SLEEP_CALLBACKS=y
```

`spike_light_sleep.cpp` 注册控制台命令（`Platform::console_commands`）：

- `lsprobe run <minutes> <delay_s>`：立即回复 `lsprobe: unplug USB within <delay_s> s`，然后在一个新任务（`flash_access = true`，栈 6 KB，内部 RAM）上依次：
  1. 等 `delay_s` 秒；记下 `esp_timer_get_time()`、`clock.main` 的 `get_unix()`、`power.main` 的电量、`esp_reset_reason()`。
  2. `os::alloc(2 MiB, Mem::Large)`，用 xorshift32（种子 `0x5EED1234`）填满，算 `wisp::crc32`。
  3. `esp_wifi_stop()`。
  4. `ui->post` 调 `PowerManager::enter_standby()`（现有代码：面板、触摸、IMU 关，现有钩子打开自动浅睡）。
  5. `esp_pm_light_sleep_register_cbs()` 注册进入/退出回调，累计次数与睡眠时长（`esp_timer_get_time()` 差）。
  6. 启动 1000 ms 周期 `os::Timer`，记每次触发相对计划时刻（`t0 + k·1000 ms`）的迟到量：最大、平均、漏掉的次数。
  7. 每秒检查 `PowerManager::standby()`；用户按键唤醒时提前结束，`interrupted = true`。
  8. 到时（或被打断）后：`fmemopen` 一个 2 KB 缓冲调用 `esp_pm_dump_locks()` 存下锁表；注销回调、停定时器；`ui->post` 调 `exit_standby()`；`esp_wifi_start()` 并轮询 `WifiService` 状态到 `connected`（最多 20 秒），记耗时；重算 PSRAM CRC；RTC 漂移 = `(rtc_end − rtc_start)·1000 − (esp_end − esp_start)/1000` 毫秒。
  9. 结果写入 `<int>/system/spike_ls.json`（`StorageManager::write_atomic`），并打印一行 `WISP_SPIKE {json}`。
- `lsprobe show`：打印保存的 JSON、当前 `esp_timer_get_time()` 换算的运行时长和 `esp_reset_reason()`。

JSON 字段：`minutes`、`interrupted`、`sleeps`、`sleep_ratio`（睡眠时长 / 运行时长）、`timer_fires`、`timer_missed`、`timer_max_late_ms`、`timer_mean_late_ms`、`rtc_drift_ms`、`psram_ok`、`wifi_reconnect_ms`、`battery_start`、`battery_end`、`reset_reason`、`pm_locks`（字符串）。

- [ ] **Step 2: 控制者构建并烧录**

```bash
git switch -c spike/light-sleep feat/scene-device
# 写好 Step 1 后：
bash -c '. ~/esp/esp-idf-v6.0.3/export.sh >/dev/null && cd firmware && \
  idf.py -B build-spike -D SDKCONFIG=build-spike/sdkconfig \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;components/gen_bmgr_codes/board_manager.defaults;sdkconfig.spike" \
    build && idf.py -B build-spike -p /dev/ttyACM0 flash'
```

设备连好家里的 Wi-Fi、电池已充电（≥ 50%）。

- [ ] **Step 3: 测量（控制者发串口命令，用户做物理操作）**

| # | 控制者 | 用户 | 记录 |
|---|---|---|---|
| a | 串口发 `lsprobe run 10 30` | 30 秒内拔掉 USB 线（设备靠电池运行），放着不动 11 分钟 | — |
| b | — | 10 分钟后屏幕应自动亮起：看画面有无花屏、缺块，亮度是否与之前相同；点自检界面上任一按钮 | 画面、触摸是否正常 |
| c | 插回 USB；不复位，直接重新打开串口监视器，发 `lsprobe show` | — | 串口是否无需复位即可重连；整行 JSON；运行时长是否连续（没有复位） |
| d | 发 `lsprobe run 10 30` | 拔掉 USB；约 3 分钟时短按电源键，用秒表或目测记下从按键到亮屏的时间 | 按键唤醒延迟 |
| e | 插回 USB，发 `lsprobe show` | — | `interrupted` 为 true，其余字段 |

- [ ] **Step 4: 按下表裁定**

| 项 | 通过线 |
|---|---|
| 确实进入了浅睡 | a 的 `sleeps > 0` 且 `sleep_ratio ≥ 0.5`（低于时看 `pm_locks` 找出持锁者，修掉后重测） |
| PSRAM | `psram_ok` 为 true |
| 屏幕、触摸 | b 正常；d 的唤醒延迟 ≤ 300 ms（硬件文档第 27 项） |
| 定时器精度 | `timer_missed == 0`、`timer_max_late_ms ≤ 50`（场景定时器最小间隔）、`|rtc_drift_ms| ≤ 2000`（RTC 精度 1 秒） |
| 没有复位 | `reset_reason` 与运行时长都表明没有复位 |
| Wi-Fi 恢复 | `wifi_reconnect_ms ≤ 15000` |
| USB 串口重连 | c 无需复位即可重连 |

- 除"USB 串口重连"以外全部通过 → **A**。USB 串口重连失败只记入硬件文档（`power/light_sleep` 正是为调试时关掉浅睡准备的，spec 4.1），不影响裁定。
- 有任何其他一项不通过 → **B**：深度待机只关 Wi-Fi、不浅睡（spec 10 风险第 1 条）。不要在本任务里尝试修驱动；把现象写进硬件文档，留作以后的工作。

- [ ] **Step 5: 记录并提交（回到 `feat/scene-device`）**

```bash
git switch feat/scene-device
```

硬件文档新增"浅睡兼容性（3b-1 spike）"一节：日期、固件提交号（`spike/light-sleep` 的 HEAD）、两次运行的 JSON 原文、b/c/d 的观察、上表每项的结论和最终裁定；裁定为 B 时写明现象。同时把"真机验收清单"第 29、48 项（多次浅睡眠后无异常）的结果栏填上本次结论。本任务下追加"执行记录"小节：裁定、日期、硬件文档的节名。

```bash
git add docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md docs/superpowers/plans/2026-11-05-scene-device.md
git commit -m "docs(hardware): record the light-sleep spike result"
```

`spike/light-sleep` 分支保留在本地备查，不合入、不 push。

**执行记录**：裁定 **A**（深度待机打开自动浅睡眠），2026-11-05。记录见
`docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md` 的"浅睡兼容性（3b-1
spike）"一节。前提——待机期间关闭音频 I2S 通道——会带入 Task 2 的 HAL power-level
任务：`audio_in`/`audio_out` 声明 caps standby 为 `off`，不在 Active/Dim 时关闭各自
的 I2S 通道。

---

### Task 2: HAL 电源级别与 caps `standby`

**Files:**
- Modify: `components/wisp_hal/include/wisp/hal/device.hpp`、`components/wisp_hal/src/device.cpp`、`components/wisp_hal/include/wisp/hal/{display,touch,imu}.hpp`（删除 `set_power(bool)`，`powered()` 保留）
- Modify: `components/wisp_port_esp/include/wisp/port/esp/{esp_display,esp_touch,esp_imu,esp_button}.hpp`、`components/wisp_port_esp/dev/{esp_display,esp_touch,esp_imu,esp_button}.cpp`、`components/wisp_port_sim/include/wisp/port/sim/sim_board.hpp`、`components/wisp_port_sim/dev/{sim_display,sim_devices.hpp,sim_touch,sim_imu,sim_button}.cpp`、`tests/support/fake_devices.hpp`、`tests/ui/{overlay_test,ui_runtime_test}.cpp`（测试里的显示器替身）
- Modify: `components/wisp_app/src/power_manager.cpp`、`components/wisp_app/src/self_check_overlays.cpp`（调用改为 `set_power(PowerLevel)`；`PowerManager` 的两态行为本任务不变）
- Modify: `AGENTS.md` §3"设备模型"（细化第 6 条那一句，同一提交，提交信息另起 `docs(agents)` 一行说明）
- Test: `tests/hal/power_level_test.cpp`（新，注册进 `tests/CMakeLists.txt`）、`tests/sim/sim_board_test.cpp`（追加）、`tests/app/power_manager_test.cpp`（改用新接口，不改断言）

**Interfaces:**
- Produces（`wisp::hal`，`device.hpp`）：

```cpp
enum class PowerLevel : uint8_t { Active, Dim, Standby, DeepStandby };
const char* to_string(PowerLevel level);      // "active" "dim" "standby" "deep_standby"
enum class StandbyPolicy : uint8_t { None, Off, Throttle, WakeSource };
// caps "standby": "off" | "throttle" | "wake_source"; missing or anything else: None.
StandbyPolicy standby_policy(const DeviceInfo& info);
// Off: powered only at Active and Dim. Throttle, WakeSource: always. None: true.
bool powered_at(StandbyPolicy policy, PowerLevel level);
class Device {
public:
    // Default: Unsupported ("power levels unsupported"). A device that declares
    // caps "standby" overrides it.
    virtual Result<void> set_power(PowerLevel level);
};
```

- 显示器的 `set_power(PowerLevel)` 仍要求在 UI 线程调用（`assert_owner_thread()`）；`Standby`/`DeepStandby` 时面板睡眠，`Active`/`Dim` 时唤醒，重复调用同一状态不重复发命令。触摸、IMU 同理（IMU 关闭期间 `read()` 失败，沿用现有约定；ESP 触摸只在驱动有睡眠钩子时声明 `"off"`，CST9217 没有，故不声明）。按键覆盖为返回 `ok` 的空操作（它是唤醒源，要一直工作）。
- `audio_in`、`audio_out` 声明 `"off"`（见细化第 6 条与硬件文档"浅睡兼容性（3b-1 spike）"一节）：ESP 端 `Standby`/`DeepStandby` 时关掉自己的 I2S 通道，`Active`/`Dim` 时重新打开，幂等。通道由板级代码从 bmgr 的 codec 设备配置取得后传入，外设名不进 `wisp_port_esp`。有流打开时、或 TX 的双工 RX 仍开着时，下行返回 `Busy` 并保持通道开着（TX 为 RX 提供时钟）。模拟器的音频设备声明同样的 caps，并记录一个 `powered` 标志。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(PowerLevel, Names)                 // to_string 四个值；DeepStandby -> "deep_standby"
TEST(StandbyPolicy, ParsedFromCaps)     // "off" Off, "throttle" Throttle, "wake_source" WakeSource, 缺失/"x"/数字 None
TEST(StandbyPolicy, PoweredAt)          // Off: Active/Dim true, Standby/DeepStandby false；其余三种全 true
TEST(Device, SetPowerDefaultsToUnsupported)  // 一个只实现 info() 的设备：ErrorCode::Unsupported
TEST(SimBoard, DevicesDeclareStandbyPolicy)  // display/touch/imu/audio_in/audio_out "off"；三个按键 "wake_source"；power/clock/storage 没有 "standby"
TEST(SimBoard, DisplayFollowsPowerLevel)     // set_power(Standby) -> powered()==false；set_power(Dim) -> true
TEST(SimBoard, AudioFollowsPowerLevel)       // 模拟器音频设备的 powered 标志随级别变化
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'PowerLevel|StandbyPolicy|Device\.|SimBoard' --output-on-failure` 编译失败。
- [ ] **Step 3: 实现** 上述接口；各端口设备在构造 `DeviceInfo` 时写 `caps["standby"]`；`PowerManager` 的 `enter_standby()`/`exit_standby()` 改调 `set_power(PowerLevel::Standby)`/`set_power(PowerLevel::Active)`，现有日志与 `Unsupported` 豁免不变。
- [ ] **Step 4: 验证** — 全量 ctest（`power_manager_test`、`self_check_screen_test`、`overlay_test`、`ui_runtime_test` 不改断言即通过）；`bash tools/ci.sh`；固件 `idf.py build`。
- [ ] **Step 5: Commit** — `feat(hal): give every device a power level and a declared standby policy`

---

### Task 3: 唤醒锁登记与五级 `PowerManager`

**Files:**
- Create: `components/wisp_core/include/wisp/core/wakelock.hpp`、`components/wisp_core/src/wakelock.cpp`
- Modify: `components/wisp_core/include/wisp/core/system_settings.hpp`、`src/system_settings.cpp`（细化第 7 条；`SystemSettingsValues` 加 `int deep_after_s; bool light_sleep; int notify_vol;`，`set()` 支持布尔）
- Modify: `components/wisp_app/include/wisp/app/power_manager.hpp`、`src/power_manager.cpp`、`include/wisp/app/app.hpp`（`Platform::set_standby` → `set_light_sleep`）、`src/app.cpp`、`src/app_impl.hpp`（`App::Impl` 持有 `core::WakeLockRegistry wakelocks`）、`src/self_check_sampler.cpp`/`src/self_check_screen.cpp`（`power.state` 的 `standby` 与 `deep_standby` 都算灭屏）、`src/net_wiring.cpp`（本任务只把 `standby()` 的读取保持编译通过，Wi-Fi 行为在 Task 4 改）
- Modify: `firmware/main/main.cpp`（**裁定 A**：`platform.set_light_sleep = [](bool on) { apply_pm_config(on); };`；**裁定 B**：不设置，`apply_pm_config(false)` 只在启动时调用一次，注释写明 Task 1 的结论与硬件文档的节名）；`simulator/main.cpp`（记录 `sim: light sleep on|off`）
- Modify: `docs/architecture.md`（`power.state` 取值、新事件 `power.wakelock`）
- Test: `tests/core/wakelock_test.cpp`（新）、`tests/app/power_manager_test.cpp`、`tests/core/system_settings_test.cpp`

**Interfaces:**
- Consumes：Task 2 的 `hal::PowerLevel`、`standby_policy()`、`Device::set_power(PowerLevel)`。
- Produces（`wisp::core`，`wakelock.hpp`）：

```cpp
enum class WakeLockKind : uint8_t { Network, Cpu, Screen };
const char* to_string(WakeLockKind kind);               // "network" "cpu" "screen"
struct WakeLockInfo {
    uint32_t id = 0;
    std::string holder;   // "system.<module>" or "scene.<id>"
    WakeLockKind kind = WakeLockKind::Cpu;
    std::string reason;
    uint64_t since_ms = 0;
};
class WakeLockRegistry {
public:
    static constexpr uint64_t kReminderMs = 30 * 60 * 1000;
    explicit WakeLockRegistry(EventBus& bus, std::function<uint64_t()> now_ms = os::now_ms);
    uint32_t acquire(std::string holder, WakeLockKind kind, std::string reason);  // id > 0
    bool release(uint32_t id);
    size_t release_holder(std::string_view holder);  // every lock of `holder`
    bool held(WakeLockKind kind) const;
    std::vector<WakeLockInfo> list() const;           // by id
    // Runs after every acquire/release, outside the lock, on the caller's thread.
    void set_on_change(std::function<void()> cb);
    // Locks whose held time crossed the next multiple of kReminderMs since
    // their last reminder (each multiple reported once).
    std::vector<WakeLockInfo> due_reminders(uint64_t now_ms);
};
class WakeLock {  // RAII, move-only; empty when default-constructed
public:
    WakeLock() = default;
    WakeLock(WakeLockRegistry& reg, std::string holder, WakeLockKind kind, std::string reason);
    ~WakeLock();             // release()
    void release();
    bool held() const;
};
```

- 事件 `power.wakelock`：`source = "power"`，`payload = {"op": "acquire"|"release", "holder", "kind", "reason"}`，在 `acquire`/`release` 里发布。
- Produces（`wisp::app`）：

```cpp
struct PowerManagerDeps {
    core::EventBus& bus;
    core::DeviceRegistry& registry;
    core::WakeLockRegistry& locks;
    core::Settings settings;                        // namespace "power"
    std::shared_ptr<hal::Display> display;          // brightness only; may be null
    core::StorageManager* storage = nullptr;
    core::PowerMonitor* power_monitor = nullptr;
    ui::UiRuntime* ui = nullptr;
    core::ButtonService* buttons = nullptr;
    std::function<void(bool enable)> set_light_sleep;  // Platform::set_light_sleep; may be empty
};
class PowerManager {
public:
    static constexpr uint32_t kDimLeadMs = 10000;
    static constexpr int kDimPercent = 30;
    explicit PowerManager(PowerManagerDeps deps);
    void enter_standby();          // any level -> Standby (power key, idle)
    void exit_standby();           // any level -> Active
    hal::PowerLevel level() const;
    bool standby() const;          // level() >= Standby (screen off)
    bool light_sleep_on() const;   // what set_light_sleep was last told
    bool light_sleep_supported() const;  // set_light_sleep is set
    void note_activity();          // Dim -> Active (posted to the UI task)
    void tick(uint64_t now_ms);    // Active->Dim->Standby->DeepStandby and back on locks
    void set_brightness(uint8_t level);
    void set_power_key_owned(bool owned);
};
```

- 级别规则（spec 4.1–4.3）：
  - 有 `screen` 锁：`tick()` 视为有操作，不进 `Dim`/`Standby`；电源键短按仍进 `Standby`（用户意愿优先，锁保留）。
  - `Active` → `Dim`：空闲 ≥ `screen_off_s·1000 − kDimLeadMs`；`Dim` 只改亮度（细化第 5 条），在 UI 线程上执行。
  - → `Standby`：空闲 ≥ `screen_off_s·1000` 或 `enter_standby()`。暂停 UI，对每个 `standby_policy() != None` 的设备调 `set_power(Standby)`（按注册顺序；唤醒时逆序），`StorageManager`/`PowerMonitor`/`ButtonService` 的现有待机调用保留。音频例外，不受注册顺序影响：下行时先 `audio_in` 后 `audio_out`，上行时先 `audio_out` 后 `audio_in`（双工下 TX 为 RX 提供时钟；ESP 的 `audio_out` 在 RX 仍开着时返回 `Busy` 并保持开着，见 `wisp/port/esp/i2s_power.hpp`）。`Busy` 记一行日志，下次级别变化时再试。
  - `Standby` → `DeepStandby`：在 `Standby` 已满 `deep_after_s`（> 0）且没有 `network` 锁；设备再收到 `set_power(DeepStandby)`。计时从进入 `Standby` 起算，期间锁来锁去不重置：窗口锁释放后立即回到 `DeepStandby`。
  - `DeepStandby` → `Standby`：出现 `network` 锁。
  - 浅睡：`want = level()==DeepStandby && settings "light_sleep" && !locks.held(Cpu) && set_light_sleep`；与上次不同时调用钩子。`locks.set_on_change()` 投递一次重新评估（有 UI 线程时 `post`，否则直接调用），不必等下一次 `tick()`。
  - 每次级别变化发布 `power.state {"state": to_string(level)}`，并以 Info 记 `power: <level>`。
- 消费 `power.state` 的旧代码：自检采样器与场景接线以 `standby`/`deep_standby` 都为灭屏（`dim` 仍亮屏）。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(WakeLockRegistry, AcquireReleaseListAndEvents)  // acquire("scene.a", Screen, "reading") -> id 1，list 一项；
                                                     // 事件 {"op":"acquire","holder":"scene.a","kind":"screen","reason":"reading"}；release(1) 发 release
TEST(WakeLockRegistry, HeldByKind)
TEST(WakeLockRegistry, ReleaseHolderDropsAll)        // scene.a 两把、system.x 一把 -> release_holder("scene.a")==2
TEST(WakeLockRegistry, RemindersEvery30Minutes)      // 假时钟：29:59 无；30:00 一条；59:59 无；60:00 一条；释放后不再有
TEST(WakeLock, RaiiReleasesOnDestruction)
TEST(SystemSettings, PowerAndNotifyKeys)             // deep_after_s 默认 300，0 与 60..3600 可设，59/3601 InvalidArgument；
                                                     // light_sleep 默认 true，只收布尔；notify_vol 0..100 默认 60
TEST(PowerManager, DimTenSecondsBeforeScreenOff)     // screen_off_s 60、亮度 200：49 s Active；50 s Dim、亮度 60；note_activity -> Active、亮度 200
TEST(PowerManager, DimWithShortestTimeout)           // screen_off_s 15 -> 5 s 时 Dim
TEST(PowerManager, StandbyWalksDevicesByPolicy)      // off 设备 set_power(Standby)；wake_source 设备也收到但保持 powered；
                                                     // 没有 standby cap 的设备没被调用；Unsupported 不记警告
TEST(PowerManager, DeepStandbyAfterDeepAfterS)       // 300 s 后 DeepStandby，事件 "deep_standby"，设备收到 DeepStandby
TEST(PowerManager, DeepAfterZeroNeverGoesDeep)
TEST(PowerManager, NetworkLockBlocksAndLeavesDeepStandby)  // 有锁时停在 Standby；DeepStandby 中加锁 -> Standby；释放 -> 立即 DeepStandby
TEST(PowerManager, LightSleepFollowsCpuLockAndSetting)     // 钩子调用序列：进 Deep -> true；加 cpu 锁 -> false；释放 -> true；
                                                           // light_sleep=false 时始终不调 true；离开 Deep -> false
TEST(PowerManager, NoLightSleepHookStillGoesDeep)    // set_light_sleep 为空：照常进 DeepStandby，light_sleep_supported()==false
TEST(PowerManager, ScreenLockKeepsActiveButPowerKeyStillSleeps)
TEST(PowerManager, WakeFromDeepStandbyRestoresAll)   // exit_standby(): 设备逆序 Active，UI 恢复，钩子 false，事件 "active"
TEST(PowerManager, StateEventCarriesEveryLevel)      // active dim standby deep_standby 依次出现
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'WakeLock|PowerManager|SystemSettings' --output-on-failure`。
- [ ] **Step 3: 实现**；`App::start()` 用 `PowerManagerDeps` 构造（`display` 仍取 `display.main` 用于亮度），`wisp.power` 定时器照旧每秒 `tick()`；`firmware/main/main.cpp` 按 Task 1 的裁定设置或不设置钩子。
- [ ] **Step 4: 验证** — 全量 ctest（`power_key_controller_test`、`self_check_screen_test`、`app_boot_test`、`app_net_test` 通过）；`bash tools/ci.sh`；固件 `idf.py build`。
- [ ] **Step 5: Commit** — `feat(app): five power levels driven by wake locks`

---

### Task 4: 深度待机的联网：关 Wi-Fi、延迟队列、联网窗口与系统锁

**Files:**
- Create: `components/wisp_net/include/wisp/net/network_window.hpp`、`components/wisp_net/src/network_window.cpp`
- Modify: `components/wisp_hal/include/wisp/hal/wifi.hpp`（`set_radio`）、`components/wisp_port_esp/net/esp_wifi.cpp` 与头文件、`components/wisp_port_sim/net/sim_wifi.cpp` 与 `include/wisp/port/sim/sim_wifi.hpp`、`tests/support/fake_wifi.hpp`
- Modify: `components/wisp_net/include/wisp/net/wifi_service.hpp`、`src/wifi_service.cpp`（细化第 2 条；删除 `idle_off_min`）
- Modify: `components/wisp_net/include/wisp/net/scene_http.hpp`、`src/scene_http.cpp`（延迟队列）、`components/wisp_scene/src/bind_http.cpp`（回调表加 `deferred`）
- Modify: `components/wisp_app/src/net_wiring.{hpp,cpp}`（读 `PowerManager::level()`、订阅 `power.state` 唤醒自己的循环、驱动 `NetworkWindow`、校时时持 `system.ntp` 锁、热点开着时持 `system.hotspot` 锁）、`components/wisp_admin/include/wisp/admin/admin_server.hpp`（`AdminDeps::wakelocks`）、`components/wisp_admin/src/{ws_hub.cpp,admin_server.cpp,routes_firmware.cpp,routes_scenes.cpp}`（细化第 4 条的锁）、`components/wisp_app/src/scene_wiring.{hpp,cpp}`（把 `SceneHttp*` 交给 `NetWiring`）
- Test: `tests/net/network_window_test.cpp`（新）、`tests/net/scene_http_test.cpp`、`tests/net/wifi_service_test.cpp`（`idle_off_min` 的用例改写为深度待机）、`tests/scene/http_binding_test.cpp`、`tests/app/app_net_test.cpp`、`tests/admin/{ws_hub_test,routes_firmware_test,routes_scenes_test}.cpp`

**Interfaces:**
- Consumes：Task 3 的 `WakeLockRegistry`、`WakeLock`、`PowerManager::level()`；Task 2 的 `hal::PowerLevel`。
- Produces：

```cpp
// wisp::hal::Wifi
virtual Result<void> set_radio(bool on);   // default Unsupported; idempotent
// wisp::net::WifiService (replaces set_standby)
void set_power_level(hal::PowerLevel level, uint64_t now_ms);
// wisp::net::SceneHttp
struct SceneHttpLimits { ...; size_t max_deferred_per_scene = 4; uint64_t deferred_timeout_ms = 3600000; };
struct SceneHttpResult { ...; bool deferred = false; };
void set_deferring(bool on);     // on: accepted requests wait; off: queued ones move to the run queue in order
size_t deferred_count() const;
bool idle() const;               // nothing deferred, queued or in flight
// wisp::net, network_window.hpp
struct NetworkWindowDeps {
    core::WakeLockRegistry& locks;
    std::function<size_t()> pending;          // SceneHttp::deferred_count
    std::function<bool()> http_idle;          // SceneHttp::idle
    std::function<void(bool)> set_deferring;  // SceneHttp::set_deferring
    std::function<bool()> wifi_connected;
};
class NetworkWindow {
public:
    static constexpr uint64_t kIntervalMs = 15 * 60 * 1000;
    static constexpr uint64_t kConnectTimeoutMs = 60000;
    static constexpr uint64_t kTimeSyncWaitMs = 30000;
    static constexpr uint64_t kMaxOpenMs = 5 * 60 * 1000;
    explicit NetworkWindow(NetworkWindowDeps deps);
    void tick(uint64_t now_ms, hal::PowerLevel level);
    void note_time_result();   // a system.time event arrived
    bool open() const;
};
```

- `SceneHttp` 延迟队列：`set_deferring(true)` 期间，通过全部检查和频率限额的请求进入该场景的延迟表；表里已有 4 条时立即 `done("deferred_queue_full")`；`set_deferring(false)` 时按提交顺序移入执行队列（执行队列满时留在延迟表里，下次再移），结果带 `deferred = true`；worker 有延迟请求时最多等 60 秒醒一次，把排队超过 1 小时的请求以 `done("deferred_timeout")` 结束；`cancel_scene()` 同样丢弃该场景的延迟请求。`bind_http.cpp` 在回调结果表里放 `deferred = true`（只在为真时）。
- `NetworkWindow::tick` 的规则：
  - 不在 `DeepStandby`、窗口也没开：`set_deferring(false)`，记下"下次窗口时刻"为空。
  - 刚进入 `DeepStandby`：`set_deferring(true)`，下次窗口 = now + 15 分钟。
  - 窗口未开、在 `DeepStandby`、`pending() > 0`、到了下次窗口时刻：取 `system.netwindow` 的 `network` 锁（原因 `"deferred requests"`），记 `netwindow: open`。
  - 窗口开着：连上之前保持 `set_deferring(true)`；`wifi_connected()` 后 `set_deferring(false)`；`http_idle()` 且（已收到 `note_time_result()` 或连上已过 30 秒）时释放锁，记 `netwindow: closed (done)`；60 秒没连上记 `closed (no wifi)`；开满 5 分钟记 `closed (timeout)`。关闭后下次窗口 = now + 15 分钟。
- 系统锁（细化第 4 条）：`WsHub` 第一个连接打开时取 `system.admin`，最后一个关闭时释放；场景上传、固件上传的流式处理在开始时取、结束或中止时放；固件写入期间另取 `cpu` 锁。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(WifiService, DeepStandbyTurnsRadioOffAndBack)   // set_power_level(DeepStandby): disconnect、热点关、set_radio(false)，状态 off；
                                                     // set_power_level(Active): set_radio(true)，状态 connecting
TEST(WifiService, StandbyKeepsTheStation)            // Standby 一小时后仍 connected，set_radio 未被调用
TEST(SceneHttp, DeferringQueuesUpToFourPerScene)     // 第 5 条立即 "deferred_queue_full"；另一个场景不受影响
TEST(SceneHttp, ResumeSendsInOrderMarkedDeferred)    // set_deferring(false) 后假客户端按提交顺序收到，结果 deferred==true
TEST(SceneHttp, DeferredTimeoutAfterOneHour)         // 假时钟 3600001 ms -> "deferred_timeout"
TEST(SceneHttp, DeferredStillRateLimited)            // 每分钟第 31 条 "rate_limited"
TEST(SceneHttp, CancelSceneDropsDeferred)            // 不回调
TEST(NetworkWindow, OpensOnlyWithPendingEvery15Minutes)  // 进入 Deep 后 14:59 不开；15:00 有待发才开；无待发不开
TEST(NetworkWindow, FlushesAfterConnectAndWaitsForTimeSync)  // 连上前 deferring==true；连上后 false；idle + time 结果后释放锁
TEST(NetworkWindow, GivesUpAfterConnectTimeout)      // 60 s 未连上：释放锁、deferring 仍 true、下次 15 分钟后
TEST(NetworkWindow, ClosesAfterFiveMinutes)
TEST(NetworkWindow, LeavingDeepStandbyStopsDeferring)
TEST(HttpBinding, DeferredFlagReachesCallback)       // res.deferred == true；非延迟的请求 res.deferred == nil
TEST(WsHub, HoldsAdminLockWhileConnected)
TEST(AdminFirmware, UploadHoldsNetworkAndCpuLocks)   // 上传中 list() 含 system.ota 两把；结束后为空
TEST(AdminScenes, UploadHoldsNetworkLock)
TEST(AppNet, DeepStandbyTurnsWifiOffAndWakeTurnsItOn)  // App 级：PowerManager 进 Deep 后 FakeWifi radio off；exit_standby 后 on
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'WifiService|SceneHttp|NetworkWindow|HttpBinding|WsHub|AdminFirmware|AdminScenes|AppNet' --output-on-failure`。
- [ ] **Step 3: 实现**；`docs/architecture.md` 删掉 `idle_off_min` 的描述（如有），在"真机的网络"一节写联网窗口与系统锁。
- [ ] **Step 4: 验证** — 全量 ctest；`bash tools/ci.sh`；固件 `idf.py build`。
- [ ] **Step 5: Commit** — `feat(net): turn Wi-Fi off in deep standby and send scene requests in windows`

---

### Task 5: `NotificationService`

**Files:**
- Create: `components/wisp_core/include/wisp/core/notification_service.hpp`、`components/wisp_core/src/notification_service.cpp`
- Modify: `components/wisp_core/CMakeLists.txt`、`components/wisp_app/src/{app.cpp,app_impl.hpp}`（构造、启动时在 `wisp.fs` 上 `load()`；`power.battery.low` → `battery_low`；`wisp.power` 定时器里 `due_reminders()` → `wakelock_long`；`scene.state` 为 `deleted` → `dismiss_source`）、`docs/architecture.md`（`notification.posted`/`notification.dismissed`）
- Test: `tests/core/notification_service_test.cpp`（新）、`tests/app/app_boot_test.cpp`（追加）

**Interfaces:**
- Consumes：Task 3 的 `WakeLockRegistry::due_reminders()`、`WakeLockInfo`。
- Produces（`wisp::core`）：

```cpp
enum class NotifySound : uint8_t { None, Tone, Alarm };
std::optional<NotifySound> parse_notify_sound(std::string_view s);   // "none" "tone" "alarm"
enum class DismissReason : uint8_t { Tap, Dismiss, Cancel, Evict };
const char* to_string(DismissReason r);                               // "tap" "dismiss" "cancel" "evict"
namespace notice {   // system notification codes and actions
inline constexpr const char* kSceneEvictedMemory = "scene_evicted_memory";
inline constexpr const char* kSceneEvictedCount = "scene_evicted_count";
inline constexpr const char* kSceneError = "scene_error";
inline constexpr const char* kRestoreFailed = "restore_failed";
inline constexpr const char* kWakeLockLong = "wakelock_long";
inline constexpr const char* kBatteryLow = "battery_low";
inline constexpr const char* kActionReleaseLock = "release_lock";
}
struct Notification {
    std::string source;        // "system" or a scene id
    std::string id;            // unique per source
    std::string source_name;   // scene name when posted; "" for system
    std::string title, body;   // scenes
    std::string code, arg;     // system
    std::string action, action_arg;   // "" or notice::kActionReleaseLock + lock id
    NotifySound sound = NotifySound::None;
    bool wake = false;
    uint64_t posted_ms = 0;
    int64_t posted_unix = 0;   // 0: clock unset
};
struct NotificationChange {
    enum class Kind : uint8_t { Posted, Dismissed } kind;
    Notification n;
    bool listed = true;        // false: a foreground scene's own post (banner only)
    DismissReason reason = DismissReason::Dismiss;
};
struct NotificationDeps {
    EventBus& bus;
    StorageManager& storage;
    std::function<bool(std::function<void()>)> submit_io;           // StorageWorker::submit
    std::function<void(uint32_t ms, std::function<void()>)> schedule;
    std::function<uint64_t()> now_ms = os::now_ms;
    std::function<int64_t()> wall_s;                                 // 0 or empty: unknown
};
class NotificationService {
public:
    static constexpr size_t kTitleMax = 32, kBodyMax = 200, kIdMax = 32;
    static constexpr size_t kUnreadPerScene = 5, kPerMinute = 10, kStoredMax = 50;
    static constexpr uint32_t kCoalesceMs = 300;
    explicit NotificationService(NotificationDeps deps);
    Result<void> load();   // <System>/notifications.json; on wisp.fs; missing file is empty
    // Scene post: InvalidArgument for bad fields, RateLimited "rate_limited".
    // Returns the id (given or "n<seq>").
    Result<std::string> post(Notification n, bool from_foreground);
    std::string post_system(std::string code, std::string arg,
                            std::string action = {}, std::string action_arg = {});  // id "<code>:<arg>"
    bool cancel(std::string_view source, std::string_view id);       // reason Cancel
    bool dismiss(std::string_view source, std::string_view id, DismissReason reason);
    size_t dismiss_source(std::string_view source, DismissReason reason);
    void clear_all();                                                // each: Dismiss
    std::optional<Notification> find(std::string_view source, std::string_view id) const;
    std::vector<Notification> unread() const;                        // newest first
    size_t unread_count() const;
    // Runs on the caller's thread after the change, outside the lock.
    void set_listener(std::function<void(const NotificationChange&)> cb);
    bool flush(uint32_t timeout_ms);   // queue the pending write and wait for it (shutdown)
};
bool valid_notification_id(std::string_view id);   // ^[a-z0-9_-]{1,32}$
```

- 事件（spec 5.7）：`notification.posted` `{"id","source","title"}`、`notification.dismissed` `{"id","source","reason"}`，`Event::source = "notifications"`，不含正文。同 id 替换时只发一条 `posted`（旧的不发 `dismissed`）。场景超出 5 条或总数超出 50 条时被丢掉的那条发 `dismissed` `reason = "evict"`。
- 持久化：任何变化后 `schedule(300)` 合并，到点在 `submit_io` 上 `write_atomic` 整个 JSON 数组（`{"source","id","source_name","title","body","code","arg","action","action_arg","sound","posted_unix"}`；`wake` 不存，重启后不再亮屏）。`App::stop()` 调 `flush(1000)`。
- 不访问 flash 的方法可以在场景任务上调用（`post`、`cancel` 等只改内存并安排写入）。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(Notifications, PostReturnsIdAndPublishesWithoutBody)  // 事件 payload 只有 id/source/title；无 "body" 键
TEST(Notifications, GeneratedIdsAreUnique)                  // 不给 id：n1、n2
TEST(Notifications, SameIdReplaces)                         // unread_count 不变，内容为新的
TEST(Notifications, FiveUnreadPerSceneDropsOldest)          // 第 6 条进来后最早那条 dismissed reason "evict"
TEST(Notifications, RateLimitTenPerMinute)                  // 第 11 条 RateLimited，消息 "rate_limited"；60 s 后可以
TEST(Notifications, ForegroundPostsAreNotListed)            // listener listed==false；unread 为空；仍发 posted
TEST(Notifications, FiftyStoredOldestDropped)               // 11 个场景各 5 条 -> 只留 50
TEST(Notifications, ValidationErrors)                       // title "" / 33 字节、body 201、id "A!"、id 33 字节 -> InvalidArgument
TEST(Notifications, CancelDismissAndReasons)                // cancel -> "cancel"；dismiss(Tap) -> "tap"；不存在 -> false
TEST(Notifications, DismissSourceRemovesAll)
TEST(Notifications, SystemNoticesBypassRateLimitAndReplace) // 同 code+arg 两次只留一条
TEST(Notifications, PersistedCoalescedAndReloaded)          // 300 ms 内 3 次 post -> 1 次 write_atomic；新实例 load() 后 unread 相同、wake==false
TEST(AppNotifications, BatteryLowPostsSystemNotice)         // 发布 power.battery.low {battery_percent:12} -> code battery_low、arg "12"
TEST(AppNotifications, WakeLockReminderPosts)               // 场景锁满 30 分钟 -> wakelock_long、arg "scene.a"、action release_lock、action_arg 为锁 id；
                                                            // 系统锁 -> 无 action
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'Notifications|AppNotifications' --output-on-failure`。
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 运行，确认通过**；`bash tools/ci.sh`。
- [ ] **Step 5: Commit** — `feat(core): add the notification service`

---

### Task 6: 多场景调度与后台生命周期

**Files:**
- Create: `components/wisp_scene/src/bind_power.cpp`
- Modify: `components/wisp_core/include/wisp/core/scene_manifest.hpp`、`src/scene_manifest.cpp`（`bool background = false;`，非布尔报 `scene.json: background must be a boolean`）
- Modify: `components/wisp_core/include/wisp/core/scene_host.hpp`、`src/scene_host.cpp`（`SceneState::Background`、`exit()`）
- Modify: `components/wisp_scene/include/wisp/scene/scene_runtime.hpp`、`src/{scene_runtime,scene_instance,scene_message,scene_api,bind_scene,constructors}.{hpp,cpp}`（`Foreground` 改为 `Slot` 列表；`ui.ref():focus()` 在后台返回 `false, "not_in_background"`）、`components/wisp_scene/CMakeLists.txt`
- Modify: `components/wisp_app/src/scene_wiring.cpp`（`on_notice` → `NotificationService::post_system`；`locks` 传入）、`components/wisp_admin/src/routes_scenes.cpp`（`POST /scenes/{id}/exit`）、`docs/architecture.md`（`scene.state` 加 `background`）
- Test: `tests/scene/scene_background_test.cpp`（新，`TIMEOUT 90`，同 `scene_runtime_test` 加 `src` 私有 include）、`tests/core/scene_manifest_test.cpp`、`tests/scene/scene_runtime_test.cpp`（不改断言即通过）、`tests/admin/routes_scenes_test.cpp`

**Interfaces:**
- Consumes：Task 3 的 `WakeLockRegistry`；Task 5 的 `NotificationService::post_system`、`notice::*`。
- Produces（`wisp::core`）：`enum class SceneState { Stopped, Running, Background, Error };`（`to_string` 加 `"background"`）；`SceneHost::exit(const std::string& id)`（纯虚，彻底关闭；不在运行时 `Ok`）；`SceneManifest::background`。
- Produces（`wisp::scene`）：

```cpp
enum class LaunchReason : uint8_t { User, Boot, Notification };
const char* to_string(LaunchReason r);               // "user" "boot" "notification"
enum class SceneNoticeKind : uint8_t { EvictedMemory, EvictedCount, Faulted, RestoreFailed };
struct SceneNotice { SceneNoticeKind kind; std::string id; std::string name; };
struct BackgroundScene { std::string id; std::string name; uint64_t since_ms = 0; };
struct SceneRuntimeDeps {
    ...;  // 3a fields unchanged
    core::WakeLockRegistry* locks = nullptr;              // wisp.power; null: keep_screen_on returns nil, "unavailable"
    std::function<void(const SceneNotice&)> on_notice;    // system notifications
};
class SceneRuntime : public core::SceneHost {
public:
    ...;
    Result<void> exit(const std::string& id) override;
    // User/Notification: open in the foreground (a background one comes
    // forward without reloading). Boot: start in the background without a view.
    Result<void> launch(const std::string& id, LaunchReason reason, Value args = {});
    std::vector<BackgroundScene> background_scenes() const;   // any thread; LRU order
};
// SceneInstance (private header)
std::optional<ScriptError> rebuild();   // Build of the retained root with current values (refinement 10)
```

- 调度（spec 3.1–3.4）：仍是一个 `wisp.scene` 任务；`Slot` 列表最多 1 个前台、`max_background` 个后台（本任务固定 3，Task 7 才淘汰：本任务里第 4 个后台场景先直接拒绝转入后台、改为关闭，Task 7 替换为淘汰）。每个 `Slot` 有自己的 `instance`、`TimerSet`、`MinuteTicker`、`last_front_ms`、`background_since_ms`、`LaunchReason`。`target(m)` 按 `instance` 在全部 `Slot` 里找。
- 转换（spec 3.2 表）：
  - 返回手势、`wisp.scene.close()`：声明了 `background` → 转入后台；否则关闭。
  - `open(B)` 时前台是 A：A 按上一条处理，然后打开 B；B 已在后台 → 切回前台。
  - 转入后台：`on_hide`（如果 shown）→ 释放该场景的全部锁（`locks->release_holder("scene.<id>")`）→ UI 线程删视图、回到原屏幕 → `on_state("background")` → 状态 `Background`、发 `scene.state background`。之后该场景每条消息结束时清空自己的 `PatchBatch`，不提交（`ui_instance_` 不指向它）。
  - 切回前台：`rebuild()` → UI 线程建新视图并加载 → `on_show`（亮屏时）→ `on_state("foreground")` → `Running`。
  - `exit()`/面板 ×/管理后台：任何状态都彻底关闭（`on_hide`（如 shown）→ `on_close` → 刷盘 → 销毁 VM）。
  - 后台出错：`Error`，销毁 VM、取消定时器与 HTTP，不弹错误面板，`on_notice({Faulted, id, name})`。
- 定时器：声明了 `background` 的场景在后台、灭屏时都不暂停；`set_visible(false)` 对它们只调 `on_hide`（前台时）和刷盘；`minute`/`battery`/`lang` 照常（细化第 9 条）。没有声明的前台场景保持 3a。
- `wisp.scene` 新增：`state()` → `"foreground"`/`"background"`；`launch()` → 启动原因；`exit()` → 当前消息结束后彻底关闭。`wisp.power.keep_screen_on(on)`（`bind_power.cpp`）：前台时取或放 `screen` 锁（持有者 `scene.<id>`，原因 `"keep_screen_on"`），返回 `true`；后台返回 `nil, "not_in_background"`。
- `SceneWiring`：`on_notice` → `post_system(EvictedMemory→kSceneEvictedMemory, EvictedCount→kSceneEvictedCount, Faulted→kSceneError, RestoreFailed→kRestoreFailed; arg = name)`；系统层的返回处理改为 `SceneRuntime` 的"返回"（转入后台或关闭）。

- [ ] **Step 1: 写失败的测试**（夹具 `SceneRuntimeFixture`，测试包用 `manifest()` 加 `"background":true`）

```cpp
TEST(SceneManifest, BackgroundField)                       // true/false/缺省 false；"yes" -> 错误信息如上
TEST(SceneBackground, BackGestureSendsDeclaredSceneToBackground)
    // 日志顺序 show, hide, state:background；视图已删、屏幕回到自检；status Background；事件 {id,"background"}
TEST(SceneBackground, BackGestureClosesUndeclaredScene)    // 同 3a
TEST(SceneBackground, OpeningAnotherSendsDeclaredToBackground)
TEST(SceneBackground, OpeningBackgroundSceneResumesWithoutReload)
    // main.lua 只执行一次（日志计数）；计数器的值保留；show 与 state:foreground 各一次；label 显示后台期间改过的值
TEST(SceneBackground, BackgroundUpdatesTreeWithoutRendering)   // 后台定时器改状态 -> pending_patches()==0
TEST(SceneBackground, RebuildKeepsRefsWorking)             // 切回前台后 click_ref("plus") 仍生效
TEST(SceneBackground, ExitClosesFromAnyState)              // 后台场景 exit：hide 不再调用、close 调用、Stopped
TEST(SceneBackground, ErrorInBackgroundStopsAndNotifies)   // 后台定时器回调 error -> Error、无错误面板、on_notice Faulted
TEST(SceneBackground, TimersRunWhileScreenOffForDeclaredScene)   // set_visible(false) 期间 every(100) 照常触发
TEST(SceneBackground, UndeclaredSceneTimersStillPause)
TEST(SceneBackground, ForbiddenApisInBackground)           // keep_screen_on(true) -> nil,"not_in_background"；ref:focus() -> false,"not_in_background"
TEST(SceneBackground, ScreenLockFollowsForeground)         // 前台 keep_screen_on(true) -> locks.held(Screen)；转入后台 -> 释放
TEST(SceneBackground, SceneCloseApiMeansBack)              // 声明了 background 的场景 wisp.scene.close() -> Background
TEST(SceneBackground, StateAndLaunchQueries)               // state()=="foreground"，转后台后定时器里读到 "background"；launch()=="user"
TEST(SceneBackground, CallbacksSurviveBackgroundAndDieWithExit)
    // 前台发 HTTP（假客户端延迟 300 ms）后转入后台：回调执行；再发一次后 exit：回调不执行，stale_dropped 增加
TEST(SceneBackground, FourthBackgroundIsClosedForNow)      // 本任务的临时行为，Task 7 改为淘汰（届时删掉这个测试）
TEST(AdminScenes, ExitRoute)                               // 200；未知 id 404；无 runtime 503；无 X-Wisp 403
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'SceneManifest|SceneBackground|SceneRuntime|AdminScenes' --output-on-failure`。
- [ ] **Step 3: 实现**；`scene_runtime.hpp` 的文件头注释按新的多场景模型重写（线程、实例、生命周期、后台）。
- [ ] **Step 4: 验证** — 全量 ctest（`scene_runtime_test`、`scene_api_test`、`examples_test`、`app_scenes_test` 不改断言即通过）；`bash tools/ci.sh`；固件 `idf.py build`。
- [ ] **Step 5: Commit** — `feat(scene): run up to three scenes in the background`

**执行记录**：
- 转入后台按 spec 3.2 的顺序：`on_hide` 是场景在前台的最后一步（其中 `wisp.scene.state()` 为 `"foreground"`，`keep_screen_on` 照常可用）→ 释放唤醒锁 → 撤掉视图 → `on_state("background")`。`on_hide` 出错时视图照样撤掉，场景按后台出错处理（系统通知、不弹错误面板、槽位移除），因为用户已经要求离开（裁定）。
- 待加载用按实例的列表（`PendingLoad`）代替单个 `loading_`：用户/通知打开互相取代（最新的赢），并取代同一场景的开机启动；开机启动遇到已在运行或正在加载的同一场景什么都不做，不会取消正在加载的用户打开。
- 每次显示视图取一个令牌（`ui_view_`），帧钩子和 `click_ref` 按令牌而不是实例号匹配视图（同一场景转入后台再切回时实例号不变）。
- `SceneRuntime` 内部有 `kMaxBackground`/`kMaxSlots` 私有常量；Task 7 引入 `wisp::scene::kMaxBackground` 和 `config.max_background` 时把它们统一掉。

---

### Task 7: VM 总预算、淘汰与开机恢复

> 来自多场景调度评审（执行时处理）：Boot 启动 X 时若用户打开的 X 仍在加载，会返回 Ok 而不排队；之后用户再打开 Y 会替换掉 X 的加载，X 的恢复就丢了。`restore_background` 要保证每个要恢复的场景最终都起来，或者发出 RestoreFailed 通知。

**Files:**
- Create: `components/wisp_scene/include/wisp/scene/scene_schedule.hpp`、`components/wisp_scene/src/scene_schedule.cpp`
- Modify: `components/wisp_scene/include/wisp/scene/vm.hpp`、`src/vm.cpp`（`VmPool`、`VmLimits::pool`）、`include/wisp/scene/scene_runtime.hpp`、`src/scene_runtime.cpp`
- Modify: `components/wisp_app/src/{scene_wiring.cpp,app.cpp}`（`background_list_path`；`start_runtime()` 之后、发布 `system.boot.completed` 时 `restore_background()`）
- Test: `tests/scene/vm_test.cpp`（追加）、`tests/scene/scene_schedule_test.cpp`（新）、`tests/scene/scene_background_test.cpp`（追加，删掉 `FourthBackgroundIsClosedForNow`）、`tests/app/app_scenes_test.cpp`

**Interfaces:**
- Consumes：Task 6 的 `Slot`、`SceneNotice`、`LaunchReason::Boot`、`launch()`。
- Produces：

```cpp
// wisp/scene/vm.hpp
inline constexpr size_t kVmBudgetBytes = 3 * 1024 * 1024;
class VmPool {
public:
    explicit VmPool(size_t budget_bytes = kVmBudgetBytes);
    bool reserve(size_t bytes);   // false (nothing reserved) past the budget
    void release(size_t bytes);
    size_t used() const;
    size_t budget() const;
};
struct VmLimits { ...; VmPool* pool = nullptr; };   // null: no shared budget (3a behaviour)
// wisp/scene/scene_schedule.hpp
inline constexpr size_t kMaxBackground = 3;
struct SlotInfo { std::string id; bool foreground = false; uint64_t last_front_ms = 0; size_t vm_bytes = 0; };
struct Eviction { std::string id; bool memory = false; };   // memory: budget, else count
// Background scenes to close, least recently in front first: past max_background,
// then (when a foreground scene exists or `opening`) while background bytes
// exceed budget - foreground_reserve.
std::vector<Eviction> pick_evictions(std::span<const SlotInfo> slots, bool opening,
                                     size_t max_background, size_t budget, size_t foreground_reserve);
// SceneRuntimeConfig
size_t vm_budget = kVmBudgetBytes;
size_t max_background = kMaxBackground;
// SceneRuntimeDeps
std::string background_list_path;   // <System>/scenes/background.json; empty: not persisted
// SceneRuntime
void restore_background();          // reads the list on wisp.fs, launch(id, Boot) for each, in order
```

- 检查点（spec 3.5）：打开场景、转入后台、每条前台消息之前调用 `pick_evictions()`，逐个正常关闭（`on_close`、刷盘）并 `on_notice(EvictedMemory/EvictedCount)`。分配中途不淘汰：`VmPool::reserve` 失败就让这次分配失败（3a 的内存超限处理）。
- 列表（细化第 12 条）：进入或离开后台（转入、切回、关闭、淘汰、出错）后在 `wisp.fs` 上写 `{"ids": [...]}`；`stop()` 不写。恢复：建 VM → `main.lua` → 建声明树，不建视图、不调 `on_show`，`on_state("background")`；失败时跳过、`on_notice(RestoreFailed)`，并把它从列表移除后重写。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(VmPool, ReserveAndRelease)
TEST(VmPool, AllocationFailsPastPool)          // 两个 Vm 共用 1 MiB 池：第二个分配大表时报内存错误，第一个不受影响
TEST(SceneSchedule, CountEvictsLeastRecentlyFront)   // 4 个后台 -> 淘汰 last_front_ms 最小的，memory==false
TEST(SceneSchedule, MemoryEvictsUntilReserveFits)    // budget 3 MiB、reserve 2 MiB：后台 0.6+0.6 MiB -> 淘汰较旧的一个
TEST(SceneSchedule, NoMemoryEvictionWithoutForeground)
TEST(SceneBackground, FourthBackgroundEvictsOldestWithNotice)   // 已有 a,b,c 后台，d 转入 -> a 关闭（on_close 被调用）、on_notice EvictedCount
TEST(SceneBackground, ForegroundReserveEvictsBackground)       // config.vm_budget 小值：打开大场景前淘汰后台，on_notice EvictedMemory
TEST(SceneRestore, BackgroundListWrittenOnTransitions)         // 文件内容依次为 ["a"]、["a","b"]、["b"]
TEST(SceneRestore, RestoresInBackgroundWithoutView)            // launch()=="boot"；没有 show；state:background；status Background
TEST(SceneRestore, FailedRestoreSkippedNotifiedAndDropped)     // 列表含已删除的 "gone" -> RestoreFailed，重写后的文件不含它；其他照常恢复
TEST(SceneRestore, StopKeepsTheList)                           // 两个后台场景时 rt->stop()，文件仍为两项
TEST(AppScenes, RestoresAfterBootCompleted)                    // 预置 background.json，App 启动后两个场景为 Background
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'VmPool|SceneSchedule|SceneBackground|SceneRestore|AppScenes' --output-on-failure`。
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 验证** — 全量 ctest；`bash tools/ci.sh`；固件 `idf.py build`。
- [ ] **Step 5: Commit** — `feat(scene): share a VM budget, evict background scenes and restore them at boot`

**执行前须知（Task 6 评审转来）**：
- `launch(id, Boot)` 遇到 `scene.json` 未声明 `background` 的场景时拒绝：返回 `InvalidArgument`，并 `on_notice(RestoreFailed)`（`restore_background()` 随后把它从列表里移除）。加测试。
- 重装一个正在后台的场景：目前 `install_scene` 用 `exit()` 彻底关掉它、装完不重新启动。按 3a spec 6.3（安装覆盖运行中的场景等同重载），装完应在后台重新启动它（例如给 `SceneHost` 加一个钩子，因为 `SceneHost` 没有 `launch`），并更新后台列表。加测试。
- `AppScenes.RestoresAfterBootCompleted` 或恢复失败的测试要断言实际发出的系统通知：代码（`kRestoreFailed`/`kSceneError`）和参数（场景名），覆盖 `scene_wiring.cpp` 中 `SceneNotice` → `post_system` 的映射。

**执行记录**：
- `VmPool` 用原子计数，`Vm` 的分配器在自身上限之外再向池子预留每次增长、释放时归还；池子拒绝的分配同样先让 Lua 做紧急 GC 重试，再失败就是内存错误，文字为 `scene memory budget 3 MB exceeded`（区别于单个 VM 的 `memory limit 2 MB exceeded`）。`SceneRuntime` 自己持有池子（`config.vm.pool` 被忽略），`config.max_background` 超过 `kMaxBackground` 时按后者算；私有的 `kMaxBackground` 去掉。
- 检查点：打开场景（`go_back` 之后、建槽之前，`opening`）、转入后台（`to_background` 末尾，切回前台时把要切回的场景算作前台）、切回前台，以及有前台场景时每条消息和每轮定时器之前；同一轮里逐个场景执行的定时器回调和设备事件，在轮到前台场景之前也各检查一次。没有可淘汰的情况下不分配内存。被淘汰的场景按 `exit` 关闭（`on_close`、刷盘），记 `<id>: evicted (...)`，再 `on_notice`。
- 开机启动遇到后台已满时仍然拒绝（`Busy`），不淘汰别的场景：它从未在前台，按"最久未在前台"它自己就是该淘汰的那个（裁定）。
- 后台列表除了后台场景，还包含还在加载的开机启动（排在最前，按启动顺序），所以恢复过程中不会因中途断电丢项，恢复成功时文件也不重写；一个开机启动失败（找不到、未声明 `background`、后台已满、`main.lua` 出错等）就从列表消失并重写。`restore_background()` 读到文件之前（`persisted_` 为空）一律不写，避免恢复前的变化覆盖上次开机的列表；测试里先调用它。列表在每轮消息末尾与上次写入的内容比较，有变化才交给 worker 写。
- 开机启动在加载完成时才检查 `installing`/`removing`（这样拒绝也能发 `RestoreFailed`）；`main.lua` 或 `on_state` 出错时通知为 `RestoreFailed` 而不是 `Faulted`，状态仍是 `error`。
- 开机启动遇到同一场景正在加载的用户打开时"搭车"（`PendingLoad::then_boot`）；之后的用户打开取代它时，它转成开机启动继续加载（`launch()` 为 `"boot"`，参数清空，原调用者收到 `Busy`）；用户打开取代同一场景的开机启动时继承这个标记和它在列表中的位置。
- 重装：`core::SceneHost` 增加纯虚的 `open_background(id)`（`SceneRuntime` 实现为不发 `RestoreFailed` 的开机启动，并恢复该场景上次因 `exit` 离开后台时的 `launch()` 值和"最久未在前台"的位置）；`install_scene` 对 `Background` 状态的场景在替换后调用它，`Running` 的照旧 `open()`。

---

### Task 8: `wisp.notify`、通知送达场景与提示音

**Files:**
- Create: `components/wisp_scene/src/bind_notify.cpp`、`components/wisp_core/include/wisp/core/tone_player.hpp`、`components/wisp_core/src/tone_player.cpp`、`components/wisp_app/src/notification_router.{hpp,cpp}`
- Modify: `components/wisp_scene/include/wisp/scene/scene_runtime.hpp`、`src/{scene_runtime,scene_instance,scene_api,scene_message}.{hpp,cpp}`（`deliver_notification`、`scene.on_notification`）、`components/wisp_app/src/{app.cpp,app_impl.hpp}`（`TonePlayer`、`NotificationRouter`；触摸活动与任意按键调用 `TonePlayer::silence()`；`notify_vol` 缓存）
- Test: `tests/scene/notify_binding_test.cpp`（新）、`tests/core/tone_player_test.cpp`（新）、`tests/app/notification_router_test.cpp`（新）

**Interfaces:**
- Consumes：Task 5 的 `NotificationService`；Task 6 的 `launch()`、`LaunchReason::Notification`；Task 3 的 `PowerManager::exit_standby()`、`WakeLockRegistry`。
- Produces：

```cpp
// wisp::scene
struct SceneRuntimeDeps { ...; core::NotificationService* notifications = nullptr; };
// Brings `scene_id` forward (launching it with LaunchReason::Notification if it
// is not running), then calls scene.on_notification(note_id). Never waits.
Result<void> SceneRuntime::deliver_notification(const std::string& scene_id, const std::string& note_id);

// wisp::core, tone_player.hpp
enum class ToneKind : uint8_t { Tone, Alarm };
struct TonePlayerConfig { size_t stack_bytes = 4096; int priority = 3; uint32_t sample_rate = 16000; };
class TonePlayer {
public:
    static constexpr uint32_t kToneMs = 150;
    static constexpr uint32_t kAlarmSegmentMs = 1000;
    static constexpr uint32_t kAlarmMaxMs = 30000;
    TonePlayer(std::shared_ptr<hal::AudioOut> out, WakeLockRegistry* locks, TonePlayerConfig config = {});
    static os::TaskConfig task_config(const TonePlayerConfig& c);   // "wisp.tone", flash_access = false
    Result<void> start();
    void stop();
    void play(ToneKind kind, uint8_t volume_percent);   // never waits; replaces what is playing
    void silence();                                     // stops an alarm (or a tone) now
    bool playing() const;
};
// Fills `out` with the frames of `kind` from frame `offset` on (alarm loops); returns frames written.
size_t synth_tone(ToneKind kind, uint32_t sample_rate, uint32_t offset, std::span<int16_t> out);

// wisp::app, notification_router.hpp
struct NotificationRouterDeps {
    core::NotificationService& notes;
    PowerManager* power = nullptr;
    core::TonePlayer* tones = nullptr;
    scene::SceneRuntime* scenes = nullptr;
    core::WakeLockRegistry* locks = nullptr;
    std::function<uint8_t()> notify_volume;
};
class NotificationRouter {
public:
    explicit NotificationRouter(NotificationRouterDeps deps);
    void start();   // set_listener
    void stop();
    // The user tapped it: dismiss(Tap), silence, and for a scene deliver_notification();
    // a scene that cannot be launched only loses the notification.
    void tap(const std::string& source, const std::string& id);
    // The notification's action button (release_lock: WakeLockRegistry::release).
    void act(const std::string& source, const std::string& id);
    // Task 10 installs the banner.
    void set_banner(std::function<void(const core::Notification&)> show);
};
```

- Lua（spec 5.1）：`wisp.notify{title=, body=, sound=, wake=, id=}` → id，或 `nil, "rate_limited"`；字段非法抛错（`luaL_argerror`，信息含字段名）；`source_name` 取 manifest 的 `name`；`from_foreground` 为该场景是否在前台。`wisp.notify_cancel(id)` → 布尔。后台可用。
- 路由（`Posted` 回调，在发起线程上运行，只做投递）：`wake` 且 `power->standby()` → `ui->post(exit_standby)`；`sound != None` → `tones->play(kind, notify_volume())`；`set_banner` 已装 → 显示横幅。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(NotifyBinding, PostsAndReturnsId)          // wisp.notify{title="t", id="pomo"} == "pomo"；service.find("demo","pomo") 存在，source_name "Demo"
TEST(NotifyBinding, RateLimitedReturnsNilAndMessage)
TEST(NotifyBinding, BadArgumentsRaise)          // title=""、sound="beep"、id="A" -> 场景出错，消息含 "title"/"sound"/"id"
TEST(NotifyBinding, CancelById)
TEST(NotifyBinding, WorksInBackground)
TEST(NotifyBinding, ForegroundPostIsBannerOnly) // listed==false
TEST(SceneNotification, TapBringsBackgroundSceneForward)  // on_notification("pomo") 被调用、main.lua 未重跑、Running
TEST(SceneNotification, TapLaunchesStoppedScene)          // launch()=="notification" 后 on_notification
TEST(TonePlayer, SynthLengthsAndFades)          // tone 2400 帧（16 kHz × 150 ms），首尾样本绝对值 < 100；alarm 一段 16000 帧，第 2 段与第 1 段相同
TEST(TonePlayer, AlarmStopsAt30Seconds)         // 假 AudioOut 记帧数：≤ 30 s 的帧数
TEST(TonePlayer, SilenceStopsAlarm)
TEST(TonePlayer, NoSpeakerIsSilent)             // out 为空或 open() 失败：play() 不报错、playing()==false
TEST(TonePlayer, HoldsCpuLockWhilePlaying)      // 播放中 locks.held(Cpu)，结束后释放
TEST(TonePlayer, VolumeScalesAmplitude)         // volume 30 的峰值约为 100 的 0.3（±5%）
TEST(TonePlayer, TaskConfigIsPsramStack)        // "wisp.tone"、flash_access false、4096
TEST(NotificationRouter, WakeTrueWakesFromStandby)
TEST(NotificationRouter, SoundPlaysTone)
TEST(NotificationRouter, TapForMissingSceneJustDismisses)  // 场景已删除：dismissed reason "tap"，不崩溃，runtime 状态不变
TEST(NotificationRouter, ReleaseActionReleasesLock)
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'NotifyBinding|SceneNotification|TonePlayer|NotificationRouter' --output-on-failure`。
- [ ] **Step 3: 实现**；`docs/architecture.md` 任务表加 `wisp.tone`。
- [ ] **Step 4: 验证** — 全量 ctest；`bash tools/ci.sh`；固件 `idf.py build`。
- [ ] **Step 5: Commit** — `feat(scene): let scenes post notifications and route taps back to them`

---

### Task 9: 系统层的状态胶囊、下拉面板、横幅与顶边下滑

**Files:**
- Create: `components/wisp_ui/src/{status_capsule,notification_panel,banner}.cpp`、`components/wisp_ui/include/wisp/ui/system_notice.hpp`、`components/wisp_ui/src/system_notice.cpp`
- Modify: `components/wisp_ui/include/wisp/ui/system_layer.hpp`、`src/system_layer.cpp`（指针观察者在有返回处理或面板处理时都挂上；下滑判定；`raise()` 顺序：胶囊、横幅、面板、错误面板、键盘、确认框）、`components/wisp_ui/include/wisp/ui/strings.hpp`、`src/strings_{zh,ja,en}.cpp`（细化第 14 条）、`components/wisp_ui/CMakeLists.txt`
- Test: `tests/ui/system_layer_test.cpp`（追加）、`tests/ui/system_notice_test.cpp`（新）、`tests/ui/strings_test.cpp`

**Interfaces:**
- Produces（`wisp::ui`，均只在 UI 线程调用，`status_area` 等静态函数除外）：

```cpp
struct StatusCounts { int background = 0; int unread = 0; };
struct PanelScene { std::string id; std::string name; uint64_t since_ms = 0; };
struct PanelNote {
    std::string source, id;
    std::string badge;            // first character of the scene name; "" for system
    std::string title, body;      // already rendered (system_notice_text for system)
    std::string action_label;     // "" or tr(StrId::NoticeRelease)
};
struct PanelHeader { std::string time; int battery = -1; bool charging = false; std::string wifi; };
struct PanelModel { PanelHeader header; std::vector<PanelScene> scenes; std::vector<PanelNote> notes; uint64_t now_ms = 0; };
struct PanelHandlers {
    std::function<void()> on_open;   // the owner refreshes the model
    std::function<void(const std::string& id)> open_scene, exit_scene;
    std::function<void(const std::string& source, const std::string& id)> tap_note, dismiss_note, note_action;
    std::function<void()> clear_all;
};
struct BannerInfo { std::string source, id, badge, name, title, body; };
class SystemLayer {
public:
    static constexpr int32_t kCapsuleW = 72, kCapsuleH = 24, kCapsuleTopGap = 4;
    static constexpr int32_t kTopZoneHeight = 16, kTopTriggerDy = 24;
    static constexpr int32_t kPanelRowMinH = 48, kBannerH = 72;
    static constexpr uint32_t kBannerMs = 4000;
    static hal::Area status_area(const hal::ScreenShape& s);          // refinement 16
    static bool in_top_zone(const hal::ScreenShape& s, int32_t x, int32_t y);
    static bool is_top_swipe(const hal::ScreenShape& s, lv_point_t start, lv_point_t end);
    void set_status(StatusCounts c);                 // hides the capsule when both are 0
    void set_panel_handlers(PanelHandlers h);        // empty: no panel, no top swipe, capsule not tappable
    void open_panel();
    void close_panel();
    bool panel_open() const;
    void update_panel(PanelModel m);                 // also while open
    void show_banner(BannerInfo b, std::function<void()> on_tap);   // queued, one at a time
    lv_obj_t* capsule() const;  lv_obj_t* panel() const;  lv_obj_t* banner() const;   // for tests
};
// system_notice.hpp
struct NoticeText { std::string title; std::string body; };
NoticeText system_notice_text(std::string_view code, std::string_view arg);   // current ui::lang()
std::string holder_label(std::string_view holder, std::string_view scene_name);  // "system.admin" -> tr(HolderAdmin)
```

- 下滑（spec 5.6）：与返回手势同一个观察者：起点 `in_top_zone`，向下移动 ≥ 24 px 时接管（下层控件收到 `PRESS_LOST`，滚动回弹，与返回手势同一套 `cancel_press`/`settle_scroll`），松手时打开面板。键盘、确认框、面板打开时不触发。返回手势和下滑哪个先越过触发距离就算哪个。
- 面板：全屏不透明（`mark_opaque_overlay()`），内容按 `safe_rect` 与各行 `row_span` 缩进，行高 ≥ 48 px；从上到下为页眉（时间、电量、Wi-Fi）、后台场景（徽标、名称、`PanelJustNow`/`PanelMinutes`/`PanelHours`，点击 `open_scene`，× `exit_scene`）、通知（最新在上，点击 `tap_note`，左滑或 × `dismiss_note`，有 `action_label` 时一个按钮调 `note_action`，底部"全部清除"）。上滑、返回手势、点空白处关闭；面板打开时返回手势只关闭面板，不调用场景的返回处理。
- 胶囊（spec 5.5）：`status_area` 处的圆角胶囊，不透明并标记；可点时打开面板。
- 横幅（细化第 17 条）：排队；4 秒或上滑收起；点击收起并调用 `on_tap`。

- [ ] **Step 1: 写失败的测试**（每个形状相关的测试都在 rect 480、rounded_rect 70、circle 480 上跑）

```cpp
TEST(StatusCapsule, AreaPerShape)               // {212,4,267,27} / {212,28,267,51} / {212,78,267,101}
TEST(StatusCapsule, HiddenWhenBothZeroAndShowsCounts)   // {0,0} 隐藏；{2,0} 文本含 "2" 不含铃铛；{0,3}
TEST(StatusCapsule, TapOpensPanel)              // FakeTouch 点胶囊中心 -> panel_open()，on_open 调用一次
TEST(TopSwipe, ZoneFollowsRowSpan)              // circle：y=10 时 x=240 在区内、x=100 不在；y=safe.y1+16 不在
TEST(TopSwipe, TapInZoneReachesWidget)          // 区内的按钮点击照常 CLICKED
TEST(TopSwipe, DownSwipeTakesPressAndOpens)     // (240,10)->(240,40)：按钮收到 PRESS_LOST、无 CLICKED、面板打开
TEST(TopSwipe, IgnoredWhileKeyboardConfirmOrPanelOpen)
TEST(Panel, LayoutInsideVisibleArea)            // 面板里每个可点对象四角 hal::visible()，行高 >= 48
TEST(Panel, BackGestureOnlyClosesPanel)         // 返回处理计数为 0，panel_open()==false
TEST(Panel, RowsCallHandlers)                   // 点场景行 open_scene("a")、× exit_scene("a")、通知 tap/×/动作/全部清除各一次
TEST(Panel, UpdatesWhileOpen)                   // update_panel 第二次后行数变化
TEST(Banner, QueuesAndAutoHides)                // 两条：第一条可见；lv_tick 前进 4000 ms 后第二条可见；再 4000 ms 后无横幅
TEST(Banner, WidthFollowsRowSpan)               // circle 上横幅四角 visible()
TEST(Banner, TapCallsBackAndSwipeUpOnlyHides)
TEST(Banner, StacksBelowConfirm)                // open_confirm 后 raise()：确认框背板在横幅之上
TEST(SystemLayer, OverlaysKeepOcclusionSkip)    // 追加：胶囊、横幅、面板分别显示时，下层列表滚动的刷新区不进入它们去掉圆角行后的区域
TEST(SystemNotice, EveryCodeInEveryLanguage)    // 六个 code × zh/ja/en：title 非空、body 含 arg；未知 code 返回 code 本身
TEST(SystemNotice, HolderLabels)                // "system.admin" zh "管理后台"；"scene.x" -> 场景名
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'StatusCapsule|TopSwipe|Panel|Banner|SystemLayer|SystemNotice|Strings' --output-on-failure`。
- [ ] **Step 3: 实现**；`system_layer.hpp` 文件头补充胶囊、面板、横幅与 overlay 的配合。
- [ ] **Step 4: 验证** — 全量 ctest（含 `gen_cjk_font_test`、`net_ui_test`、`overlay_test`）；`bash tools/ci.sh`；固件 `idf.py build`。
- [ ] **Step 5: Commit** — `feat(ui): add the status capsule, the pull-down panel and notification banners`

---

### Task 10: `StatusShell` 接线与 `status_area`

**Files:**
- Create: `components/wisp_app/src/status_shell.{hpp,cpp}`
- Modify: `components/wisp_app/src/{app.cpp,app_impl.hpp,scene_wiring.cpp}`、`components/wisp_scene/src/{bind_device.cpp,scene_api.hpp,scene_runtime.cpp}`（`wisp.device.info().status_area`）
- Test: `tests/app/status_shell_test.cpp`（新）、`tests/scene/scene_api_test.cpp`（追加）

**Interfaces:**
- Consumes：Task 9 的 `SystemLayer` 全部新接口、`system_notice_text`、`holder_label`；Task 8 的 `NotificationRouter::tap/act/set_banner`；Task 6 的 `background_scenes()`、`launch()`、`exit()`；Task 5 的 `NotificationService::unread/unread_count/dismiss/clear_all`。
- Produces：

```cpp
struct StatusShellDeps {
    ui::UiRuntime& ui;
    ui::SystemLayer& layer;
    core::EventBus& bus;
    core::NotificationService& notes;
    NotificationRouter& router;
    scene::SceneRuntime* scenes = nullptr;
    std::function<Value()> device_status;   // same as SceneRuntimeDeps::device_status
    std::function<int()> utc_offset_min;
};
class StatusShell {
public:
    explicit StatusShell(StatusShellDeps deps);
    void start();   // subscribes scene.state, notification.*, power.state; installs panel handlers and the banner
    void stop();    // before the system layer goes
};
// DeviceInfo (scene_api.hpp) gets `hal::Area status_area;` = SystemLayer::status_area(shape)
```

- 计数：`scene.state` 与 `notification.*` 到来时在 UI 线程上 `set_status({background_scenes().size(), unread_count()})`，变化时记 `status: background=<n> unread=<m>`（细化第 26 条）。
- 面板模型：页眉时间按 `utc_offset_min` 显示 `HH:MM`（时钟未设置时 `--:--`），电量与 Wi-Fi 取 `device_status`；场景行取 `background_scenes()`；通知行取 `unread()`，系统通知经 `system_notice_text`、场景锁提醒的持有者经 `holder_label`。面板打开期间每次变化重建模型。处理函数：`open_scene` → `launch(id, User)` 并关闭面板；`exit_scene` → `exit(id)`；`tap_note` → `router.tap` 并关闭面板；`dismiss_note` → `notes.dismiss(..., Dismiss)`；`note_action` → `router.act`；`clear_all` → `notes.clear_all()`。
- 横幅：`router.set_banner()` 把 `Notification` 转成 `BannerInfo` 投递到 UI 线程；点击 → `router.tap`。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(StatusShell, CapsuleCountsFollowScenesAndNotifications)   // 场景转后台 -> {1,0}；post 两条 -> {1,2}；dismiss -> {1,1}
TEST(StatusShell, PanelListsBackgroundScenesAndNotes)          // 打开面板：一行场景（名称）、两行通知、最新在上
TEST(StatusShell, PanelActions)                                // 点场景行 -> 前台；× -> Stopped；点通知 -> 场景 on_notification；全部清除 -> 0
TEST(StatusShell, PanelUpdatesWhileOpen)                       // 面板开着时 post -> 行数 +1
TEST(StatusShell, SystemNoticeRenderedInUiLanguage)            // post_system(kSceneEvictedMemory,"天气")，lang zh -> "天气已因内存不足关闭"
TEST(StatusShell, BannerOnPostAndTapOpensScene)
TEST(SceneApi, DeviceInfoHasStatusArea)                        // circle 480: {212,78,267,101}，胶囊隐藏时照样返回
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'StatusShell|SceneApi' --output-on-failure`。
- [ ] **Step 3: 实现**；`App::start()` 在 `SystemLayer` 与场景运行时之后构造并启动 `StatusShell`，`App::stop()` 在 `SystemLayer` 销毁之前停止它。
- [ ] **Step 4: 验证** — 全量 ctest；`bash tools/ci.sh`；模拟器手动：打开番茄钟（Task 15 前用测试包）→ 返回 → 胶囊出现 → 下滑打开面板 → 点回前台。
- [ ] **Step 5: Commit** — `feat(app): show background scenes and notifications in the capsule and the panel`

---

### Task 11: `SensorService`、外设占用登记与 `wisp.sensor`

**Files:**
- Create: `components/wisp_core/include/wisp/core/{occupancy,sensor_service,gesture}.hpp`、`components/wisp_core/src/{occupancy,sensor_service,gesture}.cpp`、`components/wisp_scene/src/bind_sensor.cpp`、`components/wisp_port_sim/include/wisp/port/sim/imu_script.hpp`、`components/wisp_port_sim/imu_script.cpp`
- Modify: `components/wisp_scene/src/{scene_runtime,scene_api,scene_message}.{hpp,cpp}`（`msg::Sensor`；后台暂停、前台恢复、关闭释放）、`components/wisp_app/src/{app.cpp,app_impl.hpp,self_check_sampler.{hpp,cpp}}`（细化第 19 条）、`components/wisp_port_sim/dev/sim_imu.cpp`、`components/wisp_port_sim/include/wisp/port/sim/sim_input.hpp`、`simulator/{sim_options.hpp,sim_options.cpp,main.cpp}`（`--imu-script <文件>`）、`docs/architecture.md`（任务表 `wisp.imu`）
- Test: `tests/core/{occupancy_test,sensor_service_test,gesture_test}.cpp`（新）、`tests/scene/sensor_binding_test.cpp`（新）、`tests/sim/{imu_script_test,sim_options_test}.cpp`、`tests/app/self_check_screen_test.cpp`

**Interfaces:**
- Consumes：Task 2 的 `set_power(PowerLevel)`；Task 6 的后台状态。
- Produces（`wisp::core`）：

```cpp
// occupancy.hpp
enum class OccupancyMode : uint8_t { Shared, Exclusive };
const char* to_string(OccupancyMode m);   // "shared" "exclusive"
struct OccupancyEntry { std::string device, holder; OccupancyMode mode; uint64_t since_ms = 0; };
class OccupancyRegistry {
public:
    explicit OccupancyRegistry(std::function<uint64_t()> now_ms = os::now_ms);
    // Busy when it conflicts: an exclusive holder, or exclusive over any holder.
    // The same holder acquiring again is Ok (no second entry).
    Result<void> acquire(std::string device, std::string holder, OccupancyMode mode);
    void release(std::string_view device, std::string_view holder);
    std::vector<OccupancyEntry> list() const;
};
// gesture.hpp (m/s², ms)
class ShakeDetector {
public:
    static constexpr float kThreshold = 20.0f;
    static constexpr uint64_t kWindowMs = 500, kRefractoryMs = 1000;
    bool feed(float x, float y, float z, uint64_t t_ms);   // true once per shake
};
class FlipDetector {
public:
    static constexpr float kFaceThreshold = 7.0f;
    static constexpr uint64_t kStableMs = 300;
    bool feed(float z, uint64_t t_ms);                       // true once per flip
};
// sensor_service.hpp
enum class SensorKind : uint8_t { Accel, Gyro };
std::optional<SensorKind> parse_sensor_kind(std::string_view s);   // "accel" "gyro"
enum class Gesture : uint8_t { Shake, Flip };
std::optional<Gesture> parse_gesture(std::string_view s);          // "shake" "flip"
struct SensorSample { float x = 0, y = 0, z = 0; uint64_t t_ms = 0; };
using SensorBatch = os::LargeVector<SensorSample>;
struct SensorServiceConfig { uint32_t batch_ms = 100; size_t stack_bytes = 4096; int priority = 3; };
class SensorService {
public:
    static constexpr std::array<uint32_t, 4> kRates = {10, 25, 50, 100};
    SensorService(std::shared_ptr<hal::Imu> imu, OccupancyRegistry& occupancy, SensorServiceConfig c = {});
    static os::TaskConfig task_config(const SensorServiceConfig& c);   // "wisp.imu", flash_access = false
    Result<void> start();
    void stop();
    bool available(SensorKind kind) const;   // from the IMU's caps
    // Unavailable "unavailable"; InvalidArgument for a rate not in kRates.
    // `cb` runs on wisp.imu with up to batch_ms of samples; it must be quick.
    Result<uint32_t> subscribe(SensorKind kind, uint32_t rate_hz, std::string holder,
                               std::function<void(const SensorBatch&)> cb);
    Result<uint32_t> on_gesture(Gesture g, std::string holder, std::function<void()> cb);
    void unsubscribe(uint32_t handle);
    void set_paused(uint32_t handle, bool paused);
    void set_standby(bool standby);          // power.state: standby/deep_standby pause everything
    uint32_t imu_rate() const;               // 0 while the IMU is off
};
// wisp::scene: SceneRuntimeDeps { ...; core::SensorService* sensors = nullptr; }
// wisp::port::sim, imu_script.hpp
struct ImuScriptStep { uint64_t at_ms; enum class Kind { Tilt, Shake, Flip } kind; float x = 0, y = 0, z = 0; };
Result<std::vector<ImuScriptStep>> parse_imu_script(std::string_view text);
```

- 运行：第一个订阅（含手势，手势内部按 50 Hz 订阅 accel）`imu->set_power(Active)` 并 `set_rate(最高采样率)`；采样率变化时重设；最后一个取消时 `set_power(Standby)`。`wisp.imu` 任务按 IMU 采样率轮询 `read()`，g 换算成 m/s²（× 9.80665），按每个订阅者自己的采样率降采样（保留时间上最接近该订阅者周期的那个样本），每 `batch_ms` 把各自的样本投给回调。每个订阅者的持有者以共享方式登记 `imu.main`。
- 绑定（spec 6.2）：`wisp.sensor.subscribe(kind, {rate=}, fn)` → 句柄（`h:stop()`），`fn(samples)` 收到 `{ {x=,y=,z=,t=}, ... }`；`wisp.sensor.on("shake"|"flip", fn)` → 句柄；`wisp.sensor.available()` → `{accel=true, gyro=true}` 或 `{}`。没有 IMU 或服务：`subscribe`/`on` 返回 `nil, "unavailable"`；后台调用返回 `nil, "not_in_background"`；未知 kind、rate 抛参数错误。回调经 `msg::Sensor{handle, SensorBatch}` 回到场景任务。转入后台 `set_paused(true)`，切回前台 `false`，关闭、出错、淘汰时 `unsubscribe`。
- 假 IMU 脚本（`--imu-script`）：每行 `<ms> tilt <x> <y> <z>`（g）、`<ms> shake`（300 ms 内 3 个 3 g 的峰）、`<ms> flip`（az 从 +1 g 变 −1 g 并保持），`#` 开头为注释；时间从模拟器启动算起。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(Occupancy, SharedAndExclusive)          // 两个 shared 可共存；exclusive 遇 shared -> Busy；release 后可以
TEST(ShakeDetector, ThreePeaksWithin500ms)   // 3 个上升沿在 400 ms 内 -> true 一次
TEST(ShakeDetector, SlowPeaksIgnored)        // 间隔 300 ms 的 3 个 -> false
TEST(ShakeDetector, RefractoryOneSecond)
TEST(FlipDetector, FlipAfterStable300ms)     // +9.8 -> -9.8 保持 299 ms false，300 ms true
TEST(FlipDetector, BriefFlipIgnored)
TEST(SensorService, RefcountPowersImu)       // 订阅 -> set_power(Active)；取消最后一个 -> set_power(Standby)
TEST(SensorService, RunsAtHighestRateAndDownsamples)   // 10 与 50 Hz -> imu_rate 50；1 s 内 10 Hz 订阅者约 10 个样本（±1），50 Hz 约 50 个
TEST(SensorService, BatchesEvery100ms)       // 50 Hz 时每批 5±1 个
TEST(SensorService, ConvertsUnits)           // 假 IMU az=1 g -> z == 9.80665f；gyro 原样 °/s
TEST(SensorService, StandbyPausesAll)        // set_standby(true) 后没有回调、IMU 关；false 后恢复
TEST(SensorService, PausedHandleGetsNothing)
TEST(SensorService, NoImuIsUnavailable)
TEST(SensorService, RegistersOccupancyPerHolder)   // 两个持有者 -> list() 两项 shared；取消一个 -> 一项
TEST(SensorService, TaskConfigIsPsramStack)        // "wisp.imu"、flash_access false
TEST(SensorBinding, SubscribeDeliversSamples)      // 场景里累计样本数 > 0，字段 x y z t 齐全
TEST(SensorBinding, GestureFiresHandler)
TEST(SensorBinding, BackgroundPausesForegroundResumes)
TEST(SensorBinding, NotInBackground)               // 后台定时器里 subscribe -> nil,"not_in_background"
TEST(SensorBinding, AvailableEmptyWithoutImu)
TEST(SensorBinding, ExitReleases)                  // exit 后 occupancy 为空、IMU 关
TEST(ImuScript, ParsesStepsAndComments)            // 错误行返回 InvalidArgument，信息含行号
TEST(SimOptions, ImuScript)
TEST(SelfCheckScreen, ImuReadsThroughSensorService)   // occupancy 含 system.selfcheck
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'Occupancy|ShakeDetector|FlipDetector|SensorService|SensorBinding|ImuScript|SimOptions|SelfCheckScreen' --output-on-failure`。
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 验证** — 全量 ctest；`bash tools/ci.sh`；固件 `idf.py build`。
- [ ] **Step 5: Commit** — `feat(core): share the IMU between subscribers and expose wisp.sensor`

---

### Task 12: `SceneFiles`、`wisp.files` 与 `wisp.http.download`

**Files:**
- Create: `components/wisp_core/include/wisp/core/scene_files.hpp`、`components/wisp_core/src/scene_files.cpp`、`components/wisp_scene/src/bind_files.cpp`
- Modify: `components/wisp_core/include/wisp/core/storage_manager.hpp`、`src/storage_manager.cpp`（`DirEntry::mtime`、`read_range`）、`components/wisp_core/include/wisp/core/scene_repository.hpp`、`src/scene_repository.cpp`（细化第 20 条：替换时搬 `files/`、`load()`/导出/`code_bytes` 不含 `files/`、`remove(with_data)` 删两处 `files/`）、`components/wisp_net/include/wisp/net/scene_http.hpp`、`src/scene_http.cpp`（下载请求）、`components/wisp_scene/src/{bind_http,scene_runtime,scene_message,scene_api}.{hpp,cpp}`、`components/wisp_app/src/{scene_wiring.cpp,app.cpp}`（构造 `SceneFiles`，启动后在 `wisp.fs` 上 `scan()`；订阅 `storage.sd.inserted`/`removed` 调 `on_sd_changed()`）
- Test: `tests/core/scene_files_test.cpp`（新）、`tests/core/{storage_manager_test,scene_repository_test}.cpp`、`tests/net/scene_http_test.cpp`、`tests/scene/files_binding_test.cpp`（新）

**Interfaces:**
- Consumes：Task 4 的 `SceneHttp` 延迟队列（下载同样排队）；Task 6 的多场景消息分发。
- Produces（`wisp::core`）：

```cpp
// storage_manager.hpp
struct DirEntry { std::string name; EntryKind kind; uint64_t size; int64_t mtime = 0; };
// At most `len` bytes from `offset` (fewer at the end of the file); NotFound if missing.
Result<os::LargeBuffer> read_range(const std::string& path, uint64_t offset, size_t len);
// scene_files.hpp
enum class FilesLocation : uint8_t { None, Sd, Internal };
const char* to_string(FilesLocation l);   // "none" "sd" "int"
struct SceneFileEntry { std::string name; uint64_t size = 0; int64_t mtime = 0; };
struct SceneFilesInfo { FilesLocation location = FilesLocation::None; uint64_t quota = 0, used = 0; };
struct SceneFilesConfig {
    std::string data_root;                  // SceneRepoConfig::data_root
    uint64_t sd_quota = 64ull * 1024 * 1024;
    uint64_t int_quota = 512 * 1024;
    size_t max_write = 256 * 1024;
    size_t max_read = 64 * 1024;
};
bool valid_file_name(std::string_view name);   // [A-Za-z0-9._-]{1,64}, not starting with '.'
class FileDownload;   // see below
// Every method touches flash: call it on wisp.fs or an httpd thread with flash
// access. Thread-safe (one mutex for the usage table and the location records).
// Errors: InvalidArgument "bad_name", QuotaExceeded "quota_exceeded",
// StorageUnavailable "unavailable", InvalidArgument "too_large", NotFound "not_found".
class SceneFiles {
public:
    SceneFiles(StorageManager& storage, SceneFilesConfig config);
    Result<void> scan();                                   // usage of every scene; .dl dropped, .done promoted
    void on_sd_changed();                                  // card inserted/removed: rescan the SD scenes
    Result<void> write(std::string_view scene, std::string_view name, std::span<const uint8_t> data);
    Result<void> append(std::string_view scene, std::string_view name, std::span<const uint8_t> data);
    Result<os::LargeBuffer> read(std::string_view scene, std::string_view name, uint64_t offset, size_t len);
    Result<std::vector<SceneFileEntry>> list(std::string_view scene);   // by name; .dl/.done hidden
    Result<void> remove(std::string_view scene, std::string_view name);
    Result<SceneFilesInfo> info(std::string_view scene);
    Result<void> remove_all(std::string_view scene);       // both locations and the record
    Result<std::shared_ptr<FileDownload>> begin_download(std::string_view scene, std::string_view name);
};
class FileDownload {   // created on wisp.fs; fed from wisp.shttp
public:
    // Copies `chunk` into PSRAM and queues its append on `submit_io`; waits while
    // more than 128 KiB are queued. False once the quota would be exceeded or a
    // write failed (the transfer is then cancelled).
    bool push(std::span<const uint8_t> chunk);
    // On wisp.fs: success renames .dl -> .done -> name; otherwise removes .dl.
    Result<uint64_t> finish(bool ok);
    std::string error() const;   // "quota_exceeded", "unavailable", ...
};
// wisp::net: SceneHttpRequest gets
//   std::function<bool(std::span<const uint8_t>)> on_body;  // set: streamed, no max_response
//   uint64_t max_body = 0;  bool download = false;
//   static constexpr uint32_t kDownloadTimeoutMs = 300000;
// wisp::scene: SceneRuntimeDeps { ...; core::SceneFiles* files = nullptr; }
```

- Lua（spec 7.1）：七个调用全部异步，作业在 `wisp.fs` 上执行，结果经 `msg::FilesDone{cb_id, error, payload}` 回到场景任务，`cb(err, result)` 有自己的 200 ms。`write`/`append` 的 `data` 是字符串；`read` → 字符串；`list` → `{ {name=, size=, mtime=}, ... }`；`info` → `{location="sd"|"int", quota=, used=}`（还没写过时 `location` 为 `nil`）；`remove` → `true`。名字非法时不排作业，直接在下一条消息里回调 `"bad_name"`。场景关闭时它排队的回调丢弃（实例不匹配），已排的写作业照常完成。
- `wisp.http.download(url, name, opts, cb)`：名字检查 → `wisp.fs` 上 `begin_download` → 提交 `download = true` 的请求（`on_body = FileDownload::push`，超时 `kDownloadTimeoutMs`，深度待机中照样延迟排队）→ 完成后 `wisp.fs` 上 `finish(ok)` → `cb(err, {status=, size=, deferred=})`；只有 2xx 写入，非 2xx 时 `err = nil`、`size = 0`、文件不动；传输错误时 `err` 为 3a 的传输错误码，配额超出为 `"quota_exceeded"`。场景关闭时 `cancel_scene` 中止传输，`finish(false)` 删除 `.dl`。
- 位置（spec 7.2）：第一次写入时按 manifest 的 `files`（`auto`/`sd`/`int`，细化第 20 条）选定，记在 `data/files_loc.json`；之后不变。位置在 SD 而卡不在 → `unavailable`。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(StorageManager, ReadRangeAndMtime)
TEST(SceneFiles, NameRules)                    // ok: "a.txt" "A-b_9.bin"；bad: "" ".x" "a/b" "é" 65 字节
TEST(SceneFiles, LocationChosenOnFirstWriteAndRemembered)   // SD 在线 -> sd；之后 SD 离线 -> 写入 "unavailable"；无 SD 的场景 -> int
TEST(SceneFiles, InternalQuota512KiB)          // 512 KiB 写满后再 append 1 字节 -> "quota_exceeded"，文件不变
TEST(SceneFiles, SdQuotaFromConfig)
TEST(SceneFiles, WriteIsAtomicAppendExtends)
TEST(SceneFiles, ReadRangeAndLimits)           // read(0, 65537) -> "too_large"；write 262145 字节 -> "too_large"；越过文件尾返回剩余部分
TEST(SceneFiles, ListRemoveInfo)
TEST(SceneFiles, UsageScannedAtBootAndRecoversDownloads)   // 预置 ".x.dl" 被删、".y.done"（无 y）改名为 "y"、used 正确
TEST(SceneFiles, RemoveAllChecksBothLocations)
TEST(SceneFiles, SdRemovedMidDownload)         // push 到一半拔卡：push 返回 false、finish(false) 不留文件、之后 "unavailable"；
                                               // 插卡后 on_sd_changed() -> 再次可写，used 与磁盘一致
TEST(SceneRepository, ReplaceKeepsFilesDir)
TEST(SceneRepository, LoadAndExportSkipFiles)  // code_bytes 不含 files/
TEST(SceneRepository, RemoveWithDataRemovesFiles)   // data=0 保留，data=1 两处都删
TEST(SceneHttp, DownloadStreamsWithoutResponseCap)  // 1 MiB 响应体经 on_body 全部到达
TEST(FilesBinding, CallbacksCarryResults)      // write -> list 含 {name,size,mtime} -> read 内容一致 -> info.location "int"
TEST(FilesBinding, BadNameIsReportedAsync)     // 回调 err == "bad_name"
TEST(FilesBinding, DownloadWritesFileWithinQuota)   // 假客户端 300 KiB -> size 300*1024，文件存在
TEST(FilesBinding, DownloadQuotaExceededLeavesNoFile)
TEST(FilesBinding, DownloadNon2xxWritesNothing)
TEST(FilesBinding, DownloadDeferredInDeepStandby)   // set_deferring(true) 时回调稍后到达且 deferred == true
TEST(FilesBinding, ExitDuringDownloadRemovesPartial)
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'StorageManager|SceneFiles|SceneRepository|SceneHttp|FilesBinding' --output-on-failure`。
- [ ] **Step 3: 实现**；`scene_repository.hpp` 文件头写明 `files/` 与 `data/` 同等对待。
- [ ] **Step 4: 验证** — 全量 ctest；`bash tools/ci.sh`；固件 `idf.py build`。
- [ ] **Step 5: Commit** — `feat(scene): give each scene a private files directory and streamed downloads`

---

### Task 13: 数据迁移 `migrate`

**Files:**
- Modify: `components/wisp_core/src/scene_manifest.cpp`（`schema_version` 1–1000）、`components/wisp_core/include/wisp/core/scene_store.hpp`、`src/scene_store.cpp`（数据版本、`commit_migration`）、`components/wisp_scene/include/wisp/scene/vm.hpp`、`src/vm.cpp`（`call` 的 `exec_ms`）、`components/wisp_scene/src/{scene_instance,scene_runtime}.{hpp,cpp}`
- Create: `tests/scene/fixtures/migrate_v1/{scene.json,main.lua}`、`tests/scene/fixtures/migrate_v2/{scene.json,main.lua}`
- Test: `tests/core/{scene_manifest_test,scene_store_test}.cpp`、`tests/scene/migrate_test.cpp`（新）

**Interfaces:**
- Consumes：Task 7 的开机恢复路径（迁移同样在恢复时执行）。
- Produces：

```cpp
// SceneStore
int data_version() const;                      // after load(): meta.json, else 1 with data, else 0 (new)
void set_initial_version(int v);               // a new store (data_version()==0): records v
// Commits the open batch together with the new version through journal.json
// (refinement 21); InvalidArgument if no batch is open.
Result<void> commit_migration(int new_version);
// Vm
std::optional<ScriptError> call(int nargs, int nresults, Budget budget = Budget::Fresh, uint32_t exec_ms = 0);  // 0: limits().exec_ms
// SceneInstance
std::optional<ScriptError> start(std::optional<int> migrate_from = std::nullopt);
inline constexpr uint32_t kMigrateMs = 2000;
```

- 打开流程（spec 7.3）：`load()` 后比较 `data_version()` 与 manifest：
  - `0` → `set_initial_version(manifest)`；
  - 小于 manifest → `start(old)`：执行 `main.lua` → `begin_batch()` → 有 `scene.migrate` 就以 2 秒调用 → 成功 `commit_migration(new)`，失败或超时 `abort_batch()`、状态 `Error`、错误面板 `migrate failed: <msg>`；没有 `migrate` 直接 `commit_migration(new)`；再建树；
  - 大于 manifest → 不建 VM，状态 `Error`，错误面板 `data version <n> newer than scene <m>`。
- 测试场景：`migrate_v1` 的 `main.lua` 往集合 `items` 写两条 `{name=}`；`migrate_v2` 的 `scene.json` `schema_version: 2`，`scene.migrate = function(old) ... end` 给每条加 `done = false`，用 `args` 控制成功、`error("boom")` 或死循环。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(SceneManifest, SchemaVersionRange)        // 0、1001、1.5、"2" -> InvalidArgument；1、1000 ok
TEST(SceneStore, DataVersionDefaults)          // 全新目录 0；只有 kv.log 1；meta.json 优先
TEST(SceneStore, MigrationCommitIsAtomicWithVersion)   // 测试钩子让 journal 写完后"掉电"：重新 load 后数据与版本都是新的；
                                                       // journal 写之前"掉电"：都是旧的
TEST(Migrate, SuccessCommitsAndBumps)          // v1 数据 -> 安装 v2 打开：两条都有 done=false，data_version()==2
TEST(Migrate, ErrorRollsBack)                  // error("boom")：数据不变、版本 1、Error、面板含 "migrate failed: " 与 "boom"
TEST(Migrate, TimeoutAfterTwoSeconds)          // 死循环：约 2 s 后 Error，数据不变
TEST(Migrate, NoHookJustBumps)
TEST(Migrate, DowngradeRefused)                // 数据 3、manifest 2 -> "data version 3 newer than scene 2"
TEST(Migrate, NewSceneStartsAtManifestVersion) // 从未打开过的 v2 场景：不调用 migrate，data_version()==2
TEST(Migrate, RunsOnBootRestore)               // background.json 含该场景：恢复时迁移，成功后 Background
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'SceneManifest|SceneStore|Migrate' --output-on-failure`。
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 验证** — 全量 ctest；`bash tools/ci.sh`。
- [ ] **Step 5: Commit** — `feat(scene): migrate scene data when its schema version goes up`

---

### Task 14: 管理后台的 REST 与网页

**Files:**
- Create: `components/wisp_admin/src/{routes_power,routes_scene_files}.cpp`
- Modify: `components/wisp_admin/include/wisp/admin/admin_server.hpp`（`AdminDeps`：`core::WakeLockRegistry* wakelocks`、`core::OccupancyRegistry* occupancy`、`core::SceneFiles* files`、`std::function<Value()> power_status`）、`components/wisp_admin/src/{admin_server.cpp,admin_internal.hpp,routes_scenes.cpp}`、`components/wisp_net/include/wisp/net/http.hpp`（`HttpResponse::stream`）、`components/wisp_net/src/http.cpp`、`components/wisp_port_esp/net/esp_http_server.cpp`、`components/wisp_port_sim/net/sim_http_server.cpp`、`components/wisp_app/src/{app.cpp,net_wiring.cpp}`（填 `AdminDeps`；`power_status` 由 `PowerManager`、`WakeLockRegistry`、`SceneHttp::deferred_count` 组成）
- Modify: `web/app/pages/{scenes,system}.js`、`web/app/i18n/{zh,ja,en}.js`、`web/app/api.js`（下载链接）
- Test: `tests/admin/{routes_power_test,routes_scene_files_test}.cpp`（新）、`tests/admin/routes_scenes_test.cpp`、`tests/sim/sim_http_server_test.cpp`、`tests/smoke/admin_smoke.py`

**Interfaces:**
- Consumes：Task 3、4、6、11、12 的服务。
- Produces：

```cpp
// wisp::net::HttpResponse
// Set: the body is pulled from it in pieces until it returns 0 (or an error,
// which ends the connection); sent as Content-Length when stream_length > 0,
// else chunked. Runs on the server's thread.
std::function<Result<size_t>(std::span<uint8_t> buf)> stream;
uint64_t stream_length = 0;
```

- 路由（鉴权、CSRF、错误格式沿用连接 spec 6.3）：

| 方法与路径 | 说明 |
|---|---|
| `GET /system/power` | 细化第 25 条；`wakelocks` 为空时 503 `unavailable` |
| `GET /system/occupancy` | 细化第 25 条 |
| `POST /scenes/{id}/exit` | Task 6 已加，本任务只补网页 |
| `GET /scenes/{id}/files` | `{"location": "sd"\|"int"\|null, "quota": int, "used": int, "files": [{"name","size","mtime"}]}`；无 `files` 服务 503 |
| `GET /scenes/{id}/files/{name}` | `application/octet-stream`，`Content-Disposition: attachment; filename="<name>"`，经 `stream` 每次 `read_range` 32 KiB |
| `DELETE /scenes/{id}/files/{name}` | 204；需要 `X-Wisp` |

  `bad_name` → 400 `bad_name`，`not_found` → 404，`unavailable` → 503。
- 网页：场景页的状态徽标多 `background`（"后台"/"バックグラウンド"/"Background"），"关闭"按钮改调 `/exit`，展开的场景详情加"文件"列表（下载、删除、位置与用量）；系统页加"电源"卡片（当前级别、浅睡、排队的请求数、唤醒锁表：持有者、类型、原因、持续时长）和"外设占用"表，设置表单加 `deep_after_s`、`light_sleep`、`notify_vol`（细化第 7 条）。文案三种语言，`tools/check_i18n.py` 通过。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(AdminPower, ReportsLevelLightSleepAndLocks)   // 两把锁 -> locks 两项，held_ms 随假时钟增长；light_sleep "unsupported"（无钩子）
TEST(AdminPower, OccupancyList)
TEST(AdminPower, RequiresToken)                    // 无 cookie 401
TEST(AdminSceneFiles, ListDownloadDelete)          // 列表；GET 内容一致且 Content-Disposition 正确；DELETE 204
TEST(AdminSceneFiles, Errors)                      // 名字 ".x" 400 bad_name；不存在 404；DELETE 无 X-Wisp 403；files 为空 503
TEST(AdminScenes, ListShowsBackground)             // 后台场景 state "background"
TEST(SimHttpServer, StreamedResponseBody)          // 3 MiB 的 stream 响应完整到达，Content-Length 正确；stream_length 0 时分块
```

`admin_smoke.py` 追加：`GET /api/v1/system/power` 200 且 `level` 为 `active`；上传 `counter` 后 `GET /api/v1/scenes/counter/files` 200、`files` 为空。

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'AdminPower|AdminSceneFiles|AdminScenes|SimHttpServer|admin_smoke' --output-on-failure`。
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 验证** — 全量 ctest；`bash tools/ci.sh`（含 `check_i18n.py`、网页资源包）；固件 `idf.py build`；模拟器打开网页手动看场景页与系统页（中/日/英各看一次）。
- [ ] **Step 5: Commit** — `feat(admin): show power, wake locks, peripheral use and scene files`

**执行前须知（Task 6 评审转来）**：Task 6 只加了 `POST /scenes/{id}/exit` 路由，网页上还没有入口；本任务把场景页的"关闭"按钮改调 `/exit`（见上文网页一段），并加 `background` 徽标。

---

### Task 15: 示例场景、冒烟测试与文档

**Files:**
- Modify: `scenes/examples/pomodoro/{scene.json,main.lua}`（`"background": true`；到点 `wisp.notify{title="番茄钟", body="25 分钟到了", sound="alarm", wake=true, id="pomo"}`；`scene.on_notification` 回到计时页）、`scenes/examples/weather/{scene.json,main.lua}`（`"background": true`；`wisp.timer.every(30 * 60 * 1000, refresh)`；显示 `res.deferred` 时的"上次更新"时间）
- Create: `scenes/examples/level/{scene.json,main.lua}`（订阅 `accel` 25 Hz，`canvas` 画气泡，`font = "xxl"` 显示倾角度数，按 `safe_area` 与 `row_span` 布局；`schema_version` 1）
- Modify: `tests/scene/examples_test.cpp`（`level` 用假 IMU；`pomodoro`/`weather` 的 `background` 字段；三种形状上的四角可见检查覆盖 `level`）、`tests/smoke/sim_smoke.py`、`simulator/main.cpp`（调试命令，见下）
- Modify: `docs/architecture.md`（`git merge feat/scene-runtime-core` 之后再改：事件表 `power.state`、`power.wakelock`、`notification.*`、`scene.state`；任务表 `wisp.imu`、`wisp.tone`；"场景运行时"一节加后台、预算与恢复；"真机的网络"一节确认联网窗口已写）、`docs/porting-guide.md`（caps `standby`、`hal::Wifi::set_radio`、`Platform::set_light_sleep` 与浅睡验证方法引用硬件文档）、`components/wisp_scene/src/schema.cpp` 与生成的 `docs/scene-api.md`（`wisp.notify`、`wisp.notify_cancel`、`wisp.power`、`wisp.sensor`、`wisp.files`、`wisp.http.download`、`wisp.scene.state/launch/exit`、`scene.on_state/on_notification/migrate`、`status_area`、`background` 与 `schema_version`）、`docs/superpowers/specs/2026-11-04-scene-runtime-core-design.md`（3.4/3.5/6.2 处各加一行"3b-1 修订见 2026-11-05-scene-device-design.md"）
- Test: 同上

**Interfaces:**
- Consumes：前面所有任务。
- 模拟器调试命令（只在 `WISP_SIM_DEBUG_COMMANDS` 下编译，与 `debug scene-click` 同）：`debug panel-tap <index>`（点面板第 `index` 个可点行，从 0 开始）输出 `panel-tap=ok|<错误>`。
- 冒烟用例（spec 9.2）：
  - `scene_background`：`--scenes-dir`（复制的示例）+ `--open-scene pomodoro`；`--swipe 4,240,240,240@<ms>` 返回 → 日志 `scene.state` 的 `pomodoro: background` 与 `status: background=1 unread=0`；`--swipe 240,30,240,120@<ms>` 下滑 → `panel: open`；`debug panel-tap 0` → `pomodoro: running`、`panel: closed`。
  - `scene_banner`：测试包 `tests/scene/fixtures/notify_demo`（打开 1 秒后 `wisp.notify{title="hi"}`）在后台运行 → 日志 `status: background=1 unread=1`，横幅期间截图非空。
  - `scene_level`：`--imu-script`（2 s 时 `tilt 0.5 0 0.87`）+ `--open-scene level` → 场景日志 `angle=30`（±1）。
  - 截图（spec 9.2）：面板与横幅在 `--shape rect`、`rounded_rect`、`circle` 上各一张，`check_screenshot()` 非空，文件留在冒烟测试的临时目录并打印路径，供人工查看。

- [ ] **Step 1: 写失败的测试**（`examples_test.cpp` 的新用例与冒烟用例先落地）。
- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'Examples|sim_smoke' --output-on-failure`。
- [ ] **Step 3: 写示例与文档，修正暴露出的问题**（每个修正先补单元测试）。
- [ ] **Step 4: 验证** — `bash tools/ci.sh` 全绿（`Schema.DocIsUpToDate`、注释守卫覆盖 `docs/`）；模拟器手动看三张面板截图，确认圆形屏上面板与横幅的文字完整可见。
- [ ] **Step 5: Commit** — `feat(scenes): run pomodoro and weather in the background and add the level scene`（文档可单独提交为 `docs: describe background scenes, power levels and notifications`）

---

### Task 16: 真机验收清单与内存、耗电实测

**Files:**
- Modify: `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`（"真机验收清单"表追加行、"内部 RAM"一节追加"场景设备侧（3b-1）"小节、"待机与省电"一节追加 3b-1 的待机级别）、本计划（执行记录）

**Interfaces:**
- 依赖：串口命令 `mem`；Task 1 的裁定（决定第 4、5 行在哪一级测浅睡）。

- [ ] **Step 1: 在验收表末尾追加以下行**（编号接现有最后一行，"结果"栏留空；来源写 "scene-device spec 9.3"）：

| 项目 | 怎么测 | 预期结果 |
|---|---|---|
| 番茄钟后台提醒 | 番茄钟设 1 分钟（临时改 `main.lua` 的时长后推送），分别在 (a) 返回让它转入后台、(b) 后台 + 短按电源键灭屏、(c) 后台 + 灭屏等满 `deep_after_s`（临时设为 60）进入深度待机三种情况下等到点 | 三种情况都亮屏、响铃（最长 30 秒，触摸即停）、出现横幅；点横幅回到番茄钟 |
| 天气深度待机刷新 | 天气在后台，设备进入深度待机后放 40 分钟；串口看 `netwindow:` 日志与场景日志 | 至少一次 `netwindow: open` → `closed (done)`，两次之间间隔 ≥ 15 分钟；天气的回调看到 `deferred = true`；窗口外 Wi-Fi 关着（`GET /system/power` 在窗口外 `level` 为 `deep_standby`） |
| 胶囊与面板 | 圆角屏上：两个后台场景 + 三条未读时看胶囊；从顶边下滑与点胶囊各打开一次面板；面板里切回、关闭、清除；在待办列表里上下快速滚动 20 次 | 胶囊、面板、横幅的文字与按钮完整可见；下滑能打开面板；列表滚动从不误开面板、点击不丢 |
| 深度待机与浅睡 | 裁定 A：拔掉 USB，深度待机 10 分钟后按电源键；裁定 B：同样操作 | A：唤醒正常，`mem` 无异常，`GET /system/power` 的 `light_sleep` 在深度待机期间为 `on`；B：唤醒正常，`light_sleep` 为 `unsupported` |
| 待机耗电 | 充满电后断开 USB，分别在 `standby`（`deep_after_s` 设 0）与 `deep_standby`（默认 300）下放置 2 小时，各记开始与结束的电量百分比 | 两组数据记入"待机与省电"一节；`deep_standby` 的下降不多于 `standby` |
| 唤醒锁表 | 打开网页日志页（WebSocket）、上传一个场景、在前台场景里 `keep_screen_on(true)`，看系统页"电源"卡片；关掉网页，等 30 分钟 | 表里依次出现 `system.admin`、`system.upload`、`scene.<id>`（screen）；锁与实际行为一致（有 network 锁时不进深度待机）；30 分钟后出现提醒通知，场景锁有"释放"，点了后锁消失 |
| 后台场景的内存 | 天气、番茄钟、计数器在后台（计数器临时加 `"background": true`），前台打开待办放 200 条；执行 `mem` | 记录 PSRAM 空闲、`internal free`/`largest`；内部 RAM 不低于 `ram_budget.hpp` 的空闲下限 |
| 切回前台的重建耗时 | 上一行状态下从面板依次切回三个后台场景；看日志 `rebuilt in <ms> ms` | 记录三个值；单个超过 300 ms 时停下来报告 |
| 新任务的栈 | 水平仪打开 1 分钟、番茄钟响铃期间各执行一次 `mem` | `wisp.imu`、`wisp.tone` 的栈在 `psram`，记录 high-water mark；余量不足 25% 时按实测加大并改代码与 `docs/architecture.md` |
| 水平仪 | 打开水平仪，把设备放平、倾斜约 30 度、摇一摇 | 度数与实际一致（±3 度），气泡随倾斜移动；返回后 `GET /system/occupancy` 为空 |

- [ ] **Step 2: 内存实测步骤**（写进"内部 RAM"一节，交给用户执行并填表）：分别在 (a) 开机空闲无场景、(b) 上表"后台场景的内存"的状态、(c) (b) 再加 STA + 热点 + 网页日志页 + 上传一个场景 zip 时执行 `mem`，记录 `internal free`/`largest`/`min`、PSRAM 空闲与 `wisp.scene`、`wisp.fs`、`wisp.shttp`、`wisp.imu`、`wisp.tone` 的栈位置与 high-water mark；(b) 相对 (a) 的 PSRAM 用量对照 spec 10 风险第 2 条（PSRAM 可用约 4.3 MB），不够时报告用户，按实测调 `kVmBudgetBytes`。
- [ ] **Step 3: 交给用户执行** Step 1 与 Step 2，逐项记录结果；不通过的项回到对应任务，先在模拟器里写出能复现的失败测试再修。
- [ ] **Step 4: 验证** — `bash tools/ci.sh`；固件 `idf.py build`。
- [ ] **Step 5: Commit** — `docs(hardware): add the scene device acceptance checklist and power and RAM checks`

---

## spec 缺口（写计划时发现，裁定见上文细化）

> 2026-11-05 用户批准计划，控制者按以下建议修订了 spec：第 1、2、5、8、12 条已在 spec 中解决；其余按计划里的细化执行。

1. spec 7.2 的 SD 路径 `/<sd>/library/scenes/<id>/files` 与基础 spec 的 Library 根 `<sd>/wisp/library` 不一致（细化第 20 条）。
2. spec 3b-1 的错误字符串带空格（`rate_limited`），3a 用下划线（`rate_limited`）；同一个场景会同时看到两种风格（细化第 23 条）。
3. spec 4.1 让 `standby` 不再浅睡，子项目 1 现在是浅睡的；这是行为变化，`standby` 的耗电会上升（细化第 1 条）。
4. `net/idle_off_min` 与 `deep_after_s` 重叠且与 `network` 锁冲突，spec 没提（细化第 2 条）。
5. "安全区顶部 16 px" 在圆形屏上从 y=74 开始，手指够不着物理顶边（细化第 16 条）。
6. spec 5.4 要端口提供 PCM 提示音，但播放只经 `hal::AudioOut`，没有平台差异（细化第 18 条）。
7. spec 5.3/5.6 要"场景图标"，目前没有任何地方解码场景图标（细化第 15 条）。
8. "管理后台关闭"是彻底关闭，但 3a 已有 `POST /close`（返回语义）；spec 没说两者怎么共存（细化第 8 条）。
9. 下载的超时、与普通请求共用一个 `wisp.shttp` worker 的串行影响，spec 没给（细化第 20 条）。
10. 自检界面的采样直接读 IMU，绕过 `SensorService`（细化第 19 条）。
11. 网页下载 64 MB 文件需要流式响应，现有 `HttpResponse` 不支持（细化第 22 条）。
12. 后台场景是否收 `minute`/`battery`/`lang`、`on_state` 是否在首次打开时触发，spec 没写（细化第 8、9 条）。
13. 系统通知的多语言文字与 `notification.posted` 里的 `title` 怎么填，spec 没写（细化第 13、14 条）。
14. spec 前置里引用的矢量字体 spec 不在本分支上（在 `feat/vector-font`），本期不依赖它（细化第 24 条）。
