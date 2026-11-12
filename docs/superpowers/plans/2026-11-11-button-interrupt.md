# 按键中断优先与熄屏唤醒实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 Wisp 的按键输入改为中断优先；只有不支持中断的按键才轮询，并在熄屏时停止普通回退轮询、为非中断电源键保留 100 ms 回退，同时让圆屏 1.75C 的 PWR 键通过已确认的 SYS_OUT/GPIO3 输入唤醒自动浅睡。

**Architecture:** `hal::Button` 提供不带平台类型的输入模式、边沿接收器和唤醒控制接口。`wisp_os` 提供固定容量、ISR-safe 的按键边沿通道；`ButtonService` 运行一个 `wisp.button` 工作任务，统一处理 ISR 边沿、消抖、长按期限和 PollOnly 回退，普通上下文才发布现有 EventBus 事件。ESP GPIO 按键使用边沿/浅睡 GPIO 唤醒，AXP2101 无 GPIO 的按键保留 PollOnly；`PowerManager` 只切换 ButtonService 的 Active/Standby 输入策略，不再给所有按键设置一个全局 50 ms 周期。

**Tech Stack:** C++20, ESP-IDF v6.0.3 GPIO/automatic light sleep, FreeRTOS/`esp_timer` through `wisp_os`, PC simulator, GoogleTest, CMake/Ninja.

**Spec:** `docs/superpowers/specs/2026-11-11-button-interrupt-design.md`

## Global Constraints

- Portable layers must not include ESP-IDF, FreeRTOS, driver or `wisp/port/*` headers; platform-specific GPIO/ISR code stays in `wisp_port_esp` and board code.
- C++20, `-fno-exceptions -fno-rtti`, errors through `wisp::Result<T>`, English code/comments and no review/task history markers in comments.
- ISR code must not allocate, block, take ordinary mutexes, call `EventBus::publish()` or run application logic.
- The ISR edge channel is fixed-capacity, bounded and backed by memory reachable from the ISR; overflow is counted and recovered in task context.
- No periodic button timer remains for `EdgeEvent` buttons; PollOnly fallback is 20 ms while lit, ordinary non-power PollOnly buttons stop in standby, and PollOnly `button.power` uses 100 ms in standby/deep standby.
- PMU battery/charging/VBUS sampling remains 1 s while lit and 30 s while screen-off; it is independent of PWR-key input detection.
- The new `wisp.button` task declares `flash_access = false`, is not a hot-stack task, and its internal-RAM/PSRAM use must be measured without changing `ram_budget.hpp`.
- GPIO3/SYS_OUT polarity, GPIO3 strapping behavior and light-sleep wake behavior must be verified on the assembled 1.75C board before claiming hardware validation.
- Every new behavior follows TDD: write a failing test, observe the expected failure, implement the smallest change, then run the focused and regression tests.

## Review Focus

1. A press and release that both arrive before the button worker runs must still produce exactly `down`, `up`, `short`; test in Task 2 with queued edge timestamps.
2. A GPIO level held active through light-sleep wake must not cause an immediate sleep/wake loop; test the wake-arm/disarm/re-arm state in Task 3 and the board acceptance procedure in Task 5.
3. ISR queue overflow or input-driver initialization failure must not silently lose the power key; test downgrade to PollOnly and overflow recovery in Tasks 1–3.
4. Switching Active/Standby while a debounce or long-press deadline is pending must not duplicate or lose gestures; test concurrent mode transitions in Task 2/4.
5. The PWR key must not be registered twice (GPIO3 plus PMU fallback) and PMU sampling must remain independent; test board construction/fallback and PowerMonitor cadence in Task 3/4.

---

### Task 1: HAL input contract and ISR-safe edge channel

**Files:**
- Create: `components/wisp_os/include/wisp/os/isr_queue.hpp`
- Modify: `components/wisp_hal/include/wisp/hal/button.hpp`
- Create: `components/wisp_port_esp/os/isr_queue_esp.cpp`
- Create: `components/wisp_port_sim/os/isr_queue_sim.cpp`
- Modify: `components/wisp_port_esp/CMakeLists.txt`, `components/wisp_port_sim/CMakeLists.txt`
- Test: `tests/os/os_test.cpp`; ButtonService behavior is tested in `tests/core/button_service_test.cpp`.

**Interfaces:**
- Produces in `wisp/hal/button.hpp`:
  ```cpp
  enum class ButtonInputMode : uint8_t { PollOnly, EdgeEvent };
  struct ButtonEdgeSink {
      void* context = nullptr;
      bool (*push)(void* context, bool pressed, uint64_t timestamp_us) noexcept = nullptr;
  };
  ```
  `hal::Button` gains `virtual ButtonInputMode input_mode() const`, defaulting to `PollOnly`; `virtual Result<void> set_edge_sink(ButtonEdgeSink)` defaulting to `Unsupported`; and `virtual uint32_t standby_poll_ms() const`, defaulting to `0` (no standby polling). Devices use their existing `set_power(PowerLevel)` to enable their wake source at Standby/DeepStandby and disable it on wake. The sink callback receives a per-device `source` identity through the owning ButtonService binding; it does not expose platform types.
- Produces in `wisp/os/isr_queue.hpp`:
  ```cpp
  struct IsrQueueRecord {
      uint32_t source = 0;  // ButtonService button index
      uint32_t value = 0;   // 0=release, 1=press
      uint64_t timestamp_us = 0;
  };
  class IsrQueue {
  public:
      static Result<std::unique_ptr<IsrQueue>> create(size_t capacity);
      bool push_from_isr(const IsrQueueRecord& item) noexcept;
      bool push(const IsrQueueRecord& item) noexcept;
      bool pop(IsrQueueRecord& item, uint32_t timeout_ms);
      void wake();
      uint32_t dropped() const;
  };
  ```
  Queue storage is a bounded fixed ring allocated from internal-RAM-accessible memory. `push_from_isr()` is non-blocking, does not allocate, and signals a waiting consumer; overflow returns false and increments `dropped()`. The ESP port protects the ring with an ISR-safe spinlock and uses an ISR-safe notification/semaphore; the simulator uses a deterministic condition-variable-backed equivalent. `wake()` also unblocks `pop()` for state changes and shutdown even when no event record is enqueued.

- [ ] **Step 1: Write failing tests** in `tests/os/os_test.cpp` for `IsrQueue.FifoAndTimeout`, `IsrQueue.OverflowIsBounded`, `IsrQueue.WakeUnblocksPop`, and `IsrQueue.ConcurrentTaskConsumer`; add a `ButtonService` test for a base `hal::Button` that reports the default `PollOnly`/no-fallback behavior.
- [ ] **Step 2: Run focused tests**: `ctest --test-dir build/sim --output-on-failure -R 'IsrQueue|ButtonService'`; expected failures show missing queue/contract behavior.
- [ ] **Step 3: Implement the portable contract and both OS-port queue implementations**. ISR methods must not allocate or block; normal `pop()` may block. Keep queue slots in `Mem::Fast`/internal-accessible memory rather than ordinary PSRAM.
- [ ] **Step 4: Run focused tests and the existing OS/core regressions**; expected all pass with clean output.
- [ ] **Step 5: Run `clang-format --dry-run --Werror` and `python3 tools/check_layering.py`, then commit**:
  `feat(input): add button edge input contract`

---

### Task 2: Event-driven ButtonService and mixed fallback state machine

**Files:**
- Modify: `components/wisp_core/include/wisp/core/button_service.hpp`
- Modify: `components/wisp_core/src/button_service.cpp`
- Modify: `components/wisp_core/CMakeLists.txt` only if a new source is needed
- Test: `tests/core/button_service_test.cpp`
- Test: `tests/app/power_manager_test.cpp` for service mode transitions if needed

**Interfaces:**
- Consumes Task 1's `ButtonInputMode`, `ButtonEdgeSink`, and `IsrQueueRecord`/`IsrQueue`.
- Internal button-edge record in `components/wisp_core/include/wisp/core/button_service.hpp`:
  ```cpp
  struct ButtonEdgeEvent {
      uint16_t button_index = 0;
      bool pressed = false;
      uint64_t timestamp_us = 0;
  };
  ```
  The `ButtonEdgeSink::context` points to a stable per-button binding containing the `ButtonService*` and index; service storage is finalized before sinks attach and sinks detach before bindings are destroyed.
- Produces on `ButtonService`:
  ```cpp
  Result<void> start();
  void stop();
  void set_standby(bool standby);
  void poll(uint64_t now_ms);  // deterministic processing seam; worker calls the same path
  ```
  Remove the global `set_poll_interval_ms()`/`poll_interval_ms()` policy; polling cadence is per `ButtonState`, selected from input mode, `PowerLevel` and `standby_poll_ms()`.
`set_edge_sink()` is attempted only for `EdgeEvent` devices; success means the service must not poll that device. If registration fails, the device falls back to PollOnly and the service records one warning; `standby_poll_ms()` then determines whether/how it is sampled while asleep. If queue/task setup fails, the service unregisters any installed edge sinks and starts the legacy Timer-based fallback for all buttons (20 ms lit, 100 ms for a power key in standby, ordinary buttons paused), so the power key remains usable.

- [ ] **Step 1: Add failing tests**:
  - `EdgeEvent.ShortPressQueuedBeforeWorkerRuns`: push down/up with timestamps, run worker, expect `button.power.down`, `.up`, `.short` once.
  - `EdgeEvent.LongPressFiresOnceWhileHeld`: down then deadline then up, expect one long and no short.
  - `EdgeEvent.DebounceAndDuplicateEdges`: noisy duplicate/reversed edges do not duplicate gestures; use 20 ms debounce.
  - `EdgeEvent.HeldAcrossWake`: active level remains pressed after wake; no repeated down/sleep cycle; release completes one gesture.
  - `PollOnly.ActivePollsAt20Ms`: fallback device is sampled while lit.
  - `PollOnly.StandbyStopsOrdinaryButton`: ordinary PollOnly button is not sampled in standby.
  - `PollOnly.PowerFallbackPollsAt100Ms`: a PollOnly button reporting `standby_poll_ms() == 100` is sampled at 100 ms in standby and restored to 20 ms when lit.
  - `MixedInputs.EdgeButtonIsNeverPolled`: edge-capable device's `pressed()` counter remains zero while edge events are handled.
  - `ButtonService.ModeSwitchKeepsPendingLongPress`: Active/Standby transition does not duplicate or lose a pending long press.
  - `ButtonService.EdgeQueueOverflowRecovers`: overflow is observed, state is re-read in task context, and the service does not remain stuck down.
- [ ] **Step 2: Run the focused tests** and capture RED failures before production implementation.
- [ ] **Step 3: Implement the single worker loop**. It waits on `IsrQueue` events when no PollOnly deadline/debounce/long deadline is pending; uses a timed wait for the nearest required PollOnly/debounce/long deadline; processes all edge timestamps in order; publishes EventBus events only from the worker task. On `set_standby(true)`, call each button's existing `set_power(PowerLevel::Standby)` through the normal PowerManager path and stop ordinary fallback polling; use each PollOnly power button's `standby_poll_ms()` (100 ms for the PMU fallback). On `set_standby(false)`, restore 20 ms fallback and let the existing device power transition re-enable edge wake. Attach edge sinks after indexing devices and detach them before worker shutdown. If an edge sink or worker/queue initialization fails, unregister installed sinks and run the legacy Timer fallback (20 ms lit, 100 ms for a PollOnly power key in standby, ordinary PollOnly buttons paused) so the power key is never silently lost.
- [ ] **Step 4: Run focused ButtonService tests, existing `ButtonService` tests, and `PowerManager` tests**; all must pass with no warnings.
- [ ] **Step 5: Format, run layering, and commit**:
  `feat(input): make button service interrupt-first`

---

### Task 3: ESP GPIO edge buttons, GPIO3 wake, and PMU fallback

**Files:**
- Modify: `components/wisp_port_esp/include/wisp/port/esp/esp_button.hpp`
- Modify: `components/wisp_port_esp/dev/esp_button.cpp`
- Modify: `components/wisp_port_esp/include/wisp/port/esp/esp_power.hpp` only if shared power access needs a narrow interface change
- Modify: `boards/waveshare_esp32_s3_touch_amoled_1_75c/board.cpp`
- Modify: `boards/waveshare_esp32_s3_touch_amoled_1_75c/README.md` if pin behavior needs implementation notes
- Modify: `boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp` to mark its GPIO button devices as EdgeEvent; read that board's pin setup and hardware document before editing.
- Test: create `tests/port_esp/button_input_test.cpp` for platform-free polarity/wake-state helpers; service behavior is covered in `tests/core/button_service_test.cpp`.

**Interfaces:**
- `EspGpioButton` implements `ButtonInputMode::EdgeEvent`, `set_edge_sink()`, `standby_poll_ms()`, and the existing `set_power(PowerLevel)` behavior for GPIO wake levels. It installs the shared GPIO ISR service once, registers edge interrupts only after a sink is attached, and sends only fixed edge records from ISR context.
- `EspPmuPowerKey` remains `PollOnly`, returns `standby_poll_ms() == 100`, and keeps its AXP2101 REG 0x49 fallback implementation for boards with no direct GPIO signal.
- The 1.75C board declares `GPIO_NUM_3` as SYS_OUT/PWR input, configures it with the schematic-confirmed polarity after hardware validation, and registers exactly one `button.power`: GPIO edge device on success, PMU PollOnly fallback if GPIO configuration/edge setup cannot be used. It must not register both implementations for the same id.
- GPIO sleep wake setup uses the ESP-IDF high/low level GPIO wake API plus the global GPIO wake enable; it disables wake after a wake-causing press until the task observes release, then re-arms. GPIO3's active level and strapping behavior are logged/validated, not assumed silently.

- [ ] **Step 1: Write failing tests** for the schematic-derived polarity helper, edge-to-press mapping, held-level wake disarm/re-arm state, and PMU fallback interval (`100 ms`).
- [ ] **Step 2: Run the focused helper tests** and observe RED failures.
- [ ] **Step 3: Implement ESP GPIO ISR registration, GPIO wake arming, GPIO3 board configuration, and PMU fallback metadata**. ISR functions must be IRAM-safe and contain no logging/allocation/locks; all stateful work is deferred to ButtonService.
- [ ] **Step 4: Run the helper tests, build the ESP component through the board firmware build, and inspect the generated manifest for one `button.power` device**.
- [ ] **Step 5: Run formatting/layering and commit**:
  `feat(port): add gpio button edge and wake input`

---

### Task 4: PowerManager integration and simulator edge source

**Files:**
- Modify: `components/wisp_app/src/power_manager.cpp`
- Modify: `components/wisp_app/include/wisp/app/power_manager.hpp` comments/contracts
- Modify: `components/wisp_port_sim/include/wisp/port/sim/sim_input.hpp`
- Modify: `components/wisp_port_sim/dev/sim_button.cpp` and related simulator device headers
- Modify: `components/wisp_app/src/app.cpp` only if ButtonService lifecycle ordering needs explicit changes
- Test: `tests/app/power_manager_test.cpp`, `tests/core/button_service_test.cpp`, simulator input/device tests as needed

**Interfaces:**
- `PowerManager::screen_off()` calls `ButtonService::set_standby(true)` rather than setting a global 50 ms timer; `screen_on()` calls `set_standby(false)`. It preserves existing transition lock release discipline and button event wake semantics.
- The simulator `SimButton` exposes deterministic edge events through `SimInput` (or remains an explicit PollOnly source if the simulator port cannot provide edge injection); its tests must make the chosen mode explicit rather than accidentally relying on a timer.
- `PowerMonitor` remains unchanged at 1 s active / 30 s standby; add/retain tests proving its interval is independent of ButtonService mode.

- [ ] **Step 1: Write failing integration tests**:
  - entering standby no longer changes every button to a shared 50 ms interval;
  - ordinary PollOnly buttons stop in standby;
  - PMU fallback is 100 ms in standby;
  - edge buttons remain event-driven through the transition;
  - waking restores the active fallback interval and existing `button.power` ownership behavior.
- [ ] **Step 2: Run focused PowerManager tests** and capture RED failures.
- [ ] **Step 3: Implement the integration and simulator event path**, preserving existing `power_key_owned` and UI-thread transition rules.
- [ ] **Step 4: Run all ButtonService, PowerManager, simulator input and shell/TalkKey tests**; confirm clean output.
- [ ] **Step 5: Format/layering check and commit**:
  `feat(power): integrate interrupt-first button policy`

---

### Task 5: Documentation, full verification, and hardware acceptance handoff

**Files:**
- Modify: `docs/hardware/waveshare_esp32_s3_touch_amoled_1_75c.md`
- Modify: `docs/architecture.md` (button task, input modes, and flash/RAM task table)
- Modify: `CHANGELOG.md` under `## Unreleased` only; do not bump `WISP_BASE_VERSION`
- Test: all relevant host tests and the firmware build for 1.75C and 2.16

- [ ] **Step 1: Update docs** to state the implemented input modes, GPIO3 verification status, PollOnly 100 ms fallback, ordinary PollOnly standby pause, and PMU 1 s/30 s sampling. Do not claim GPIO3 hardware validation before the board procedure has run.
- [ ] **Step 2: Run `clang-format --dry-run --Werror` on all changed C++ files, `git diff --check`, and `python3 tools/check_layering.py`.
- [ ] **Step 3: Configure/build the host simulator and run the complete `ctest --test-dir build/sim --output-on-failure` suite.
- [ ] **Step 4: Run `bash tools/ci.sh`.
- [ ] **Step 5: Build both ESP board targets** with pinned ESP-IDF v6.0.3; use the existing `waveshare_esp32_s3_touch_amoled_1_75c` build directory for that board and configure a clean dedicated `waveshare_esp32_s3_touch_amoled_2_16` build directory without deleting any user files. Do not flash automatically. Record image sizes and warnings.
- [ ] **Step 6: Commit documentation and verification updates**:
  `docs(power): document interrupt-first button policy`
- [ ] **Step 7: Hand the user the hardware-only acceptance checklist**: measure GPIO3 levels while PWR is released/pressed/released; test PWR held during reset; verify GPIO wake without a held-level wake loop; verify short/long/held behavior, BOOT/TalkKey, `sleep` histogram, `mem`, and PMU 30 s sampling. Do not claim these device results until the user performs/authorizes the test.

---

## Execution Order and Handoff

Tasks 1–4 are implementation tasks with independent test cycles and should be reviewed after each task. Task 5 is the final verification/documentation task. The implementation must not begin until this plan is reviewed and approved; after approval, use the selected execution method (subagent-driven or native) and follow TDD for every behavior change.

## 2026-11-12 integration ruling

All five software tasks and final scoped review are complete. The user reported button-function testing successful on both boards and authorised merging into `main` with a version update. Use version 0.4.0 for this behaviour change; do not push without separate authorisation.

Ruling: promote the exercised GPIO3 path to the round-board default (`kUseSysOut=true`) while keeping electrical level/strapping, battery-only light-sleep wake, cache-off stress and worst-case RAM/power checks explicitly pending. This relaxes the spec's original all-hardware-checks-before-enable gate in response to functional acceptance and integration approval; it does not claim those unperformed checks passed. Cost: the default ships with those hardware acceptance risks still open. PMU preparation fallback and GPIO sink-failure PollOnly downgrade remain available.

Device bring-up reproduced a Level-1 interrupt-allocation failure followed by IPC stack overflow. Requesting IRAM-safe C interrupt levels 1–3 instead of Level 1 alone allowed boot, and both boards then passed user-reported button testing. The allocation-failure IPC stack-pressure path itself remains untested under exhaustion; normal success is not proof of that fallback path.
