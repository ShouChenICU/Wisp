# State Cells 2: Cells Beside the Events — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every owner of state that others follow (spec 3.4) holds a `core::StateCell` and sets it where it publishes today; where a topic carries that state, the hand-written publish becomes the cell's mirror. Nothing subscribes to a cell yet, and the bus traffic is byte-for-byte what it was: a golden test pins the admin WebSocket's forwarded stream for a scripted sequence before the first cell and is run by every task.

**Architecture:** Each cell lives inside its owner and is exposed as `const core::StateCell<T>&` (no `set` for others). Snapshot types are small structs with defaulted `operator==`, built from `wisp::FixedString` where they hold text, so a `set()` from a periodic sample is free when nothing changed. Payload builders sit next to each type (`wisp_net` for network types, and so on), never in `wisp/core/topics.hpp`.

**Tech Stack:** C++20 (`-fno-exceptions -fno-rtti`), GoogleTest, the simulator build (`build/sim`).

**Spec:** `docs/superpowers/specs/2026-11-14-state-cells-design.md`, sections 3.3, 3.4, 3.6 and 4 step 2. Builds on plan 1 (`2026-11-14-state-cells-1-primitives.md`).

## Global Constraints

- Plan 1's Global Constraints apply unchanged (branch `feat/state-cells`, AGENTS.md, lock rules, `misuse()`, no sleeps as synchronisation, clean test output, commit format, device checks never reported as done).
- Every task's test filter includes `ForwardedStream` (Task 1). A task that needs to change the golden frames is wrong unless this plan says so in that task.
- Existing tests that assert published events stay green without edits (spec 4 step 2: that is the proof the wire did not move). Tests that construct an owner whose constructor gains a parameter are updated mechanically; list them in the commit body.
- A mirror is a plain function (`StateCell<T>::Mirror`) and cannot capture its owner, so it publishes with the `Event::source` its owner uses today, written as a constant (listed per task); `Event::source` is not part of the WebSocket frame (`admin::event_frame()`), and tests that check it keep passing.
- A cell is set under the same owner lock its event is published under today, so concurrent setters produce mirror events in the order they produce them now.

## Re-verify before starting (plan 1 changed what this plan consumes)

1. Plan 1's interfaces as merged: `StateCell` (constructor, `Mirror` signature `void(*)(EventBus&, const T&, const T&)`, `seed()`, `update()`), `CellSubscription`, `wisp::FixedString`, `admin::event_frame()`, `WsHub::forwards()`, `topics::k*` names, `ManualDispatcher`, `BusFlood`. Adjust the signatures below if they differ.
2. Every publish site cited below (line numbers drift): `PowerManager::set_level()`/`publish_state()`, `PowerMonitor::sample()`, `WifiService::set_state_locked()`/`set_enabled()`/`publish_ap_locked()`, `PairingService` (code issued, code lapsed), `SystemSettings::set()`, `TimeSyncService` (end of attempt), `StorageManager::refresh()`, `FirmwareUpdateService::publish()`, `factory_reset.cpp` `publish_state()`, `SceneRuntime::publish()`/`set_status()`, `NotificationService::Impl::publish_posted()/publish_dismissed()`, `AgentService::Impl::set_state()`, `VoiceService::publish()`.
3. Whether any owner publishes the same state twice in a row today (`set_level()` with an unchanged level, `publish_ap_locked()` with unchanged values). A cell suppresses an equal `set()`; if a duplicate is published today, the golden test of Task 1 must include it and the owner must keep it (see Task 6's pattern), or the task records that no path produces it.
4. The `Event::source` each owner uses today: `"app.power"` (PowerManager), the power device id (`PowerMonitor::source_`, `"power.main"` on every board, as App looks it up), the Wi-Fi device id (`WifiService::source_`, `"net.wifi"`), `"pairing"`, `"app"` (settings, factory reset, firmware), `""` (time sync sets none), `"storage.sd"`, `"agent"`.
5. The App test harness of Task 1 (`tests/app/ui_net_harness.hpp`) can drive every step of the scripted sequence: FakeWifi, FakeSntp, FakeFirmwareUpdater, a power device whose status the test sets, an SD device that goes on and off line. Where one is missing, add it to the harness (test-only) rather than dropping the step.
6. The simulator sets `Platform::sntp` (`simulator/main.cpp`, `SimSntp`), so a `TimeSyncService` exists whenever the simulator has Wi-Fi.

## Review Focus

1. The forwarded stream for the scripted sequence is identical before and after every task (`ForwardedStream.ScriptedSequenceIsUnchanged`). Task 1 pins it; every task runs it.
2. Order of two events from one change: disabling Wi-Fi while connected publishes `net.wifi.state` (`off`) before `net.wifi.enabled`, and a sample where charging starts and the battery crosses the threshold publishes `power.charging.changed` before `power.battery.low`. Tests in Tasks 3 and 4.
3. First values publish nothing, except where today's code does: the first power sample, the settings seed and the SD state at boot publish nothing; a battery already low at the first sample still publishes `power.battery.low`. Tests in Tasks 3, 6 and 8.
4. Transients survive coalescing: the agent's `error` then `idle`, the voice's `error` then `idle` and an `idle` with `not_heard` each move a counter that stays moved after the state is `idle` again. Tests in Task 11.
5. `system.factory_reset` `done` and `system.firmware` `confirmed` still reach `WsHub` before the wipe or the restart (`EventBus::wait_idle()` now also waits for slots; the mirrors publish inside `set()`). Tests in Task 9.

## Deviations from the spec

1. **Type names that already exist are not reused.** `net::WifiStatus`, `net::ApStatus` and `core::FirmwareStatus` exist with other meanings (`WifiService::status()`, the admin page's hotspot view, `GET /system/firmware`); `net::TimeSyncStatus` is `TimeSyncService::status()`. The cell types are `net::WifiSnapshot`, `net::ApSnapshot`, `core::FirmwareProgress` and `core::TimeSyncResult`.
2. **`core::TimeSyncResult` lives in `wisp_core`** (`wisp/core/time_sync_result.hpp`), not in `wisp_net`: `core::ReminderScheduler`, one of its subscribers (spec 3.8), is in `wisp_core` and may not name a `wisp_net` type (AGENTS.md section 3). `net::TimeSyncService` owns the cell.
3. **A settings batch that stores values equal to the current ones still publishes `system.settings`.** Today every stored batch publishes; with the cell an equal `set()` is a no-op, which would drop that event. `SystemSettings::set()` publishes it directly when `cell.set()` returns false, so the wire does not change and the cell does not move.
4. **`ResetStatus::seq` and `FirmwareProgress::seq` move on every state the owner publishes,** not only on requests, so every call that publishes today still changes the cell (two `cancelled` in a row are two events, as now). Plan 3's dialog opens on `Requested`/`Received` with a `seq` it has not handled yet.
5. **The power supply's first sample is `seed()` with `low = false`, then `set()` with the computed `low`.** Spec 3.4 says the first sample is `seed()`; but today `power.battery.low` is published at the first sample when the battery is already low (`PowerMonitor::sample()` does not need a previous sample for it), and the low-battery notice depends on that.
6. **The voice cell gets no mirror in this plan.** `VoiceService` keeps publishing `voice.state` by hand, `level` included, because Home and the talk overlay still read `level` from the bus until plan 3 moves them to `VoiceService::level()`. Plan 3's last task gives the cell its mirror and drops `level` (spec 3.6).
7. **The simulator's `debug clock-advance` reports through `TimeSyncService::note_external_sync()`** instead of publishing `system.time` itself, because plan 3's subscribers follow the time-sync cell, not the bus.
8. **`agent.state`'s `tool` is cut to 72 bytes** when the model names an unknown tool longer than that (every registered tool name fits, `valid_tool_name()`). Spec 3.3 cuts it "in the status only"; the event is built from the status by the mirror, and `agent.state` is neither forwarded (`ws_hub.cpp`) nor, after plan 3, subscribed on the device, so nothing observes the difference.
9. **The scenes overview's `catalog` moves through `SceneHost::note_catalog_changed()`, not `on_installed()`.** `install_scene()` calls `on_installed()` only for a scene that was not running; a running one is closed and reopened with `open()`/`open_background()`, so a hook in `on_installed()` would miss every replace of a running scene. `SceneHost` gains `virtual void note_catalog_changed() {}` (a no-op by default), which `install_scene()` calls once per install that changed what is stored (also one that failed part way through the replace, by `SceneRepository::changed_since()`), and `SceneWiring::remove()` calls `SceneRuntime::note_catalog_changed()` before `deleted` once the scene is no longer installed. Dev-mode file edits and a card coming or going do not move `catalog`: a follower that lists scenes from the card or in dev mode keeps its own trigger.

---

### Task 1: Pin the forwarded stream

**Files:**
- Create: `tests/support/forwarded_recorder.hpp`
- Create: `tests/app/forwarded_stream_test.cpp`; Modify: `tests/CMakeLists.txt` (link as `net_ui_test` does; `TIMEOUT 60`)
- Modify (test-only, if re-verify item 5 finds a gap): `tests/app/ui_net_harness.hpp`

**Interfaces:**
- Consumes: `admin::event_frame()`, `admin::WsHub::forwards()` (plan 1 Task 7).
- Produces `tests/support/forwarded_recorder.hpp`:

```cpp
namespace wisp::test {
// Records, in publish order, the frame WsHub sends to an "events" client for
// every forwarded event on `bus` (admin::event_frame() of each event whose
// topic WsHub::forwards()). Subscribes to "" on construction.
class ForwardedRecorder {
public:
    explicit ForwardedRecorder(core::EventBus& bus);
    ~ForwardedRecorder();
    std::vector<std::string> take();   // the frames so far, cleared; call after bus.wait_idle()
};
}
```

- Scripted sequence (one helper per step, each followed by `app.bus().wait_idle(2000)`; the expected frames are a literal `std::vector<std::string>` in the test file):
  1. `PowerManager::enter_standby()`, then `exit_standby()`.
  2. `system_settings().set({{"language", "en"}})`, then the same batch again (pins deviation 3), then `{{"brightness", 300}}` (rejected: no frame).
  3. FakeWifi: add a network and connect (`connecting`, `connected` with the fake's ip and rssi), then `set_enabled(false)` (`off`, then `net.wifi.enabled`).
  4. `start_provisioning()`, then `stop_provisioning()` (`net.ap.state` active, inactive).
  5. Issue a pairing code, then `cancel_code()` (`net.pairing.code` active, inactive).
  6. FakeSntp answers one attempt (`system.time` synced with the fake's time and server), then fails one.
  7. The SD device goes off line, `queue_storage_refresh()` runs; then on line again.
  8. The power device reports charging; then not charging with `battery_percent = 10` (below the default 15: `power.battery.low`).
  9. `request_factory_reset()`, then `cancel_factory_reset()`.
  10. A firmware upload is received, then declined.

- [ ] **Step 1: Write the test** `TEST(ForwardedStream, ScriptedSequenceIsUnchanged)`: run the sequence on the current tree with an empty expected list, print the recorded frames, paste them into the test as the expected list, and check each frame by hand against `docs/architecture.md`'s payload descriptions (a frame that contradicts the table is a bug to report, not to pin). Add `TEST(ForwardedStream, RecorderMatchesWhatAClientReceives)`: an events client on the App's own `WsHub` (`net->ws_hub()`, as `net_ui_test.cpp` reaches it) receives the same frames as the recorder for steps 1 and 3.
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R ForwardedStream --output-on-failure` three times. Expected: PASS each time (a characterisation test: it passes on the tree it describes; flakiness here would make it useless as a gate, so fix any ordering race in the harness, never in the expected list).
- [ ] **Step 3: Commit** `test(app): pin the forwarded event stream`.

---

### Task 2: The power level cell

**Files:**
- Modify: `components/wisp_app/include/wisp/app/power_manager.hpp`, `components/wisp_app/src/power_manager.cpp`
- Test: `tests/app/power_manager_test.cpp`

**Interfaces:**
- Produces: `const core::StateCell<hal::PowerLevel>& PowerManager::level_cell() const;` The member `core::StateCell<hal::PowerLevel> level_cell_{bus_, hal::PowerLevel::Active, &power_state_mirror}` replaces `std::atomic<hal::PowerLevel> level_`; `level()` returns `level_cell_.get()`, `standby()` derives from it. `set_level()` calls `level_cell_.set(level)` (still under `transition_mutex_`); `publish_state()` is deleted.
- Mirror (file-local): publishes `{topics::kPowerState, "app.power", 0, {{"state", hal::to_string(after)}}}`.

- [ ] **Step 1: Write the failing tests:**

```cpp
TEST(PowerManager, LevelCellFollowsEveryTransition)
// ManualDispatcher md; sub on level_cell(); run_all() -> Active. enter_standby(); run_all() -> Standby;
// exit_standby(); run_all() -> Active; level_cell().revision() == 3.
TEST(PowerManager, LevelReadsTheCell)
// after enter_standby(): level() == level_cell().get() == Standby; standby() == true.
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'PowerManager|ForwardedStream' --output-on-failure`. Expected: build fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same filter plus `-R 'AppNet|NetWiring|StatusShell|SelfCheck'`. Expected: PASS, golden unchanged.
- [ ] **Step 5: Commit** `feat(app): keep the power level in a state cell`.

---

### Task 3: The power supply cell

**Files:**
- Modify: `components/wisp_core/include/wisp/core/power_monitor.hpp`, `components/wisp_core/src/power_monitor.cpp`
- Test: `tests/core/power_monitor_test.cpp`

**Interfaces:**
- Produces in `power_monitor.hpp`:

```cpp
struct PowerSupply {
    bool charging = false;
    bool usb_present = false;
    int battery_percent = -1;  // -1: unknown
    bool low = false;          // the low-battery state after the existing hysteresis
    bool operator==(const PowerSupply&) const = default;
};
const StateCell<PowerSupply>& PowerMonitor::supply() const;
```

- `sample()`: a successful read computes `low` with today's threshold and hysteresis, then, under `sample_mutex_`: on the first successful sample `seed({charging, usb_present, pct, false})` followed by `set({..., low})`; afterwards `set({...})`. A failed read leaves the cell. `have_sample_`, `last_charging_`, `last_usb_present_`, `low_fired_` are replaced by the cell's value (`low_fired_` is `supply.low`).
- Mirror (file-local), source `"power.main"`: when `charging` or `usb_present` differ, `power.charging.changed {charging, usb_present, battery_percent}`; then, when `!before.low && after.low`, `power.battery.low {battery_percent}`. The `WLOG_I` lines stay in `sample()`.

- [ ] **Step 1: Write the failing tests:**

```cpp
TEST(PowerMonitor, FirstSampleSeedsTheSupplyWithoutAnEvent)
// device charging, 80 %: sample(); supply().get() == {true, true, 80, false}; no event on the bus.
TEST(PowerMonitor, LowBatteryAtTheFirstSampleStillPublishes)
// device at 10 %: sample(); supply().get().low; exactly one power.battery.low {battery_percent: 10}.
TEST(PowerMonitor, PercentChangeMovesTheCellButPublishesNothing)
// 80 -> 79: revision moved; no event.
TEST(PowerMonitor, ChargingAndLowInOneSamplePublishInTodaysOrder)
// 20 % not charging, then a sample at 12 % charging: events [power.charging.changed, power.battery.low].
TEST(PowerMonitor, FailedReadLeavesTheSupply)
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'PowerMonitor|ForwardedStream' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.** Keep the file header's event description; add the cell to it.
- [ ] **Step 4: Run** the same filter plus `-R 'AppBoot|NotificationRouter|SceneRuntimeTest'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): keep the power supply in a state cell`.

---

### Task 4: The Wi-Fi and hotspot cells

**Files:**
- Create: `components/wisp_net/include/wisp/net/wifi_snapshot.hpp`, `components/wisp_net/src/wifi_snapshot.cpp` (add to `SRCS`)
- Modify: `components/wisp_net/include/wisp/net/wifi_service.hpp`, `components/wisp_net/src/wifi_service.cpp`
- Test: `tests/net/wifi_service_test.cpp`, new `tests/net/wifi_snapshot_test.cpp`

**Interfaces:**
- Produces in `wifi_snapshot.hpp` (namespace `wisp::net`):

```cpp
struct WifiSnapshot {
    bool enabled = false;
    WifiState state = WifiState::Off;
    FixedString<32> ssid;   // raw bytes, as WifiStatus::ssid
    FixedString<15> ip;     // dotted IPv4; empty unless Connected
    int rssi = 0;           // as published with the last state change, not live
    FixedString<11> reason; // "", "not_found", "auth_failed", "other"
    bool operator==(const WifiSnapshot&) const = default;
};
struct ApSnapshot {
    bool active = false;
    FixedString<32> ssid;
    int clients = 0;
    bool operator==(const ApSnapshot&) const = default;
};
Properties wifi_state_payload(const WifiSnapshot&);   // {state, ssid, ssid_hex, ip, rssi, reason}
Properties wifi_enabled_payload(const WifiSnapshot&); // {enabled}
Properties ap_state_payload(const ApSnapshot&);       // {active, ssid, clients}
void wifi_mirror(core::EventBus&, const WifiSnapshot& before, const WifiSnapshot& after);
void ap_mirror(core::EventBus&, const ApSnapshot& before, const ApSnapshot& after);
```

  `wifi_mirror` publishes `net.wifi.state` when `state`, `ssid`, `ip` or `reason` differ, then `net.wifi.enabled` when `enabled` differs (source `"net.wifi"`). `ap_mirror` publishes `net.ap.state` for any difference.
- Produces in `WifiService`: `const core::StateCell<WifiSnapshot>& wifi_cell() const;`, `const core::StateCell<ApSnapshot>& ap_cell() const;`, private `void publish_wifi_locked();` (builds the snapshot from `enabled_` and `status_`, then `set()`), called where `set_state_locked()` publishes today (only when `changed`) and where `set_enabled()` publishes. `publish_ap_locked()` sets the hotspot cell. `WifiService::status()` and `ap_status()` keep their types and callers.
- Ordering argument (for the review): `set_enabled()` stores `enabled_` before `apply_radio_locked()`, so a state change inside it carries the new `enabled` too and the mirror publishes `net.wifi.state` then `net.wifi.enabled` from that one `set()`; the later `publish_wifi_locked()` in `set_enabled()` is then a no-op. Without a state change only `net.wifi.enabled` goes out. Both match today.

- [ ] **Step 1: Write the failing tests:**

```cpp
TEST(WifiSnapshot, PayloadsMatchTheDocumentedShape)
// an SSID with a 0xff byte: ssid_hex is lower-case hex of the raw bytes; reason "" unless backoff.
TEST(WifiService, CellFollowsStateChangesButNotRssiAlone)
// connect: cell state Connected with ip; a tick that only moves rssi leaves revision unchanged.
TEST(WifiService, DisablingWhileConnectedPublishesStateThenEnabled)
// events in order: net.wifi.state {state:"off"}, net.wifi.enabled {enabled:false}; cell enabled == false.
TEST(WifiService, EnablingWithNothingSavedPublishesOnlyEnabled)
TEST(WifiService, HotspotCellFollowsOpenCloseAndClients)
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'WifiService|WifiSnapshot|ForwardedStream' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same filter plus `-R 'AppNet|NetUi|NetWiring|Pairing'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(net): keep the Wi-Fi and hotspot state in cells`.

---

### Task 5: The pairing cell

**Files:**
- Modify: `components/wisp_net/include/wisp/net/pairing_service.hpp`, `components/wisp_net/src/pairing_service.cpp`
- Test: `tests/net/pairing_service_test.cpp`

**Interfaces:**
- Produces: `struct PairingSnapshot { bool active = false; uint64_t expires_ms = 0; bool operator==(const PairingSnapshot&) const = default; };` and `const core::StateCell<PairingSnapshot>& PairingService::pairing_cell() const;`. Both publish sites of `net.pairing.code` set the cell under `mutex_`; the mirror publishes `{active, expires_ms}` with source `"pairing"`. `net.pairing.failed` stays a plain publish (a notification).
- [ ] **Step 1: Write the failing tests:** `PairingService.CellFollowsIssueAndLapse` (issue → `{true, deadline}`; `cancel_code()` → `{false, 0}`; events unchanged), `PairingService.ReissueMovesTheDeadline`.
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'PairingService|ForwardedStream' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same filter plus `-R 'NetUi|Admin'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(net): keep the pairing state in a cell`.

---

### Task 6: The settings cell

**Files:**
- Modify: `components/wisp_core/include/wisp/core/system_settings.hpp`, `components/wisp_core/src/system_settings.cpp`
- Modify: `components/wisp_app/src/app_impl.hpp` (the `system_settings` member: no `on_changed` lambda), `components/wisp_app/src/app.cpp` (seed after the settings cache is attached)
- Modify (mechanical): every test that constructs `core::SystemSettings` (`grep -rn "SystemSettings[ ({]" tests`)
- Test: `tests/core/system_settings_test.cpp`, `tests/app/app_boot_test.cpp`

**Interfaces:**
- Produces:
  - `SystemSettingsValues` gets default member initializers equal to the documented defaults (brightness 200, volume 60, screen_off_s 60, language "zh", utc_offset_min 480, ntp_servers `kDefaultNtpServers`, deep_after_s 300, notify_vol 60, tts_mute false, time_24h true, agent_max_steps 60, agent_turn_min 15) and `bool operator==(const SystemSettingsValues&) const = default;`.
  - `SystemSettings(SettingsBackend& backend, EventBus& bus, std::function<void(uint8_t)> apply_brightness);` (the `on_changed` parameter is gone; its one user published the event).
  - `const StateCell<SystemSettingsValues>& cell() const;` and `void seed_cell();` (`cell_.seed(get())`; called once by App).
  - `set()`: after a stored batch and `apply_brightness`, under `mutex_`: `if (!cell_.set(get())) bus_.publish(settings_event(values))` (deviation 3). The mirror publishes `settings_event(after)`: `{topics::kSystemSettings, "app", 0, to_map(after)}`. `revision()` stays (plan 4 removes it).
- App: `impl->system_settings.seed_cell()` right after the settings-cache block in `App::start()` (after `load_and_attach()` or its fallback), before the UI language is read. Nothing subscribes before that point (verify: the first subscriber in plan 3 is PowerManager, built later).

- [ ] **Step 1: Write the failing tests:**

```cpp
TEST(SystemSettings, CellFollowsEveryStoredBatch)
// set({"volume", 30}): cell().get().volume == 30; one system.settings event with volume 30.
TEST(SystemSettings, UnchangedBatchPublishesButLeavesTheCell)
// set({"volume", 30}) twice: two events; revision of the cell moved once.
TEST(SystemSettings, RejectedOrFailedBatchLeavesTheCell)
// an out-of-range value; a backend that fails the second key: no event, cell unchanged.
TEST(SystemSettings, SeedPublishesNothing)
// backend holds language "ja"; seed_cell(): cell().get().language == "ja"; no event.
TEST(SystemSettings, DefaultsMatchAnEmptyBackend)
// SystemSettingsValues{} == SystemSettings(empty backend, ...).get().
TEST(AppBoot, SettingsCellIsSeededFromStorage)
// boot with sys/lang "en" stored: system_settings().cell().get().language == "en"; no
// system.settings event during start().
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SystemSettings|AppBoot|ForwardedStream' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.** Update the file header (the cell replaces `on_changed`; consumers follow it) and `docs/architecture.md`'s `system.settings` row publisher column ("`core::SystemSettings`'s state cell").
- [ ] **Step 4: Run** the same filter plus `-R 'Settings|PowerManager|SystemDuties|SelfCheck|Admin'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): keep the system settings in a state cell`.

---

### Task 7: The time-sync cell

**Files:**
- Create: `components/wisp_core/include/wisp/core/time_sync_result.hpp`
- Modify: `components/wisp_net/include/wisp/net/time_sync.hpp`, `components/wisp_net/src/time_sync.cpp`
- Modify: `components/wisp_app/include/wisp/app/app.hpp`, `components/wisp_app/src/app.cpp` (`App::time_sync()`), `simulator/main.cpp` (`clock-advance`)
- Test: `tests/net/time_sync_test.cpp`

**Interfaces:**
- Produces:

```cpp
namespace wisp::core {
struct TimeSyncResult {
    enum class Outcome : uint8_t { None, Synced, Failed };
    Outcome outcome = Outcome::None;
    FixedString<63> server;  // ntp_servers entries are at most 63 characters
    int64_t time = 0;        // UTC seconds; Synced only
    uint32_t attempts = 0;   // finished attempts this boot; every attempt moves it
    bool operator==(const TimeSyncResult&) const = default;
};
}
// net::TimeSyncService:
const core::StateCell<core::TimeSyncResult>& result_cell() const;
// A clock set by other means (the simulator's debug command): counted as a
// successful attempt from `server` at `time`, as a sync would be.
void note_external_sync(std::string_view server, int64_t time);
// app::App:
net::TimeSyncService* time_sync();  // null without one
```

  The end of each attempt sets the cell (after `status_` is updated, outside `mutex_`, where the event is published today); the mirror publishes `system.time {state, server, time (synced only)}` with an empty source, as today.
- `simulator/main.cpp`: `clock-advance` calls `app.time_sync()->note_external_sync("debug", now)`; without a `TimeSyncService` it answers `clock-advance=no time sync` (the simulator always has one when it has Wi-Fi; re-verify item 6).
- [ ] **Step 1: Write the failing tests:** `TimeSync.CellCountsEveryAttempt` (synced then failed: attempts 1, 2; outcome and server follow; two events as today), `TimeSync.ExternalSyncCountsAsASyncedAttempt`.
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'TimeSync|ForwardedStream' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same filter plus `-R 'Reminders|SceneRuntimeTest|NetWiring|AppNet'`, and the simulator smoke tests that use `clock-advance` (`ctest -R smoke` lists them). Expected: PASS.
- [ ] **Step 5: Commit** `feat(net): keep the time sync result in a state cell`.

---

### Task 8: The SD card cell

**Files:**
- Modify: `components/wisp_core/include/wisp/core/storage_manager.hpp`, `components/wisp_core/src/storage_manager.cpp`
- Test: `tests/core/storage_manager_test.cpp`

**Interfaces:**
- Produces: `struct SdStatus { bool online = false; uint32_t changes = 0; bool operator==(const SdStatus&) const = default; };` and `const StateCell<SdStatus>& StorageManager::sd_cell() const;`. `init()` seeds `{sd_online_, 0}` (nothing is published at boot, as today); `refresh()` sets `{now_online, changes + 1}` where it publishes today; the mirror publishes `storage.sd.inserted` or `storage.sd.removed` with source `"storage.sd"` and an empty payload.
- [ ] **Step 1: Write the failing tests:** `StorageManager.SdCellSeededAtInitWithoutAnEvent`, `StorageManager.SdCellCountsEveryChange` (off, on: changes 1, 2; events removed, inserted).
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'StorageManager|ForwardedStream' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same filter plus `-R 'SceneFiles|AppScenes'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): keep the SD card state in a cell`.

---

### Task 9: The firmware and factory-reset cells

**Files:**
- Modify: `components/wisp_core/include/wisp/core/firmware_update.hpp`, `components/wisp_core/src/firmware_update.cpp` (`Deps::publish` removed; the bus comes in through the constructor)
- Modify: `components/wisp_app/src/factory_reset.hpp`, `components/wisp_app/src/factory_reset.cpp`, `components/wisp_app/src/app_impl.hpp`, `components/wisp_app/src/app.cpp` (the firmware publish lambda deleted)
- Modify (mechanical): tests constructing `FirmwareUpdateService`
- Test: `tests/core/firmware_update_test.cpp`, `tests/app/factory_reset_test.cpp`, `tests/app/firmware_confirm_test.cpp`

**Interfaces:**
- Produces:

```cpp
namespace wisp::core {
struct FirmwareProgress {
    enum class Phase : uint8_t { Idle, Received, Confirmed, Rejected, Failed };
    Phase phase = Phase::Idle;
    FixedString<31> version;  // the uploaded image's
    FixedString<23> error;    // Failed only: wrong_board, invalid_image, payload_too_large, write_failed, aborted
    uint32_t seq = 0;         // moves with every published state
    bool operator==(const FirmwareProgress&) const = default;
};
Properties firmware_payload(const FirmwareProgress&);  // {state, version, error (Failed only)}
// FirmwareUpdateService(Deps deps, EventBus& bus, StorageWorker* worker = nullptr);
const StateCell<FirmwareProgress>& progress_cell() const;
}
namespace wisp::app {
struct ResetStatus {
    enum class Phase : uint8_t { Idle, Requested, Cancelled, Done, Failed };
    Phase phase = Phase::Idle;
    uint32_t seq = 0;  // moves with every published state
    bool operator==(const ResetStatus&) const = default;
};
const core::StateCell<ResetStatus>& reset_status(App& app);
}
```

  `FirmwareUpdateService::publish(state, version, error)` becomes an `update()` of the cell (phase, version, error, `seq + 1`); the mirror publishes `system.firmware` with source `"app"`. `App::Impl` gains `core::StateCell<ResetStatus> reset_status{bus, {}, &reset_mirror};` declared after `bus`; `factory_reset.cpp`'s `publish_state()` becomes an `update()` (`seq + 1`); the mirror publishes `system.factory_reset {state}` with source `"app"`. `reset_pending`, `firmware_pending`, `take_*_request()` and `Deps::request_confirm` stay (plan 4).
- [ ] **Step 1: Write the failing tests:**

```cpp
TEST(FirmwareUpdate, ProgressCellFollowsEveryState)
// received -> {Received, "0.3.0", "", 1}; decline -> {Rejected, ..., 2}; a wrong-board upload ->
// {Failed, ..., "wrong_board", 3}; events identical to the existing expectations.
TEST(FactoryReset, StatusCellFollowsEveryState)
// request -> {Requested, 1}; cancel -> {Cancelled, 2}; cancel again -> {Cancelled, 3} and a second
// "cancelled" event (as today); request -> {Requested, 4}.
TEST(FactoryReset, DoneReachesTheBusBeforeTheWipe)
// a recorder subscribed to system.factory_reset and the FakeWifi's forget counter: the "done" frame
// was recorded before forget_all() ran (bus.wait_idle covers the mirror's event).
TEST(FirmwareConfirm, ConfirmedReachesTheBusBeforeTheRestart)
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'FirmwareUpdate|FactoryReset|FirmwareConfirm|ForwardedStream' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same filter plus `-R 'SystemDuties|NetUi|Admin'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(app): keep firmware and factory reset progress in cells`.

---

### Task 10: The scenes overview and notification summary cells

**Files:**
- Modify: `components/wisp_scene/include/wisp/scene/scene_runtime.hpp`, `components/wisp_scene/src/scene_runtime.cpp`, `components/wisp_scene/src/scene_runtime_tools.cpp` (headless)
- Modify: `components/wisp_app/src/scene_wiring.cpp` (delete → `note_catalog_changed()`)
- Modify: `components/wisp_core/include/wisp/core/notification_service.hpp`, `components/wisp_core/src/notification_service.cpp`
- Test: `tests/scene/scene_runtime_test.cpp`, `tests/scene/scene_background_test.cpp`, `tests/app/app_scenes_test.cpp`, `tests/core/notification_service_test.cpp`

**Interfaces:**
- Produces:

```cpp
namespace wisp::scene {
struct SceneOverview {
    FixedString<32> foreground;     // the scene in front; empty when none (kMaxSceneIdBytes)
    uint16_t background_count = 0;  // background and headless scenes running
    uint32_t catalog = 0;           // moves on every install, replace and delete
    bool operator==(const SceneOverview&) const = default;
};
const core::StateCell<SceneOverview>& SceneRuntime::overview() const;
void SceneRuntime::note_catalog_changed();  // any thread
}
namespace wisp::core {
struct NoticeSummary {
    uint16_t unread = 0;
    uint32_t changes = 0;  // moves on every post, replacement and dismissal
    bool operator==(const NoticeSummary&) const = default;
};
const StateCell<NoticeSummary>& NotificationService::summary() const;
}
```

  No mirror for either (`scene.state` and `notification.*` stay as they are, spec 3.4). The runtime recomputes the overview on the scene task after every `set_status()`, after `close_slot()` (including closing a faulted scene's error panel, which publishes nothing today) and after `on_installed()`; `note_catalog_changed()` is an `update()` that moves `catalog`. SceneWiring's delete calls it before publishing `deleted`. NotificationService seeds the summary after `load()` and sets it in `publish_posted()`/`publish_dismissed()`.
- [ ] **Step 1: Write the failing tests:**

```cpp
TEST_F(SceneRuntimeTest, OverviewFollowsTheForegroundAndTheBackground)
// open A: foreground "A"; A to background: foreground "", background_count 1; close A: 0.
TEST_F(SceneRuntimeTest, ClosingAFaultedScenesPanelClearsTheForeground)
// A faults in front (foreground "A"); close the error panel: foreground "" (no scene.state event).
TEST_F(SceneRuntimeTest, InstallMovesTheCatalog)
TEST(AppScenes, ConfirmedDeleteMovesTheCatalogBeforeDeleted)
TEST(NotificationService, SummaryCountsUnreadAndEveryChange)
// post -> {1, 1}; replace the same id -> {1, 2}; dismiss -> {0, 3}.
TEST(NotificationService, LoadSeedsTheSummary)
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SceneRuntimeTest|SceneBackground|AppScenes|NotificationService|ForwardedStream' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.** Re-verify where `foreground()` reads its answer (the runtime's status map under `status_mu_`, or slot state on the scene task) and compute the overview from the same source.
- [ ] **Step 4: Run** the same filter plus `-R 'Scene|StatusShell|ShellTalk'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(scene): keep a scenes overview and a notice summary in cells`.

---

### Task 11: The agent and voice cells

**Files:**
- Modify: `components/wisp_agent/include/wisp/agent/agent_service.hpp`, `components/wisp_agent/src/agent_service.cpp`
- Modify: `components/wisp_voice/include/wisp/voice/voice_service.hpp`, `components/wisp_voice/src/voice_service.cpp`
- Test: `tests/agent/agent_loop_test.cpp`, `tests/voice/voice_service_test.cpp`

**Interfaces:**
- Produces:

```cpp
namespace wisp::agent {
struct AgentStatus {
    AgentState state = AgentState::Idle;
    FixedString<72> tool;  // Tool only; a longer name from the model is cut here, never in the event
    uint32_t errors = 0;   // moves each time the state becomes Error
    bool operator==(const AgentStatus&) const = default;
};
const core::StateCell<AgentStatus>& AgentService::status_cell() const;
}
namespace wisp::voice {
struct VoiceStatus {
    enum class Outcome : uint8_t { None, Error, NotHeard };
    VoiceState state = VoiceState::Idle;
    Outcome outcome = Outcome::None;  // the last outcome; see outcomes
    FixedString<16> reason;           // Error only: no_asr_profile, offline, ...
    uint32_t outcomes = 0;            // moves with every error and every not_heard
    bool operator==(const VoiceStatus&) const = default;
};
const core::StateCell<VoiceStatus>& VoiceService::status_cell() const;
uint8_t VoiceService::level() const;  // 0-100, the latest microphone or playback level; atomic
}
```

  `AgentService::Impl::set_state()` updates the cell inside its existing change check (the `WLOG_I` line and the `AgentEvent` stay); the cell's mirror publishes `agent.state {state, tool (Tool only)}` with source `"agent"` (deviation 8 for names longer than 72 bytes).
  `VoiceService::publish()` updates the cell when the state changes or a reason or result is given (moving `outcomes`), and stores `level` in an atomic read by `level()`; it keeps publishing `voice.state` by hand, unchanged (deviation 6).
- [ ] **Step 1: Write the failing tests:**

```cpp
TEST_F(AgentLoop, StatusCellCountsErrorsThatEndInIdle)
// an "unauthorized" turn: the cell ends {Idle, "", errors 1}; events unchanged.
TEST_F(AgentLoop, UnknownLongToolNameIsCutInTheStatus)
// a 100-character unknown tool name from the scripted model: the cell's tool and the agent.state
// event's "tool" are its first 72 bytes; the turn's tool-error reply to the model is unchanged.
TEST(VoiceService, StatusCellCountsErrorAndNotHeard)
// a failed ASR: cell {Idle, Error, "asr_failed", 1}; a silent hold: {Idle, NotHeard, "", 2}.
TEST(VoiceService, LevelFollowsTheMicrophone)
// a listening capture with a loud FakeMic: level() > 0 while listening; events unchanged.
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'AgentLoop|VoiceService|ForwardedStream' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same filter plus `-R 'HomeTest|ShellTalk|ConversationTest|AgentWs'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(agent): keep agent and voice status in state cells`.

---

### Task 12: Check

- [ ] **Step 1:** Run `bash tools/ci.sh`. Expected: exit 0, no warnings in test output, `ForwardedStream.ScriptedSequenceIsUnchanged` with the frames committed in Task 1.
- [ ] **Step 2:** `git diff <Task 1's commit>..HEAD -- tests/app/forwarded_stream_test.cpp` is empty.
- [ ] **Step 3:** No commit unless a document needed a correction found here (`docs: ...`).

**Device checks for the owner (not run by the agent):**
- `mem` at idle and in the worst case of AGENTS.md section 5 on both boards: internal free and largest block against `ram_budget.hpp`; this plan adds 14 cell locks (about 1.4 KB of internal RAM, spec 7).
- The admin page's Overview, Network and System pages follow Wi-Fi, the hotspot, the SD card, charging, a time sync, a factory-reset request and a firmware upload exactly as before.
