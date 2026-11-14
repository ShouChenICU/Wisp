# Wisp 单一 flash 线程设计文档

- 日期：2026-11-06
- 状态：已批准（2026-11-06），实施中
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

1. 运行期间，只有 `wisp.fs` 读、写、擦除、映射内部 flash，以及读写内部 LittleFS 上的文件。只读写 SD 卡的文件工作交给 SD I/O 线程 `wisp.sd.io`（2.1）；`wisp.fs` 也可以读写 SD 卡。其他线程一律把请求交给这两个线程之一，自己不碰。
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

- 线程 `wisp.fs` 称为 **flash 线程**。在它启动之后、停止之前（即"运行期"），以下碰内部 flash 的操作只能在它上面执行：
  - NVS 的任何调用（`nvs_*`）；
  - 分区读写与映射（`esp_partition_*`）、OTA（`esp_ota_*`）；
  - 内部存储上的文件操作：`StorageManager` 对 `/int` 路径的读写、列目录、删除、改名，以及其他访问 `/int` 路径的调用（`fopen`/`opendir` 等）。
- `/sd` 路径上的文件操作和 `StorageManager::refresh()` 按下面的 SD I/O 规则执行：在 `wisp.sd.io` 或 `wisp.fs` 上，其他线程都不行。
- 运行期以外有两个例外，都在单线程阶段：
  - **开机阶段**：`wisp.fs` 启动之前，`app_main` 映射字体分区、初始化 NVS、挂载文件系统；
  - **关机阶段**：`wisp.fs` 停止之后（只在测试和模拟器退出时出现）。
- SD 卡的挂载与探测（`wisp.sd`）不访问内部 flash，也不读写文件，保持现状。
- **SD I/O 线程** `wisp.sd.io`（`core::StorageWorker`，`WorkerRole::SdIo`，由 `SceneWiring` 持有，板子注册了 `storage.sd` 才启动）：只读写 SD 卡的文件工作（`/sd` 路径、`refresh()`）在它上面执行，由 `core::StorageRouter` 按 `Medium::Sd` 分派（某个场景在卡上的 `wisp.files` 调用、换卡后的重新统计、`StorageManager::refresh()`）。规则：
  - `wisp.sd.io` 不碰内部 flash，也从不等待 `wisp.fs`（`run_sync`、`drain`、`stop`、`call_on_worker` 对 flash worker 在这个线程上一律拒绝）；
  - `wisp.fs` 也可以读写 SD 卡；同时碰两种介质的工作（场景的位置记录在内部、文件在卡上，`scan()`、恢复出厂设置、`status()` 等）留在 `wisp.fs`；
  - `wisp.fs` 只有在不持有任何卡锁时才能等待 `wisp.sd.io`；
  - 锁的顺序（外层在前，持有后面的锁时不再取前面的）：`SceneFiles::io_int_mu_` → `SceneFiles::io_sd_mu_` → `SceneRepository::mutex_` → `StorageManager::int_io_mu_` → `StorageManager::sd_io_mu_` → `StorageManager::state_mu_`。`wisp.sd.io` 上只会取 `io_sd_mu_`、`sd_io_mu_`（和 `state_mu_`）。
  - 没有 SD I/O 线程（没有卡槽、启动失败、`kSdOwnWorker` 为 false）、它还没启动或已经 join 之后，卡上的工作回到 `wisp.fs` 执行。从 `stop_sd()` 开始到 join 完成之间（它还在跑停止前已接受的任务），新的卡上工作一律拒绝（`submit()` 返回 false，`run_sync()` 返回 `Busy`），不转给 `wisp.fs`，否则会越过还在队列里的卡上任务（同一场景的两次写、一次下载的各块会乱序）。

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

### 3.1 `StorageWorker`：三条队列与同步等待

`components/wisp_core/include/wisp/core/storage_worker.hpp`：

```cpp
// In priority order.
enum class IoLane : uint8_t { Interactive, Normal, Bulk };

// Queues `job` on `lane`; within a lane, FIFO; across lanes, no order.
bool submit(std::function<void()> job, IoLane lane = IoLane::Normal);

// Runs `job` on the worker and waits for it. Runs it inline when called
// on the worker, or before start() / after stop() (the single-threaded
// boot and shutdown phases). Fails with ErrorCode::Busy if the lane is
// full, and ErrorCode::Timeout if it did not finish in `timeout_ms`; the
// job may still run later, so it must own what it touches.
Result<void> run_sync(std::function<void()> job, uint32_t timeout_ms,
                      IoLane lane = IoLane::Normal);
```

- 三条 `os::Queue`，按优先级：
  - **Interactive**（`wisp.fs` 16 项）：短小、有人在等的任务。设置落盘（`CachedSettingsBackend` 的保存与 `flush()`，以及没有缓存时 `SettingsFront` 的直接调用），对话追加（`ConversationStore::append`，经 `AgentDeps::run_io_interactive`），记忆与待办（`memory.*`、`todo.*` 工具声明 `ToolThread::FsInteractive`；管理后台删除笔记与待办；`ReminderScheduler` 的提醒检查；通知上"完成/稍后提醒"按钮的待办操作）。
  - **Normal**（`wisp.fs` 32 项）：其他 JSON 文档（通知、Wi-Fi、配对、模型配置、场景顺序）、场景数据与列表、设置的开机加载、对话的加载/归档/清空、记忆与待办的开机加载、恢复出厂设置、场景安装/导出、文件工具等较长的工作。
  - **Bulk**（`wisp.fs` 4 项）：固件块、下载块（每块至多 `kIoChunk`）。
  - `wisp.sd.io` 的容量是 Interactive 1、Normal 16、Bulk 4。
- 用一个计数信号量唤醒线程。挑选规则：`passed[L]` 记录车道 L 有任务等待期间、自它上次执行以来跑过的更高车道任务数。上一次不是强制轮次，且某个非空车道 `passed >= kLaneTurnAfter`（8）时，取其中最低的那条（强制轮次）；否则取最高的非空车道。执行车道 L 的一个任务后 `passed[L] = 0`，比它低的每条非空车道 `passed` 加一。
- 等待上限（除了入队时正在执行的那一个任务）：队首的 Interactive 任务至多再等一个低车道的强制轮次（所以 Bulk 至多让它多等一块）；Bulk 至多 9 个任务；Normal 至多 10 个任务。所以连续的 Interactive 负载也饿不死 Normal 和 Bulk。`wisp.fs` 上正在执行的任务和强制轮次的任务各自还可能在卡锁上等一次卡操作（`SceneFiles` 对每个没有以 Internal 载入的场景都取它的 SD I/O 锁，新场景的第一次调用也算），所以 Interactive 任务最坏要等两个任务加两次卡操作。
- 顺序约定：每个 worker 的每条车道内先进先出；跨车道、跨 worker 没有顺序。需要两个任务有先后的生产者，等第一个完成（`run_sync`、`call_on_worker`）再提交第二个。改到 Interactive 的调用方都满足这一点：对话的每次读写 `wisp.agent` 都同步等待，所以追加（Interactive）与归档/清空/加载（Normal）仍按调用顺序；工具调用一个一个同步执行；设置保存写的是执行那一刻的全部脏键，与谁先谁后无关；`ReminderScheduler::stop()` 靠在同一条 Interactive 车道上排在后面等待已排队的检查和按钮操作；车道满、排不进去时改为等 worker 排空（`ReminderDeps::drain_io`，同样有上限），不改投别的车道（跨车道没有顺序）。
- 车道满时：设置保存（以及 `flush()` 的任务）改投 Normal，因为它与顺序无关，不能让修改一直留在内存里；同步等待的调用方照旧（`call_on_worker` 与 `App::Impl::run_io()` 重试至多 2 s 后返回 `Busy`，工具返回 `busy`，管理后台返回忙）；提醒检查由重试定时器再提交，按钮操作记警告。
- Interactive 任务必须短：一次 NVS 批量写、一行对话追加、一次笔记或待办文件重写（笔记至多 64 条 × 200 字、待办至多 200 条 × 100 字，最坏几十 KB，通常几 KB）。长的工作（场景安装、导出、文件工具、对话归档与损坏尾部修复）留在 Normal。
- `drain()`、`stop()` 等所有车道排空，`App::flush_storage()` 因此也等 Interactive 任务（3.8）。
- 时延按**单个任务**计：线程不抢占正在执行的任务；Normal 和 Bulk 任务最多读写 32 KB（`kIoChunk`），单次耗时以 flash 擦写为上限（几十 ms）。
- 长操作一律拆成多个任务，每个任务至多 32 KB，其他任务（设置落盘、下载块）在块与块之间执行：
  - OTA：每块 `esp_ota_write` 一个 Bulk 任务（3.6）；
  - 场景文件下载：每块 `read_range` 一个 Bulk 任务（3.6）；
  - 场景安装：先在一个任务里完成上一次中断的替换并清掉暂存目录，再把每个文件按 32 KB 一块写进 `.<id>.tmpdir`（第一块 `write_atomic`，其余 `append`），每块一个 Normal 任务、不持仓库锁（暂存目录只属于这次安装，同一 id 的第二次安装被 `installing()` 拒绝，`list()` 跳过点目录），最后一个持锁的任务把它换成正式包；
  - 场景导出：一个任务列出文件，再按每块 32 KB 一个任务读取；每块读取前在锁内检查仓库的代数（每次替换、删除都会加一），变了就以 `Busy` 失败，不会混合两个版本。
- `call_on_worker<T>(worker, fn)`（`storage_worker.hpp`）：在 worker 上执行一个返回 `Result<T>` 的调用并一直等待；队列满时重试至多 2 s，仍满则返回 `Busy`。等待没有超时，所以 `fn` 可以引用调用方栈上的数据（被拒绝的任务不会执行，排上队的任务在返回前一定已经执行完）。
- `agent_service.cpp` 里的 `run_io()` 和 `scene_wiring.cpp` 里开机扫描的同步等待都改用 `run_sync`。

### 3.2 flash 线程守卫

`components/wisp_os/include/wisp/os/flash_guard.hpp`（可移植层）：

```cpp
namespace wisp::os {
// Registers the calling thread as the flash thread (wisp.fs calls it as
// its first action). A later call on another thread takes over.
void set_flash_thread();
// Unregisters the calling thread if it is the registered flash thread; a
// no-op on any other thread (wisp.fs calls it as its last action).
void clear_flash_thread();
// True on the flash thread, or when no flash thread is registered (the
// boot and shutdown phases).
bool on_flash_thread();

// The second role, the SD I/O thread (wisp.sd.io), with the same
// take-over rule. on_flash_thread() is always false on it.
void set_sd_io_thread();
void clear_sd_io_thread();
bool on_sd_io_thread();
// Card I/O allowed here: the SD I/O thread, or wherever on_flash_thread().
bool sd_io_allowed();

// Device builds report a violation through this hook (wisp_os sits below
// the logging layer); wisp_core's log::init() installs one that logs with
// WLOG_E. `sd` tells an SD I/O thread rule violation (WISP_ASSERT_SD_IO_THREAD)
// from a flash one. Null: not reported.
using FlashViolationReporter = void (*)(const char* what, bool sd);
void set_flash_violation_reporter(FlashViolationReporter reporter);
}  // namespace wisp::os

// At every flash entry point: StorageManager's I/O methods, both settings
// backends, both firmware updaters, the font-partition mapping.
#define WISP_ASSERT_FLASH_THREAD(what) ...
// At card-only entry points.
#define WISP_ASSERT_SD_IO_THREAD(what) ...
// StorageManager's path operations: the SD rule when `on_sd`, else the
// flash rule.
#define WISP_ASSERT_MEDIUM_THREAD(on_sd, what) ...
```

- **两个角色**：flash 规则（`WISP_ASSERT_FLASH_THREAD`）只放行 flash 线程，或没有登记 flash 线程时的任何线程，但**任何阶段都不放行 SD I/O 线程**（它从不算作 flash 线程）；SD 规则（`WISP_ASSERT_SD_IO_THREAD`）放行 SD I/O 线程和 flash 规则放行的线程，所以 flash 线程也可以读写卡。`StorageManager` 的每个路径操作按路径所在介质选规则（`WISP_ASSERT_MEDIUM_THREAD`）。
- **登记**：登记的是线程 id（每个线程第一次用到守卫时从计数器取一个不重复的 32 位 id，不复用，已退出的线程不会被后来的线程冒充）。`clear_flash_thread()` 只有登记的那个线程调用才生效；后登记的线程接管，先前那个线程之后的 clear 不会撤掉新的登记。所以同一进程里同时有两个 worker（测试）时，一个停下不会让守卫对另一个失效。
- **模拟器与单元测试**：违反时向 stderr 打印 `flash access off the flash thread: <调用点>`，然后 `abort()`，所以放错线程的代码在 ctest 里就会失败。
- **设备**：违反时每个调用点只报告一次（宏展开处的静态标志），经 `set_flash_violation_reporter()` 装上的钩子发出；`log::init()` 装的钩子打 `WLOG_E("flash", "flash access off the flash thread: %s", what)`（SD I/O 线程规则的违反，`sd` 为 true，则打 `WLOG_E("sd", "sd access off the sd i/o thread: %s", what)`）。不主动 abort：栈在 PSRAM 的任务会被硬件 abort，栈在内部 RAM 的任务能继续运行，日志留下线索。
- `StorageWorker::on_worker()` 是 `t_current_worker == this`（worker 线程在 `run()` 里记下的线程局部指针），不用 `on_flash_thread()`：后者在没有登记时也返回 true（会让开机、关机阶段任何线程上的 `run_sync()` 都就地执行），也分不清同时存在的两个 worker。
- 测试里大量直接在 gtest 主线程调用 `StorageManager`：没有注册 flash 线程时守卫放行，所以不启动 `StorageWorker` 的测试不受影响。启动了 worker、又在测试线程直接访问存储的测试要改成经 `run_sync`，或在启动 worker 之前做完准备，这本身就是在消除同类问题。

### 3.3 静态检查

`tools/check_layering.py` 增加一条规则：`nvs_`、`esp_partition_`、`esp_ota_` 的调用以及 `#include "nvs.h"`/`"esp_ota_ops.h"`/`"esp_partition.h"` 只允许出现在白名单文件中：

- NVS 设置后端；
- 固件服务；
- 字体分区映射；
- NVS 初始化与恢复出厂的擦除入口（与 NVS 设置后端是同一个文件）。

平台层的 LittleFS 挂载经 `esp_vfs_littlefs_register()` 按分区标签挂载，不直接调用这些接口，所以不在白名单里；以后真要用时再加。白名单写在脚本里，每一项附一行理由，并为这条规则补自测（`tools/check_layering_test.py`）。

### 3.4 Settings：内存缓存 + 异步落盘

- `SettingsBackend` 新增（`components/wisp_core/include/wisp/core/settings.hpp`）：

```cpp
// One stored key, as load_all() returns it.
struct SettingsEntry { std::string ns, key; Value value; };
// One change for write_batch(): store `value`, or erase the key when nullopt.
struct SettingsChange { std::string ns, key; std::optional<Value> value; };

// Every key of every namespace. A platform backend reads its storage
// (flash); CachedSettingsBackend calls this once, at boot or on the
// flash thread, and serves get() from memory after that.
virtual Result<std::vector<SettingsEntry>> load_all() = 0;
// Applies `changes` in order, committed together where the backend can
// (one file rewrite, one NVS commit per namespace). Every name is
// validated first. A platform backend touches flash here, so it is
// called on the flash thread.
virtual Result<void> write_batch(std::span<const SettingsChange> changes) = 0;
```

- 脏键由缓存记录，不由平台后端记录：落盘任务把脏键整理成一批 `SettingsChange` 交给 `write_batch()`。
- 新类 `core::CachedSettingsBackend` 包装平台后端（NVS 或 JSON 文件）：
  - `get()` 只读缓存，缓存放在 PSRAM（`os::LargeString`），互斥锁保护，任何线程可调用；
  - `set()`/`erase()` 先改缓存，记下脏键；如果还没有待执行的落盘任务，就提交一个 Interactive 任务（Interactive 满时改投 Normal，3.1）。同一个键的连续写入在落盘前合并为一次；
  - 落盘失败：缓存保留新值、脏键保留，记 `WLOG_W`，等下一次 `set()` 或 `flush()` 重试；连续 3 次失败时发一条系统通知（"设置未能保存"）；
  - `flush(timeout_ms)`：同步等待脏键写完（Interactive 车道，满时改投 Normal），给恢复出厂、重启、深度待机前调用；`load()` 只在开机时执行一次，用 Normal。
- 模拟器的 JSON 文件后端本来就有内存表，每次 `write_batch()` 整文件重写一次。
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
- **场景文件下载**：`routes_scene_files.cpp` 每次经 `core::run_files()`（`SceneFiles::submit()`）在 Bulk 队列里读一块（32 KB）到 PSRAM 缓冲，httpd 再发送。文件在 SD 卡上的场景在 `wisp.sd.io` 上读，其余在 flash 线程上读（与 Lua 路径一致），所以从卡上下载大文件不占用 `wisp.fs`。`docs/architecture.md` 里"或允许访问 flash 的 httpd 线程"的例外删除。
- **系统设置路由**：`SystemSettings` 走缓存（3.4），不需要改动。
- 之后管理后台 httpd 声明 `flash_access = false`，10 KB 栈可放 PSRAM。

### 3.7 其他调用点

| 位置 | 改法 |
|---|---|
| `net_wiring.cpp` 读 `ntp_servers` | 走 Settings 缓存，无需改动；删除架构文档里"Settings 会访问 flash"的说明 |
| `power_manager.cpp` 读电源设置 | 走缓存，无需改动 |
| `self_check_screen.cpp` 读写 `perf_overlay` | 走缓存，无需改动 |
| `loopback.cpp` 读音量 | 走缓存，无需改动 |
| `app.cpp` 每 2 s `refresh()` | 定时器回调经 `StorageRouter` 以 `Medium::Sd` 提交一个 Normal 任务（有 `wisp.sd.io` 时在它上面，否则在 `wisp.fs` 上）；上一个还没执行完就跳过这一次。拔卡时 `refresh()` 持卡的 I/O 锁把卡标记为离线并释放它，排在后面的卡任务以 `StorageUnavailable` 失败 |
| `firmware_confirm.cpp` 标记固件有效 | `run_sync` 交给 flash 线程 |
| `factory_reset.cpp` | 先 `StorageRouter::stop_sd()`（`wisp.sd.io` 做完已排队的任务后停止，其间新的卡任务被拒绝；返回之后的卡任务排在 `wisp.fs` 上），再把整个擦除过程（也会删卡上的文件）作为一个 Normal 任务交给 flash 线程（`run_sync`，超时 30 s），完成后在 `wisp.reset` 上重启 |
| `wisp.bus` 的订阅回调 | 规则同样适用；守卫会抓出违规的订阅者 |

### 3.8 关机、重启、深度待机

`Platform::restart`、`request_power_off` 和进入深度待机之前，依次：

1. `Settings::flush(2000)`；
2. `StorageRouter::drain(5000)`：`wisp.fs` 和 `wisp.sd.io` 两个 worker 都排空（`App::flush_storage()`）。

`App::stop()` 里 `SceneWiring::stop()` 先排空两个 worker，再停 `wisp.sd.io`，最后停 `wisp.fs`。

超时只记警告，然后继续。OTA 完成后的重启同样如此。

### 3.9 任务声明的终态

| 任务 | `flash_access` | `hot_stack` | 栈位置（ESP，PSRAM 有空间时） |
|---|---|---|---|
| `wisp.fs` | true | false | 内部 |
| `wisp.sd.io`（只读写 SD 卡，栈 8192） | false | false | PSRAM |
| `wisp.shell.list`（场景列表，栈 12288，`Shell::kListStackBytes`） | false | false | PSRAM |
| `wisp_ui` | false | true | 内部 |
| 管理后台 httpd（8900） | false | false | PSRAM |
| `wisp.net`、`wisp.console`、`wisp.reset`、`wisp.fwup`、`wisp.bus`、`wisp.loop`、`wisp.sample`、`wisp.sd` | false | false | PSRAM |
| 已在 PSRAM 的任务（`wisp.scene`、`wisp.agent` 等） | false | false | PSRAM |

`wisp.sd` 只做挂载探测：挂载 FAT 会访问 SD 卡，但不触及内部 flash 和 cache。如果实测挂载代码会 abort，就把挂载移到 flash 线程，并在硬件文档里记录。

`wisp.sd.io` 的栈在 PSRAM，在上面执行 FATFS/sdmmc 的读写（以及拔卡后 `refresh()` 里的卸载）需要真机验证。如果实测从 PSRAM 栈写卡会 abort，把 `scene_wiring.hpp` 里的 `kSdOwnWorker`（`SceneWiringDeps::sd_own_worker` 的默认值）改为 false：卡上的工作全部回到 `wisp.fs`，其余不变。

### 3.10 SD I/O 线程与车道：实现中确定的细节

- **`StorageManager`**：`Medium {Internal, Sd}`；`medium_of(path)` 给出路径所在介质（`storage.sd` 挂载点及以下为 `Sd`，其余与不合法的路径为 `Internal`），`has_sd()` 表示板子注册了 `storage.sd`。每个路径操作按介质检查线程（`WISP_ASSERT_MEDIUM_THREAD`），并在整个 I/O 期间持该介质的 I/O 锁，所以卡的读写不等 flash 擦写。锁的顺序 `int_io_mu_` → `sd_io_mu_` → `state_mu_`。无法规范化的路径在线程检查之前就返回 `InvalidArgument`（任何线程上都一样，写、读、追加、删除、改名、列目录、`visit_dir`、`tree_size` 全部如此）；跨介质的 `rename()` 返回 `InvalidArgument`。
- **`StorageWorker`**：
  - `IoLane {Interactive, Normal, Bulk}`；`wisp.fs` 的容量 Interactive 16（`interactive_capacity`）、Normal 32、Bulk 4；`kLaneTurnAfter = 8`。等待上限见 3.1：Interactive 至多再等一个强制轮次，Bulk 至多 9 个任务，Normal 至多 10 个。
  - `LaneStats {jobs, max_wait_us, max_job_us}` 与 `stats(lane)`：每条车道自启动以来的任务数、最长排队时间和最长执行时间；`run_sync` 就地执行的不计。
  - `try_submit(job&)`：被拒绝时任务留在调用方手里（不被移走），调用方可以改投另一个 worker。`run_queued(job&, timeout)`：`run_sync` 的等待部分，但从不就地执行。
  - `WorkerRole {Flash, SdIo}`；`StorageWorker::sd_io_config()` 给出 `wisp.sd.io` 的配置（Interactive 1、Normal 16、Bulk 4，栈 8192，优先级 3）。SD I/O 线程从不等 flash worker：flash worker 的 `run_sync`、`drain`、`stop` 以及 `call_on_worker` 在这个线程上一律拒绝。
- **`StorageRouter`**（`App::storage_router()`）：按介质选 worker，`Medium::Sd` 从 `wisp.sd.io` 启动到它被 join（`StorageWorker::joined()`）为止交给它，其余交给 `wisp.fs`。`stop_sd()` 排空期间新的卡任务被拒绝（`submit()` false、`run_sync()` `Busy`），不改投 `wisp.fs`，以免越过还在排队的卡任务；因 `wisp.sd.io` 已 join 而被拒绝的卡任务改投 `wisp.fs` 一次；车道满则报告给调用方，不改投（会打乱那条车道的顺序）。在 `wisp.sd.io` 的任务里调用时，`Medium::Sd` 就地执行（包括 `stop_sd()` 排空期间）。`wisp.fs` 只有在不持任何卡锁时才能等 `wisp.sd.io`。
- **`SceneFiles`**：
  - 按场景分派（`submit()`）：场景还有经 `Internal` 分派、尚未结束的任务时（每个场景一个计数），它的调用继续留在 `wisp.fs`；位置记录已加载且为 `Sd` 后才转到 `wisp.sd.io`。所以同一场景的任务在两个 worker 之间仍保持顺序。
  - 换卡：`sd_changed_begin()`（`wisp.fs`）标记卡上的场景，`recount()` 在 `wisp.sd.io` 上逐个重新统计，`sd_changed_end()`（`wisp.fs`）记录没有场景的卡上文件。
  - `run_files()`：管理后台的文件路由经它把调用作为场景的任务提交并等待。
  - 下载留在开始时的 worker 上，追加块走 Bulk 车道；结束经 `FileDownload::submit()` 排在追加之后（在下载自己的 worker 上只试一次，因为那时没有别人能排空车道）。
  - 每次加载位置记录都有新的代数（generation）；卡任务带着分派时的代数，场景的文件被删除后才执行的卡任务以 `Busy` 失败。被放弃的下载（`FileDownload::abandon()`）使场景在下一次调用时重新统计，删掉留下的 `.dl`。
- **应用**：`wisp.sd.io` 在 `wisp.fs` 之后启动、之前停止；每次落盘排空（`App::flush_storage()`、`SceneWiring::stop()`）都覆盖两者。恢复出厂先停 `wisp.sd.io`；恢复失败时它保持停止直到重启，卡上的工作在 `wisp.fs` 上执行。每 2 s 的 `refresh()` 在 `wisp.sd.io` 上执行。开关 `kSdOwnWorker`（`scene_wiring.hpp`，经 `SceneWiringDeps::sd_own_worker` 暴露，便于测试）关掉第二个 worker。
- **Interactive 车道的调用方**：设置保存与 `flush()`（车道满时改投 Normal），对话追加，记忆笔记，待办（agent 工具声明 `ToolThread::FsInteractive`，管理后台的记忆与待办路由），提醒检查与通知按钮。开机加载与长任务留在 Normal。
- **场景仓库的读取**：`manifest`/`info`/`manifests`/`list` 每个场景一个任务，读出的 `scene.json` 在调用方解析，或用 `ParseOn::Worker` 在任务里解析（管理后台的路由用它，因为 httpd 任务的栈小）；`load`、导出、`recover`、安装的换入和 `declared` 在各自的任务里解析。agent 工具 `scene.list` 在 `wisp.agent` 上执行。
- **`wisp.shell.list`**：一次性任务（栈 12288 B，`Shell::kListStackBytes`；优先级 2；`flash_access = false`，`hot_stack = false`），列出场景并解码图标（解码不在 `wisp.fs` 的 8 KB 内部栈上）。同一时间只有一个，运行期间的请求合并为再跑一轮；结果带仓库的代数，旧的结果不会覆盖新的；缓存仍然有效时打开列表不启动任务。`Shell::stop()` 至多等正在执行的那个存储任务，再加一次文件位置探测（`kFilesProbeMs` = 5000 ms）。
- **`mem`**：在堆与任务栈之后，每个 worker 的每条车道一行：`storage <worker> <interactive|normal|bulk> jobs=<n> wait_max_ms=<w> job_max_ms=<j>`（向上取整到毫秒），先 `wisp.fs`；没有 SD I/O worker 时一行 `storage wisp.sd.io off`（`app::storage_report()`）。

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
- SD I/O 线程与车道（3.10）：
  - `FlashGuard`：SD I/O 线程与 flash 线程一样登记与接管，但从不算作 flash 线程；在它上面访问内部 flash、在两个线程之外访问卡都会 abort（death test）；设备上的报告钩子分得清是哪条规则。
  - `StorageManager`：`medium_of`、`has_sd`；卡上的操作不挡内部操作、同一介质的操作串行、`refresh()` 等正在进行的卡操作；跨介质改名被拒绝；SD I/O 线程上不合法的路径在每个入口（写、读、追加、删除、改名、`visit_dir`、`list_dir`、`tree_size`）都返回 `InvalidArgument`。
  - `StorageWorker`：Interactive 越过排队的 Normal 和 Bulk、紧跟在正在执行的 Bulk 之后；持续到来的高车道任务饿不死低车道（8 个之后的强制轮次）；`LaneStats` 记下最长等待与执行，就地执行的不计；`try_submit` 被拒绝时保留任务；`run_queued` 从不就地执行；从 SD I/O 线程对 flash worker 的 `run_sync`、`stop` 被拒绝。
  - `StorageRouter`：按介质分派；没有或已 join 的 SD worker 时卡任务在 flash worker 上执行；`stop_sd()` 先执行已排队的卡任务，其间新的卡任务被拒绝；`drain` 等两个 worker。
  - `SceneFiles`：有未结束的内部任务时场景留在内部；位置记录加载为卡后转到卡；下载的块留在同一个 worker；场景文件被删除后执行的卡任务失败、完成的下载被丢弃；被放弃的下载使场景重新统计；`run_files` 在场景的 worker 上等待。
  - 应用：有卡槽才启动 `wisp.sd.io`；`refresh()` 在它上面执行；`flush_storage()` 排空它；恢复出厂在擦除前停止它；`kSdOwnWorker` 关掉时卡任务回到 `wisp.fs`；设置保存、对话追加、记忆与待办走 Interactive，车道满时设置改投 Normal。
  - 场景列表：列出与解码不在 `wisp.fs` 上、也不在 UI 线程上；`Shell::stop()` 在正在执行的任务之后结束列表，被停止的列表不投递任何结果；`mem` 的车道行格式（`Console.StorageReport*`）。

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
5. **卡上的传输被 flash 擦写推迟**：卡任务留在 `wisp.fs` 上时（没有 `wisp.sd.io`、`kSdOwnWorker` 为 false、它已停止，或下载开始时场景的位置还在内部），它与 OTA 等 flash 工作共用 Bulk 车道，每块要排在一次 flash 擦写之后；有 `wisp.sd.io` 时卡上的下载与 OTA 各在一个 worker 上。真机检查：OTA 上传与卡上的下载同时进行，两者都完成、日志里没有 sdmmc 超时，其间改的设置立即生效（`docs/backlog.md`"Storage worker lanes: device checks not run"第 3 项）。
