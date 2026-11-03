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
│  wisp_admin   管理后台路由 · 请求过滤 · 配对接口         │  ← 可移植
│  wisp_net     HTTP 接口类型 · WispPack 资源包            │  ← 可移植
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

真机（`firmware/main/main.cpp`）和模拟器（`simulator/main.cpp`）共用同一套 `wisp::app::App::start()`（`components/wisp_app/src/app.cpp`），区别只在各自提供的 `Platform`（日志 sink、设置后端、`emit_line`、`request_power_off`、`heap_info`、`set_standby`）：

1. **日志初始化**：`wisp::log::init()`，建立 32 KB 的内存环形缓冲（真机落在 PSRAM，模拟器为普通内存）。
2. **板级初始化**：调用 `Platform::board->init()`（真机对应 `boards/<板>/board.cpp`，模拟器对应 `wisp::port::sim::SimBoard`），构造各设备并注册进 `DeviceRegistry`；`init()` 自己只记录非 `Ok` 的 `DeviceReport`（缺失/失败），`App::start()` 随后为注册表里没有报告过的设备补一条 `Ok`。
3. **核心服务**：启动 `EventBus`，调用 `StorageManager::init()`（挂载失败时把启动报告的 `storage` 备注设为 `"readonly"`，降级继续而不中止），启动一个 2000 ms 周期的 `StorageManager::refresh()` 定时器，启动 `ButtonService`。
4. **UI 与自检界面**：若 `display.main` 已注册，把 `display` 这个 Settings 命名空间的 `brightness`（默认 200）应用到它，基于它（和 `touch.main`，如果也在）启动 `UiRuntime`；否则在启动报告里记一条 `"mode": "headless"`。无论哪种情况都查找 `power.main` 设备、构造 `core::PowerMonitor` 并 `start()`它自己的采样定时器（spec 6.3/6.7，发布下表的 `power.charging.changed`/`power.battery.low`；缺少 `power.main` 时完全惰性），再构造 `PowerManager`（spec 6.7，持有指向 `PowerMonitor` 的非持有指针）并启动它的周期性闲置检查定时器；如果启动了 `UiRuntime`，再在其上构建自检界面（`self_check_screen.cpp`，spec 8 节），阻塞到它真正跑完一次。
5. **收尾**：记录 `boot_ms`，依次输出 `"WISP_BOOT_REPORT <json>"`、`"WISP_MANIFEST <json>"`（通过 `Platform::emit_line`），再发布 `system.boot.completed` 事件。

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

`power.charging.changed`/`power.battery.low` 的字段名跟随 `wisp::hal::PowerStatus` 本身的成员名（`charging`/`usb_present`/`battery_percent`），不是 "vbus_present" 这个措辞——后者出自计划中驱动结构体 `axp2101_status_t` 的字段名（`drivers/axp2101/include/axp2101.h`），不是 spec 原文；spec 本身没有提到这个字段名，沿用 hal 已有的字段名更一致。

事件总线本身（`wisp_core/event_bus.hpp`）：前缀订阅（空串匹配所有主题）、按发布顺序派发、单个分发任务、队列默认容量 64（满时丢弃最旧并计数，见 `dropped()`）、处理函数里可以安全地再 `publish`/`subscribe`/`unsubscribe`。

## 机器可读输出

三条都是"前缀 + 单行 JSON"（AGENTS.md 第 4 节）：改动下面任一字段集合时，要同步更新 `tests/smoke/sim_smoke.py` 和本节。

- **`WISP_BOOT_REPORT <json>`**（启动流程第 5 步；`core::BootReport::to_value()`）：
  ```
  {"board": string, "boot_ms": int, "fw": string,
   "devices": [{"id": string, "class": string, "state": "ok"|"degraded"|"missing"|"failed", "reason": string}, ...],
   "notes": {...}}
  ```
  `fw` 是 `Platform::firmware_version`（真机：`esp_app_get_description()->version`，即 IDF 的 `PROJECT_VER`；模拟器：常量 `"0.1.0"`）。`notes` 是自由格式的诊断备注，目前用到的键：`mode`（值 `"headless"`，无 `display.main` 时）、`storage`（值 `"readonly"`，内置 Flash 挂载失败时）、`library_roots`（`core::StorageManager::roots(Area::Library)` 的字符串数组，spec 9.2 的"`--no-sd` 时 Library 只有内置根"就是靠这个字段机器可读地核实的）。
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
7. 字体：`lv_font_montserrat_16` 为主字体，回退到从 MyOS 移植的 `lv_font_cjk_16`（GB2312 全集，4bpp）。
8. 模拟器截图用自带的极简 PNG 编码器（未压缩 deflate），不引入第三方图像库。
9. 新增性能监视（spec 未要求）：`UiRuntime` 自行统计 FPS、UI 线程占用率、平均刷新耗时；自检界面显示，每 5 s 输出一行 `WISP_PERF <json>`；可选开启 LVGL 自带的 sysmon 浮层。作为后续场景运行时的性能回归基准。
10. 屏幕形状与安全区（执行期由用户批准加入，spec 5.6 节）：Display caps 的 `shape` 取 `rect`/`rounded_rect`/`circle`（原 `rounded_square` 改名为 `rounded_rect`），`wisp_hal` 提供 `visible`/`safe_rect`/`row_span` 几何函数，能力清单为 Display 条目附带 `safe_area`，自检界面与模拟器遮罩都以此为准。
11. 熄屏省电（执行期由用户批准加入，spec 6.7 节、验收第 5 条）：新增 `PowerManager`，熄屏进入待机，此时屏幕和触摸进入睡眠、UI 暂停渲染、按键放慢轮询，真机开启 DFS 和待机自动浅睡眠；同时支持闲置自动熄屏。后台场景和唤醒锁留到子项目 3（spec 1.6）。
