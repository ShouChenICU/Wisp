# Storage Worker Lanes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A settings change, a conversation append, a memory note or a todo never waits behind an SD-card transfer, a scene listing or a stream of scene-data jobs; SD-card file work no longer holds the flash thread; and `mem` shows how long each queue really waits.

**Architecture:** `StorageWorker` gains a third lane, Interactive, drained before Normal and Bulk under a turn rule that bounds starvation, and records the longest wait and job of each lane. A second worker, `wisp.sd.io` (role `SdIo`, stack in PSRAM), runs file work that touches only the SD card; a `StorageRouter` picks the worker per job by `core::Medium`, and `SceneFiles::submit()` routes each scene's file calls so that one scene's calls stay in order. `StorageManager` enforces the split itself: its entry points check the thread by the path's medium (`WISP_ASSERT_MEDIUM_THREAD`), and one lock per medium replaces the single mutex, so internal-flash work never waits for card I/O inside it. Read paths of `SceneRepository` read bytes in one job per scene and parse on the caller; the shell's scene listing and its PNG decoding move to a one-shot task `wisp.shell.list`.

**Tech Stack:** C++20 (`-fno-exceptions -fno-rtti`), GoogleTest, the simulator build (`build/sim`), ESP-IDF FATFS/sdmmc and LittleFS (device side, unchanged APIs).

**Spec:** `docs/superpowers/plans/2026-11-14-architecture-optimization-roadmap.md` (phase G), amending `docs/superpowers/specs/2026-11-06-flash-thread-design.md` sections 1.2, 2.1, 3.1, 3.7, 3.8, 3.9 and AGENTS.md section 5 (Tasks 6, 7 and 11 record the amendments).

## Global Constraints

- Branch: `feat/storage-lanes`, created from `main`. No commits on `main`.
- AGENTS.md applies: `wisp::Result<T>`, no exceptions/RTTI, `Mem::Large` for anything that can exceed a few KB, comments explain why (no review ids, no change history), the layering order (`wisp_os` must not include `wisp_core`).
- Every task ends with `cmake --build build/sim && ctest --test-dir build/sim --output-on-failure -R '<the task's tests>'` green; the last task runs `bash tools/ci.sh`.
- Commit messages: Conventional Commits, ending with the `Co-Authored-By` line of the session's attribution rules.
- A task that changes a rule (who may touch the card, how many lanes there are) updates AGENTS.md section 5, `docs/architecture.md` and the flash-thread spec in its own commit (roadmap "Global Constraints"); the commit subject keeps the code's type and the body names the documents changed.
- Internal-RAM, stack high-water and latency numbers are device checks for the owner; nothing in this plan claims them.

## Review Focus

1. A job on `wisp.sd.io` that waits for `wisp.fs` while `wisp.fs` waits for a card lock (the SceneFiles card lock or StorageManager's SD lock): a deadlock. `StorageWorker::run_sync()` refuses a Flash-role worker from the SD I/O thread, and SceneFiles never loads a record on that thread. Tests in Tasks 4 and 5.
2. A settings save submitted while a Bulk chunk runs and Normal jobs are queued: it runs after the running job and at most one forced turn of a lower lane. Test in Task 3 (`LowerLanesGetATurnAfterEightJobs`).
3. A steady stream of Interactive jobs: Normal and Bulk still run (one turn after `kLaneTurnAfter` higher jobs). Test in Task 3.
4. One scene's `wisp.files` calls when its location becomes known mid-stream (the first write, on `wisp.fs`, picks the card; the next goes to `wisp.sd.io`): the second never runs before the first. Test in Task 5 (`SceneJobsStayInternalWhileAnInternalJobIsInFlight`).
5. The SD card pulled while card work runs or waits: no `deinit` of the card under an in-flight FATFS call, the queued jobs fail `unavailable` rather than crash, and internal-flash work is not held by it. Tests in Tasks 2 (`RefreshWaitsForAnInFlightSdOperation`, `SdOperationDoesNotBlockInternal`) and 6 (`FilesBindingSd.CardPulledDuringAWriteOrAppend` stays green with the SD worker).

---

## Verification of the roadmap against the code

Each roadmap decision was checked against the tree at `22c4629`. Where it does not fit, the plan's choice is recorded under "Deviations".

**Can SD I/O leave the flash thread?** Yes, for work that touches only the card:
- FATFS on the card never touches internal flash (no wear levelling on SD); the card's sectors go through `sdmmc_send_cmd()`, serialized per controller by the host driver's mutex, and FATFS's own per-volume mutex (`FF_FS_REENTRANT=1`); both facts are already relied on in `wisp/port/esp/esp_storage.hpp`. LittleFS has its own per-filesystem lock. Two tasks working on the two mounts at once are safe below `StorageManager`.
- `wisp.sd` already mounts the card and runs `f_getfree()` on a PSRAM stack (architecture task table), so sector reads from a PSRAM stack are exercised today. Buffers handed to FATFS are already in PSRAM from `wisp.fs`; the sdmmc driver bounces non-DMA buffers itself. Writes from a PSRAM stack are a device check (owner check 2).
- While `wisp.fs` erases or writes internal flash, the cache is off: the other core is parked in IRAM and the scheduler on the flash core is suspended, so a PSRAM-stack task is simply not running (with `CONFIG_SPI_FLASH_AUTO_SUSPEND` it keeps running with the cache on). The SDMMC host interrupt is not IRAM-safe, so it is deferred during that window; a 4 KB LittleFS erase (tens of ms) is far inside sdmmc's command timeouts, but this overlap is new for data transfers (today only the CMD13 probe overlaps). Owner check 3 runs an OTA upload and a card download together.
- `WISP_ASSERT_FLASH_THREAD` sits at the top of every `StorageManager` entry point regardless of the path, and `StorageManager` holds one mutex across every operation; `EspSdStorage::online()` relies on that mutex to `deinit` the card with no FATFS call in flight. Both change in Task 2: the guard is checked by medium after normalization, and one I/O lock per medium replaces the mutex; `refresh()` and `status()` take the SD lock, so the `deinit` invariant holds.
- Operations spanning both mounts: `rename()` already refuses two devices, `write_atomic()` stages next to its target, quota accounting is internal only. But jobs span both: SceneFiles keeps each scene's location record on internal flash (`<int>/scenes/<id>/data/files_loc.json`) and its files on the card, and asks the repository for the declaration (`declared`, internal); factory reset, `SceneRepository::remove(with_data)` and `status()` touch both. These stay on `wisp.fs`, which may touch the card (conservative behaviour); only work that is card-only moves.

**Ordering callers rely on today.** Everything is FIFO on one task. Checked per caller:
- Settings: reads come from the cache; saves and `flush()` both go to Interactive, in order. Load at boot is synchronous.
- Conversation: every `ConversationStore` call is a synchronous `run_io` from `wisp.agent`, so append (Interactive) and archive/clear (Normal) stay ordered by the caller waiting.
- Memory and todos: tools run their whole call on the worker and wait; reminders' saves are asynchronous but all go to Interactive; loads are synchronous at boot.
- Scene install then list: install waits for its swap job; the list starts after it returns.
- Scene files: a scene's calls are asynchronous and must stay in order; Task 5's sticky routing keeps them so.
- Factory reset: one job on `wisp.fs`, after `wisp.sd.io` is stopped (its queued work drained first; later card work queues behind the reset).
- Shutdown, reboot, deep standby: `flush_storage()` drains both workers.

**Ordering contract after this phase:** per worker, each lane is FIFO; across lanes and across workers there is no order. A producer that needs two jobs in order across lanes or workers waits for the first (`run_sync`, `call_on_worker`) before submitting the second; every current producer that mixes lanes already does (list above). The one exception is a scene's `wisp.files` stream, ordered per scene by `SceneFiles::submit()`.

---

## Deviations from the roadmap

1. **"`StorageManager` routes by mount point."** `StorageManager` calls are synchronous and run inside jobs, and many jobs span both mounts, so a call cannot move itself to another task. Routing is per job instead: `StorageRouter` (by `Medium`) and `SceneFiles::submit()` (per scene). `StorageManager` *enforces* by mount: the thread check and the lock follow the path's medium.
2. **What moves to `wisp.sd.io`.** Card-only work: SceneFiles calls for scenes whose files are on the card (write, append, read, list, stat, remove, info, `begin_download`, download appends and finish), the per-scene recount after a card change, and `StorageManager::refresh()`. Work that touches both mounts stays on `wisp.fs`, which keeps the right to touch the card: SceneFiles calls for scenes whose location is not known yet, `scan()`, `remove_all()`, the orphan log, `SceneRepository::remove(with_data)`, factory reset, `status()`, the agent's own `@agent` file tools (`ToolThread::Fs`).
3. **Manifest parsing leaves `wisp.fs` only on the read paths a waiting caller uses** (`manifest()`, `manifests()`, `info()`, `list()`). `load()`, `install()`'s swap, `export_zip()`, `removal()`/`remove()`, `recover()`, SceneFiles' `declared` and the tool-index refresh keep parsing inside their job: their file work depends on the parsed manifest, or their caller cannot wait (the scene task, the bus). The `wisp.fs` stack is therefore not shrunk in this phase.
4. **PNG decoding does not run "on the caller's task"**: the caller of the scene list is the UI thread, which must neither wait for the worker nor decode (up to 256×256 per icon, a visible stall). The listing and the decoding move together to a one-shot task `wisp.shell.list` (PSRAM stack); each read is its own worker job.
5. **"Normal jobs longer than one file operation are split"** is applied where splitting keeps atomicity cheaply; other multi-operation jobs stay one job, each with its reason (table in "Contracts").
6. **`wisp.sd.io` stack in PSRAM, with a fallback.** If owner check 2 shows card writes abort from a PSRAM stack, `kSdOwnWorker` in `scene_wiring.cpp` is set to false: the router then sends card work to `wisp.fs` (today's behaviour) and nothing else changes.

---

## Contracts decided by this plan

**Threads.**

| Task | Owner | Stack | `flash_access` / `hot_stack` | Registers | Runs |
|---|---|---|---|---|---|
| `wisp.fs` | `core::StorageWorker` (`WorkerRole::Flash`) | 8192, internal | true / false | `os::set_flash_thread()` | every internal-flash job; may touch the card |
| `wisp.sd.io` | `core::StorageWorker` (`WorkerRole::SdIo`), owned by `SceneWiring` | 8192 (`StorageWorker::sd_io_config()`), PSRAM | false / false | `os::set_sd_io_thread()` | card-only jobs; never internal flash; never waits for `wisp.fs` |
| `wisp.shell.list` | `shell::Shell`, one-shot | 12288 (`Shell::kListStackBytes`), PSRAM, priority 2 | false / false | - | one scene listing and its icon decodes, then exits |

Started only when the board registers `storage.sd` (`StorageManager::has_sd()`); priority 3 like `wisp.fs`. Queue capacities of `wisp.sd.io`: Interactive 1, Normal 16, Bulk 4 (internal RAM for the queue storage stays small). `wisp.fs` gets Interactive 16 (Normal 32, Bulk 4 unchanged).

**Lock order** (outer first; never take an earlier one while holding a later one):
`SceneFiles::io_int_mu_` → `SceneFiles::io_sd_mu_` → `SceneRepository::mutex_` → `StorageManager::int_io_mu_` → `StorageManager::sd_io_mu_` → `StorageManager::state_mu_`. `SceneFiles::mu_` and `FileDownload`'s lock are never held across a `StorageManager` call (as today). On `wisp.sd.io` only `io_sd_mu_` and `sd_io_mu_` (plus `state_mu_`) are ever taken. `wisp.fs` may block on a card lock for at most one card operation; `wisp.sd.io` never blocks on anything `wisp.fs` holds.

**Scheduling rule (one worker).** Lanes in priority order Interactive, Normal, Bulk. `passed[L]` counts the jobs of higher lanes that ran while lane L was non-empty, since L last ran. Picking the next job: if the previous pick was not a forced turn and some non-empty lane has `passed >= kLaneTurnAfter` (8), take the lowest such lane (a forced turn); otherwise take the highest non-empty lane. Running a job of lane L sets `passed[L] = 0` and adds one to `passed` of every non-empty lower lane. Consequences: an Interactive job waits at most for the running job plus one forced job, so Bulk delays it by at most one chunk (`kIoChunk`); under a steady higher load, a waiting Normal or Bulk job runs within 8 higher jobs (plus one forced turn of the other lower lane).

**Jobs longer than one file operation.**

| Job | Decision |
|---|---|
| `SceneRepository::list()`, `manifests()` | split: one job lists the code root, then one job per scene (Task 8) |
| `SceneRepository::info()` | one job (manifest bytes, raw package walk, data size); parse and filtering on the caller (Task 8) |
| `SceneToolIndexObserver::load_installed()` | no longer one job: calls `manifests()` from its own thread (Task 8) |
| shell scene listing (`SceneWiring::shell_scenes()` + icons) | no longer one job: runs on `wisp.shell.list`, each read its own job (Task 9) |
| `SceneFiles::on_sd_changed()` | split: `sd_changed_begin()` on `wisp.fs`, one `recount(id)` per card scene on `wisp.sd.io`, `sd_changed_end()` (orphan log) on `wisp.fs` (Task 5/6) |
| `SceneRepository::load()` | stays one job: its opener (the scene task) cannot wait for a chain; bounded by `max_code_bytes` (512 KB); its duration shows in `mem` (owner check 4) |
| `SceneRepository::remove()` / `removal()` | stay one job: the repository lock across package, previous version, data and card files keeps any list or install from seeing half a scene; rare and user-confirmed |
| factory reset | stays one job (flash-thread spec 3.7), after `StorageRouter::stop_sd()` |
| `recover()` + `SceneFiles::scan()` | stay one boot job, before any service runs |
| install swap, export chunks, OTA chunks | already split (flash-thread spec 3.1) |
| `StorageManager::status()` | one call; slow on LittleFS but a single operation |

**Behaviour at the edges.** Lane full: `submit()` false / `run_sync()` Busy, as today; a full SD lane is not retried on `wisp.fs` (that would break per-scene order). No SD worker (no slot, spawn failure, `kSdOwnWorker` false) or SD worker stopped: `StorageRouter` sends `Medium::Sd` to `wisp.fs`. Stop: `SceneWiring::stop()` drains the router, stops `wisp.sd.io`, then `wisp.fs`. SD card removed: a running card job fails with the filesystem's error; `refresh()` on `wisp.sd.io` then marks the card offline under `sd_io_mu_` and `deinit`s it there; later card jobs fail `StorageUnavailable` ("unavailable" to scripts) without card I/O. A card job whose scene record was removed meanwhile fails Busy (SceneFiles never loads a record on `wisp.sd.io`).

**Memory.** New per-job bookkeeping (enqueue time, 8 bytes) lives in the queue items; lane statistics are atomics inside `StorageWorker` (which sits in `Mem::Large` with `SceneWiring`). Listing results, manifest bytes and package scans are `os::LargeBuffer` / `os::LargeVector`. New internal RAM: two TCBs (`wisp.sd.io` permanent, `wisp.shell.list` while it runs), the SD worker's queues and semaphore, and 16 more queue slots on `wisp.fs` (owner check 1).

---

### Task 1: The SD I/O thread role in the flash guard

**Files:**
- Modify: `components/wisp_os/include/wisp/os/flash_guard.hpp`, `components/wisp_os/src/flash_guard.cpp`
- Test: `tests/os/flash_guard_test.cpp`

**Interfaces:**
- Produces in `wisp::os`:
  - `void set_sd_io_thread();` `void clear_sd_io_thread();` `bool on_sd_io_thread();` (false when none is registered; same take-over and stale-clear rules as the flash thread).
  - `bool sd_io_allowed();` true on the registered SD I/O thread or wherever `on_flash_thread()` is true.
  - Changed: `on_flash_thread()` is false on the registered SD I/O thread, always (even with no flash thread registered): that thread must never touch internal flash.
  - Macros: `WISP_ASSERT_SD_IO_THREAD(what)` (host: prints `"sd access off the sd i/o thread: <what>"` and aborts unless `sd_io_allowed()`; device: reports once per call site through the existing reporter) and `WISP_ASSERT_MEDIUM_THREAD(on_sd, what)` (`on_sd` a bool: the SD rule when true, else `WISP_ASSERT_FLASH_THREAD`).
  - `detail::sd_guard_abort_if_violated(const char* what);`

- [ ] **Step 1: Write the failing tests** in `tests/os/flash_guard_test.cpp`, using the file's `Task`/`Semaphore` pattern:
  - `FlashGuard.SdIoThreadRegistersLikeTheFlashThread`: a task calls `set_sd_io_thread()`; there `on_sd_io_thread()` is true; on the test thread false; after the task's `clear_sd_io_thread()` false.
  - `FlashGuard.SdIoThreadIsNeverTheFlashThread`: no flash thread registered; on the SD task `on_flash_thread()` is false; on the test thread true.
  - `FlashGuard.SdRuleAllowsTheSdAndFlashThreads`: a flash task F and an SD task S registered; `sd_io_allowed()` true on F and on S, false on the test thread.
  - `FlashGuard.SdRuleAllowsEverythingWithNothingRegistered`: `sd_io_allowed()` true on the test thread.
  - `FlashGuardDeathTest.SdAssertAbortsOffBothThreads` (threadsafe style, as `AssertAbortsOffTheFlashThread`): F and S registered; `WISP_ASSERT_SD_IO_THREAD("StorageManager::append")` on the test thread dies matching `"sd access off the sd i/o thread.*append"`.
  - `FlashGuardDeathTest.FlashAssertAbortsOnTheSdIoThread`: the death statement spawns a task that calls `set_sd_io_thread()` then `WISP_ASSERT_FLASH_THREAD("x")` and joins it; dies matching `"flash access off the flash thread"`.
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R FlashGuard --output-on-failure`. Expected: compile failure (new functions).
- [ ] **Step 3: Implement** with a second `thread_local` id and atomic owner, mirroring the flash thread's. Update the file header (the SD I/O thread, what each rule allows).
- [ ] **Step 4: Run** the same filter. Expected: PASS, including the existing guard tests.
- [ ] **Step 5: Commit** `feat(os): add the sd i/o thread role to the flash guard`.

---

### Task 2: `StorageManager` checks and locks by medium

**Files:**
- Modify: `components/wisp_core/include/wisp/core/storage_manager.hpp`, `components/wisp_core/src/storage_manager.cpp`
- Modify: `components/wisp_port_esp/include/wisp/port/esp/esp_storage.hpp` (the comment that says `online()` runs under "StorageManager's own mutex": now `sd_io_mu_`)
- Test: `tests/core/storage_manager_test.cpp`

**Interfaces:**
- Consumes: `WISP_ASSERT_MEDIUM_THREAD` (Task 1).
- Produces:
  - `enum class Medium : uint8_t { Internal, Sd };` in `storage_manager.hpp`.
  - `Medium StorageManager::medium_of(const std::string& path) const;` normalizes `path`; `Sd` when it is at or under storage.sd's mount (online or not), else `Internal` (paths outside both mounts, malformed paths and every path before `init()` are `Internal`). No file work, `state_mu_` only.
  - `bool StorageManager::has_sd() const;` whether `storage.sd` is registered (after `init()`).
  - Locks: `mutable os::Mutex state_mu_` (ready, mounts, `sd_online_`; never held across I/O), `os::Mutex int_io_mu_`, `os::Mutex sd_io_mu_` (each held across one operation's I/O on its medium). Order: `int_io_mu_` → `sd_io_mu_` → `state_mu_`.
  - Per entry point: `write_atomic`, `read_file`, `read_file_large`, `read_range`, `append`, `list_dir`, `visit_dir`, `remove_tree`, `tree_size` normalize, take the medium, `WISP_ASSERT_MEDIUM_THREAD(medium == Medium::Sd, "<name>")`, then that medium's I/O lock; `rename` does the same with `from`'s medium (both ends are on one device already). `init()` and `status()`: flash rule, both I/O locks. `refresh()`: SD rule, `sd_io_mu_` (it calls `online()`, which may `deinit` the card). `roots()`, `write_root()`, `internal_root()`, `medium_of()`, `has_sd()`: `state_mu_` only, no guard. `set_standby()` unchanged (no lock). `visit_dir`'s callback runs with that medium's I/O lock held (header comment updated).
  - The offline-SD check moves inside `sd_io_mu_` (re-read under `state_mu_` after taking it), so a removal seen by `refresh()` is seen by the next SD operation.

- [ ] **Step 1: Write the failing tests** with the file's `Fixture(sd_online = true)`:
  - `StorageManager.MediumOfPaths`: before `init()`, `medium_of(f.sd + "/x") == Medium::Internal`; after: `f.sd + "/wisp/library/a"` is `Sd`; `f.sd + "//wisp/./library"` is `Sd`; `f.in + "/system/a"` is `Internal`; `"/elsewhere/a"` is `Internal`; after `sd_dev->online_flag = false; refresh()`, `f.sd + "/x"` is still `Sd`.
  - `StorageManager.HasSd`: true with the fixture; false for a registry holding only `storage.int`.
  - `StorageManager.SdOperationDoesNotBlockInternal`: write `f.sd + "/wisp/library/d/f"`; thread A calls `visit_dir(f.sd + "/wisp/library/d", cb)` where `cb` gives `inside` and takes `release`; after `inside`, thread B calls `write_atomic(f.in + "/system/x", ...)` and gives `done`; `ASSERT_TRUE(done.take(5000))` while A still holds the card; then give `release`, join both.
  - `StorageManager.SdOperationsSerialize`: as above, B appends to an SD file instead; `EXPECT_FALSE(done.take(100))` (can only pass falsely, never fail falsely), give `release`, `ASSERT_TRUE(done.take(5000))`.
  - `StorageManager.RefreshWaitsForAnInFlightSdOperation`: A holds the card in `visit_dir`; `sd_dev->online_flag = false`; `sd_dev->on_online` gives `probed`; thread B calls `refresh()`; `EXPECT_FALSE(probed.take(100))`; release; `ASSERT_TRUE(probed.take(5000))`; after `bus.wait_idle()` `collected_topics()` ends with `"storage.sd.removed"`.
  - `StorageManager.SdWriteOnTheFlashThreadIsAllowed`: a task registered with `os::set_flash_thread()` writes `f.sd + "/wisp/library/a"`; ok.
  - `StorageManager.SdWriteOnTheSdIoThreadIsAllowed`: a task registered with `os::set_sd_io_thread()` writes and reads it; ok; it also calls `refresh()` without aborting.
  - `StorageManagerDeathTest.InternalWriteOnTheSdIoThreadAborts` (threadsafe style): the death statement spawns a task that registers as the SD I/O thread and calls `write_atomic(f.in + "/system/a", ...)`; dies matching `"flash access off the flash thread.*write_atomic"`.
  - `StorageManagerDeathTest.SdWriteOffBothThreadsAborts`: a flash task registered and parked; the test thread's `append(f.sd + "/wisp/library/a", ...)` dies matching `"sd access off the sd i/o thread.*append"`.
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R StorageManager --output-on-failure`. Expected: compile failure, then the concurrency tests fail (one mutex) and the death tests print the old message.
- [ ] **Step 3: Implement.** Keep `calls()` counting as it is. Update the header's "Every public method here serializes on one mutex" paragraph and `set_standby()`'s comment to the per-medium locks. The ESP comment change is wording only.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'StorageManager|SceneFiles|SceneRepository|FilesBinding' --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `refactor(core): check and lock storage by medium`.

---

### Task 3: Three lanes, the turn rule and lane statistics

**Files:**
- Modify: `components/wisp_core/include/wisp/core/storage_worker.hpp`, `components/wisp_core/src/storage_worker.cpp`
- Test: `tests/core/storage_worker_test.cpp`

**Interfaces:**
- Produces:
  - `enum class IoLane : uint8_t { Interactive, Normal, Bulk };` (Normal stays every default argument).
  - `inline constexpr size_t kLaneCount = 3;` `inline constexpr uint32_t kLaneTurnAfter = 8;`
  - `StorageWorkerConfig` gains, in this order before `queue_capacity`: `size_t interactive_capacity = 16;` and at the end `std::function<uint64_t()> now_us;` (empty: `os::now_us`; tests inject a clock).
  - `struct LaneStats { uint64_t jobs = 0; uint64_t max_wait_us = 0; uint64_t max_job_us = 0; };` and `LaneStats StorageWorker::stats(IoLane lane) const;` (since `start()`; wait = start of the job minus its enqueue time; job = its run time; jobs run inline by `run_sync()` are not counted). Stored as relaxed atomics written only by the worker thread.
  - Queue items become `struct Item { std::function<void()> fn; uint64_t queued_us; }`.
  - Scheduling: the rule in "Contracts". `stop()` no longer uses a sentinel: it clears `accepting_` under the mutex, sets an atomic `stopping_`, gives `work_` once and joins; the worker exits on a wake-up that finds every lane empty while `stopping_` is set. `work_`'s limit is the three capacities plus one.

- [ ] **Step 1: Write the failing tests** (the file's `block_worker()` and `ReleaseOnExit`; `record(name)` as in `NormalJobsOvertakeQueuedBulk`):
  - `StorageWorker.InteractiveOvertakesQueuedNormalAndBulk`: block; submit B1 (Bulk), N1 (Normal), I1, I2 (Interactive); release; order `{I1, I2, N1, B1}`.
  - `StorageWorker.LowerLanesGetATurnAfterEightJobs`: block; submit B1, N1, then I1..I9; release; order exactly `{I1..I8, B1, I9, N1}`.
  - `StorageWorker.InteractiveRunsRightAfterTheRunningBulkJob`: block with a Bulk gate job instead; submit N1..N3 then I1; release; `order.front() == "I1"`.
  - `StorageWorker.InteractiveLaneHoldsItsCapacity`: `interactive_capacity = 2`; block; two Interactive submits true, the third false; `run_sync(job, 0, IoLane::Interactive)` is Busy.
  - `StorageWorker.StatsRecordLongestWaitAndJob`: `std::atomic<uint64_t> clock{0}`, `now_us = [&]{ return clock.load(); }`; submit Normal gate G (waits on `release`); submit I1 whose body does `clock += 3000`; set `clock = 7000`; release; drain. `stats(Interactive)` == `{1, 7000, 3000}`; `stats(Normal).jobs == 1`, `.max_wait_us == 0`, `.max_job_us == 7000`; `stats(Bulk).jobs == 0`.
  - `StorageWorker.InlineRunsAreNotCounted`: `run_sync` before `start()`; after `start()` and `drain()`, every lane's `jobs == 0`.
  - Rename `StopRunsQueuedJobsInBothLanes` to `StopRunsQueuedJobsInEveryLane` and add an Interactive job to it; all three run before `stop()` returns.
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R StorageWorker --output-on-failure`. Expected: compile failure, then FAIL.
- [ ] **Step 3: Implement.** Rewrite the file header's lane paragraph (three lanes, the turn rule, the bound on an Interactive job's wait).
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'StorageWorker|CachedSettings|SceneRepository|FirmwareUpdate' --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): add the interactive storage lane and lane statistics`.

---

### Task 4: Worker roles and `StorageRouter`

**Files:**
- Modify: `components/wisp_core/include/wisp/core/storage_worker.hpp`, `components/wisp_core/src/storage_worker.cpp`
- Create: `components/wisp_core/include/wisp/core/storage_router.hpp`, `components/wisp_core/src/storage_router.cpp` (add to `components/wisp_core/CMakeLists.txt`)
- Create: `tests/core/storage_router_test.cpp` (register in `tests/CMakeLists.txt` next to `storage_worker_test`, `LIBS wisp_core wisp_port_sim_os TIMEOUT 20`)
- Test: `tests/core/storage_worker_test.cpp`

**Interfaces:**
- Consumes: Task 1's roles, Task 2's `Medium`, Task 3's lanes.
- Produces in `storage_worker.hpp`:
  - `enum class WorkerRole : uint8_t { Flash, SdIo };`; `StorageWorkerConfig` gains, first: `const char* name = "wisp.fs";` `WorkerRole role = WorkerRole::Flash;`.
  - `static StorageWorkerConfig StorageWorker::sd_io_config();` → `{.name = "wisp.sd.io", .role = WorkerRole::SdIo, .interactive_capacity = 1, .queue_capacity = 16, .bulk_capacity = 4, .stack_bytes = 8192, .priority = 3}`.
  - `task_config()`: `flash_access = (role == Flash)`, `hot_stack = false`, `name` from the config. `run()` registers `set_flash_thread()` or `set_sd_io_thread()` by role and clears it on exit.
  - `bool running() const;` (between a successful `start()` and the start of `stop()`), `const char* name() const;`, `WorkerRole role() const;`.
  - `run_sync()` on a Flash-role worker called on the SD I/O thread (`os::on_sd_io_thread()`): `ErrorCode::InvalidArgument` ("the sd worker must not wait for the flash worker"), the job never runs, checked before the inline paths. `call_on_worker()` returns that error without retrying.
- Produces in `storage_router.hpp`:

```cpp
class StorageRouter {
public:
    explicit StorageRouter(StorageWorker& flash, StorageWorker* sd = nullptr);
    StorageWorker& flash() const;
    StorageWorker* sd() const;               // null without one
    // The SD worker for Medium::Sd while it is running(), else the flash worker.
    StorageWorker& worker_for(Medium m) const;
    bool on_worker() const;                  // on either worker's thread
    // Medium::Sd while the SD worker runs goes there; a refusal because it
    // stopped meanwhile is retried on the flash worker, a full lane is not.
    bool submit(Medium m, std::function<void()> job, IoLane lane = IoLane::Normal);
    Result<void> run_sync(Medium m, std::function<void()> job, uint32_t timeout_ms,
                          IoLane lane = IoLane::Normal);
    // Until both are idle at the same check, or the timeout.
    bool drain(uint32_t timeout_ms);
    size_t pending() const;
    // Drains and joins the SD worker; from then on Medium::Sd goes to the
    // flash worker. Idempotent; a no-op without one.
    void stop_sd();
};

template <typename T, typename Fn>
Result<T> call_on(StorageRouter* router, Medium m, Fn&& fn, IoLane lane = IoLane::Normal,
                  uint32_t busy_retry_ms = 2000);  // null router: fn() inline
```

- [ ] **Step 1: Write the failing tests.**
  - `storage_worker_test.cpp`: `StorageWorker.SdIoConfig` (`task_config(sd_io_config())`: name `"wisp.sd.io"`, `flash_access` false, `hot_stack` false, `stack_bytes` 8192, priority 3; `StackIsInternal` unchanged); `StorageWorker.SdIoWorkerRegistersItsThread` (inside a job of an SdIo worker `os::on_sd_io_thread()` is true and `os::on_flash_thread()` false); `StorageWorker.RunningTracksStartAndStop`; `StorageWorker.RunSyncFromTheSdIoThreadOntoTheFlashWorkerIsRefused` (flash worker F, SD worker S; inside an S job, `F.run_sync(set_flag, kForever)` is InvalidArgument and the flag stays false; the same with F never started, so no inline run either).
  - `storage_router_test.cpp`: `StorageRouter.SdJobsRunOnTheSdWorker` (job records `sd.on_worker()` true); `StorageRouter.InternalJobsRunOnTheFlashWorker`; `StorageRouter.WithoutAnSdWorkerSdJobsRunOnTheFlashWorker`; `StorageRouter.AfterStopSdSdJobsRunOnTheFlashWorker` (`stop_sd()`, then `submit(Sd)` and `run_sync(Sd)` both record `flash.on_worker()` true, never the test thread); `StorageRouter.StopSdRunsQueuedSdJobsFirst` (gate the SD worker, queue S1, call `stop_sd()` on a thread, release; S1 ran before `stop_sd()` returned); `StorageRouter.DrainWaitsForBothWorkers` (a gate job in each; `drain(100)` false; release both; `drain(5000)` true, `pending() == 0`); `StorageRouter.CallOnReturnsTheResult` (`call_on<int>(&r, Medium::Sd, []{ return Result<int>::ok(7); })` is 7; with a null router it runs inline).
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'StorageWorker|StorageRouter' --output-on-failure`. Expected: FAIL to compile.
- [ ] **Step 3: Implement.** `storage_router.hpp`'s header states the ordering contract ("Contracts") and the rule that the SD worker never waits for the flash worker.
- [ ] **Step 4: Run** the same filter plus `-R FlashGuard`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): add worker roles and the storage router`.

---

### Task 5: `SceneFiles` routes each scene's calls

**Files:**
- Modify: `components/wisp_core/include/wisp/core/scene_files.hpp`, `components/wisp_core/src/scene_files.cpp`
- Modify: `components/wisp_scene/src/scene_runtime.cpp` (`wire_files_api`: `files_run`, the download's `begin` and `finish`, `close_gate`)
- Modify: `components/wisp_app/src/scene_wiring.cpp` (`files_config.submit_io`: the new signature, still the flash worker only; the SD event subscriber calls `on_sd_changed()` as before)
- Test: `tests/core/scene_files_test.cpp`

**Interfaces:**
- Consumes: `Medium` (Task 2), `IoLane` (Task 3), `os::on_sd_io_thread()` (Task 1).
- Produces:
  - `SceneFilesConfig::submit_io` becomes `std::function<bool(Medium, IoLane, std::function<void()>)>` (empty: run on the caller, as today).
  - `SceneFilesConfig::on_io`: `std::function<void(std::string_view scene)>`, a test seam called with the scene's I/O lock held before its file work; empty in production.
  - `bool SceneFiles::submit(std::string_view scene, std::function<void()> job, IoLane lane = IoLane::Normal);` Routing: `Medium::Internal` while the scene has a job routed Internal that has not finished, or its record is not loaded, or its location is not `Sd`; otherwise `Medium::Sd`. Each Internal job is counted in `Record::int_in_flight` from submission to the end of the job (a refused submission undoes the count). The location never moves from Sd back to Internal at run time (it changes only through `scan()` at boot and `remove_all()`), so a scene's jobs keep their order.
  - `Medium SceneFiles::medium_of(std::string_view scene) const;` the routing above without counting (memory only).
  - `template <typename T, typename Fn> Result<T> run_files(SceneFiles& files, std::string_view scene, Fn fn, uint32_t timeout_ms = os::kForever, IoLane lane = IoLane::Normal);` runs `fn(files)` through `submit()` and waits (inline on the flash thread, or on the SD I/O thread when the scene's medium is Sd); Busy if refused, Timeout if late (then `fn` must own its captures).
  - `Record` gains `uint32_t int_in_flight = 0;` and `FilesWant want = FilesWant::Auto;` (cached from `declared` when the record loads; `info()` reports it).
  - Locks: `io_mu_` splits into `io_int_mu_` and `io_sd_mu_`. On `wisp.sd.io`, a public method takes `io_sd_mu_` only; if the scene's record is not loaded it fails `Busy` ("busy") without loading, choosing or calling `declared`. Elsewhere it takes `io_int_mu_`, plus `io_sd_mu_` unless the record is loaded with location Internal. `scan()`, `remove_all()`, `sd_changed_begin()`, `sd_changed_end()` take both.
  - `on_sd_changed()` splits, and stays as their composition: `std::vector<std::string> sd_changed_begin();` (marks card scenes uncounted, returns them; empty while the card is out), `Result<void> recount(std::string_view scene);` (card-only, `io_sd_mu_`; a scene not on the card is a no-op), `void sd_changed_end();` (the orphan log; flash).
  - `FileDownload` dispatches its appends and is finished through `SceneFiles::submit(scene, ...)`; it gains `const std::string& scene() const;`.

- [ ] **Step 1: Write the failing tests** with the file's `Fixture` and a fake `submit_io` that records `(Medium, lane)` and either runs the job at once or keeps it in a `std::vector<std::function<void()>> held` (field `hold`):
  - `SceneFiles.UnknownSceneJobsGoToInternal`: `submit("a", job)` with no record records `Medium::Internal`.
  - `SceneFiles.CardSceneJobsGoToTheCardOnceLoaded`: `declared["a"] = FilesWant::Sd`, `declared["b"] = FilesWant::Int`; direct `write("a", ...)` and `write("b", ...)` on the test thread; then `submit("a")` records `Sd`, `submit("b")` records `Internal`; `medium_of("a") == Medium::Sd`.
  - `SceneFiles.SceneJobsStayInternalWhileAnInternalJobIsInFlight`: `hold = true`; `submit("a", J1)` (Internal, held); a direct `write("a", ...)` loads the record as Sd; `submit("a", J2)` records `Internal`; run and clear `held`; `submit("a", J3)` records `Sd`. Recorded media `{Internal, Internal, Sd}`.
  - `SceneFiles.RefusedSubmitLeavesNoJobInFlight`: `submit_io` returns false once; `submit("a", J)` is false and J never runs; after a direct write, `submit("a")` records `Sd`.
  - `SceneFiles.SdThreadNeverLoadsARecord`: an SD worker `S(StorageWorker::sd_io_config())`; on S, `write("x", ...)` for an unknown scene is Busy, `fs::exists(record("x"))` is false and the `declared` call count is unchanged.
  - `SceneFiles.SdDataCallsNeverAskTheDeclaration`: `declared["a"] = Sd`; direct write on the test thread; note the `declared` count n; on S: `write`, `append`, `read`, `list`, `stat`, `remove`, `info` all ok; count still n; `info("a").want == FilesWant::Sd`.
  - `SceneFiles.CardWorkDoesNotHoldInternalScenes`: scenes `c` (card) and `i` (internal) loaded; `on_io` blocks on `release` when called for `c`; S runs `write("c", ...)`; after `on_io` signalled, the test thread's `write("i", ...)` returns ok; release.
  - `SceneFiles.RecountRunsPerSceneOnTheCard`: two card scenes; a file copied into one's card dir behind its back; `set_sd(false)`, `set_sd(true)`; `sd_changed_begin()` returns both ids; `recount(id)` on S for each is ok; `info()` shows the new usage; `sd_changed_end()` on the test thread logs nothing new.
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SceneFiles' --output-on-failure`. Expected: FAIL to compile.
- [ ] **Step 3: Implement.** `scene_runtime.cpp`: `files_run` becomes `files.submit(id, ...)` (refusal → `busy(cb)` as now); `begin` and `finish` go through `files.submit(id, ...)`; `close_gate` uses `files.submit(dl->scene(), ...)`. `scene_wiring.cpp`: the lambda ignores the medium and calls `worker_.submit(job, lane)` until Task 6. Update the file header of `scene_files.hpp` (threads, the routing rule, the lock split).
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'SceneFiles|FilesBinding|AdminSceneFiles|AppScenes' --output-on-failure`. Expected: PASS, including `SdRemovedMidDownload` and `PushWaitsForTheWorkerUntilCancelled`.
- [ ] **Step 5: Commit** `feat(core): route each scene's file calls by where its files live`.

---

### Task 6: `wisp.sd.io` in the app

**Files:**
- Modify: `components/wisp_app/src/scene_wiring.hpp`, `components/wisp_app/src/scene_wiring.cpp` (`start_storage`, `stop`, `files_config.submit_io`, the SD event subscriber)
- Modify: `components/wisp_app/src/app.cpp` (`queue_storage_refresh`, `flush_storage`, `storage_router()`), `components/wisp_app/include/wisp/app/app.hpp`, `components/wisp_app/src/storage_io.hpp`
- Modify: `components/wisp_app/src/factory_reset.cpp`
- Modify: `components/wisp_admin/src/routes_scene_files.cpp` (the file routes)
- Modify: AGENTS.md section 5, `docs/superpowers/specs/2026-11-06-flash-thread-design.md` (1.2 goal 1, 2.1, 3.7 rows for `refresh()` and factory reset, 3.8, 3.9), `docs/architecture.md` (task table rows `wisp.fs`, `wisp.sd`, new `wisp.sd.io`; boot-flow step 3's refresh sentence; "Scene storage")
- Test: `tests/app/app_boot_test.cpp`, `tests/app/factory_reset_test.cpp`, `tests/scene/files_binding_test.cpp`, `tests/admin/routes_scene_files_test.cpp`

**Interfaces:**
- Consumes: Tasks 2-5.
- Produces:
  - `SceneWiring` owns `core::StorageWorker sd_worker_{core::StorageWorker::sd_io_config()};` and `core::StorageRouter router_;` (`router_` built with `&sd_worker_` only when it started). `core::StorageRouter& router();` `constexpr bool kSdOwnWorker = true;` in `scene_wiring.cpp` (the fallback switch, Deviation 6).
  - `start_storage()`: start `wisp.fs`; then, when `kSdOwnWorker && deps_.storage.has_sd()`, start `wisp.sd.io` (failure: `WLOG_W`, card work stays on `wisp.fs`); then the boot `recover()` + `scan()` job as now.
  - `files_config.submit_io = [this](Medium m, IoLane l, auto job) { return router_.submit(m, std::move(job), l); };`
  - SD events: `sd_changed_begin()` on `wisp.fs` (submit), whose job submits one `recount(id)` per id to `Medium::Sd` and then `sd_changed_end()` to `wisp.fs`.
  - `stop()`: `router_.drain(kDrainMs)`, `router_.stop_sd()`, then `worker_.stop()`.
  - `core::StorageRouter* App::storage_router();` (null without scenes). `queue_storage_refresh()` submits `refresh()` with `Medium::Sd` through the router (inline without one, as now). `flush_storage()` drains the router instead of the flash worker, with the same `kShutdownDrainMs`.
  - Factory reset: `app.storage_router()->stop_sd()` right before the erase job is submitted.
  - Admin file routes (stat, read, write, remove, list, info) run through `core::run_files<T>(*files, id, fn, kFlashWaitMs)`; the read route keeps `IoLane::Bulk`. `scenes->info(id)` (line 103) stays on `wisp.fs` until Task 8.

- [ ] **Step 1: Write the failing tests.**
  - `AppBoot.SdWorkerRunsWithACardSlot`: a board registering `storage.sd` (FakeStorage); after start, `app.storage_router()->sd() != nullptr` and `running()`.
  - `AppBoot.NoSdWorkerWithoutACardSlot`: board without it; `sd() == nullptr`.
  - `AppBoot.StorageRefreshRunsOnTheSdWorker`: the SD FakeStorage's `on_online` records `os::on_sd_io_thread()`; `queue_storage_refresh(app)`; `wait_until` the hook ran; recorded true.
  - `AppBoot.FlushStorageDrainsTheSdWorker`: a gate job on `router->sd()`; `flush_storage()` on a thread; release after it started; it returns with `router->pending() == 0`.
  - `FactoryReset.StopsTheSdWorkerBeforeErasing`: with a card; after the reset, `router->sd()->running()` is false and `<sd>/wisp/library/agent` is gone (extends `ClearsAgentFiles`).
  - `FilesBindingSd.CardFilesRunOnTheSdWorker`: the fixture builds its router with an SD worker; a scene declaring `"files": "sd"` writes twice; `on_io` records `os::on_sd_io_thread()` per call: `{false, true}`.
  - `AdminSceneFiles.CardFileReadRunsOnTheSdWorker`: a card scene's file read through the route; `on_io` records true.
  - `FilesBindingSd.CardPulledDuringAWriteOrAppend` stays green unchanged (Review Focus 5).
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'AppBoot|FactoryReset|FilesBinding|AdminSceneFiles' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement**, then the documents: AGENTS.md section 5 gains, after the `wisp.fs` sentence, "File work that touches only the SD card runs on `wisp.sd.io` (`core::StorageRouter`, `Medium::Sd`); `wisp.fs` may also touch the card; `wisp.sd.io` never touches internal flash and never waits for `wisp.fs`." The spec's 2.1 rule and 3.9 table say the same, with the lock order; 3.7's refresh row reads "submitted with `Medium::Sd`"; 3.8 says both workers are drained.
- [ ] **Step 4: Run** `ctest --test-dir build/sim --output-on-failure` (the whole suite: the guard aborts on any card or flash access left on the wrong thread). Expected: PASS.
- [ ] **Step 5: Commit** `feat(app): run sd card file work on wisp.sd.io`.

---

### Task 7: Interactive callers

**Files:**
- Modify: `components/wisp_core/src/cached_settings.cpp` (`schedule()`, `flush()`), `components/wisp_core/include/wisp/core/cached_settings.hpp` (comment)
- Modify: `components/wisp_app/src/settings_front.hpp` (`forward()`)
- Modify: `components/wisp_agent/include/wisp/agent/agent_service.hpp`, `components/wisp_agent/src/agent_service.cpp` (the append at line ~208)
- Modify: `components/wisp_agent/include/wisp/agent/tool_registry.hpp`, `components/wisp_agent/src/tool_registry.cpp`, `components/wisp_agent/src/tools_memory.cpp`, `components/wisp_agent/src/tools_todo.cpp`
- Modify: `components/wisp_admin/src/routes_memory.cpp`
- Modify: `components/wisp_app/src/app_impl.hpp`, `components/wisp_app/src/app.cpp` (`run_io`, reminders' `submit_io`, `AgentDeps`), `components/wisp_app/src/agent_wiring.cpp` (`ToolExecutors`)
- Modify: AGENTS.md section 5 ("There are three queues: Interactive (settings saves, conversation appends, memory notes, todos), Normal (other JSON documents, scene data), Bulk (firmware chunks, file download chunks)"), flash-thread spec 3.1, `docs/architecture.md` storage section
- Test: `tests/core/cached_settings_test.cpp`, `tests/app/settings_front_test.cpp`, `tests/agent/agent_loop_test.cpp`, `tests/agent/tool_registry_test.cpp`, `tests/agent/memory_tools_test.cpp`, `tests/agent/todo_tools_test.cpp`, `tests/admin/routes_memory_test.cpp`, `tests/support/agent_fixture.hpp`

**Interfaces:**
- Consumes: `IoLane::Interactive` (Task 3).
- Produces:
  - `CachedSettingsBackend`: saves (`schedule()`, also its `run_sync(job, 0)` fallback) and `flush()` use `IoLane::Interactive`; `load()` stays Normal.
  - `SettingsFront::forward()`: `call_on_worker<T>(worker, ..., IoLane::Interactive)`.
  - `AgentDeps::run_io_interactive`: `std::function<Result<void>(std::function<void()>)>`, empty → `run_io`. `ConversationStore::append` goes through it; `archive`, `clear_all`, `load` keep `run_io`.
  - `ToolThread::FsInteractive`; `ToolExecutors::fs_interactive` (empty → `fs`). Every memory and todo tool declares it.
  - `App::Impl::run_io(job, timeout_ms, busy_budget_ms, core::IoLane lane = core::IoLane::Normal)`; the agent's `run_io_interactive` and the reminders' `submit_io` use Interactive; `agent_wiring.cpp` sets `.fs_interactive` to a submit on Interactive.
  - `routes_memory.cpp`'s `on_flash` uses `IoLane::Interactive`.

- [ ] **Step 1: Write the failing tests** (an order vector shared by the jobs and the fake backend; the worker blocked first with a gate):
  - `CachedSettings.SavesOvertakeANormalBacklog`: block; queue N1, N2 (Normal, recording); `set("audio", "volume", 3)`; release; drain; order `{save, N1, N2}` (the fake backend records `"save"` in `write_batch`).
  - `CachedSettings.FlushWaitsOnTheInteractiveLane`: block; queue N1; set a key; `flush(5000)` on a thread; release; order `{save, N1}` and `flush` ok.
  - `SettingsFront.DirectCallsOvertakeANormalBacklog`: front without a cache, worker set and blocked; queue N1; `set()` on a thread; release; the platform write precedes N1.
  - `AgentLoop.AppendsUseTheInteractiveIo`: the fixture counts calls of `run_io_interactive` and `run_io`; a one-reply turn appends through `run_io_interactive` only; `new_conversation()` archives through `run_io`.
  - `ToolRegistry.FsInteractiveToolsUseTheirExecutor` and `ToolRegistry.FsInteractiveFallsBackToFs`.
  - `MemoryTools.DeclareTheInteractiveThread` and `TodoTools.DeclareTheInteractiveThread`: `registry.find(name)->thread == ToolThread::FsInteractive` for every tool of the set.
  - `MemoryRoutes.RemoveOvertakesANormalBacklog`: block; queue N1; DELETE a note; release; the removal ran before N1.
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'CachedSettings|SettingsFront|AgentLoop|ToolRegistry|MemoryTools|TodoTools|MemoryRoutes' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement**, then the documents (the AGENTS.md sentence above; spec 3.1's lane list, the turn rule and the wait bound; architecture's storage section).
- [ ] **Step 4: Run** the same filter plus `-R 'Reminders|AppBoot|App'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): put settings, appends, memory and todos on the interactive lane`.

---

### Task 8: Repository read paths: one job per scene, parsing on the caller

**Files:**
- Modify: `components/wisp_core/include/wisp/core/scene_repository.hpp`, `components/wisp_core/src/scene_repository.cpp` (`manifest`, `manifests`, `info`, `list` and their `_here` helpers)
- Modify: `components/wisp_app/src/agent_wiring.cpp` (`SceneToolIndexObserver::load_installed`), `components/wisp_agent/src/scene_workspace.cpp` (the `repo.list()` inside the `call_on_worker` at line ~240), `components/wisp_admin/src/routes_scene_files.cpp` (line ~103: call `scenes->info(id)` directly)
- Test: `tests/core/scene_repository_test.cpp`, `tests/app/scene_tool_index_observer_test.cpp`

**Interfaces:**
- Produces:
  - `SceneRepoConfig::on_read` (`std::function<void(std::string_view id)>`, called inside the worker job that reads scene `id` for a read path) and `SceneRepoConfig::on_parse` (`std::function<void(std::string_view id)>`, called on the thread that parses it). Test seams; empty in production.
  - Private: `struct SceneScan { std::string id; os::LargeBuffer manifest; os::LargeVector<std::pair<std::string, uint64_t>> files; uint64_t data_bytes = 0; };` `Result<SceneScan> scan_here(std::string_view id, bool with_files);` (worker, repository lock: `scene.json` bytes; with files: every top-level `scene.json`/`*.lua` and every regular file under `lib/` and `assets/`, at most `kMaxFiles + 1` each, with sizes, and `tree_size(data_dir(id))`); `Result<std::vector<std::string>> ids_here();` (worker: valid scene ids in the code root, sorted); `Result<SceneManifest> parse_scan(const SceneScan&);` and `Result<SceneInfo> info_of(const SceneScan&);` (caller: parse, the id check, `allowed_file(rel, entry)`, the `kMaxFiles` limit, the sums: the same results and errors as today's `info_locked()`).
  - `manifest(id)`, `info(id)`: one `call_on_worker` each, parse on the caller. `manifests()`, `list()`: `ids_here()`, then one `call_on_worker` per id; a scene gone meanwhile (NotFound) is skipped, other errors skipped with today's warning. Called on the worker, they run inline as today.
  - `load_installed()` calls `repo_.manifests()` directly (no `run_sync` around it). The other `manifest()` calls inside jobs stay (one file each; Deviation 3).

- [ ] **Step 1: Write the failing tests** (a `StorageWorker W` started, `config.worker = &W`, three scenes installed):
  - `SceneRepository.ReadPathsParseOnTheCaller`: `on_parse` records `W.on_worker()`; `manifest(a)`, `manifests()`, `info(a)`, `list()` from the test thread record only false; the same four inside `W.run_sync(..., kForever)` record only true.
  - `SceneRepository.ListReadsOneSceneAtATime`: `on_read` records `"read:" + id` and, the first time, submits an Interactive job recording `"X"`; `list()` gives order `{read:a, X, read:b, read:c}`.
  - `SceneRepository.SceneRemovedDuringAListIsSkipped`: `on_read` for `a` removes `b`'s directory through `storage_.remove_tree()` (it runs on the worker); `list()` returns `a` and `c`.
  - `SceneRepository.InfoIgnoresStrayTopLevelFiles`: a stray `other.lua` written into `a`'s code directory; `info(a).code_bytes` unchanged.
  - `SceneRepository.TooManyFilesStillRefused`: 257 files written under `a/lib/`; `info(a)` is InvalidArgument mentioning "more than 256 files"; `list()` skips `a` with the warning.
  - `SceneToolIndexObserverTest.LoadInstalledParsesOffTheWorker`: `on_parse` records `W.on_worker()`; all false.
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SceneRepository|SceneToolIndexObserverTest' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.** `walk_package()` stays for `load()`, `export_dir()` and `recover()`. Update the header comments of `list()`/`manifests()` (per-scene snapshots, not one).
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'SceneRepository|SceneInstall|SceneToolIndexObserverTest|SceneWorkspaceTest|AdminSceneFiles|AdminScenes|AppScenes' --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `perf(core): read scenes one job at a time and parse manifests on the caller`.

---

### Task 9: The scene listing and icon decoding leave the worker

**Files:**
- Modify: `components/wisp_shell/include/wisp/shell/shell.hpp`, `components/wisp_shell/src/shell.cpp` (`list_scenes`, `IconCache`, `stop`)
- Modify: `components/wisp_app/src/scene_wiring.hpp`, `components/wisp_app/src/scene_wiring.cpp` (`shell_scenes`, `shell_icon`, `can_run`; their thread comments)
- Modify: `components/wisp_shell/include/wisp/shell/scene_list.hpp` (comment: decoded on the listing task)
- Test: `tests/shell/scene_list_test.cpp`, `tests/app/app_scenes_test.cpp`

**Interfaces:**
- Consumes: Task 8 (the repository reads from any task), Task 5 (`run_files`).
- Produces:
  - `static constexpr size_t Shell::kListStackBytes = 12288;` `static constexpr int Shell::kListPriority = 2;` task name `"wisp.shell.list"`, `flash_access` false, `hot_stack` false.
  - `ShellDeps::list_scenes` and `read_scene_icon`: called on `wisp.shell.list`; they may wait for the storage worker and must not touch flash themselves. `ShellDeps::decode_icon`: `std::function<std::shared_ptr<const SceneIcon>(const os::LargeBuffer&, int32_t side)>`, empty → `decode_scene_icon` (test seam).
  - `Shell` replaces `listing_` with a list-run state under its own mutex: `Idle` → spawn the task (joining the previous, finished one); `Running` → `Again`; `Again` → nothing. The task runs a listing, then under the mutex either turns `Again` into `Running` and lists once more, or sets `Idle` and returns. Each listing posts the cards first, then decodes the new icons one at a time on the task, posting each as today. `Shell::stop()` sets the page's `alive` false and joins the task.
  - `SceneWiring::shell_scenes()` / `shell_icon()`: "on the shell's listing task, one call at a time"; `can_run()` asks `core::run_files<core::SceneFilesInfo>(*files_, id, ...)`.

- [ ] **Step 1: Write the failing tests.** In the fixture, `list_scenes` also records `std::this_thread::get_id()`; the UI thread's id is taken once with `harness->on_ui`.
  - `SceneListTest.ListingRunsOffTheWorkerAndTheUiThread`: open the page; `on_worker_listing` false; the recorded id differs from the UI thread's and the test thread's.
  - `SceneListTest.IconsDecodeOffTheWorker`: `decode_icon` blocks on `release` after recording `agent.worker.on_worker()` (false); meanwhile `agent.worker.run_sync(noop, 5000)` is ok; release; `ui_until(page()->icon(0) != nullptr)`.
  - `SceneListTest.BurstOfEventsDuringAListingListsOnceMore`: `list_scenes` blocks on `release` the first time and counts calls; three `scene.state` events while it blocks; release; `wait_until(calls == 2)`; after `harness->ui_until` the cards settle, `calls` is still 2.
  - `SceneListTest.StopJoinsTheListingTask`: `list_scenes` blocks; a helper thread releases it after the test calls the harness's shell stop; stop returns; no further post reaches the destroyed page (ASAN-clean).
  - Existing `IconDecodedOnceAcrossListings`, `IconsSwapInAfterCards`, `ReopenedWhileAListingWaitsIsFilled` stay green.
  - `AppScenes.ShellScenesListsAvailabilityDataAndIcons` and `ListingCachedAcrossOpens`: call `scenes->shell_scenes()` from the test thread (no `run_sync` around it); same assertions.
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SceneListTest|SceneIcon|AppScenes' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.** The task captures what it needs by value (`alive`, `icons`, the deps' functions), as today's job does. Update `ShellDeps`' comments and the `IconCache` comment ("touched on the listing task only").
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'SceneList|Shell|AppScenes|PageDrag' --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `perf(shell): list scenes and decode icons on their own task`.

---

### Task 10: `mem` prints the lanes

**Files:**
- Modify: `components/wisp_app/include/wisp/app/console.hpp`, `components/wisp_app/src/console.cpp`
- Modify: `firmware/main/main.cpp` (the `mem` command)
- Test: `tests/app/console_test.cpp`

**Interfaces:**
- Consumes: `LaneStats` (Task 3), `StorageRouter` (Task 4), `App::storage_router()` (Task 6).
- Produces: `std::string storage_report(const core::StorageRouter* router);` one line per worker and lane, flash worker first:
  `storage <worker> <lane> jobs=<n> wait_max_ms=<ceil(max_wait_us/1000)> job_max_ms=<ceil(max_job_us/1000)>` with `<lane>` `interactive`, `normal`, `bulk`; `storage wisp.sd.io off` when there is no SD worker; `storage off` for a null router. Lines joined by `"\n"`, no trailing newline. The `mem` command returns `mem_report() + "\n" + storage_report(app.storage_router())`.

- [ ] **Step 1: Write the failing tests** (two workers with injected clocks, as in `StatsRecordLongestWaitAndJob`):
  - `Console.StorageReportListsEveryLane`: flash worker stats as in Task 3's test (Interactive wait 7000 µs, job 3000 µs; Normal job 7000 µs), an SD job of 2500 µs on Normal; the report equals exactly the six lines, the SD normal line `storage wisp.sd.io normal jobs=1 wait_max_ms=0 job_max_ms=3`.
  - `Console.StorageReportWithoutAnSdWorker`: three flash lines, then `storage wisp.sd.io off`.
  - `Console.StorageReportWithoutStorage`: `storage_report(nullptr) == "storage off"`.
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R Console --output-on-failure`. Expected: FAIL to compile.
- [ ] **Step 3: Implement**; update the `mem` usage line in `main.cpp` ("heap, task stacks and storage queues").
- [ ] **Step 4: Run** the same filter. Expected: PASS. The firmware compiles in `tools/ci.sh` (Task 11).
- [ ] **Step 5: Commit** `feat(app): show each storage lane's longest wait and job in mem`.

---

### Task 11: Documents and full check

**Files:**
- Modify: `docs/architecture.md` (task table: the `wisp.fs` row's job list and three lanes, `wisp.sd.io` and `wisp.shell.list` rows, the `wisp.sd` row's last sentence; "Serial console" for `mem`'s storage lines)
- Modify: `docs/superpowers/specs/2026-11-06-flash-thread-design.md` (section 4: the new tests; section 6: risk "a card transfer deferred by a flash erase" with owner check 3)
- Modify: `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md` only to add empty rows for the owner's measurements (stack high-water of `wisp.sd.io`, `wisp.shell.list`; lane maxima)
- Modify: `CHANGELOG.md` (an `Unreleased` entry; the version bump happens at merge, AGENTS.md section 7)

- [ ] **Step 1:** Edit the documents. Every amended paragraph names its constant (`kLaneTurnAfter`, `StorageWorker::sd_io_config()`, `Shell::kListStackBytes`, `kSdOwnWorker`).
- [ ] **Step 2:** Run `bash tools/ci.sh`. Expected: exit 0, no warnings in test output, both firmware boards build, `check_layering.py` clean (`wisp_os` gained nothing from above it).
- [ ] **Step 3:** Run the simulator with an SD directory: install two example scenes, one with `"files": "sd"`; open the scene list (icons appear); write files from the scene; change a setting meanwhile; pull the card and put it back with the simulator's card toggle (`SimInput::toggle_sd()`), and check the scene's files are counted again. Record what was observed in the commit body.
- [ ] **Step 4: Commit** `docs: record the storage lanes and the sd i/o worker`.

**Device checks for the owner (not run by the agent):**
1. `mem` at idle and in the AGENTS.md section 5 worst case: `wisp.sd.io` and `wisp.shell.list` stacks in PSRAM, `wisp.fs` internal; high-water marks of the three; internal free and largest block against `wisp/app/ram_budget.hpp`, compared with the numbers before this branch.
2. Card file I/O from the PSRAM stack: a scene with `"files": "sd"` writes, appends, reads and downloads several MB; bytes match; no abort, no `sd access off the sd i/o thread` or `flash access off the flash thread` line. If it aborts, set `kSdOwnWorker = false` and record it in `docs/hardware/<board>.md`.
3. An OTA upload and a card download at the same time: both complete, no sdmmc timeout in the log; a setting changed meanwhile applies at once.
4. `mem` after a normal session (scene list opened, scenes opened, a voice turn, settings changed): each lane's longest wait and job, recorded in `docs/hardware/<board>.md`; the Interactive lane's longest wait should not exceed the longest job of the other lanes (the turn rule's bound).
5. The scene list opened cold with the example scenes: cards first, icons fill in, no stutter while scrolling; the `scenes ms_list=... ms_icons=...` line compared with before.
6. Card pulled during a download and put back: the download reports `unavailable`, the scene keeps running, its files are counted again.
7. Factory reset with a card in: `<sd>/wisp/library/agent` is gone after the restart.
8. Deep standby entered right after changing a setting and writing a card file: both are on disk after waking (the drain covers both workers).
