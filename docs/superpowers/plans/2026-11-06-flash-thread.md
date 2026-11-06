# 单一 flash 线程 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 运行期间只有 `wisp.fs` 访问内部 flash 和存储介质，其他线程异步请求；放错线程在模拟器测试里就失败；内部 RAM 只留给必须放内部的东西和热路径。

**Architecture:** 可移植层加 flash 线程守卫（`wisp_os`）和 `hot_stack` 声明；`StorageWorker` 分 Normal/Bulk 两条队列并提供 `run_sync`；Settings 包一层缓存后端、异步落盘；三个持久化服务、OTA、下载、恢复出厂、定时刷新改经 worker；最后打开入口守卫、加静态检查、把所有任务改为不访问 flash。

**Tech Stack:** C++20（-fno-exceptions -fno-rtti）、GoogleTest、ESP-IDF v6.0.3、Python 3（`tools/check_layering.py`）

**Spec:** `docs/superpowers/specs/2026-11-06-flash-thread-design.md`（下文"spec"）。执行在 `feat/agent-core`，插在 MVP 计划 Task 3 与 Task 4 之间。

## Global Constraints

- 只有 `wisp.fs` 声明 `flash_access = true`；只有 `wisp_ui` 声明 `hot_stack = true`（spec 2.2）。
- 运行期 = `os::set_flash_thread()` 之后、`os::clear_flash_thread()` 之前；运行期外守卫放行（spec 2.1、3.2）。
- 守卫：模拟器/测试违规时打印位置与线程名后 `abort()`；设备每个调用点只 `WLOG_E` 一次，日志文本含 `flash access off the flash thread`（spec 3.2、4 真机第 6 项）。
- Normal 队列 32 项，Bulk 队列 4 项；Bulk 单块 ≤ 32 KB（`kIoChunk = 32 * 1024`）；Normal 先于已排队的 Bulk（spec 3.1）。
- Settings 连续 3 次落盘失败发一条系统通知"设置未能保存"（spec 3.4）。
- 关机顺序：`Settings::flush(2000)` → `StorageWorker::drain(5000)`，超时只警告（spec 3.8）。
- 恢复出厂的擦除经 `run_sync`，超时 30 s（spec 3.7）。
- 所有持久化写入仍走 `StorageManager::write_atomic`；大块缓冲用 `os::alloc(Mem::Large)`/`os::Large*`。
- AGENTS.md 全部规则；注释不写任务/评审编号、不引用 `.superpowers/`；提交带 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`。
- `python3 tools/check_layering.py`（用 python3，不用 bash）。

## Review Focus

1. **`run_sync` 在 worker 自己线程上被调用**（job 里再 `run_sync`）——期望内联执行、不死锁。Task 2 `RunSyncOnWorkerRunsInline`。
2. **`run_sync` 超时后 job 晚到执行**——期望 job 持有的数据仍然有效（捕获 `shared_ptr`，不捕获栈引用）。Task 2 `RunSyncTimeoutStillRunsJobSafely`、Task 5 OTA 缓冲用 `shared_ptr`。
3. **设置刚改完立即重启**——期望重启后设置仍在。Task 3 `FlushWritesDirtyKeys` + Task 4 `RestartFlushesSettingsFirst`。
4. **OTA 进行中改设置**——期望设置不排在 OTA 后面。Task 2 `NormalJobsOvertakeQueuedBulk`、Task 5 `SettingsWriteDuringUploadIsNotDelayed`。
5. **守卫打开后，事件总线订阅者或 esp_timer 回调间接访问存储**——期望 ctest 全绿即证明无遗漏。Task 6 Step 2。

---

### Task 1: flash 线程守卫与 `hot_stack`

**Files:**
- Create: `components/wisp_os/include/wisp/os/flash_guard.hpp`、`components/wisp_os/src/flash_guard.cpp`
- Modify: `components/wisp_os/include/wisp/os/os.hpp`（`TaskConfig::hot_stack`，`flash_access` 注释按 spec 2.2）、`components/wisp_port_esp/include/wisp/port/esp/stack_placement.hpp`、ESP 端 `Task::spawn` 与 `EspHttpServerConfig`（加 `hot_stack`）、`components/wisp_ui/src/ui_runtime.cpp`（UI 任务 `hot_stack = true`）
- Test: `tests/os/flash_guard_test.cpp`

**Interfaces:**
- Produces: `os::set_flash_thread()`、`os::clear_flash_thread()`、`os::on_flash_thread()`、`WISP_ASSERT_FLASH_THREAD(what)`（`what` 是 `const char*`，如 `"StorageManager::write_atomic"`）；`TaskConfig::hot_stack`；`stack_may_use_psram(bool flash_access, bool hot_stack)`。
- 守卫实现：一个原子保存注册线程的标识（`std::thread::id` 的哈希或端口提供的当前任务句柄，经 `wisp_os` 的平台无关接口取得）。设备上"每个调用点只打一次"用宏里的 `static std::atomic<bool>`。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(FlashGuard, AllowsEverythingWithNoFlashThread)   // on_flash_thread() true on any thread
TEST(FlashGuard, OnlyTheRegisteredThreadIsTheFlashThread) // set on thread A: A true, B false; clear: both true
TEST(FlashGuardDeathTest, AssertAbortsOffTheFlashThread)  // EXPECT_DEATH(..., "flash access off the flash thread.*write_atomic")
TEST(StackPlacement, HotOrFlashStacksStayInternal)    // host-built test of the constexpr: (false,false)->true, (true,*)->false, (false,true)->false
```

- [ ] **Step 2:** `ctest --test-dir build/sim -R 'FlashGuard|StackPlacement'` 失败。
- [ ] **Step 3: 实现。** 宏在非 ESP 构建调用 `std::abort()`，ESP 构建 `WLOG_E` 一次。`stack_placement` 测试若无法在主机编译 ESP 头，则把 constexpr 规则移到 `wisp_os` 的无平台头里，ESP 端口引用它。
- [ ] **Step 4:** 通过；`bash tools/ci.sh`；固件 `idf.py build`。
- [ ] **Step 5: Commit** `feat(os): add the flash-thread guard and the hot-stack declaration`

---

### Task 2: `StorageWorker` 两条队列与 `run_sync`

**Files:**
- Modify: `components/wisp_core/include/wisp/core/storage_worker.hpp`、`components/wisp_core/src/storage_worker.cpp`、`components/wisp_agent/src/agent_service.cpp`（`run_io` 改用 `run_sync`）、`components/wisp_app/src/scene_wiring.cpp`（开机扫描的信号量等待改用 `run_sync`）
- Test: `tests/core/storage_worker_test.cpp`

**Interfaces:**
- Consumes: Task 1 `set_flash_thread`/`clear_flash_thread`/`on_flash_thread`。
- Produces: `enum class IoLane : uint8_t { Normal, Bulk };`、`bool submit(std::function<void()> job, IoLane lane = IoLane::Normal);`、`Result<void> run_sync(std::function<void()> job, uint32_t timeout_ms, IoLane lane = IoLane::Normal);`（错误码 `Busy`、`Timeout`）、`StorageWorkerConfig::bulk_capacity = 4`、`inline constexpr size_t kIoChunk = 32 * 1024;`。`on_worker()` 改为 `on_flash_thread()` 且线程已注册。
- worker 线程启动后第一件事 `set_flash_thread()`，退出前 `clear_flash_thread()`。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(StorageWorker, NormalJobsOvertakeQueuedBulk)    // block worker; queue B1,B2 (Bulk) then N1 (Normal); release -> order N1,B1,B2
TEST(StorageWorker, BulkQueueHoldsFour)              // 5th Bulk submit while blocked -> false
TEST(StorageWorker, RunSyncWaitsForTheJob)           // returns Ok after job ran on wisp.fs (job records on_worker()==true)
TEST(StorageWorker, RunSyncOnWorkerRunsInline)       // run_sync inside a job completes, no deadlock
TEST(StorageWorker, RunSyncBeforeStartRunsInline)
TEST(StorageWorker, RunSyncTimeoutStillRunsJobSafely) // blocked worker, timeout 50 -> Timeout; job (owning shared_ptr) runs after release
TEST(StorageWorker, RegistersItselfAsFlashThread)    // inside a job os::on_flash_thread() true; on test thread false while running; true after stop()
```

- [ ] **Step 2:** `ctest -R StorageWorker` 失败。
- [ ] **Step 3: 实现。** 两条 `os::Queue` 加一个计数 `os::Semaphore`；停止哨兵走 Normal 队列且在 Bulk 已清空后才生效（`stop()` 仍"执行完所有已排队的任务"）。
- [ ] **Step 4:** 通过（新测试 `--repeat until-fail:10`）；`AgentLoop`、`SceneWiring` 相关测试仍通过；`bash tools/ci.sh`。
- [ ] **Step 5: Commit** `feat(core): give the storage worker a bulk lane and a synchronous call`

---

### Task 3: Settings 缓存后端

**Files:**
- Create: `components/wisp_core/include/wisp/core/cached_settings.hpp`、`components/wisp_core/src/cached_settings.cpp`
- Modify: `components/wisp_core/include/wisp/core/settings.hpp`（`SettingsBackend::load_all()`、`flush_dirty()` 改为由缓存后端驱动的平台接口，见下）、`components/wisp_port_esp/settings_nvs.cpp`、`components/wisp_port_sim/settings_json_file.cpp`
- Test: `tests/core/cached_settings_test.cpp`、现有 settings 测试

**Interfaces:**
- Consumes: Task 2 `StorageWorker::submit/run_sync`。
- Produces:

```cpp
// Platform backends gain:
virtual Result<std::vector<SettingsEntry>> load_all() = 0;   // {ns, key, Value}
virtual Result<void> write_batch(std::span<const SettingsChange> changes) = 0; // set or erase per entry, one commit
struct SettingsEntry { std::string ns, key; Value value; };
struct SettingsChange { std::string ns, key; std::optional<Value> value; }; // nullopt = erase

class CachedSettingsBackend final : public SettingsBackend {
public:
    CachedSettingsBackend(SettingsBackend& platform, StorageWorker& worker,
                          std::function<void()> on_persist_failed);  // called once per 3 consecutive failures
    Result<void> load();                       // run_sync(platform.load_all) — boot
    Result<void> flush(uint32_t timeout_ms);   // waits until no dirty key
    // SettingsBackend: get from cache; set/erase mark dirty + schedule one Normal job; erase_all clears cache and schedules
};
```

- 缓存键值存 PSRAM（`os::LargeString`/`os::LargeVector`）。`Settings` 公有接口不变。
- JSON 文件后端的 `write_batch` 整文件重写一次；NVS 后端的 `write_batch` 每个命名空间 `nvs_open` 一次、最后 `nvs_commit`；`load_all` 用 `nvs_entry_find` 遍历现有命名空间。值格式不变。

- [ ] **Step 1: 写失败的测试**（用一个记录调用线程与次数的 `FakePlatformBackend`）

```cpp
TEST(CachedSettings, GetNeverTouchesThePlatform)
TEST(CachedSettings, RepeatedSetsOfOneKeyPersistOnce)   // 100 sets while worker blocked -> one write_batch with the last value
TEST(CachedSettings, PersistsOnTheFlashThread)          // FakePlatformBackend records on_flash_thread()==true
TEST(CachedSettings, FailedPersistKeepsDirtyAndRetries) // fail once, next set -> batch contains both keys
TEST(CachedSettings, ThreeFailuresNotifyOnce)           // on_persist_failed called once after 3rd consecutive failure; success resets
TEST(CachedSettings, FlushWritesDirtyKeys)              // set; flush(1000) Ok; platform holds value
TEST(CachedSettings, EraseAllClearsCacheAndPlatform)
TEST(SettingsJsonFile, WriteBatchRewritesTheFileOnce)
```

- [ ] **Step 2:** 失败。
- [ ] **Step 3: 实现**，并在 `simulator/main.cpp`、`firmware/main/main.cpp`、`App` 的接线里用 `CachedSettingsBackend` 包住平台后端（worker 启动之后 `load()`；`on_persist_failed` 发系统通知"设置未能保存"，走现有 `post_system` 路径）。
- [ ] **Step 4:** 通过；全部现有 Settings/SystemSettings 测试通过；`bash tools/ci.sh`；固件构建。
- [ ] **Step 5: Commit** `feat(core): serve settings from memory and persist them on the flash thread`

---

### Task 4: 持久化服务、定时刷新、恢复出厂、固件确认与关机顺序

**Files:**
- Modify: `components/wisp_net/src/wifi_service.cpp`、`components/wisp_admin/src/pairing_service.cpp`（以实际路径为准）、`components/wisp_core/src/model_profiles.cpp`、`components/wisp_app/src/app.cpp`（`refresh()` 定时器）、`components/wisp_app/src/factory_reset.cpp`、`components/wisp_app/src/firmware_confirm.cpp`、`Platform::restart`/`request_power_off`/深度待机入口的调用方
- Test: 各服务现有测试文件加用例；`tests/app/factory_reset_test.cpp`；`tests/app/app_test.cpp`（或现有 App 测试）

**Interfaces:**
- Consumes: Task 2 `submit/run_sync`；Task 3 `CachedSettingsBackend::flush`。
- Produces: 三个服务构造时多一个 `std::function<bool(std::function<void()>)> submit_io` 参数（与 `ConversationStore` 的约定一致；为空时同步执行，供无 worker 的测试使用）。`App::flush_storage()`：按 Global Constraints 的关机顺序。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(WifiService, SavesOffTheCallingThread)        // persisted bytes written by a job on the fake worker, not inline
TEST(WifiService, LastSnapshotWins)                // 3 changes while worker blocked -> one write, final state
TEST(PairingService, SavesOffTheCallingThread)
TEST(ModelProfiles, SavesOffTheCallingThread)
TEST(App, StorageRefreshRunsOnTheWorker)           // refresh observed on flash thread; skipped while previous pending
TEST(FactoryReset, ErasesOnTheFlashThread)
TEST(FirmwareConfirm, MarksValidOnTheFlashThread)
TEST(App, RestartFlushesSettingsFirst)             // set; restart path -> platform backend holds value before restart callback runs
```

- [ ] **Step 2:** 失败。
- [ ] **Step 3: 实现**（spec 3.5、3.7、3.8）。
- [ ] **Step 4:** 通过；`bash tools/ci.sh`；固件构建。
- [ ] **Step 5: Commit** `refactor: persist services, reset and firmware confirm on the flash thread`

---

### Task 5: OTA 与场景文件下载经 flash 线程

**Files:**
- Modify: `components/wisp_core/include/wisp/core/firmware_update.hpp`、`components/wisp_core/src/firmware_update.cpp`（`FirmwareUpdateService` 持有 worker，`write/end/confirm` 把 `FirmwareUpdater` 调用经 `run_sync(…, IoLane::Bulk)` 执行；按 `kIoChunk` 攒块）、`components/wisp_admin/src/routes_firmware.cpp`、`components/wisp_admin/src/routes_scene_files.cpp`、`docs/architecture.md`（删除 SceneFiles 的 httpd 例外）
- Test: `tests/core/firmware_update_test.cpp`、`tests/admin/routes_firmware_test.cpp`、`tests/admin/routes_scene_files_test.cpp`（以实际文件名为准）

**Interfaces:**
- Consumes: Task 2 `run_sync`、`kIoChunk`。
- Produces: `FirmwareUpdateService` 构造多一个 `StorageWorker*`（空指针时直接调用，供现有测试）。攒块缓冲用 `shared_ptr` 持有、`os::alloc(Mem::Large)`。
- 下载：每块 `run_sync([buf, …]{ files.read(...) }, timeout, IoLane::Bulk)`，`buf` 为 `shared_ptr` 持有的 PSRAM 缓冲。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(FirmwareUpdate, ChunksReachTheUpdaterOnTheFlashThread)  // fake updater records on_flash_thread()
TEST(FirmwareUpdate, WritesAreBatchedToTheChunkSize)         // 100 x 1 KB writes -> updater sees <= 32 KB chunks, total bytes equal
TEST(FirmwareUpdate, EndRunsOnTheFlashThread)
TEST(RoutesSceneFiles, DownloadReadsOnTheFlashThread)        // bytes match; reads recorded on flash thread
TEST(RoutesFirmware, SettingsWriteDuringUploadIsNotDelayed)  // upload with blocked bulk chunk; Settings set persists before chunk released
```

- [ ] **Step 2:** 失败。
- [ ] **Step 3: 实现**（spec 3.6）。
- [ ] **Step 4:** 通过；`admin_smoke` 通过；`bash tools/ci.sh`；固件构建。
- [ ] **Step 5: Commit** `refactor(admin): stream firmware uploads and file downloads through the flash thread`

---

### Task 6: 打开入口守卫与静态检查

**Files:**
- Modify: `components/wisp_core/src/storage_manager.cpp`（每个公有方法入口 `WISP_ASSERT_FLASH_THREAD`）、`components/wisp_port_esp/settings_nvs.cpp`、`components/wisp_port_sim/settings_json_file.cpp`、ESP 固件更新器、`tools/check_layering.py`、`tools/check_layering_test.py`；以及守卫打开后暴露出的测试和调用点
- Test: 完整 ctest

**Interfaces:**
- Consumes: Task 1–5。
- 静态规则（spec 3.3）：`nvs_`、`esp_partition_`、`esp_ota_` 调用与 `nvs.h`、`esp_ota_ops.h`、`esp_partition.h` 的 include 只允许出现在白名单文件里；白名单每项一行理由。

- [ ] **Step 1: 写失败的测试** — `check_layering_test.py` 新用例：白名单外文件含 `nvs_get_str(` 或 `#include "esp_ota_ops.h"` 报错；白名单内放行。
- [ ] **Step 2:** `python3 tools/check_layering_test.py` 失败。
- [ ] **Step 3: 实现规则与入口守卫**；跑完整 ctest，逐个修复守卫中止的测试或调用点（启动了 worker 又在测试线程直接访问存储的测试改经 `run_sync`）。在报告里列出每个修复点及其线程。
- [ ] **Step 4:** 完整 ctest 全绿（守卫开启）；`python3 tools/check_layering.py`；`bash tools/ci.sh`；固件构建。
- [ ] **Step 5: Commit** `feat: refuse flash access off the flash thread`

---

### Task 7: 任务声明终态与文档

**Files:**
- Modify: `components/wisp_os/include/wisp/os/os.hpp`（`flash_access` 缺省改 false）、`components/wisp_port_esp/include/wisp/port/esp/esp_http_server.hpp`（缺省 false）、所有显式 `flash_access = true` 的创建点（只保留 `wisp.fs`）、`AGENTS.md` 第 5 节、`docs/architecture.md`、`docs/porting-guide.md`、MVP spec 第 4 节任务栈放置
- Test: `tests/os/task_config_test.cpp`（或现有任务配置测试）

**Interfaces:**
- Consumes: Task 6（守卫开启后改声明才安全）。
- 终态见 spec 3.9。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(TaskConfig, DefaultsToNoFlashAccess)   // os::TaskConfig{}.flash_access == false; EspHttpServerConfig likewise (ESP-only header: assert in the port's host-buildable part or skip)
```

`check_layering_test.py` 新用例：`flash_access = true` 或 `flash_access{true}` 只允许出现在 `components/wisp_core/src/storage_worker.cpp`，其他文件出现即报错。

- [ ] **Step 2:** 失败。
- [ ] **Step 3: 实现**；文档按 spec 第 5 节改写（AGENTS.md 与实现同一提交，提交信息另起一行 `docs(agents): keep internal RAM for the flash thread and hot paths`）。
- [ ] **Step 4:** `bash tools/ci.sh`；固件构建；`idf.py size` 记录镜像大小变化。
- [ ] **Step 5: Commit** `refactor: only the flash thread declares flash access`

---

### Task 8: 真机验收（用户执行）

主控烧录后把 spec 第 4 节"真机"6 项交给用户，结果记入 `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md` 的"内部 RAM"一节（改动前基线：`internal free` 79679 B、largest 31744 B）。
