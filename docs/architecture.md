# 架构（子项目 1：基础骨架）

最高权威是 `docs/superpowers/specs/2026-11-01-foundation-design.md`；本文档是对当前代码的概述，两者冲突以 spec 为准，但本文档只记录已经落地的内容。

## 分层

```
┌─────────────────────────────────────────────────────┐
│  入口层   firmware/main (真机)   │  simulator/ (PC)   │
├─────────────────────────────────────────────────────┤
│  wisp_app     启动编排 · 自检界面 · PowerManager         │  ← 可移植
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

1. 可移植组件（`wisp_base`、`wisp_os`、`wisp_hal`、`wisp_core`、`wisp_net`、`wisp_admin`、`wisp_ui`、`wisp_app`）禁止 include 任何平台头文件：`esp_*`、`freertos/*`、`driver/*`、IDF 的 `hal/*`、`soc/*`、`rom/*`、`lwip/*`、`mbedtls/*`、`SDL*`、`curl/*`，以及 `sdkconfig.h`、`nvs.h`、`nvs_flash.h`、`sdmmc_cmd.h` 和 bmgr 生成的 `dev_*`/`gen_board_*`（文件名前缀，不带目录）；只能依赖 C++20 标准库（包括 POSIX 文件 API，如 `<unistd.h>`/`<sys/stat.h>`/`<dirent.h>` —— ESP-IDF 的 VFS 和宿主机都提供，`StorageManager` 依赖它遍历/删改文件）、LVGL、ArduinoJson，以及按层次自上而下依赖其他可移植组件。
2. 每个可移植组件只能 include 自己这一层或更低层的头文件（按本文件"分层"一节的顺序：`wisp_base < wisp_os < wisp_hal < wisp_core < wisp_net < wisp_admin < wisp_ui < wisp_app`），禁止反向依赖"更上层"。
3. 只有 `wisp_port_esp`、`wisp_port_sim`、`boards/*`、`drivers/*` 和入口层（`firmware/main`、`simulator/`）可以接触平台。
4. 每个 `components/wisp_{base..app}/CMakeLists.txt` 的 `wisp_component()` 调用，`IDF_REQUIRES`/`HOST_LIBS` 只能出现 ArduinoJson（`wisp_base`）和 LVGL（`wisp_ui`）这两个外部依赖——防止绕开 `#include` 检查，直接在 CMake 里给某个可移植组件挂一个平台或宿主机专属依赖。
5. 规则由 `tools/check_layering.py` 强制检查（`bash tools/ci.sh` 会跑它，先跑 `tools/check_layering_test.py` 自测），扫描每个可移植组件下的 `.hpp/.cpp/.h/.c`（`wisp_ui/fonts/` 下的移植字体数据除外）及其 `CMakeLists.txt`。

## 启动流程

真机（`firmware/main/main.cpp`）和模拟器（`simulator/main.cpp`）共用同一套 `wisp::app::App::start()`（`components/wisp_app/src/app.cpp`），区别只在各自提供的 `Platform`（日志 sink、设置后端、`emit_line`、`request_power_off`、`heap_info`、`set_standby`，以及子项目 2 新增的 `random`、`http_server`、`http_client`、`assets`、`http_port`（默认 8900）、`captive_server`、`http_always_on`、`reboot`、`read_line`、`firmware`（固件上传写入器 `core::FirmwareUpdater`，连接 spec 6.5；为空时不支持网页上传）、`firmware_version`）：

1. **日志初始化**：`wisp::log::init()`，建立 32 KB 的内存环形缓冲（真机落在 PSRAM，模拟器为普通内存）。每条记录是 16 字节头（`ts`、`seq`、长度、级别）加 tag 和消息，按整条淘汰；读取时只在锁内拷贝原始字节，解码在锁外；再次以相同大小和内存类型调用 `init()` 会保留已有记录；`seq` 与 `ts` 在同一把锁里分配，sink 收到的 `LogRecord` 与缓冲区里的完全一致（连接 spec 6.2）。
2. **板级初始化**：调用 `Platform::board->init()`（真机对应 `boards/<板>/board.cpp`，模拟器对应 `wisp::port::sim::SimBoard`），构造各设备并注册进 `DeviceRegistry`；`init()` 自己只记录非 `Ok` 的 `DeviceReport`（缺失/失败），`App::start()` 随后为注册表里没有报告过的设备补一条 `Ok`。
3. **核心服务**：启动 `EventBus`，调用 `StorageManager::init()`（挂载失败时把启动报告的 `storage` 备注设为 `"readonly"`，降级继续而不中止），启动一个 2000 ms 周期的 `StorageManager::refresh()` 定时器，启动 `ButtonService`，加载模型配置（`core::ModelProfileService::init()`，读 System 区的 `secrets/models.json`；只有旧的 `secrets/llm.json` 时把它迁移成一份对话配置）。
4. **UI 与自检界面**：若 `display.main` 已注册，把 `display` 这个 Settings 命名空间的 `brightness`（默认 200）应用到它，基于它（和 `touch.main`，如果也在）启动 `UiRuntime`；否则在启动报告里记一条 `"mode": "headless"`。无论哪种情况都查找 `power.main` 设备、构造 `core::PowerMonitor` 并 `start()`它自己的采样定时器（spec 6.3/6.7，发布下表的 `power.charging.changed`/`power.battery.low`；缺少 `power.main` 时完全惰性），再构造 `PowerManager`（spec 6.7，持有指向 `PowerMonitor` 的非持有指针）并启动它的周期性闲置检查定时器；如果启动了 `UiRuntime`，再在其上构建自检界面（`self_check_screen.cpp`，spec 8 节），阻塞到它真正跑完一次。
   **网络**（连接 spec 4.1，`components/wisp_app/src/net_wiring.{hpp,cpp}`，在 `PowerManager` 之后、自检界面之前）：有 `Platform::random` 时构造 `net::PairingService`；若 `net.wifi` 已注册，再构造 `net::WifiService` 并 `init()`（只读取已保存的网络并安排第一次尝试，不碰射频）。`WifiService::tick()` 与 `PairingService::tick()` 都在独立任务 `wisp.net` 里每 250 ms 跑一次——扫描可能阻塞约 3 s，不能放进共享的 `os::Timer` 分发任务，所以开机从不等待联网。`wisp.net` 任务每次循环把 `PowerManager::standby()` 交给 `WifiService::set_standby()`（`power.state` 只用来及时唤醒任务，事件丢了或迟到都不会弄错待机状态）。有 `Platform::http_server` 且有 `WifiService` 时构造 `admin::AdminServer`（含 `WsHub`；本次开机的标识 `boot_id` 在这里用 `Platform::random` 生成一次，见连接 spec 6.2）：STA 拿到 IP 或热点开启时由 `wisp.net` 任务启动 HTTP 服务、两者都断开时停止（接口状态每次循环都从 `WifiService::status()`/`ap_status()` 读取；`net.wifi.state`/`net.ap.state` 只用来及时唤醒任务，丢了事件也不会让服务停在错误状态）；`http_always_on`（模拟器）则开机即启动、断网也不停。另外，只要热点开着，`wisp.net` 任务就在 80 端口运行 `Platform::captive_server`（真机才有），处理函数是 `AdminServer::captive_handler()`：从热点接口来的任何请求都 `302`（其他接口返回 404）到 `http://<热点地址>:<http_port>/setup.html`，让手机的强制门户探测打开配网页；热点关闭时停止。启动失败记日志，并在接口仍然开着时退避重试（2 s 起翻倍，上限 60 s）。每次启动成功输出一行 `WISP_HTTP {"port": n}`。启动报告记录 `notes.net`。
   **固件上传服务**（连接 spec 6.5，在网络之前构造，因为后台路由要把上传流写进它）：`core::FirmwareUpdateService` 包着 `Platform::firmware`，没有 UI 时不设确认回调（上传在 `begin` 时就以 `unavailable` 拒绝，因为没有屏幕可以确认）；`wisp.net` 任务每次循环调用它的 `tick()`，未确认的镜像 2 分钟后丢弃。
5. **收尾**：记录 `boot_ms`，依次输出 `"WISP_BOOT_REPORT <json>"`、`"WISP_MANIFEST <json>"`（通过 `Platform::emit_line`），再发布 `system.boot.completed` 事件。
6. **串口控制台**：有 `Platform::read_line` 时启动 `wisp.console` 任务（见下文"串口控制台"）。
7. **确认镜像可用**：`FirmwareUpdater::mark_valid()`。真机上新装的镜像在这一步之前崩溃或复位，引导程序都会回滚到旧镜像（`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`，见 `docs/porting-guide.md`"固件与分区"）；模拟器什么都不做。

**停止顺序**（`App::stop()`）：串口控制台 → 自检界面 → 等待已确认的恢复出厂（`wisp.reset`）和固件安装（`wisp.fwup`）任务结束 → 网络（先 join `wisp.net` 任务，再停强制门户服务和 HTTP 服务——`SimHttpServer::stop()` 会在 join 连接线程时回调 `on_close`，所以调用时不持有任何锁——然后退订事件、销毁 `AdminServer`、`WifiService`、`PairingService`）→ 固件上传服务 → `PowerManager` → `PowerMonitor` → 存储定时器 → `ButtonService` → `UiRuntime` → `EventBus`。HTTP 处理函数会经 `SystemSettings` 调用 `PowerManager::set_brightness()`，所以网络必须先于 `PowerManager` 停止。

**失败处理**（spec 6.1）：设备初始化失败只记录、不注册，系统降级继续；`Display` 失败则不启动 UI，走 headless 路径；内置 Flash 挂载失败走只读降级。`WISP_BOOT_REPORT`、`WISP_MANIFEST`、`WISP_PERF` 都是"前缀 + 单行 JSON"（AGENTS.md 第 4 节）。

## 事件主题清单

当前代码里实际发布的主题（`grep -rn` 核实，不是 spec 草案）：

| 主题 | 发布方 | 说明 |
|---|---|---|
| `button.<id>.down` / `.up` / `.short` / `.long` | `core::ButtonService` | `<id>` 为按键设备 id（`button.power`、`button.vol_up`、`button.vol_down`），同时是 `Event::source` |
| `power.state` | `app::PowerManager` | `payload = {"state": "active"\|"standby"}` |
| `power.charging.changed` | `core::PowerMonitor` | `power.main` 设备缺失时不发布；`charging`/`usb_present` 任一变化时发布（首次采样不算"变化"，不发布），`payload = {"charging": bool, "usb_present": bool, "battery_percent": int}`；`Event::source` 为设备 id（`power.main`） |
| `power.battery.low` | `core::PowerMonitor` | `battery_percent`（已知，即不是 -1）降到 ≤ Settings `power`/`low_batt_pct`（默认 15）时发布一次；回升超过阈值 +5 才重新武装，`payload = {"battery_percent": int}`，`Event::source` 为设备 id |
| `storage.sd.inserted` / `storage.sd.removed` | `core::StorageManager` | SD 卡插拔检测 |
| `system.boot.completed` | `app::App` | 启动流程第 5 步收尾时发布 |
| `system.factory_reset` | `app::App`（`components/wisp_app/src/factory_reset.cpp`） | `payload = {"state": "requested"\|"cancelled"\|"done"}`。`POST /system/factory-reset` 发 `requested`，自检界面据此弹出确认框（请求同时记为 pending，自检界面在收到事件、建好界面时和每秒的重新同步里取走它，所以界面尚未订阅或事件被总线丢弃时请求也不会丢；同一时间只有一个确认框：重置框已开时重复请求被忽略，关机确认框开着时直接回 `cancelled`；反过来重置框开着时长按电源键不弹关机框）；点"否"发 `cancelled`；点"是"后在一次性任务 `wisp.reset` 上先发 `done`，最多等 500 ms 让事件总线和管理后台 WebSocket 发送线程把它送出，再清空 Settings、忘记全部 Wi-Fi、撤销全部配对浏览器、清除全部模型配置（`secrets/models.json`），最后 `Platform::reboot`；`wisp.reset` 任务创建失败时什么都不清除、发 `cancelled`，之后的请求可以重新弹框。Library/Backup 数据保留 |
| `system.firmware` | `app::App`（`core::FirmwareUpdateService`，`components/wisp_core/src/firmware_update.cpp`） | 连接 spec 6.5，`payload = {"state": "received"\|"confirmed"\|"rejected"\|"failed", "version": string, "error": string（仅 `failed`）}`，`version` 为上传镜像的版本。`POST /system/firmware` 把镜像流式写完并校验通过后发 `received`，同时记为 pending 并请自检界面弹出确认框（与恢复出厂同一套 pending/每秒重新同步机制；同一时间只有一个确认框，已有其他确认框时直接 `rejected`）；点"是"后在一次性任务 `wisp.fwup` 上把它设为启动镜像，成功则发 `confirmed`，等事件总线空闲并最多 500 ms 让后台 WebSocket 送出，再 `Platform::reboot`；点"否"或 2 分钟无人确认发 `rejected` 并丢弃镜像。上传开始后的失败发 `failed`，`error` 为 `wrong_board`、`invalid_image`、`payload_too_large`、`write_failed`，或连接中途断开时的 `aborted` |
| `system.settings` | `app::App`（`core::SystemSettings` 的 `on_changed` 回调） | 每批设置写入成功后发布（连接 spec 8.3），`payload = {"brightness": int, "volume": int, "screen_off_s": int, "language": "zh"\|"ja"\|"en"}`，即写入后的全部当前值（与 `GET /settings` 同形）；自检界面据 `language` 在 UI 线程上 `ui::set_lang()` 并重建文字 |
| `net.wifi.state` | `net::WifiService` | 状态变化时发布，`payload = {"state": "off"\|"connecting"\|"backoff"\|"connected", "ssid": string, "ssid_hex": string（SSID 原始字节的小写十六进制，见连接 spec 6.1）, "ip": string, "rssi": int, "reason": ""\|"not_found"\|"auth_failed"\|"other"}`；`reason` 只在 `backoff` 时非空；`Event::source` 为设备 id（`net.wifi`）。不含密码 |
| `net.ap.state` | `net::WifiService` | 配网热点开关时发布，`payload = {"active": bool, "ssid": string, "clients": int}`，不含热点密码 |
| `net.pairing.code` | `net::PairingService` | 发出配对码时 `{"active": true, "expires_ms": int}`（`os::now_ms()` 时间基准下的截止时刻），失效时（过期、配对成功、改发新码前，或在设备的配对卡片上点"拒绝"即 `PairingService::cancel_code()`）`{"active": false, "expires_ms": 0}`；不含配对码 |
| `net.pairing.failed` | `net::PairingService` | 配对码错误时 `{"attempts": int, "locked": bool}` |
| `model.profiles` | `core::ModelProfileService` | 模型配置增删改或切换当前配置时发布，`Event::source` 为 `"models"`，`payload = {"active": {"llm": string, "asr": string, "tts": string}, "count": int}`（`active` 为各类当前配置 id，没有时为空串；不含任何 API Key） |

`power.charging.changed`/`power.battery.low` 的字段名跟随 `wisp::hal::PowerStatus` 本身的成员名（`charging`/`usb_present`/`battery_percent`），不是 "vbus_present" 这个措辞——后者出自计划中驱动结构体 `axp2101_status_t` 的字段名（`drivers/axp2101/include/axp2101.h`），不是 spec 原文；spec 本身没有提到这个字段名，沿用 hal 已有的字段名更一致。

事件总线本身（`wisp_core/event_bus.hpp`）：前缀订阅（空串匹配所有主题）、按发布顺序派发、单个分发任务、队列默认容量 64（满时丢弃最旧并计数，见 `dropped()`）、处理函数里可以安全地再 `publish`/`subscribe`/`unsubscribe`。

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
   "heap_internal_free": int, "heap_large_free": int}
  ```
  `fps`/`flush_ms` 来自 `ui::UiRuntime::perf()`，本质是浮点数，但 JSON 往返时整数值会被识别为 `Int` 而非 `Double`（`Value`/JSON 对 Int/Double 的区分按实际值而非来源类型，见 `tests/base/value_json_test.cpp`）；解析方不应假设其中任何一个字段固定是哪一种 JSON 数字形式。`heap_internal_free`/`heap_large_free` 来自 `Platform::heap_info`（可能为空，此时都是 0）。

- **`WISP_HTTP <json>`**（网络步骤；`NetWiring::reconcile_http()`）：管理后台 HTTP 服务每次启动成功时一行，`{"port": int}`，是 `HttpServer::port()` 报告的实际端口。可能出现在 `WISP_BOOT_REPORT` 之前（`http_always_on` 时开机即启动）。模拟器 `--http-port 0` 时不启动服务，也就没有这一行；需要后台的冒烟测试自己挑一个空闲端口传进去（`admin_smoke.py` 在端口被抢走、日志出现 `admin server failed to start` 时换端口重试）。
- **`WISP_REBOOT`**（仅模拟器，无 JSON）：`Platform::reboot` 被调用（例如 `POST /api/v1/system/reboot`，或恢复出厂设置完成、固件更新确认后）时输出，随后主循环返回，经正常的 `App::stop()` 以退出码 0 结束进程（不在定时器线程里直接 `exit()`，以免静态析构与其他线程竞争）。真机对应 `esp_restart()`。
- **`pair-code=<6 位数字>`**（仅模拟器调试命令，无 JSON）：控制台命令 `debug pair-code` 的回答，当前没有有效配对码时为 `pair-code=none`。只在 `wisp_sim` 目标定义了 `WISP_SIM_DEBUG_COMMANDS` 时经 `Platform::console_commands` 编译进去（`simulator/main.cpp`），可移植层和固件都没有；供 `tests/smoke/admin_smoke.py` 不看屏幕完成配对。

## 模拟器的网络

- `net.wifi` 是 `wisp::port::sim::SimWifi`（model `sim-wifi`）：内置 `WispLab`（-45 dBm，密码 `wisp1234`）、`Neighbor`（-70，`neighbor99`）、`OpenCafe`（-80，开放）；`--wifi-fail` 让所有连接以 `not_found` 失败；热点只改状态，`ap_clients()` 在热点开启时恒为 1。
- 管理后台由 `SimHttpServer` 提供，只监听 `127.0.0.1`，端口 `--http-port`（默认 8900，与真机后台端口相同；0 表示不启动）。模拟器没有 80 端口的强制门户服务（`Platform::captive_server` 为空）。出站请求（模型连通测试 `POST /ai/profiles/{id}/test`）走 `SimHttpClient`（libcurl，主机构建 `find_package(CURL 7.80 REQUIRED)`；可移植层禁止 include `curl/`）。
- 触摸脚本：`--tap X,Y@MS`（按下 100 ms）和 `--swipe X1,Y1,X2,Y2@MS`（400 ms 内拖动，停 200 ms 后松开）在主循环开始后第 MS 毫秒向 `SimInput` 注入触摸，可重复使用，坐标 0–479。标准输入里以 `tap` / `swipe` 开头的行（`tap X,Y` / `swipe X1,Y1,X2,Y2`）也立即执行，供需要先等设备上某件事发生再点的脚本使用：冒烟测试 `factory_reset` 和 `firmware` 等到 WebSocket 日志里出现确认框的日志行，再经标准输入点"Yes"。
- 标准输入只有一个读取者（`simulator/sim_stdin.hpp` 的 `StdinReader`，带超时轮询，退出时不会卡在阻塞读上）：不论是否 `--headless`、stdin 是不是终端都会读取；触摸行交给触摸脚本，其余每行都是串口控制台（serial console）命令，经 `Platform::read_line` 交给控制台任务；控制台没有 `tap`/`swipe` 命令。`tap`/`swipe` 与坐标之间可以是空格或制表符。stdin 是控制终端且进程在后台进程组（`wisp_sim &`）时读取者暂不读，`fg` 后恢复；模拟器忽略 `SIGTTIN`，所以不会被作业控制停住。
- 信号：`SIGINT`/`SIGTERM` 让主循环像重启一样返回，经正常的 `App::stop()` 以退出码 0 结束（SDL 自己的信号处理被 `SDL_HINT_NO_SIGNAL_HANDLERS` 关掉，否则无头模式收不到）；处理是一次性的，关机卡住时再发一次信号按默认方式结束进程。

## 真机的网络

- `net.wifi` 是 `wisp::port::esp::EspWifi`（`components/wisp_port_esp/net/esp_wifi.cpp`），由板级 `init()` 最后注册，并在那里就打开射频（`Platform::random` 依赖它）。驱动配置只放 RAM（`WIFI_STORAGE_RAM`），已保存的网络只由 `WifiService` 管。`link()` 读取事件回调维护的快照；断开原因按 `wifi_reason.hpp` 映射，握手失败只在信号足够好（RSSI ≥ -75 dBm）时才算 `auth_failed`；断开事件没带 RSSI 时，用两分钟内最近一次扫描里同名网络的最强信号代替，都没有就不算。热点：AP+STA、WPA2-PSK、信道 1、最多 4 台，热点关着时 AP 接口留着一份隐藏、随机密码的 WPA2 占位配置，切到 AP+STA 的瞬间不会广播开放网络；同时在热点地址的 UDP 53 端口运行强制门户 DNS（`net/captive_dns.cpp`，所有 A 查询都答热点地址）。STA 第一次拿到 IP 时启动 mDNS：`wisp.local`，广播 `_http._tcp` 端口 8900。
- 管理后台是 `EspHttpServer`（`esp_http_server`）：端口 8900，最多 7 个连接（`lru_purge_enable`；每个请求都把打开的 WebSocket 标为最近使用，连接满时先关空闲的 keep-alive 连接），`/api/v1/ws` 的升级在握手前交给 `WsHandlers::on_open`（`CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT`），拒绝就是普通的 403；任务优先级 3，低于 UI。80 端口的强制门户是第二个实例（另一个控制端口、4 个连接、无 WebSocket），`AdminServer::captive_handler()` 只对热点接口的请求重定向，其余返回 404。请求所在接口按连接的本地地址和对端地址判断（`net_iface.hpp`），不看请求头。
- 出站请求走 `EspHttpClient`：HTTPS 用 IDF 证书包校验（含主机名），不跟随重定向，响应体上限 64 KB。
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

## 存储逻辑区

`core::StorageManager`（`enum class Area { System, Library, Backup }`）：

| 逻辑区 | 根目录列表 | 配额 |
|---|---|---|
| `System` | `[<int>/system]` | — |
| `Library` | SD 在线：`[<sd>/wisp/library, <int>/library]`；否则：`[<int>/library]` | `<int>/library` 默认 3 MiB（`StorageConfig::internal_library_quota`），超出返回 `QuotaExceeded`；SD 卡那一层不限额 |
| `Backup` | SD 在线：`[<sd>/wisp/backup]`；否则：`[]`（不可用） | — |

写入永远进列表第一个根；读取按顺序枚举所有根。真机挂载点：内置 Flash（LittleFS）在 `/int`，SD 卡（FAT，1-bit SDMMC）在 `/sd`；模拟器对应 `<data-dir>/int/`、`<data-dir>/sd/`。原子写（`write_atomic`）和 FAT 上 `rename` 不能覆盖已存在文件的坑，见 `AGENTS.md` 第 8 节。

## 相对 spec 的细化（实施前已核实，经用户确认）

1. 新增两个可移植组件：`wisp_base`（`Result`、`ErrorCode`、`Value`/`Properties`、JSON 互转，位于 os/hal 之下）和 `wisp_app`（启动编排 + 自检界面，位于 ui 之上）。spec 的 `wisp_ui` 只保留 LVGL 运行时。
2. 触摸芯片实际为 **CST9217**（MyOS 代码与 Waveshare 官方 BSP 均如此，MyOS 硬件文档写的 CST9220 有误），驱动用 `waveshare/esp_lcd_touch_cst9217`。
3. SD 卡按 Waveshare 官方 BSP 走 **1-bit SDMMC**（CLK=GPIO2、CMD=GPIO1、D0=GPIO3），不是 spec 写的 SPI。
4. 屏幕、触摸、音频编解码器、SD 卡由 esp_board_manager 的设备初始化（YAML 参照 ESP-Claw 中已验证的同款板定义，Apache-2.0，需标注来源）；IMU、PMU、RTC、按键、内置 LittleFS 由 Wisp 自己的驱动初始化，挂在 bmgr 提供的 I2C 总线上。
5. `hal::Button` 只提供电平 `pressed()`；短按/长按由可移植的 `ButtonService` 统一判定（长按阈值 1000 ms）。
6. 版本锁定：ESP-IDF **v6.0.3**，LVGL **9.5.0**。
7. 字体：`lv_font_montserrat_16` 为主字体，回退到 `lv_font_cjk_16`（16 px、4bpp；GB2312 ∪ 日文假名与符号 ∪ JIS X 0208 第一水准汉字，由 `tools/gen_cjk_font.py` 从上游 Noto Sans CJK SC 生成，见连接 spec 8.3 与 `docs/references.md`）。
8. 模拟器截图用自带的极简 PNG 编码器（未压缩 deflate），不引入第三方图像库。
9. 新增性能监视（spec 未要求）：`UiRuntime` 自行统计 FPS、UI 线程占用率、平均刷新耗时；自检界面显示，每 5 s 输出一行 `WISP_PERF <json>`；可选开启 LVGL 自带的 sysmon 浮层。作为后续场景运行时的性能回归基准。
10. 屏幕形状与安全区（执行期由用户批准加入，spec 5.6 节）：Display caps 的 `shape` 取 `rect`/`rounded_rect`/`circle`（原 `rounded_square` 改名为 `rounded_rect`），`wisp_hal` 提供 `visible`/`safe_rect`/`row_span` 几何函数，能力清单为 Display 条目附带 `safe_area`，自检界面与模拟器遮罩都以此为准。
11. 熄屏省电（执行期由用户批准加入，spec 6.7 节、验收第 5 条）：新增 `PowerManager`，熄屏进入待机，此时屏幕和触摸进入睡眠、UI 暂停渲染、按键放慢轮询，真机开启 DFS 和待机自动浅睡眠；同时支持闲置自动熄屏。后台场景和唤醒锁留到子项目 3（spec 1.6）。
