# Wisp 单一 flash 线程设计文档

- 日期：2026-11-06
- 状态：待用户审阅
- 范围：所有对内部 flash 和存储介质的读写都收到一个专门的线程（`wisp.fs`）上执行，其他线程一律异步请求；内部 RAM 只留给必须放在内部的东西和高性能路径。属于基础设施，插在 MVP 计划（`2026-11-06-mvp.md`）Task 3 与 Task 4 之间，在 `feat/agent-core` 上实施。
- 前置：子项目 1 基础骨架、子项目 2 连接与管理后台（`2026-11-03-connectivity-admin-design.md`，下文"连接 spec"）、3b-1 场景设备（`2026-11-05-scene-device-design.md`）
- 依据：2026-11-06 对 `feat/agent-core`（`5e3a5a4` 之后）的全仓排查，下文"排查"指它

---

## 1. 背景与目标

### 1.1 现状

- 已有存储线程 `wisp.fs`（`core::StorageWorker`，一条 32 项 FIFO 队列，栈 6 KB 在内部 RAM）。场景运行时、通知、场景文件、对话记录都经它读写。`wisp.scene`、`wisp.agent`、`wisp.imu`、`wisp.tone`、`wisp.shttp`、`wisp.ws`、`wisp.dns`、`wisp.ntp` 声明不访问 flash，栈可在 PSRAM。
- 排查到 13 处不在 `wisp.fs` 上的 flash 访问：

| 位置 | 线程 | 操作 |
|---|---|---|
| `model_profiles.cpp` | 管理后台 httpd | `write_atomic` models.json |
| `wifi_service.cpp` | httpd、`wisp.console`、`wisp.net` | `write_atomic` wifi.json |
| `pairing_service.cpp` | httpd、`wisp.reset` | `write_atomic` clients.json |
| `routes_scene_files.cpp` → `SceneFiles::read()` | httpd | `read_range`（下载） |
| `routes_firmware.cpp` → `esp_firmware.cpp` | httpd | `esp_ota_write/end`、设启动分区 |
| `routes_device.cpp` | httpd | `SystemSettings` 读写（NVS） |
| `net_wiring.cpp` | `wisp.net` | 读 `ntp_servers`（NVS） |
| `power_manager.cpp` | esp_timer 任务 | 读电源设置（NVS） |
| `self_check_screen.cpp` | UI 线程 | 读写 `ui/perf_overlay`（NVS） |
| `loopback.cpp` | `wisp.loop` | 读 `audio/volume`（NVS） |
| `factory_reset.cpp` | `wisp.reset` | `remove_tree`、清 Settings/Wi-Fi/配对 |
| `app.cpp` | esp_timer 任务 | 每 2 s `StorageManager::refresh()` |
| `firmware_confirm.cpp` | `wisp.fwup` | 标记固件有效（otadata） |

- 设备上 Settings 没有缓存，每次 `get` 都 `nvs_open` + 读 flash，每次 `set` 都立即 `nvs_commit`。
- 约束靠自觉：`TaskConfig::flash_access` 只是声明，模拟器完全忽略。声明错了只在真机上表现为 abort，且开了 XIP 的板子上大多测不出来（AGENTS.md 第 5 节）。
- 结果：管理后台 httpd（10 KB）、`wisp.net`、`wisp.console`、`wisp.reset`、`wisp.bus`、`wisp.loop`、`wisp.sample`、`wisp.fwup` 和 UI 线程的栈都只能放内部 RAM。

### 1.2 目标

1. 运行期间，只有 `wisp.fs` 读、写、擦除、映射内部 flash，以及读写存储介质上的文件（内部 LittleFS、SD 卡）。其他线程一律把请求交给它，自己不碰。
2. 规则由机制强制执行：放错线程的访问在模拟器的单元测试里就会失败，而不是等真机 abort。
3. 内部 RAM 只用于：DMA 缓冲、`wisp.fs` 的栈、ISR 与 IRAM 代码数据、ESP-IDF 自己的任务，以及明确声明的**高性能路径**（UI 线程的栈）。其他任务的栈和数据一律放 PSRAM。
4. 调用方接口尽量不变：Settings 的 `get/set` 签名不变；三个持久化服务（Wi-Fi、配对、模型配置）对外接口不变。
5. 大块写入（OTA、大文件）不阻塞小写入（设置、JSON、场景数据）。

### 1.3 已定决策

用户已定（2026-11-06）：

- 现在就做，作为基础设施插进 MVP 计划，在 `feat/agent-core` 上实施。
- UI 线程同样不准访问 flash，但栈留在内部 RAM，理由是渲染热路径的性能；"要不要访问 flash"和"栈要不要放内部"分开声明。
- 内部 RAM 专门留给高性能部分使用。

### 1.4 不做

- 不改 ESP-IDF 自己的任务（Wi-Fi 存校准数据到 NVS、`esp_timer` 任务本身、`ipc` 等），它们的栈本来就在内部 RAM，不受本规则约束。规则约束的是**我们的代码**，包括挂在 esp_timer 上的回调。
- 不改场景 API 和 Lua 绑定的语义，它们已经经由 `wisp.fs`。
- 不做 Settings 的跨设备同步或版本迁移。

---

## 2. 规则

### 2.1 唯一的 flash 线程

- 线程 `wisp.fs` 称为 **flash 线程**。在它启动之后、停止之前（即"运行期"），以下操作只能在它上面执行：
  - NVS 的任何调用（`nvs_*`）；
  - `StorageManager` 的所有读写、列目录、删除、改名、`refresh()`；
  - 分区读写与映射（`esp_partition_*`）、OTA（`esp_ota_*`）；
  - 其他直接读写存储介质上文件的调用（`fopen`/`opendir` 等访问 `/int`、`/sd` 的路径）。
- 运行期以外有两个例外，都在单线程阶段：
  - **开机阶段**：`wisp.fs` 启动之前，`app_main` 映射字体分区、初始化 NVS、挂载文件系统；
  - **关机阶段**：`wisp.fs` 停止之后（只在测试和模拟器退出时出现）。
- SD 卡的挂载与探测（`wisp.sd`）不访问内部 flash，也不读写文件，保持现状。

### 2.2 任务声明

- `os::TaskConfig::flash_access` 保留，**只有 `wisp.fs` 声明 true**。其他任务全部改为 false，包括管理后台 httpd（`EspHttpServerConfig::flash_access`）。缺省值从 true 改为 false。
- 新增 `os::TaskConfig::hot_stack`（缺省 false）。声明 true 表示这个任务处在高性能路径上，栈放内部 RAM 的理由是速度而不是 flash。端口的栈放置规则改为：

```cpp
// wisp/port/esp/stack_placement.hpp
constexpr bool stack_may_use_psram(bool flash_access, bool hot_stack) {
    return !flash_access && !hot_stack;
}
```

- 目前只有 UI 线程（`wisp_ui`）声明 `hot_stack = true`。新增其他热栈任务要在 spec 里说明理由（例如音频实时处理）。

### 2.3 内部 RAM 的用途

AGENTS.md 第 5 节"内部 RAM 预算"改写为：内部 SRAM 只留给

1. DMA 缓冲（`Mem::Dma`）；
2. flash 线程 `wisp.fs` 的栈；
3. ISR 与 IRAM 代码、数据；
4. ESP-IDF 自己的任务与驱动；
5. 声明为热路径的任务栈（`hot_stack`，目前只有 UI 线程）。

其余超过几 KB 的一律用 `Mem::Large`（PSRAM）。

---

## 3. 组件

### 3.1 `StorageWorker`：两条队列与同步等待

`components/wisp_core/include/wisp/core/storage_worker.hpp`：

```cpp
enum class IoLane : uint8_t { Normal, Bulk };

// Queues `job` on `lane`. Normal jobs (settings, JSON documents, scene
// data) always run before queued Bulk jobs (firmware chunks, file
// download chunks); within a lane, FIFO.
bool submit(std::function<void()> job, IoLane lane = IoLane::Normal);

// Runs `job` on the worker and waits for it. Runs it inline when called
// on the worker, or before start() / after stop() (the single-threaded
// boot and shutdown phases). Fails with ErrorCode::Busy if the queue is
// full, and ErrorCode::Timeout if it did not finish in `timeout_ms`; the
// job may still run later, so it must own what it touches.
Result<void> run_sync(std::function<void()> job, uint32_t timeout_ms,
                      IoLane lane = IoLane::Normal);
```

- 实现：两条 `os::Queue`（Normal 32 项、Bulk 4 项），用一个计数信号量唤醒线程。线程每次先取 Normal，Normal 空了再取 Bulk。
- 一块 Bulk 任务最多处理 32 KB，单次耗时以 flash 擦写为上限（几十 ms），Normal 任务的最坏等待就是一块 Bulk 的耗时。
- `agent_service.cpp` 里的 `run_io()` 和 `scene_wiring.cpp` 里开机扫描的同步等待都改用 `run_sync`。

### 3.2 flash 线程守卫

`components/wisp_os/include/wisp/os/flash_guard.hpp`（可移植层）：

```cpp
namespace wisp::os {
// Registers the calling thread as the flash thread (wisp.fs calls it as
// its first action) / clears it (its last action).
void set_flash_thread();
void clear_flash_thread();
// True on the flash thread, or when no flash thread is registered (the
// boot and shutdown phases).
bool on_flash_thread();
}  // namespace wisp::os

// At every flash entry point: StorageManager's public methods, the NVS
// settings backend, the firmware (OTA) service, the partition reads.
#define WISP_ASSERT_FLASH_THREAD(what) ...
```

- **模拟器与单元测试**：违反时打印调用位置和线程名，然后 `abort()`，所以放错线程的代码在 ctest 里就会失败。
- **设备**：违反时每个调用点只打一次 `WLOG_E`（含任务名），不主动 abort。栈在 PSRAM 的任务会被硬件 abort，栈在内部 RAM 的任务能继续运行，日志留下线索。
- 用 `on_flash_thread()` 实现 `StorageWorker::on_worker()`，两者是同一个概念。
- 测试里大量直接在 gtest 主线程调用 `StorageManager`：没有注册 flash 线程时守卫放行，所以不启动 `StorageWorker` 的测试不受影响。启动了 worker、又在测试线程直接访问存储的测试要改成经 `run_sync`，这本身就是在消除同类问题。

### 3.3 静态检查

`tools/check_layering.py` 增加一条规则：`nvs_`、`esp_partition_`、`esp_ota_` 的调用以及 `#include "nvs.h"`/`"esp_ota_ops.h"`/`"esp_partition.h"` 只允许出现在白名单文件中：

- NVS 设置后端；
- 固件服务；
- 字体分区映射；
- NVS 初始化与恢复出厂的擦除入口；
- 平台层存储挂载。

白名单写在脚本里，每一项附一行理由，并为这条规则补自测（`tools/check_layering_test.py`）。

### 3.4 Settings：内存缓存 + 异步落盘

- `SettingsBackend` 新增：

```cpp
// Loads every key of every namespace into memory. Runs on the flash
// thread (or in the boot phase) once, before any get().
virtual Result<void> load_all() = 0;
// Writes the keys changed since the last flush. Flash thread only.
virtual Result<void> flush_dirty() = 0;
```

- 新类 `core::CachedSettingsBackend` 包装平台后端（NVS 或 JSON 文件）：
  - `get()` 只读缓存，缓存放在 PSRAM（`os::LargeString`），互斥锁保护，任何线程可调用；
  - `set()`/`erase()` 先改缓存，记下脏键；如果还没有待执行的落盘任务，就提交一个 Normal 任务。同一个键的连续写入在落盘前合并为一次；
  - 落盘失败：缓存保留新值、脏键保留，记 `WLOG_W`，等下一次 `set()` 或 `flush()` 重试；连续 3 次失败时发一条系统通知（"设置未能保存"）；
  - `flush(timeout_ms)`：同步等待脏键写完，给恢复出厂、重启、深度待机前调用。
- 模拟器的 JSON 文件后端本来就有内存表，改为只在 `flush_dirty()` 时整文件重写。
- NVS 后端的 `load_all()` 用 `nvs_entry_find` 遍历 Wisp 自己的分区与命名空间。值都是字符串（现有格式不变），不涉及数据迁移。
- `Settings` 类的公有接口不变，所有调用方不用改。

### 3.5 三个持久化服务：异步保存

`WifiService`、`PairingService`、`ModelProfileService` 都已在内存里持有完整状态，各自的 `persist_locked()` 改为：

1. 在锁内把状态序列化成 `os::LargeString` 快照；
2. 提交一个 Normal 任务，用 `write_atomic` 写入；
3. 同一服务已有未执行的保存任务时，只替换快照、不重复提交，最后一次的快照胜出。

开机时的加载（`load()`）在开机阶段或经 `run_sync` 执行。写失败的处理与 Settings 相同。对外接口不变。

### 3.6 管理后台 httpd：OTA 与下载

- **OTA**：`FirmwareSink::on_data` 把收到的数据攒进一块 PSRAM 缓冲（32 KB），攒满或请求结束时用 `run_sync(…, IoLane::Bulk)` 交给 flash 线程执行 `esp_ota_write`，等这一块写完再继续收。`on_end` 用 `run_sync` 执行 `esp_ota_end` 与校验、设启动分区（映射 flash 的那一步也在 flash 线程上）。httpd 自己只做网络收发和等待。
- **场景文件下载**：`routes_scene_files.cpp` 每次用 `run_sync(…, IoLane::Bulk)` 让 flash 线程 `read_range` 一块（32 KB）到 PSRAM 缓冲，httpd 再发送。`SceneFiles::read()` 只在 flash 线程上调用（与 Lua 路径一致），`docs/architecture.md` 里"或允许访问 flash 的 httpd 线程"的例外删除。
- **系统设置路由**：`SystemSettings` 走缓存（3.4），不需要改动。
- 之后管理后台 httpd 声明 `flash_access = false`，10 KB 栈可放 PSRAM。

### 3.7 其他调用点

| 位置 | 改法 |
|---|---|
| `net_wiring.cpp` 读 `ntp_servers` | 走 Settings 缓存，无需改动；删除架构文档里"Settings 会访问 flash"的说明 |
| `power_manager.cpp` 读电源设置 | 走缓存，无需改动 |
| `self_check_screen.cpp` 读写 `perf_overlay` | 走缓存，无需改动 |
| `loopback.cpp` 读音量 | 走缓存，无需改动 |
| `app.cpp` 每 2 s `refresh()` | 定时器回调改为 `submit` 一个 Normal 任务；上一个还没执行完就跳过这一次 |
| `firmware_confirm.cpp` 标记固件有效 | `run_sync` 交给 flash 线程 |
| `factory_reset.cpp` | 整个擦除过程作为一个 Normal 任务交给 flash 线程（`run_sync`，超时 30 s），完成后在 `wisp.reset` 上重启 |
| `wisp.bus` 的订阅回调 | 规则同样适用；守卫会抓出违规的订阅者 |

### 3.8 关机、重启、深度待机

`Platform::restart`、`request_power_off` 和进入深度待机之前，依次：

1. `Settings::flush(2000)`；
2. `StorageWorker::drain(5000)`。

超时只记警告，然后继续。OTA 完成后的重启同样如此。

### 3.9 任务声明的终态

| 任务 | `flash_access` | `hot_stack` | 栈位置（ESP，PSRAM 有空间时） |
|---|---|---|---|
| `wisp.fs` | true | false | 内部 |
| `wisp_ui` | false | true | 内部 |
| 管理后台 httpd（8900） | false | false | PSRAM |
| `wisp.net`、`wisp.console`、`wisp.reset`、`wisp.fwup`、`wisp.bus`、`wisp.loop`、`wisp.sample`、`wisp.sd` | false | false | PSRAM |
| 已在 PSRAM 的任务（`wisp.scene`、`wisp.agent` 等） | false | false | PSRAM |

`wisp.sd` 只做挂载探测：挂载 FAT 会访问 SD 卡，但不触及内部 flash 和 cache。如果实测挂载代码会 abort，就把挂载移到 flash 线程，并在硬件文档里记录。

---

## 4. 测试

**单元测试（模拟器）：**

- `FlashGuard`：
  - 注册后，其他线程访问会 abort（death test）；
  - 未注册时放行；
  - `on_worker()` 与 `on_flash_thread()` 一致。
- `StorageWorker`：
  - Normal 任务插在已排队的 Bulk 任务前面；
  - `run_sync` 在 worker 上直接执行，不死锁；
  - `run_sync` 超时返回 `Timeout`，任务事后仍然执行；
  - 队列满时返回 `Busy`。
- `CachedSettingsBackend`：
  - `get` 不访问后端；
  - 连续 `set` 同一个键只落盘一次；
  - 落盘失败后保留脏键，下次重试；
  - 连续 3 次失败发通知；
  - `flush` 等待写完。
- 三个服务：保存不在调用线程上执行，多次保存只写最后一份快照；重启（重新加载）后数据一致。
- 管理后台：
  - OTA 上传与场景文件下载的现有测试全部通过；
  - 新增断言：数据块都在 flash 线程上写入或读出（用测试替身记录线程）。
- 守卫覆盖：完整 ctest 在守卫开启的情况下全绿。这本身就验证了没有遗漏的直接访问。

**冒烟测试：** `admin_smoke`、`sim_smoke` 全部通过。

**真机（交给用户执行）：**

1. 每个任务在 `mem` 里的栈位置符合 3.9。
2. 开机空闲与最坏情况的内部 RAM（AGENTS.md 第 5 节的组合：STA+AP、两个 httpd、WebSocket 日志、TLS 握手、OTA 上传），对比本次改动前：`internal free` 79679 B，largest 31744 B。
3. OTA 上传 4 MB 期间，在管理后台改一项设置，设置立即生效且不等 OTA 结束；上传速度与改动前相比下降不超过 20%。
4. 下载几 MB 的场景文件，校验字节一致。
5. 恢复出厂、重启、深度待机前改的设置都已落盘：改完立刻重启后，设置仍在。
6. 全程串口没有 `flash access off the flash thread` 错误日志。

---

## 5. 文档

- AGENTS.md 第 5 节："内部 RAM 预算"按 2.3 改写；任务声明一段按 2.2 改写（只有 `wisp.fs` 访问 flash，`hot_stack` 的含义）。与实现放在同一个提交，提交信息 `docs(agents): ...`。
- `docs/architecture.md`：任务表、"存储"一节写明 flash 线程与两条队列；删除 httpd 和 `wisp.net` 访问 flash 的例外说明。
- `docs/porting-guide.md`：新端口要实现 `os::set_flash_thread`/`on_flash_thread`，栈放置规则看 `flash_access` 与 `hot_stack`。
- `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`："内部 RAM"一节记录改动前后的实测。
- MVP spec：第 4 节提到的任务栈放置按本 spec 更新；MVP 计划里后续的工具任务一律经 flash 线程访问存储。

---

## 6. 风险

1. **异步写的丢失窗口**：`set()` 返回到真正落盘之间断电，会丢最后一次修改。这对设置、Wi-Fi、配对是可以接受的（最坏情况是重新设置一次）；需要确定性的路径（恢复出厂、OTA、重启）都先 `flush`。
2. **守卫的覆盖面**：守卫只放在 2.1 列出的入口上。绕过这些入口、直接调用 libc 文件函数的代码，只能靠静态检查和评审发现。
3. **OTA 速度**：每 32 KB 一次线程间往返，预计开销小于 1%。真机第 3 项会验证。
4. **测试改动量**：启动了 worker 又在测试线程直接访问存储的测试需要修改。改动量要等守卫打开后才能统计出来，计划里单列一个任务处理。
