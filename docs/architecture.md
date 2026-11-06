# 架构（子项目 1：基础骨架 + 子项目 3：场景运行时与设备侧接口）

最高权威是 `docs/superpowers/specs/2026-11-01-foundation-design.md`（基础骨架）、`docs/superpowers/specs/2026-11-04-scene-runtime-core-design.md`（场景运行时）和 `docs/superpowers/specs/2026-11-05-scene-device-design.md`（场景设备侧接口：后台场景、电源分级、通知、传感器、场景文件）；本文档是对当前代码的概述，与 spec 冲突以 spec 为准，但本文档只记录已经落地的内容。

## 分层

```
┌─────────────────────────────────────────────────────┐
│  入口层   firmware/main (真机)   │  simulator/ (PC)   │
├─────────────────────────────────────────────────────┤
│  wisp_app     启动编排 · 自检界面 · PowerManager         │  ← 可移植
│  wisp_scene   Lua 场景运行时（third_party/lua）          │  ← 可移植
│  wisp_ui      LVGL 驱动对接、UI 任务                     │  ← 可移植
├─────────────────────────────────────────────────────┤
│  wisp_admin   管理后台路由 · 请求过滤 · WebSocket 推送   │  ← 可移植
│  wisp_net     HTTP 类型 · WispPack · Wi-Fi · 配对        │  ← 可移植
│  wisp_core    日志 · 事件总线 · 设备注册表 · 配置 · 存储 │  ← 可移植
├─────────────────────────────────────────────────────┤
│  wisp_hal     外设接口 + 能力描述（纯接口）            │  ← 可移植
│  wisp_base    Result/ErrorCode · Value/Properties · JSON │  ← 可移植
│  wisp_os      任务/队列/锁/定时器/时钟/内存（纯接口）    │  ← 可移植
├──────────────────────────┬──────────────────────────┤
│  wisp_port_esp           │  wisp_port_sim           │
│  FreeRTOS + IDF 驱动实现   │  std::thread + SDL2 实现   │
│  + boards/<板>            │  + 模拟外设               │
└──────────────────────────┴──────────────────────────┘
```

`wisp_base`、`wisp_app` 是相对 spec 的新增（见下面"相对 spec 的细化"第 1 条）；spec 原文把 `Result`/`Value` 归在 `wisp_hal` 之下、把启动编排归在 `wisp_ui` 里，实际实现拆成了两个独立组件。

## 依赖规则

1. 可移植组件（`wisp_base`、`wisp_os`、`wisp_hal`、`wisp_core`、`wisp_net`、`wisp_admin`、`wisp_ui`、`wisp_scene`、`wisp_app`）禁止 include 任何平台头文件：`esp_*`、`freertos/*`、`driver/*`、IDF 的 `hal/*`、`soc/*`、`rom/*`、`lwip/*`、`mbedtls/*`、`SDL*`、`curl/*`，以及 `sdkconfig.h`、`nvs.h`、`nvs_flash.h`、`sdmmc_cmd.h` 和 bmgr 生成的 `dev_*`/`gen_board_*`（文件名前缀，不带目录）；只能依赖 C++20 标准库（包括 POSIX 文件 API，如 `<unistd.h>`/`<sys/stat.h>`/`<dirent.h>` —— ESP-IDF 的 VFS 和宿主机都提供，`StorageManager` 依赖它遍历/删改文件）、LVGL、ArduinoJson，以及按层次自上而下依赖其他可移植组件。Lua 的头文件（`lua.h`、`lauxlib.h`、`lualib.h`、`lua.hpp`、`luaconf.h`）只有 `wisp_scene` 能 include。
2. 每个可移植组件只能 include 自己这一层或更低层的头文件（按本文件"分层"一节的顺序：`wisp_base < wisp_os < wisp_hal < wisp_core < wisp_net < wisp_admin < wisp_ui < wisp_scene < wisp_app`），禁止反向依赖"更上层"。
3. 只有 `wisp_port_esp`、`wisp_port_sim`、`boards/*`、`drivers/*` 和入口层（`firmware/main`、`simulator/`）可以接触平台。
4. 每个 `components/wisp_{base..app}/CMakeLists.txt` 的 `wisp_component()` 调用，`IDF_REQUIRES`/`HOST_LIBS` 只能出现 ArduinoJson（`wisp_base`）和 LVGL（`wisp_ui`）这两个外部依赖——防止绕开 `#include` 检查，直接在 CMake 里给某个可移植组件挂一个平台或宿主机专属依赖。`REQUIRES` 只能列更低层的可移植组件；唯一的例外是 `third_party/` 下原样引入的组件：`wisp_scene` 可以 `REQUIRES lua`，其他层都不行。
5. 规则由 `tools/check_layering.py` 强制检查（`bash tools/ci.sh` 会跑它，先跑 `tools/check_layering_test.py` 自测），扫描每个可移植组件下的 `.hpp/.cpp/.h/.c`（`wisp_ui/fonts/` 下的移植字体数据除外）及其 `CMakeLists.txt`。

## 启动流程

真机（`firmware/main/main.cpp`）和模拟器（`simulator/main.cpp`）共用同一套 `wisp::app::App::start()`（`components/wisp_app/src/app.cpp`），区别只在各自提供的 `Platform`（日志 sink、设置后端、`emit_line`、`request_power_off`、`heap_info`、`set_light_sleep`（场景设备 spec 4.1，深度待机时开关自动浅睡；可以为空），以及子项目 2 新增的 `random`、`http_server`、`http_client`、`assets`、`http_port`（默认 8900）、`captive_server`、`http_always_on`、`reboot`、`read_line`、`firmware`（固件上传写入器 `core::FirmwareUpdater`，连接 spec 6.5；为空时不支持网页上传）、`firmware_version`，以及子项目 3a 新增的 `install_image_decoders`（经 `ui::UiConfig` 在 UI 任务上安装平台自己的 LVGL 图像解码器，目前是 `ui.image` 的 baseline JPEG：真机用 esp_new_jpeg，模拟器用 LVGL 自带的 TJpgDec；PNG 由 `lv_conf.h` 打开的 LodePNG 负责，两端相同）、`scenes_dir`（非空即开发模式：场景代码从这个目录读、只读）和 `scenes_data_dir`（开发模式下的场景数据根目录，为空时用 `<内置根>/scenes`））：

1. **日志初始化**：`wisp::log::init()`，建立 32 KB 的内存环形缓冲（真机落在 PSRAM，模拟器为普通内存）。每条记录是 16 字节头（`ts`、`seq`、长度、级别）加 tag 和消息，按整条淘汰；读取时只在锁内拷贝原始字节，解码在锁外；再次以相同大小和内存类型调用 `init()` 会保留已有记录；`seq` 与 `ts` 在同一把锁里分配，sink 收到的 `LogRecord` 与缓冲区里的完全一致（连接 spec 6.2）。
2. **板级初始化**：调用 `Platform::board->init()`（真机对应 `boards/<板>/board.cpp`，模拟器对应 `wisp::port::sim::SimBoard`），构造各设备并注册进 `DeviceRegistry`；`init()` 自己只记录非 `Ok` 的 `DeviceReport`（缺失/失败），`App::start()` 随后为注册表里没有报告过的设备补一条 `Ok`。
3. **核心服务**：启动 `EventBus`，调用 `StorageManager::init()`（挂载失败时把启动报告的 `storage` 备注设为 `"readonly"`，降级继续而不中止），启动一个 2000 ms 周期的 `StorageManager::refresh()` 定时器，启动 `ButtonService`，加载模型配置（`core::ModelProfileService::init()`，读 System 区的 `secrets/models.json`；只有旧的 `secrets/llm.json` 时把它迁移成一份对话配置）。
   **场景存储**（场景 spec 2.1/3，`components/wisp_app/src/scene_wiring.{hpp,cpp}`）：启动 `core::StorageWorker`（任务 `wisp.fs`），在 `<内置根>/scenes`（开发模式见上面的 `scenes_dir`）上构造 `core::SceneRepository`，并在 `wisp.fs` 上跑完 `recover()`（清理中断的安装）后才继续——此后任何列出或打开场景的操作都看到恢复后的状态。同时在 `<内置根>/scenes` 上构造 `core::SceneFiles`（`wisp.files`，场景设备 spec 7；开发模式也放这里，因为 `StorageManager` 只在存储挂载点内改名和删除），同一个 `wisp.fs` 作业在 `recover()` 之后跑它的 `scan()`（统计每个场景的用量、清理和完成中断的下载）；订阅 `storage.sd.inserted`/`storage.sd.removed`，在 `wisp.fs` 上 `on_sd_changed()` 重新统计 SD 卡上的场景。没有内置存储时没有场景，降级继续。
   **通知**（场景设备 spec 5，`core::NotificationService`）：随后构造通知服务并在 `wisp.fs` 上 `load()` System 区的 `notifications.json`（最多等 2 s；没有存储工作线程时就在启动线程上读），订阅 `power.battery.low`（先移除旧的 `battery_low` 通知，再发一条参数为电量百分比的）、`power.charging.changed`（开始充电时移除 `battery_low`）和 `scene.state`（`deleted` 时移除该场景的全部通知）。有未保存的变化时启动一个 300 ms 周期的 `os::Timer`（`wisp.notify`，第一次需要时创建，之后一直复用），每个周期在 `wisp.fs` 上整体 `write_atomic` 一次，磁盘上已是最新时定时器在自己的回调里停下；投递或写入失败按 600 ms 起翻倍重试，连续失败 5 次后停下，等下一次变化或 `flush()` 再试。没有存储工作线程时不保存。`App::stop()` 在存储工作线程停止之前 `close()`（停掉定时器）再 `flush(1000)`。第 4 步在构造 `PowerManager` 之前构造 `core::SensorService`（`imu.main` 上的订阅共享，任务 `wisp.imu`，没有 IMU 时不建任务；订阅 `power.state`，`standby`/`deep_standby` 暂停全部订阅；每个订阅者以共享方式登记在 `core::OccupancyRegistry` 的 `imu.main` 下），交给场景运行时（`wisp.sensor`）和自检界面（经它读 IMU，持有者 `system.selfcheck`；有场景在前台盖住自检界面时退订，回到自检界面时重新订阅，所以前台场景不用 IMU 时 IMU 关着）；再启动 `core::TonePlayer`（任务 `wisp.tone`，没有 `audio_out.main` 时不建任务、提示音静默跳过），`PowerManager` 的活动回调（每次触摸读数和按键事件）调用它的 `silence()`；场景运行时构造之后建 `app::NotificationRouter` 作为通知服务的监听者：`wake` 或 `sound = "alarm"` 且灭屏时把 `exit_standby()` 投递到 UI 线程（闹铃要响到用户触摸为止，所以总是亮屏），有声音时按缓存的 `audio/notify_vol` 播放（灭屏且不唤醒时没有声音，音频随屏幕关闭，所以不带 `wake` 的 `tone` 在灭屏时被丢弃），装了横幅时显示横幅（由下面的状态外壳装）；点击通知时以 `tap` 移除、静音并经 `SceneRuntime::deliver_notification()` 把场景切回前台或以 `"notification"` 启动后调用 `scene.on_notification(id)`。
4. **UI 与自检界面**：若 `display.main` 已注册，把 `display` 这个 Settings 命名空间的 `brightness`（默认 200）应用到它，基于它（和 `touch.main`，如果也在）启动 `UiRuntime`：`Platform::font_image`（真机是 `app_main` 映射的 `fonts` 分区，模拟器是读进内存的 `fonts.bin`）经 `ui::UiConfig::font_image` 传入，`UiRuntime::start()` 内部用 `ui::open_font_image()` 校验（矢量字体 spec 3.6），不通过只让 20 px 及以上的文字降级为位图字，不影响这一步本身的启动。否则在启动报告里记一条 `"mode": "headless"`。无论哪种情况都查找 `power.main` 设备、构造 `core::PowerMonitor` 并 `start()`它自己的采样定时器（spec 6.3/6.7，发布下表的 `power.charging.changed`/`power.battery.low`；缺少 `power.main` 时完全惰性），再构造 `PowerManager`（spec 6.7 与场景设备 spec 4，持有指向 `PowerMonitor` 的非持有指针，跟随 `App` 自己的 `core::WakeLockRegistry`）并启动它的周期性闲置检查定时器 `wisp.power`（每秒一次 `tick()`，同时把持有满 30 分钟的唤醒锁作为系统通知 `wakelock_long` 发出）。`PowerManager` 管五个级别：`active`；距灭屏 10 秒的 `dim`（亮度降到设定值的 30%，不调用设备）；`standby`（暂停 UI，按设备 caps `standby` 逐个 `set_power()`，下行按 `DeviceRegistry::all()` 的顺序、音频输入在输出之前，上行完全逆序；`Busy` 记日志并在之后每次 `tick()` 重试）；`deep_standby`（`standby` 满 `power/deep_after_s` 且没有 `network` 锁）；以及在 `deep_standby` 中、`power/light_sleep` 为真且没有 `cpu` 锁时调用的 `Platform::set_light_sleep(true)`。`screen` 锁让闲置计时不走。唤醒锁变化时不等下一次 `tick()` 就重新评估：有 UI 时总是合并成一个任务投递到 UI 线程（在 UI 线程上改锁也一样），从不在改锁的线程里执行切换。`power` 命名空间的设置只在构造时、每次 `system.settings` 之后的下一次 `tick()`，以及每 60 次 `tick()`（防止事件被丢弃）读取，越界的存储值按默认值处理（与 `SystemSettings::get()` 一致）。所有级别切换在有 UI 时都在 UI 线程上执行；如果启动了 `UiRuntime`，再在其上构建自检界面（`self_check_screen.cpp`，spec 8 节），阻塞到它真正跑完一次。
   **场景运行时**（场景 spec 3/6）：有 `UiRuntime` 且有仓库时，在 `SystemLayer` 之后、网络之前构造场景的 HTTP 代理 `net::SceneHttp`（有 `Platform::http_client` 时；它比运行时活得久，只在关机时停止）和 `scene::SceneRuntime`（先不启动），好让管理后台路由拿到它们（`AdminDeps` 的 `scenes`、`scene_host`、`request_scene_delete`）。`device_status`（`wisp.device.info()`）在场景任务上执行、不碰 flash：电量取 `PowerMonitor::last_status()`，Wi-Fi 状态取 `wisp.net` 任务每次循环写下的原子快照（`WifiService::status()` 的锁可能正被持有着写 flash，所以不在场景任务上调用它）。自检界面建好之后再启动代理和运行时（场景视图要替换的、关闭后要回到的正是这块屏），并订阅 `power.state`：待机时 `set_visible(false)`，唤醒时恢复（每次都读 `PowerManager::standby()`，不信事件里的值；`set_visible()` 记下想要的状态，消息只用来唤醒场景任务，所以场景队列满、消息被丢时，场景任务处理完手头的消息后照样补上）。用户在设备上确认的删除没能执行（场景没有关闭、存储工作线程的队列满了或删除本身出错）时，记日志并发系统通知 `scene_delete_failed`（参数为场景名），场景保持原样，不在用户背后重试。没有屏幕时没有运行时（headless）。
   **网络**（连接 spec 4.1，`components/wisp_app/src/net_wiring.{hpp,cpp}`，在 `PowerManager` 之后、自检界面之前）：有 `Platform::random` 时构造 `net::PairingService`；若 `net.wifi` 已注册，再构造 `net::WifiService` 并 `init()`（只读取已保存的网络并安排第一次尝试，不碰射频）。`WifiService::tick()` 与 `PairingService::tick()` 都在独立任务 `wisp.net` 里每 250 ms 跑一次——扫描可能阻塞约 3 s，不能放进共享的 `os::Timer` 分发任务，所以开机从不等待联网。`wisp.net` 任务每次循环把 `PowerManager::level()` 交给 `WifiService::set_power_level()`（`power.state` 只用来及时唤醒任务，事件丢了或迟到都不会弄错级别）：`deep_standby` 关热点、断开并关射频（`hal::Wifi::set_radio(false)`），离开 `deep_standby` 时开射频并立即重连；`standby` 不动 Wi-Fi。有场景代理时同一循环在 Wi-Fi 之前驱动 `net::NetworkWindow`（见"真机的网络"的深度待机一条）。有 `Platform::http_server` 且有 `WifiService` 时构造 `admin::AdminServer`（含 `WsHub`；本次开机的标识 `boot_id` 在这里用 `Platform::random` 生成一次，见连接 spec 6.2）：STA 拿到 IP 或热点开启时由 `wisp.net` 任务启动 HTTP 服务、两者都断开时停止（接口状态每次循环都从 `WifiService::status()`/`ap_status()` 读取；`net.wifi.state`/`net.ap.state` 只用来及时唤醒任务，丢了事件也不会让服务停在错误状态）；`http_always_on`（模拟器）则开机即启动、断网也不停。另外，只要热点开着，`wisp.net` 任务就在 80 端口运行 `Platform::captive_server`（真机才有），处理函数是 `AdminServer::captive_handler()`：从热点接口来的任何请求都 `302`（其他接口返回 404）到 `http://<热点地址>:<http_port>/setup.html`，让手机的强制门户探测打开配网页；热点关闭时停止。启动失败记日志，并在接口仍然开着时退避重试（2 s 起翻倍，上限 60 s）。每次启动成功输出一行 `WISP_HTTP {"port": n}`。启动报告记录 `notes.net`。有 `Platform::sntp` 且有 `WifiService` 时构造 `net::TimeSyncService`（连接 spec 5.6）：`wisp.net` 任务每次循环以"STA 是否有 IP"调用它的 `tick()`，到期时在本任务内读出 `ntp_servers`（Settings 会访问 flash），再唤醒常驻任务 `wisp.ntp`（4096 B 栈、优先级 3、`flash_access=false`）去执行 SNTP 请求、设置系统时钟（`Platform::set_wall_clock`，ESP 上与板子开机从 RTC 设时间共用 `wisp/port/esp/wall_clock.hpp`）、写 `clock.main` 并发布 `system.time`——请求可能每个服务器阻塞几秒，不能卡住 `wisp.net`。
   **状态外壳**（场景设备 spec 5.3/5.5/5.6，`components/wisp_app/src/status_shell.{hpp,cpp}`）：有 `SystemLayer` 时，在网络之后、自检界面之前构造 `app::StatusShell` 并 `start()`——它的 `device_status` 与 `wisp.device.info()` 共用同一个（读 `PowerMonitor::last_status()` 和 `wisp.net` 的 Wi-Fi 原子快照，不碰 flash），所以要等网络对象建好；时区取 App 从 `system.settings` 缓存的 `utc_offset_min`（UI 线程不读 Settings）。它订阅 `scene.state`、`notification.*` 和 `power.state`：事件在总线分发线程上到达，合并成一个投递到 UI 线程的刷新任务（同一时刻最多排一个），刷新时以 `{background_scenes().size(), unread_count()}` 调 `SystemLayer::set_status()`，计数变化时记 `status: background=<n> unread=<m>`，面板开着时顺带重建它的模型；灭屏（`standby`/`deep_standby`）时关闭面板。面板的处理函数：打开时（记 `panel: open`）按当前状态建模型——页眉是按 `utc_offset_min` 换算的 `HH:MM`（时钟未设置时 `--:--`）、电量和 Wi-Fi 状态，然后是后台场景和未读通知（最新在上）；系统通知经 `ui::system_notice_text()` 按当前界面语言生成文字，参数按原样代入（场景类通知发出时参数已是场景名），唤醒锁提醒的持有者先经 `ui::holder_label()`（`scene.<id>` 取场景的名称：运行中（前台或后台）用 `SceneRuntime::running_name()`，否则在 `wisp.fs` 上读仓库里的清单并缓存在外壳里，读完再刷新一次；只有场景未安装时显示 id）；关闭时记 `panel: closed`（`PanelHandlers::on_close`）。点场景行 `launch(id, User)`，× `exit(id)`；点通知 `NotificationRouter::tap()`，× 或左滑以 `dismiss` 移除，操作按钮 `NotificationRouter::act()`，"全部清除" `clear_all()`。横幅经 `NotificationRouter::set_banner()` 接入：回调在发通知的线程（`wisp.notify` 时是场景任务）上只把通知投递到 UI 线程，在那里生成文字并 `show_banner()`，点击即 `tap()`。通知服务的唯一监听者仍是路由。没有屏幕时不建。
   **固件上传服务**（连接 spec 6.5，在网络之前构造，因为后台路由要把上传流写进它）：`core::FirmwareUpdateService` 包着 `Platform::firmware`，没有 UI 时不设确认回调（上传在 `begin` 时就以 `unavailable` 拒绝，因为没有屏幕可以确认）；`wisp.net` 任务每次循环调用它的 `tick()`，未确认的镜像 2 分钟后丢弃。
5. **收尾**：记录 `boot_ms`，依次输出 `"WISP_BOOT_REPORT <json>"`、`"WISP_MANIFEST <json>"`（通过 `Platform::emit_line`），再发布 `system.boot.completed` 事件。
6. **串口控制台**：有 `Platform::read_line` 时启动 `wisp.console` 任务（见下文"串口控制台"）。
7. **确认镜像可用**：`FirmwareUpdater::mark_valid()`。真机上新装的镜像在这一步之前崩溃或复位，引导程序都会回滚到旧镜像（`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`，见 `docs/porting-guide.md`"固件与分区"）；模拟器什么都不做。

**停止顺序**（`App::stop()`）：串口控制台 → 状态外壳（`StatusShell`：退订事件、撤下横幅回调，再在 UI 线程上撤下面板处理函数并等此前投递的任务跑完；之后还在路上的横幅回调和留在系统层里的横幅点击都什么也不做）→ 通知路由（`NotificationRouter`，退订后等它投递到 UI 线程的唤醒做完）→ 场景运行时（退订 `power.state`，关闭前台场景并在自检界面还在时把它装回屏幕）→ 自检界面（退订它的 IMU 订阅）→ 等待已确认的恢复出厂（`wisp.reset`）和固件安装（`wisp.fwup`）任务结束 → 网络（先 join `wisp.net` 任务，再取消校时服务并 join `wisp.ntp` 任务（进行中的尝试在问下一个服务器前停止，最多等正在进行的那一次查询），再停强制门户服务和 HTTP 服务——`SimHttpServer::stop()` 会在 join 连接线程时回调 `on_close`，所以调用时不持有任何锁——然后退订事件、销毁 `AdminServer`、`WifiService`、`PairingService`）→ 固件上传服务 → 场景代理 `SceneHttp`（可能要等一个进行中的请求）→ `StorageWorker`（最多等 5 s 把队列里的活——包括关闭的场景最后一次 flush——做完，再停）→ `SensorService`（退订 `power.state`，停 `wisp.imu`；场景运行时和自检界面都已退订）→ `PowerManager` → `TonePlayer`（`PowerManager` 的活动回调会调用它）→ `PowerMonitor` → 存储定时器 → `ButtonService` → `UiRuntime`（自己在 `lv_deinit()` 之前先销毁全部矢量字体，矢量字体 spec 4.5：它们和各档的字形缓存都在 LVGL 的堆与 `lv_cache` 上，`lv_deinit()` 之后再碰就是悬空指针）→ `EventBus`。HTTP 处理函数会经 `SystemSettings` 调用 `PowerManager::set_brightness()`，所以网络必须先于 `PowerManager` 停止；后台路由会用场景仓库和运行时，所以网络也先于场景代理和 `StorageWorker` 停止，而 `StorageWorker` 要在存储定时器之前停。

**场景相关的任务**（AGENTS.md 第 5 节：每个任务如实声明 `flash_access`，栈放哪里由端口决定）：

| 任务 | 所有者 | 栈 | `flash_access` | 说明 |
|---|---|---|---|---|
| `wisp.scene` | `scene::SceneRuntime` | 128 KB（`SceneRuntimeConfig::stack_bytes`，Lua 解释器递归最深处要这么多） | `false`，真机放 PSRAM | 只跑 Lua 和场景消息；读写文件（`SceneStore` 的 flush、加载包）都交给 `wisp.fs`，`device_status` 也只读内存 |
| `wisp.fs` | `core::StorageWorker` | 6144（`StorageWorkerConfig::stack_bytes`） | `true`，内部 RAM | 先进先出地替不能碰 flash 的任务做文件操作：仓库的 `recover`/`list`/`remove`、场景存储的加载与 flush、通知的加载与保存、自检界面的场景列表、`SceneFiles` 的 `scan`/`on_sd_changed` 和每个 `wisp.files` 调用、下载的开始（`begin_download`）、分块追加与收尾（`finish`） |
| `wisp.shttp` | `net::SceneHttp` | 8192 | `false`，真机放 PSRAM | 场景 `wisp.http` 请求一次一个地经 `Platform::http_client` 发出；不读 Settings、不碰文件。`wisp.http.download` 的响应体经 `FileDownload::push()` 只拷进 PSRAM 并把追加排到 `wisp.fs`（排队超过 128 KB 时等待，取消即醒）；下载的超时是 5 分钟，期间独占这一个 worker |
| `wisp.tone` | `core::TonePlayer` | 4096（`TonePlayerConfig::stack_bytes`） | `false`，真机放 PSRAM | 通知提示音（场景设备 spec 5.4）：运行时合成 16 kHz PCM 写到 `audio_out.main`，播放期间持有 `cpu` 锁 `system.tone`；音量由 `app::NotificationRouter` 传入（App 从 `system.settings` 缓存的 `audio/notify_vol`），不读 Settings、不碰文件 |
| `wisp.imu` | `core::SensorService` | 4096（`SensorServiceConfig::stack_bytes`，按 `mem` 实测再定） | `false`，真机放 PSRAM | IMU 共享（场景设备 spec 6.1）：有订阅时按最高采样率轮询 `imu.main`，每 100 ms 把样本按订阅者打包交给回调；回调只拷贝样本（场景的 `wisp.sensor` 投递 `msg::Sensor` 给 `wisp.scene`，自检界面写进自己的快照），不读 Settings、不碰文件。没有订阅、全部暂停或待机时 IMU 关闭、任务阻塞等待 |

**失败处理**（spec 6.1）：设备初始化失败只记录、不注册，系统降级继续；`Display` 失败则不启动 UI，走 headless 路径；内置 Flash 挂载失败走只读降级。`WISP_BOOT_REPORT`、`WISP_MANIFEST`、`WISP_PERF` 都是"前缀 + 单行 JSON"（AGENTS.md 第 4 节）。

## 事件主题清单

当前代码里实际发布的主题（`grep -rn` 核实，不是 spec 草案）：

| 主题 | 发布方 | 说明 |
|---|---|---|
| `button.<id>.down` / `.up` / `.short` / `.long` | `core::ButtonService` | `<id>` 为按键设备 id（`button.power`、`button.vol_up`、`button.vol_down`），同时是 `Event::source` |
| `power.state` | `app::PowerManager` | 每次级别变化时发布，`payload = {"state": "active"\|"dim"\|"standby"\|"deep_standby"}`；`standby` 与 `deep_standby` 都是灭屏，`dim` 仍亮屏 |
| `power.wakelock` | `core::WakeLockRegistry` | 每次取得或释放唤醒锁时发布，`Event::source` 为 `power`，`payload = {"op": "acquire"\|"release", "holder": "system.<模块>"\|"scene.<id>", "kind": "network"\|"cpu"\|"screen", "reason": string}` |
| `notification.posted` | `core::NotificationService` | 场景设备 spec 5.7，每条通知发出时发布（包括前台场景只弹横幅、不进未读列表的那种），`Event::source` 为 `"notifications"`，`payload = {"id": string, "source": "system"\|<场景 id>, "title": string}`，系统通知的 `title` 是它的代码（如 `battery_low`）。不含正文。同一来源同一 id 再发一次是替换，只发这一条，旧的不发 `notification.dismissed` |
| `notification.dismissed` | `core::NotificationService` | 场景设备 spec 5.7，一条未读通知被移除时发布，`Event::source` 为 `"notifications"`，`payload = {"id", "source", "reason": "tap"\|"dismiss"\|"cancel"\|"evict"}`；`evict` 是超出每场景 5 条或总数 50 条时被挤掉的最旧那条。场景被删除（`scene.state` 为 `deleted`）时它的通知以 `dismiss` 移除 |
| `power.charging.changed` | `core::PowerMonitor` | `power.main` 设备缺失时不发布；`charging`/`usb_present` 任一变化时发布（首次采样不算"变化"，不发布），`payload = {"charging": bool, "usb_present": bool, "battery_percent": int}`；`Event::source` 为设备 id（`power.main`） |
| `power.battery.low` | `core::PowerMonitor` | `battery_percent`（已知，即不是 -1）降到 ≤ Settings `power`/`low_batt_pct`（默认 15）时发布一次；回升超过阈值 +5 才重新武装，`payload = {"battery_percent": int}`，`Event::source` 为设备 id |
| `storage.sd.inserted` / `storage.sd.removed` | `core::StorageManager` | SD 卡插拔检测 |
| `system.boot.completed` | `app::App` | 启动流程第 5 步收尾时发布 |
| `system.factory_reset` | `app::App`（`components/wisp_app/src/factory_reset.cpp`） | `payload = {"state": "requested"\|"cancelled"\|"done"}`。`POST /system/factory-reset` 发 `requested`，自检界面据此弹出确认框（请求同时记为 pending，自检界面在收到事件、建好界面时和每秒的重新同步里取走它，所以界面尚未订阅或事件被总线丢弃时请求也不会丢；同一时间只有一个确认框：重置框已开时重复请求被忽略，关机确认框开着时直接回 `cancelled`；反过来重置框开着时长按电源键不弹关机框）；点"否"发 `cancelled`；点"是"后在一次性任务 `wisp.reset` 上先发 `done`，最多等 500 ms 让事件总线和管理后台 WebSocket 发送线程把它送出，再清空 Settings、忘记全部 Wi-Fi、撤销全部配对浏览器、清除全部模型配置（`secrets/models.json`），最后 `Platform::reboot`；`wisp.reset` 任务创建失败时什么都不清除、发 `cancelled`，之后的请求可以重新弹框。Library/Backup 数据保留 |
| `system.firmware` | `app::App`（`core::FirmwareUpdateService`，`components/wisp_core/src/firmware_update.cpp`） | 连接 spec 6.5，`payload = {"state": "received"\|"confirmed"\|"rejected"\|"failed", "version": string, "error": string（仅 `failed`）}`，`version` 为上传镜像的版本。`POST /system/firmware` 把镜像流式写完并校验通过后发 `received`，同时记为 pending 并请自检界面弹出确认框（与恢复出厂同一套 pending/每秒重新同步机制；同一时间只有一个确认框，已有其他确认框时直接 `rejected`）；点"是"后在一次性任务 `wisp.fwup` 上把它设为启动镜像，成功则发 `confirmed`，等事件总线空闲并最多 500 ms 让后台 WebSocket 送出，再 `Platform::reboot`；点"否"或 2 分钟无人确认发 `rejected` 并丢弃镜像。上传开始后的失败发 `failed`，`error` 为 `wrong_board`、`invalid_image`、`payload_too_large`、`write_failed`，或连接中途断开时的 `aborted` |
| `system.settings` | `app::App`（`core::SystemSettings` 的 `on_changed` 回调） | 每批设置写入成功后发布（连接 spec 8.3），`payload = {"brightness": int, "volume": int, "screen_off_s": int, "language": "zh"\|"ja"\|"en", "utc_offset_min": int, "ntp_servers": string, "deep_after_s": int, "light_sleep": bool, "notify_vol": int}`，即写入后的全部当前值（与 `GET /settings` 同形）；自检界面据 `language` 在 UI 线程上 `ui::set_lang()` 并重建文字 |
| `system.time` | `app::App`（`net::TimeSyncService`，`components/wisp_net/src/time_sync.cpp`） | 连接 spec 5.6，每次校时尝试结束后在 `wisp.ntp` 任务上发布，`payload = {"state": "synced"\|"failed", "server": string, "time": int（仅 `synced`，UTC 秒）}`；`failed` 时 `server` 是最后问过的服务器。场景运行时收到 `synced` 时（亮屏）补发一次 `minute`；后台"系统"页的"立即同步"据它给出结果 |
| `net.wifi.state` | `net::WifiService` | 状态变化时发布，`payload = {"state": "off"\|"connecting"\|"backoff"\|"connected", "ssid": string, "ssid_hex": string（SSID 原始字节的小写十六进制，见连接 spec 6.1）, "ip": string, "rssi": int, "reason": ""\|"not_found"\|"auth_failed"\|"other"}`；`reason` 只在 `backoff` 时非空；`Event::source` 为设备 id（`net.wifi`）。不含密码 |
| `net.ap.state` | `net::WifiService` | 配网热点开关时发布，`payload = {"active": bool, "ssid": string, "clients": int}`，不含热点密码 |
| `net.pairing.code` | `net::PairingService` | 发出配对码时 `{"active": true, "expires_ms": int}`（`os::now_ms()` 时间基准下的截止时刻），失效时（过期、配对成功、改发新码前，或在设备的配对卡片上点"拒绝"即 `PairingService::cancel_code()`）`{"active": false, "expires_ms": 0}`；不含配对码 |
| `net.pairing.failed` | `net::PairingService` | 配对码错误时 `{"attempts": int, "locked": bool}` |
| `scene.state` | `scene::SceneRuntime`；删除时 `app::App`（`scene_wiring.cpp`） | 场景 spec 3.6，`Event::source` 为 `"scene"`，`payload = {"id": string, "state": "stopped"\|"running"\|"background"\|"error"\|"deleted"}`；`running` 是前台，`background` 是转入后台继续运行（场景设备 spec 3.2，声明了 `"background": true` 的场景返回时转入后台，切回前台时再发 `running`）。`deleted` 在设备上确认删除、文件删掉之后发布；自检界面的"场景"卡片收到任何 `scene.state` 都重新列一遍（另有 30 s 一次的兜底重列）。`SceneRuntime` 发布的每一条同时以 `scene` 标签写一行日志 `<id>: <state>`，场景关闭、界面回到原屏幕后再写一行 `<id>: closed`（出错后关闭不再发布事件，状态仍为 `error`），供只看日志的一方（串口、冒烟测试）跟踪 |
| `model.profiles` | `core::ModelProfileService` | 模型配置增删改或切换当前配置时发布，`Event::source` 为 `"models"`，`payload = {"active": {"llm": string, "asr": string, "tts": string}, "count": int}`（`active` 为各类当前配置 id，没有时为空串；不含任何 API Key） |

`power.charging.changed`/`power.battery.low` 的字段名跟随 `wisp::hal::PowerStatus` 本身的成员名（`charging`/`usb_present`/`battery_percent`），不是 "vbus_present" 这个措辞——后者出自计划中驱动结构体 `axp2101_status_t` 的字段名（`drivers/axp2101/include/axp2101.h`），不是 spec 原文；spec 本身没有提到这个字段名，沿用 hal 已有的字段名更一致。

事件总线本身（`wisp_core/event_bus.hpp`）：前缀订阅（空串匹配所有主题）、按发布顺序派发、单个分发任务、队列默认容量 64（满时丢弃最旧并计数，见 `dropped()`；和所有 `os::Queue` 一样，槽位放在 `Mem::Large`，没有时退回 `Mem::Fast`）、处理函数里可以安全地再 `publish`/`subscribe`/`unsubscribe`。

管理后台 WebSocket 的 `events` 订阅（`admin::WsHub`，连接 spec 6.2）转发的主题前缀是 `net.`、`power.`、`scene.`、`storage.`、`system.`（`kForwardedPrefixes`）；`button.*` 和 `model.profiles` 不转发。

## 机器可读输出

下面带 JSON 的几条都是"前缀 + 单行 JSON"（AGENTS.md 第 4 节）：改动下面任一字段集合时，要同步更新 `tests/smoke/sim_smoke.py`、`tests/smoke/admin_smoke.py` 和本节。

- **`WISP_BOOT_REPORT <json>`**（启动流程第 5 步；`core::BootReport::to_value()`）：
  ```
  {"board": string, "boot_ms": int, "fw": string,
   "devices": [{"id": string, "class": string, "state": "ok"|"degraded"|"missing"|"failed", "reason": string}, ...],
   "notes": {...}}
  ```
  `fw` 是 `Platform::firmware_version`（真机：`esp_app_get_description()->version`，即 IDF 的 `PROJECT_VER`；模拟器：常量 `"0.1.0"`）。`notes` 是自由格式的诊断备注，目前用到的键：`mode`（值 `"headless"`，无 `display.main` 时）、`storage`（值 `"readonly"`，内置 Flash 挂载失败时）、`library_roots`（`core::StorageManager::roots(Area::Library)` 的字符串数组，spec 9.2 的"`--no-sd` 时 Library 只有内置根"就是靠这个字段机器可读地核实的）、`net`（连接 spec 4.1，开机那一刻的网络状态：`"missing"` 表示没有 `net.wifi` 设备（或平台没有提供 `random`），`"off"` 表示没有已保存的网络，`"connecting"` 表示有已保存的网络、正在后台连接）。
- **`WISP_MANIFEST <json>`**（启动流程第 5 步；`core::DeviceRegistry::manifest()`）：
  ```
  {"board": string,
   "devices": [{"id": string, "class": string, "model": string, "caps": {...}, "safe_area"?: {"x1","y1","x2","y2": int}}, ...]}
  ```
  `caps` 的键因设备类别而不同（见各 `wisp/hal/*.hpp` 头文件的说明）。`safe_area` 只出现在 `class` 为 `"display"` 的条目上（spec 5.6）。
- **`WISP_PERF <json>`**（自检界面，每 5 s 一行；`SelfCheck::emit_perf_line()`）：
  ```
  {"fps": number, "cpu": int, "flush_ms": number, "frames": int,
   "heap_internal_free": int, "heap_large_free": int,
   "wisp_fps": number, "wisp_ms": number}
  ```
  `fps`/`flush_ms` 来自 `ui::UiRuntime::perf()`；`flush_ms` 是 UI 任务每段（一块局部缓冲）花在屏幕上的平均毫秒数，即启动传输加上之后等它传完，传输与渲染重叠时远小于传输本身。两者本质是浮点数，但 JSON 往返时整数值会被识别为 `Int` 而非 `Double`（`Value`/JSON 对 Int/Double 的区分按实际值而非来源类型，见 `tests/base/value_json_test.cpp`）；解析方不应假设其中任何一个字段固定是哪一种 JSON 数字形式。`heap_internal_free`/`heap_large_free` 来自 `Platform::heap_info`（可能为空，此时都是 0）。`wisp_fps`/`wisp_ms` 来自 `ui::wisp_perf()`（`wisp/ui/wisp_model.hpp`）：最近 5 s 里所有鬼火（`WispView`）合计的出帧率，和每帧的平均毫秒数：模型推进、生成形状，加上鬼火在 LVGL 绘制事件里画线和填充的时间（软件渲染器在这些调用里完成光栅化，刷屏不算在内）；这段时间没出帧时两者都是 0（mvp spec 6.1）。电子纸只在表情变化时出一帧，所以 `wisp_fps` 接近 0。模拟器 `--wisp-demo` 用自己的屏幕（背景按面板取黑或白）循环演示五种表情，可配合 `--panel mono|epaper` 查看单色和电子纸的画法；自检界面留在后面照常输出这一行。

- **`WISP_BENCH <json>`**（串口控制台命令 `bench scroll [秒数]` 的回答；`wisp/app/bench.hpp`，真机和模拟器都经 `Platform::console_commands` 注册）：
  ```
  {"bench": "scroll", "fps": number, "frames": int, "ms": int, "cpu": int, "px": int}
  ```
  在 UI 线程里把当前界面上最大的可滚动对象（自检界面就是列表）每帧用 `lv_obj_scroll_by_raw()` 滚 `px`（6）像素，到头就反向，前后各发一次 `SCROLL_BEGIN`/`SCROLL_END`，结束后滚回原位；默认 10 s，命令阻塞到测完，期间屏幕不会自动熄灭，熄屏时拒绝运行。`frames` 是 `REFR_READY` 的次数，`ms` 是第一帧到最后一帧的时间，`fps` = (`frames` − 1) / `ms`，保留两位小数；`cpu` 是跑起来第 2 s 起每秒一次 `UiRuntime::perf()` 的 `cpu_percent` 的平均值（与 `WISP_PERF` 的 cpu 同义，是 `lv_timer_handler()` 里度过的墙钟时间）。这是硬件文档"性能基准"一节 raw 列的测法，数值可以直接和那里的基准比。
- **`WISP_HTTP <json>`**（网络步骤；`NetWiring::reconcile_http()`）：管理后台 HTTP 服务每次启动成功时一行，`{"port": int}`，是 `HttpServer::port()` 报告的实际端口。可能出现在 `WISP_BOOT_REPORT` 之前（`http_always_on` 时开机即启动）。模拟器 `--http-port 0` 时不启动服务，也就没有这一行；需要后台的冒烟测试自己挑一个空闲端口传进去（`admin_smoke.py` 在端口被抢走、日志出现 `admin server failed to start` 时换端口重试）。
- **`WISP_REBOOT`**（仅模拟器，无 JSON）：`Platform::reboot` 被调用（例如 `POST /api/v1/system/reboot`，或恢复出厂设置完成、固件更新确认后）时输出，随后主循环返回，经正常的 `App::stop()` 以退出码 0 结束进程（不在定时器线程里直接 `exit()`，以免静态析构与其他线程竞争）。真机对应 `esp_restart()`。
- **`pair-code=<6 位数字>`**（仅模拟器调试命令，无 JSON）：控制台命令 `debug pair-code` 的回答，当前没有有效配对码时为 `pair-code=none`。只在 `wisp_sim` 目标定义了 `WISP_SIM_DEBUG_COMMANDS` 时经 `Platform::console_commands` 编译进去（`simulator/main.cpp`），可移植层和固件都没有；供 `tests/smoke/admin_smoke.py` 不看屏幕完成配对。

## 模拟器的网络

- `net.wifi` 是 `wisp::port::sim::SimWifi`（model `sim-wifi`）：内置 `WispLab`（-45 dBm，密码 `wisp1234`）、`Neighbor`（-70，`neighbor99`）、`OpenCafe`（-80，开放）；`--wifi-fail` 让所有连接以 `not_found` 失败；热点只改状态，`ap_clients()` 在热点开启时恒为 1；`set_radio(false)` 断开并关热点，之后扫描和连接都失败，直到 `set_radio(true)`。
- 管理后台由 `SimHttpServer` 提供，只监听 `127.0.0.1`，端口 `--http-port`（默认 8900，与真机后台端口相同；0 表示不启动）。模拟器没有 80 端口的强制门户服务（`Platform::captive_server` 为空）。出站请求（模型连通测试 `POST /ai/profiles/{id}/test`）走 `SimHttpClient`（libcurl，主机构建 `find_package(CURL 7.80 REQUIRED)`；可移植层禁止 include `curl/`）。
- 触摸脚本：`--tap X,Y@MS`（按下 100 ms）和 `--swipe X1,Y1,X2,Y2@MS`（400 ms 内拖动，停 200 ms 后松开）在主循环开始后第 MS 毫秒向 `SimInput` 注入触摸，可重复使用，坐标 0–479。标准输入里以 `tap` / `swipe` 开头的行（`tap X,Y` / `swipe X1,Y1,X2,Y2`）也立即执行，供需要先等设备上某件事发生再点的脚本使用：冒烟测试 `factory_reset` 和 `firmware` 等到 WebSocket 日志里出现确认框的日志行，再经标准输入点"Yes"。
- 场景开发（场景 spec 6.4）：`--scenes-dir <目录>` 让场景仓库以该目录为代码根运行在开发模式，数据仍落在 `<data-dir>/scenes/<id>/data`；前台场景的文件一变就重新加载。`--open-scene <id>` 在 `App::start()` 返回后立即打开一个场景。只在 `wisp_sim` 目标定义 `WISP_SIM_DEBUG_COMMANDS` 时才有的调试命令 `debug scene-click <ref>` 点击前台场景里 `ref` 为该名字的部件（`SceneRuntime::click_ref()`），回答 `scene-click=ok` 或 `scene-click=<错误>`。`debug panel-tap <序号>` 点已打开的下拉面板的第几行（从 0 起，先是后台场景，再是通知；`SystemLayer::tap_panel_row()`），回答 `panel-tap=ok` 或 `panel-tap=<错误>`；`debug screenshot <路径>` 把屏幕当前的样子写成 PNG（与 `--screenshot` 相同，只是随时可用），回答 `screenshot=ok` 或 `screenshot=<错误>`。日志行每行写完就刷新标准输出，读管道的脚本（冒烟测试）不会被缓冲拖住。`--imu-script <文件>` 按脚本驱动模拟 IMU（`wisp/port/sim/imu_script.hpp`：`<毫秒> tilt <x> <y> <z>`、`shake`、`flip`）。
- 标准输入只有一个读取者（`simulator/sim_stdin.hpp` 的 `StdinReader`，带超时轮询，退出时不会卡在阻塞读上）：不论是否 `--headless`、stdin 是不是终端都会读取；触摸行交给触摸脚本，其余每行都是串口控制台（serial console）命令，经 `Platform::read_line` 交给控制台任务；控制台没有 `tap`/`swipe` 命令。`tap`/`swipe` 与坐标之间可以是空格或制表符。stdin 是控制终端且进程在后台进程组（`wisp_sim &`）时读取者暂不读，`fg` 后恢复；模拟器忽略 `SIGTTIN`，所以不会被作业控制停住。
- 信号：`SIGINT`/`SIGTERM` 让主循环像重启一样返回，经正常的 `App::stop()` 以退出码 0 结束（SDL 自己的信号处理被 `SDL_HINT_NO_SIGNAL_HANDLERS` 关掉，否则无头模式收不到）；处理是一次性的，关机卡住时再发一次信号按默认方式结束进程。

## 真机的网络

- `net.wifi` 是 `wisp::port::esp::EspWifi`（`components/wisp_port_esp/net/esp_wifi.cpp`），由板级 `init()` 最后注册，并在那里就打开射频（`Platform::random` 依赖它）。驱动配置只放 RAM（`WIFI_STORAGE_RAM`），已保存的网络只由 `WifiService` 管。`link()` 读取事件回调维护的快照；断开原因按 `wifi_reason.hpp` 映射，握手失败只在信号足够好（RSSI ≥ -75 dBm）时才算 `auth_failed`；断开事件没带 RSSI 时，用两分钟内最近一次扫描里同名网络的最强信号代替，都没有就不算。热点：AP+STA、WPA2-PSK、信道 1、最多 4 台，热点关着时 AP 接口留着一份隐藏、随机密码的 WPA2 占位配置，切到 AP+STA 的瞬间不会广播开放网络；同时在热点地址的 UDP 53 端口运行强制门户 DNS（`net/captive_dns.cpp`，所有 A 查询都答热点地址）。STA 第一次拿到 IP 时启动 mDNS：`wisp.local`，广播 `_http._tcp` 端口 8900。
- 管理后台是 `EspHttpServer`（`esp_http_server`）：端口 8900，最多 7 个连接（`lru_purge_enable`；每个请求都把打开的 WebSocket 标为最近使用，连接满时先关空闲的 keep-alive 连接），`/api/v1/ws` 的升级在握手前交给 `WsHandlers::on_open`（`CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT`），拒绝就是普通的 403；任务优先级 3，低于 UI。80 端口的强制门户是第二个实例（另一个控制端口、4 个连接、无 WebSocket），`AdminServer::captive_handler()` 只对热点接口的请求重定向，其余返回 404。请求所在接口按连接的本地地址和对端地址判断（`net_iface.hpp`），不看请求头。
- 出站请求走 `EspHttpClient`：HTTPS 用 IDF 证书包校验（含主机名），不跟随重定向，响应体上限 64 KB。
- 深度待机（场景设备 spec 4.1、4.4）：`EspWifi::set_radio(false)` 是 `esp_wifi_stop()`，`set_radio(true)` 是 `esp_wifi_start()`；射频关着时 `Platform::random` 不是真随机数，但这期间不会生成配对码、令牌或热点密码（都要网络客户端或亮屏才会发生）。原来的 `net/idle_off_min`（待机 10 分钟后断网）已删除，存着的值不再读取。场景的请求在深度待机中进入 `SceneHttp` 的延迟队列（每场景 4 条，超出 `deferred_queue_full`，排队超过 1 小时 `deferred_timeout`，提交时照样计入每分钟 30 次）。`net::NetworkWindow` 由 `wisp.net` 任务驱动：进入深度待机或上一个窗口结束满 15 分钟、且队列非空时取 `system.netwindow` 的 `network` 锁（日志 `netwindow: open`），`PowerManager` 因此回到 `standby`、Wi-Fi 打开重连；连上后放行队列，队列清空并收到一次 `system.time`（连上后最多等 30 秒）时释放锁（`netwindow: closed (done)`）；60 秒连不上 `closed (no wifi)`（窗口中途断线时重新排队，从断线起再等 60 秒），开满 5 分钟 `closed (timeout)`，期间被唤醒 `closed (awake)`。锁释放后设备回到深度待机、Wi-Fi 关闭，没发出的请求继续排队；开始排队时，运行队列里还没开始的请求也退回延迟队列。离开深度待机（亮屏，或别的 `network` 锁让设备回到 `standby`）时 Wi-Fi 才开始重连，所以队列等到 STA 连上、最多 60 秒才放行；放行期间新提交的请求排在积压请求之后，不会因为运行队列满而得到 `rate_limited`。
- 系统唤醒锁（场景设备 spec 4.3，持有期间不进深度待机）：`system.admin`（`network`：`WsHub` 至少有一个 WebSocket 连接时，原因 `admin session`；以及每个通过认证的 REST 请求之后 2 分钟，原因 `admin request`，只用 REST 的页面也能保持 Wi-Fi。半开的连接不会一直占着锁：`WsHub` 每 30 秒给每个客户端发 `{"t":"ping"}`，网页回 `{"t":"pong"}`，连续 3 轮没收到客户端任何消息就关闭它（日志 `client N closed: silent`）；ESP 的 httpd 另外开了 TCP keepalive（空闲 30 秒后每 10 秒探测，3 次无应答断开））、`system.upload`（`network`，场景 zip 上传从开始到结束或中止）、`system.ota`（`network` + `cpu`，固件上传与写入）、`system.ntp`（`network`，`wisp.ntp` 每次校时尝试期间）、`system.hotspot`（`network`，配网热点开着时，由 `wisp.net` 任务持有）、`system.netwindow`（`network`，联网窗口）。
- 网页资源：固件构建时 `tools/pack_assets.py` 把 `web/` 打成 `wisp_web.wpak`（超过 256 KiB 或 150 KB gzip 预算即构建失败），按 4 字节对齐链接进应用镜像（`firmware/main/CMakeLists.txt` 的 `target_add_binary_data`，符号 `_binary_wisp_web_pack_start/_end`），静态文件直接从映射的 Flash 发送。网页与固件因此总是同一版本，随 OTA 一起切换，不再有单独的 `assets` 分区。
- 固件上传（连接 spec 6.5）：`POST /api/v1/system/firmware` 的请求体不缓存，由 `EspHttpServer` 按 `HttpServer::StreamHandler` 以 4 KB 一块交给 `FirmwareUpdateService`，再由 `EspFirmwareUpdater` 用 `esp_ota_*`（顺序写）写进没在运行的 `ota_N` 槽；头 336 字节到齐时就核对项目名 `wisp`、芯片（`CONFIG_IDF_FIRMWARE_CHIP_ID`）和板名描述符（见 `docs/porting-guide.md`"固件与分区"），不符的镜像一个字节都不写。网页上传从不要求签名。httpd 只有一个任务，由此带来几条已知限制（接受现状，不在代码里绕开）：上传期间 WebSocket 日志推送暂停到上传结束；提前应答时剩余请求体超过 8 MiB（`kBodyDrainBytes`）就不再读完丢弃，应答（例如超过 8 MiB 的文件得到的 413）写出后连接直接被重置，浏览器多半只看到"无法连接"而不是 `payload_too_large`；同时发起的第二个上传不会立即得到 409，而是排在第一个后面，等第一个结束（进入待确认）、自己的请求体被读完丢弃后才得到 409 `busy`；`stop()` 要等正在进行的上传结束（客户端不再发送时最多两次 5 s 接收超时）。模拟器每个连接一个线程，以上情况都会立即应答。

## 串口控制台

- 连接 spec 8.2：命令解析在可移植层（`wisp/app/console.hpp`：`split_command()`、`run_command()`），读输入在平台层（`Platform::read_line`：最多等待 `timeout_ms`，返回 `Line`/`Timeout`/`Closed`）。控制台跑在自己的 `wisp.console` 任务上（可能阻塞几秒的命令不占用 UI 线程），`App::start()` 最后启动、`App::stop()` 最先停止。
- 命令：`help`；`wifi status`（`state=… ssid="…" ip=… rssi=…`，Backoff 时加 `reason=…`；之后每个已保存网络一行 `saved ssid="…"`，密码被拒的加 ` auth_failed`；热点开着时一行 `ap ssid="…" clients=n`）；`wifi add <ssid> [password]`；`wifi forget <ssid>`；`wifi ap on|off`。含空格的参数用双引号，引号内 `\"`、`\\` 转义；`hex:<字节>` 按十六进制字节给出 SSID（对应 API 的 `ssid_hex`）。输出的 SSID 一律加双引号，控制字符和非法 UTF-8 字节写成 `\xNN`。
- 保密：控制台不回显输入、不把输入行写进日志（输出走 `emit_line`，不经 `WLOG`），任何输出都不含 Wi-Fi 密码、热点密码、配对码、令牌或 API Key（唯一例外：模拟器调试构建的 `debug pair-code` 打印当前配对码，见上文机器可读输出）；`wifi ap on` 只打印热点 SSID 和地址，密码只显示在屏幕上。终端程序自己的本地回显不受设备控制。
- 平台扩展：`Platform::console_commands` 追加平台专用命令（出现在 `help` 末尾，不能覆盖内置命令），例如模拟器专用的调试命令 `debug pair-code`。
- 真机：`wisp::port::esp::EspConsole`（`components/wisp_port_esp/console_esp.cpp`）安装 USB-Serial-JTAG（或 UART）驱动并让 VFS 走驱动，日志输出照常；`read_line` 用驱动的带超时读取，不忙等。
- `--screenshot` 的截图（`UiRuntime::take_snapshot`）包含活动屏幕和 `lv_layer_top()`（确认框、配网卡片、配对卡片都在这一层），不含 `lv_layer_sys()`。
- 固件上传：`wisp::port::sim::SimFirmwareUpdater` 把上传的镜像写到数据目录的 `firmware.bin`，目标板名为 `simulator`、不限芯片，容量按 5 MB 计；确认后照常重启（`WISP_REBOOT`），但模拟器不能运行 ESP 镜像，重启后仍是原来的版本（网页会显示"已回滚"）。版本号来自 `cmake/wisp_version.cmake`（与固件同一规则，见 `docs/porting-guide.md`）。
- 网页资源：构建时用 `tools/pack_assets.py` 把 `web/` 打包成 `wisp_sim` 旁边的 `assets.wpak`（`web/` 不存在时打包一个空目录，只提供 API）；`--web-dir <目录>` 改为启动时在内存里按同一套 MIME/gzip 规则打包该目录（`wisp::port::sim::pack_web_dir()`）。
- 矢量字体镜像（矢量字体 spec 3.3）：构建时用 `tools/pack_font_image.py` 把 `components/wisp_ui/fonts/noto_sans_cjk_sc_subset.ttf` 打包成 `wisp_sim` 旁边的 `fonts.bin`，启动时整个读进内存交给 `Platform::font_image`；`--font-image <路径>` 换一个文件，`--font-image none` 不加载（大号文字改用位图字）。真机由 `app_main` 映射 `fonts` 分区（`wisp::port::esp::map_font_partition()`）。两者都由 `UiRuntime::start()` 用 `ui::open_font_image()` 校验，不通过只降级。

## 存储逻辑区

`core::StorageManager`（`enum class Area { System, Library, Backup }`）：

| 逻辑区 | 根目录列表 | 配额 |
|---|---|---|
| `System` | `[<int>/system]` | — |
| `Library` | SD 在线：`[<sd>/wisp/library, <int>/library]`；否则：`[<int>/library]` | `<int>/library` 默认 3 MiB（`StorageConfig::internal_library_quota`），超出返回 `QuotaExceeded`；SD 卡那一层不限额 |
| `Backup` | SD 在线：`[<sd>/wisp/backup]`；否则：`[]`（不可用） | — |

写入永远进列表第一个根；读取按顺序枚举所有根。真机挂载点：内置 Flash（LittleFS）在 `/int`，SD 卡（FAT，1-bit SDMMC）在 `/sd`；模拟器对应 `<data-dir>/int/`、`<data-dir>/sd/`。原子写（`write_atomic`）和 FAT 上 `rename` 不能覆盖已存在文件的坑，见 `AGENTS.md` 第 8 节。

场景的代码与数据不属于以上三个逻辑区，固定在内置根下的 `<int>/scenes`（`core::SceneRepository`，开发模式见上面"模拟器的网络"一节的 `--scenes-dir`）：每个场景是 `<int>/scenes/<id>/`，数据在其中的 `data/` 子目录，`wisp.files` 的文件在 `files/` 子目录（第一次写时按 `scene.json` 的 `files` 声明可能改放 `<sd>/wisp/library/scenes/<id>/files`，位置记在 `data/files_loc.json`，之后不变；见下面"场景运行时"一节的 `SceneFiles`）。替换一个场景是目录级的两阶段改名，不经单文件的 `write_atomic`，见下面"场景运行时"一节。

## 场景运行时

场景 spec 3/6；任务见上面"启动流程"末尾的"场景相关的任务"表，启动/停止顺序见同一节正文。

### 后台场景、内存预算与开机恢复

场景设备 spec 3；实现在 `scene::SceneRuntime`（`components/wisp_scene/include/wisp/scene/scene_runtime.hpp` 的文件头有完整的生命周期说明）。

- 槽位：所有场景共用 `wisp.scene` 一个任务，最多一个前台、三个后台（`SceneRuntimeConfig::max_background`）。每个槽位有自己的实例号、VM、定时器、分钟节拍和 `SceneStore`；场景在前后台之间移动时实例号不变。每条消息仍有自己的 200 ms 预算，后台场景拖不住前台。
- 转换：`scene.json` 声明 `"background": true` 的场景，返回手势、`close()`、`wisp.scene.close()`，以及在它前台时打开另一个场景，都让它转入后台（`on_hide` → 释放它持有的唤醒锁 → 撤下视图、回到原屏幕 → `scene.on_state("background")`），VM、存储、定时器和声明树都保留，界面变化只更新声明树、不发给 UI 线程；没有声明的场景照旧关闭。再次打开（列表、面板里的行、点通知）时从声明树重建视图（`SceneInstance::rebuild()`），然后 `on_show`、`scene.on_state("foreground")`。`exit()`、面板里的 ×、`wisp.scene.exit()`、管理后台的 `POST /scenes/{id}/exit` 总是彻底关闭。`scene.state` 事件对后台发 `background`，对前台发 `running`。
- 定时器：声明了 `background` 的场景（不论在前台还是后台）灭屏时定时器和分钟节拍照常运行；其余前台场景灭屏时暂停、亮屏时每个错过的定时器补一次。
- 后台不能用的 API（spec 3.4）：`wisp.sensor.subscribe`/`on`、`wisp.power.keep_screen_on` 返回 `nil, "not_in_background"`；已有的传感器订阅在转入后台时暂停、回到前台时恢复。
- 内存预算与淘汰（spec 3.5）：所有 VM 从同一个 `VmPool` 分配，总预算 `scene::kVmBudgetBytes`（3 MB），单个 VM 仍以 2 MB 为上限，也是前台保证能拿到的量；超过任一个的分配按内存错误失败。场景打开、切回前台、转入后台时，以及前台有场景时每条消息之前，按"最久未在前台"的顺序挑出要关闭的后台场景：先是超过数量的，再是让后台总量超过 `预算 - 2 MB` 的。被淘汰的场景照常 `on_close` 并刷盘，日志 `<id>: evicted (...)`，并发一条系统通知。
- 开机恢复（spec 3.6）：后台场景的 id（最久未在前台的在前，加上还在加载的开机启动）每次变化时写入 System 区的 `scenes/background.json`（`{"ids": [...]}`，`write_atomic`，在 `wisp.fs` 上）；只有 `restore_background()` 读过它以后才写，`stop()` 关闭的场景不从列表里移除，下次开机照样恢复。`system.boot.completed` 之后逐个以 `launch(Boot)` 在后台启动：建 VM、执行入口文件、建声明树，不建视图、不调 `on_show`，`wisp.scene.launch()` 为 `"boot"`。启动失败的场景跳过、记日志、发系统通知，并从列表中去掉。
- 点通知（spec 5.1）：`SceneRuntime::deliver_notification()`——前台场景留在前台，后台场景切回前台，没在运行的场景以 `launch(Notification)` 打开（`wisp.scene.launch()` 为 `"notification"`）；场景到了前台后调用 `scene.on_notification(id)`。
- 数据迁移（spec 7.3）：数据版本低于清单的 `schema_version` 时，入口文件执行完、建界面之前调用 `scene.migrate(old_version)`，时限 2 秒（`scene::kMigrateMs`）；入口文件和迁移期间对 `wisp.store` 的写入放在同一个批次里，成功时连同新版本号一起提交，出错或超时整批丢弃、数据不动，错误信息以 `migrate failed: ` 开头。数据版本高于清单时拒绝打开（`data version <n> newer than scene <m>`）。

### 消息流

- `wisp.scene` 任务一次只取一条 `SceneMessage`（正文在 PSRAM）跑进 Lua VM，每条有自己的 200 ms 预算；处理完就把场景的被动状态（reactive）刷新一遍，将这次变化整理成一个 batch 交给 `PatchQueue`。
- 消息来自三处：UI 线程（部件事件）、`wisp.fs`（包与存储加载完成的回执、`wisp.files` 作业的结果 `FilesDone`）、`SceneHost` 的请求（打开/关闭/删除，实例号为 0，按 id 认场景）。每条消息都带着实例号，不是前台场景当前实例号的消息被丢弃（`stale_dropped()`），已关闭场景的消息因此不会影响下一个打开的场景。
- 消息队列容量 64（`SceneRuntimeConfig::queue_capacity`）。UI 线程的帧钩子每帧从 `PatchQueue` 最多应用 64 条操作（`PatchQueue` 的 `per_frame`）到对应实例的 `SceneView`；一次大批量更新摊到多帧应用完时，中间那几帧会显示场景从未真正处于过的半更新画面，下一帧补齐。

### `/int/scenes/` 布局与两阶段替换

`core::SceneRepository`（`components/wisp_core/include/wisp/core/scene_repository.hpp`）：

- 布局：`<code_root>/<id>/` 下是 `scene.json`、入口文件、`lib/**/*.lua` 和 `assets/**`；场景的数据在 `data_dir(id) = <data_root>/<id>/data`，文件在 `files_dir(id) = <data_root>/<id>/files`（与 `data/` 同等对待：不属于包，`load()`、导出和 `code_bytes` 都不含它，替换时随 `data/` 一起搬，`remove()` 不带数据时保留，带数据时连同 SD 卡上的 `<sd>/wisp/library/scenes/<id>` 一起删除）。真机和非开发模式的模拟器两个根都是 `<int>/scenes`，数据目录落在包目录里面；模拟器开发模式（`--scenes-dir`）下代码来自工作目录，数据另放一处。
- 替换（安装/升级）按 AGENTS.md 第 8 节"两阶段命名"的目录版做到跨断电原子：
  1. 新文件写进 `<code_root>/.<id>.tmpdir/`；
  2. 该目录改名为 `.<id>.newdir`——从这一步起视为完整；
  3. 若旧的 `<id>/data`、`<id>/files` 存在，把它们移进 `.<id>.newdir`（任一个两边都已存在时放弃整个替换，不删任何一边）；
  4. 删除 `<id>`；
  5. `.<id>.newdir` 改名为 `<id>`。
  `recover()`（启动时在 `wisp.fs` 上先跑一次，见"启动流程"第 3 步）丢弃每一个 `.*.tmpdir`，并把每个 `.<id>.newdir` 走完第 3–5 步。staging 名字以 `dir` 结尾，是因为 `StorageManager` 把 `.tmp`/`.new` 结尾的名字留给了它自己的单文件原子写。

### `SceneFiles`：场景的私有文件

`core::SceneFiles`（`components/wisp_core/include/wisp/core/scene_files.hpp`）是 `wisp.files` 与 `wisp.http.download` 背后的服务（场景设备 spec 7.1/7.2；Lua 绑定 `bind_files.cpp`、`bind_http.cpp`）。

- 位置：第一次写（`write`/`append`/下载）时按 `scene.json` 的 `files` 声明（`SceneManifest::files`，经 `SceneFilesConfig::declared` 由 `SceneRepository::manifest()` 提供）决定：`"auto"`（默认）SD 卡在线就用 `<sd>/wisp/library/scenes/<id>/files`（配额 64 MB），否则 `<int>/scenes/<id>/files`（配额 512 KB）；`"sd"` 只用 SD 卡，没卡时返回 `unavailable`，不退回内部存储；`"int"` 总是内部存储。记录 `data/files_loc.json`（`{"location":"sd"|"int"}`，`write_atomic`），之后不变：声明后来改了，以记录为准、文件不搬，加载记录时记一条警告。位置在 SD 卡（或声明 `"sd"` 而还没选定）而卡不在时所有调用返回 `unavailable`。`info()` 同时返回 `location` 与声明 `want`。
- 系统从不自动删除场景的文件：只有场景自己的 `remove`、管理后台和带数据删除场景（`SceneRepository::remove(id, true)` 加 `SceneFiles::remove_all()`）会删。卡不在时带数据删除场景，卡上的 `files/` 留下并记一条日志；`scan()`、`on_sd_changed()` 遇到卡上没有已安装场景的 `scenes/<id>/files` 也只记日志。同 id 重新安装时沿用留下的文件：没有位置记录、声明允许 SD、卡在且卡上已有 `scenes/<id>/files` 时，第一次任何调用（包括只读的）就采用卡上的位置并写下记录。设备上带数据删除失败时，只有场景已不再安装（`scene.json` 已删）才调用 `remove_all()`。
- `list()` 最多返回 `max_list`（256）个名字（按名字排序的前 256 个），被截断时 `SceneFileList::truncated` 为真，Lua 回调为 `cb(err, list, truncated)`；目录用 `StorageManager::visit_dir` 逐项遍历，只保留前 256 个，查找单个文件也逐项遍历，不在内部 RAM 里攒整个目录（启动时的 `count_dir` 仍用 `list_dir`，因为它边遍历边删改）。SD 卡是 FAT，名字不分大小写：在卡上没有完全相同的名字时按不分大小写匹配，写、追加、读、删、下载（含进行中的重名检查）和统计都作用于卡上已有的拼写；保留后缀 `.tmp`/`.new` 也不分大小写拒绝。
- 文件名 `[A-Za-z0-9._-]`、1–64 字节、不以 `.` 开头，另外不能以 `.tmp`/`.new` 结尾（`StorageManager` 自己的原子写簿记名）；单次 `write`/`append` 最多 256 KB、`read` 最多 64 KB（`too_large`）。`write` 走 `write_atomic`，`append` 走 `StorageManager::append()`。
- 用量开机统计一次（`scan()`），之后每次写入增量更新；写入前检查配额。进行中的下载按已收到的字节预留配额。
- 下载用文件级的两阶段命名：响应体追加到 `.<name>.dl`（可能写了一半，视为垃圾），完成后改名 `.<name>.done`（此时一定完整）→ 删除旧的 `<name>` → 改名为 `<name>`。统计目录时删掉不属于进行中下载的 `.dl`，并完成每个 `.done`（旁边还有 `<name>` 文件时 `.done` 更新，先删 `<name>`；`<name>` 是目录时两者都不动，只记日志，下载收尾时也一样）。HTTP 客户端在响应体之前用 `HttpClientRequest::on_status` 报告状态码，`SceneHttp` 只把 2xx 的响应体交给下载；非 2xx 的响应体读掉丢弃（最多普通响应上限那么多，超过就停止传输），不占配额，回调得到 `err=nil`、`size=0`，`.dl` 被删、原文件不动。下载的 URL 在开始下载（选定位置）之前就在场景任务上按 `http_hosts` 检查，被拒的下载不会定下位置。
- 线程：每个方法都碰 flash，只在 `wisp.fs`（或允许访问 flash 的 httpd 线程）上调用。`FileDownload::push()` 在 `wisp.shttp` 上运行：只把数据拷进 32 KB 的 PSRAM 分块、把追加排到 `wisp.fs`，排队超过 128 KB 时等待；`cancel()`（场景关闭或出错时运行时调用，先于 `SceneHttp::cancel_scene`）唤醒它，之后在 `wisp.fs` 上 `finish(false)` 删除 `.dl`。

### `SceneStore` 文件格式与日志

`core::SceneStore`（`components/wisp_core/include/wisp/core/scene_store.hpp`）是 `wisp.store` 背后的持久化：一个键值空间加最多 16 个 JSON 记录集合（spec 5.4/5.6；Lua 侧 API 见 `docs/scene-api.md`"wisp.store"一节）。

- 索引全在内存，读写立即返回；按追加日志异步落盘：键值空间写 `data/kv.log`，每个集合写 `data/<集合名>.log`。每行一个 JSON 对象，`{"op":"put","id":<id>,"v":<value>}` 或 `{"op":"del","id":<id>}`（键值空间的 id 是键字符串本身，集合的 id 是整数），以 `\n` 结尾；加载时最后一个 `\n` 之后的半行视为断电撕裂的尾巴，丢弃并重写该文件（`write_atomic`），否则下次追加会接在撕裂的字节后面。
- 第一个挂起的写安排一次性的合并延时；到点后把各文件挂起的内容一起交给 `wisp.fs`（真机上的 `submit_io`）落盘。一次落盘涉及多个文件、或一批提交里有超过一行时，先把要写的全部行写进 `data/journal.json`（`write_atomic`），落盘完成再删除——重开时先重放还没删除的 `journal.json`（put/del 都是幂等的），追加这些行后再删除它。落盘失败按 200 ms 起翻倍、上限 5 s 重试。
- 日志按"死字节超过活字节一半"或"累计追加超过阈值"触发压缩重写（只留活记录，仍用 `write_atomic`）；压缩后的集合为已删除的最大 id 留一条 `{"op":"del"}`，保证重开后 id 分配仍然单调。

### 开发循环

- 模拟器 `--scenes-dir`（见上面"模拟器的网络"）：代码直接从本机目录读取，前台场景一改文件就重新加载，不必安装/卸载，迭代最快，但只能在模拟器里用。
- `tools/wisp_scene.py`（仅标准库）面向真机或非开发模式的模拟器，经已配对的管理后台 API 推送/管理场景（连接 spec 6.1、6.3；场景 spec 7.3）：
  - `push <目录> [--open] [--watch]`：把目录打成只含 `scene.json`、入口文件、`lib/**/*.lua`、`assets/**` 的 zip 装到设备上（`data/` 一律跳过，那是设备自己的存储）；`--watch` 每 0.5 s 比较一次文件快照，改动静止 300 ms 后自动再推一次，设备上在运行的场景据此重新加载。
  - `ls`：列出已装场景；`logs <id>`：订阅管理后台 WebSocket 的 `log`，只打印标签为 `scene.<id>` 的行。
  - 首次访问一台设备会走配对流程（配对码要在设备屏幕上读出来输进去）；令牌存在 `<配置目录>/scene_tool.json`（默认 `$XDG_CONFIG_HOME/wisp` 或 `~/.config/wisp`），权限 0600，原子替换写入；收到 401 说明设备忘记了这个令牌（撤销或恢复出厂），重新配对一次即可。
  - 每次请求的超时由环境变量 `WISP_SCENE_TIMEOUT_S` 覆盖（默认 30 s；日志跟随流不设超时，设备安静多久都等）。

## 相对 spec 的细化（实施前已核实，经用户确认）

1. 新增两个可移植组件：`wisp_base`（`Result`、`ErrorCode`、`Value`/`Properties`、JSON 互转，位于 os/hal 之下）和 `wisp_app`（启动编排 + 自检界面，位于 ui 之上）。spec 的 `wisp_ui` 只保留 LVGL 运行时。
2. 触摸芯片实际为 **CST9217**（MyOS 代码与 Waveshare 官方 BSP 均如此，MyOS 硬件文档写的 CST9220 有误），驱动用 `waveshare/esp_lcd_touch_cst9217`。
3. SD 卡按 Waveshare 官方 BSP 走 **1-bit SDMMC**（CLK=GPIO2、CMD=GPIO1、D0=GPIO3），不是 spec 写的 SPI。
4. 屏幕、触摸、音频编解码器、SD 卡由 esp_board_manager 的设备初始化（YAML 参照 ESP-Claw 中已验证的同款板定义，Apache-2.0，需标注来源）；IMU、PMU、RTC、按键、内置 LittleFS 由 Wisp 自己的驱动初始化，挂在 bmgr 提供的 I2C 总线上。
5. `hal::Button` 只提供电平 `pressed()`；短按/长按由可移植的 `ButtonService` 统一判定（长按阈值 1000 ms）。
6. 版本锁定：ESP-IDF **v6.0.3**，LVGL **9.5.0**。
7. 字体：正文（16 px 及以下）是位图字——`lv_font_montserrat_16` 回退到 `lv_font_cjk_16`（16 px、4bpp；GB2312 ∪ 日文假名与符号 ∪ JIS X 0208 第一水准汉字，由 `tools/gen_cjk_font.py` 从上游 Noto Sans CJK SC 生成，见连接 spec 8.3 与 `docs/references.md`）。`UiRuntime::default_font()` 返回的是这两个字体外面再包一层 8 位字形缓存（`components/wisp_ui/src/a8_font.cpp`，`Mem::Large`，上限 512 KB），字形第一次画时展开，之后走 LVGL 的静态 A8 位图路径，画出来与原字体逐像素相同。20 px 起是矢量字：`ui::font(px)` 把请求的像素数取不大于它的最大档位（20、24、28、32、40、48、64、80、96、128），用 `fonts` 分区镜像里的 Noto Sans CJK SC 矢量子集经 LVGL 的 Tiny TTF（stb_truetype）渲染，字形缓存在 PSRAM；回退链是矢量字 → 图标字（`lv_font_wisp_icons_{24,32,48}`，取不大于档位的最大一档，20 档没有）→ 正文字，矢量字体没有的 `LV_SYMBOL_*` 图标和字符集外的字都落到这条链上（矢量字体 spec 4.1、4.2）。字体镜像缺失、损坏或某档创建失败时只降级为"图标字 → 正文字"，`UiRuntime::start()` 照常成功、不阻止启动（矢量字体 spec 3.6）。矢量档位的行高与基线按 `line_height = ceil(0.28·px) + ceil(0.99·px)`、`base_line = ceil(0.28·px) − 1` 改写（而不是 Tiny TTF 按 `hhea` 算出的、明显偏高的行高），让字符集里墨迹最高最低的字形都落在按标签文字区裁剪的行框内（矢量字体 spec 4.3）。
8. 模拟器截图用自带的极简 PNG 编码器（未压缩 deflate），不引入第三方图像库。
9. 新增性能监视（spec 未要求）：`UiRuntime` 自行统计 FPS、UI 线程占用率、平均刷新耗时；自检界面显示，每 5 s 输出一行 `WISP_PERF <json>`；可选开启 LVGL 自带的 sysmon 浮层。作为后续场景运行时的性能回归基准。盖在内容上面的不透明卡片用 `ui::mark_opaque_overlay()`（`wisp/ui/overlay.hpp`）登记，目前是 `lv_layer_top()` 上的配网卡片和配对卡片（`net_cards.cpp` 的 `make_card()`；确认框是 `lv_msgbox`，没有登记）：下面的内容滚动时只重画卡片外的部分，卡片本身的变化照常重画。卡片或它的任一上层被隐藏、不再完全不透明（透明度、混合模式、遮罩、变换），或失效区碰到顶层/系统层上别的对象时，退回整块重画；卡片停止遮挡后的下一次刷新会把它上一帧的区域整块重画；LVGL 渲染期间不做任何裁剪。渲染加速按芯片选择，见 `docs/porting-guide.md`。
10. 屏幕形状与安全区（执行期由用户批准加入，spec 5.6 节）：Display caps 的 `shape` 取 `rect`/`rounded_rect`/`circle`（原 `rounded_square` 改名为 `rounded_rect`），`wisp_hal` 提供 `visible`/`safe_rect`/`row_span` 几何函数，能力清单为 Display 条目附带 `safe_area`，自检界面与模拟器遮罩都以此为准。
11. 熄屏省电（执行期由用户批准加入，spec 6.7 节、验收第 5 条）：新增 `PowerManager`，熄屏进入待机，此时屏幕和触摸进入睡眠、UI 暂停渲染、按键放慢轮询，真机开启 DFS 和待机自动浅睡眠；同时支持闲置自动熄屏。后台场景和唤醒锁留到子项目 3（spec 1.6）。
12. 场景运行时为不透明覆盖物和返回手势加了两条规则（执行期裁定，场景 spec 3.5）：
    - `wisp::ui::overlay` 的标记加了 `OwnChanges::InOwnEvents`：给内容可能铺满全屏、但只在自己收到的事件里变化的覆盖物用，目前是屏幕键盘和拼音候选字行（`system_layer.cpp` 的 `build_keyboard()`）——它们处理自己的事件时，落在其内的失效区仍按遮挡处理；其余任何时候、任何失效区只要碰到它们，照旧退回整块重画。错误面板沿用原有的默认规则（`OwnChanges::InsideBox`）标记；确认框（`SystemLayer::open_confirm()` 建的 `lv_msgbox`，带 LVGL 自带的全屏背板）不标记，不做跳过：它开着时背板所占的失效区本身就会碰到它，自然退回整块重画，不需要额外规则。`SelfCheck::raise_overlays()` 每次重新同步覆盖物（语言切换、Wi-Fi/配对状态变化）都先 `raise()` 配网/配对卡片，再 `raise()` `SystemLayer`（错误面板、键盘、候选行、返回箭头、确认框），让它们始终盖在卡片之上。
    - 返回手势不建常驻对象：`SystemLayer::hook_pointer()` 包一层输入设备的读回调，被动旁听每次读数据（不吞事件），只有起点在视觉上可见左边缘 16 px 内、向右移动超过 24 px 的触摸才会接管，不限角度（与安卓边缘返回一致，上下移动多少都不影响，只看向右的距离）；接管前的触摸正常落到场景上，松手前把手指拖回触发距离以内则取消，不触发返回。滑动越过 8 px（`kBackArrowSlop`，避免沿边缘滚动时箭头闪现）后，左边缘出现一个跟着手指移动、未标记的返回箭头，它隐藏后的下一帧照常整块重画被它盖过的区域。
13. 设备主题（执行期由用户批准加入）：`wisp/ui/theme.hpp` 是设备上颜色、圆角、间距的唯一来源，取值来自网页控制台的深色 token（`web/styles/tokens.css`），控制台里的半透明 token（边框、状态色的柔和底色）在这里预先混合到 `surface` 上，成为不透明色。设备只有深色一套：深色背景适合自发光屏，也是品牌的基调。颜色 token 在不依赖 LVGL 的 `wisp/ui/theme_tokens.hpp` 里，`theme.hpp` 补上依赖 LVGL 的部分。`UiRuntime::start()` 在 LVGL 默认主题（深色）之上叠一层 Wisp 主题，原生控件不用逐个设样式就是这套外观；按钮按 `ui::ButtonRole`（Primary/Secondary/Danger）上色，对话框的确认键是 Primary，销毁数据的确认（恢复出厂、删除场景）是 Danger，取消是 Secondary。场景的颜色 token（场景运行时 spec 4.4）名字不变，取值映射到同一套颜色。
