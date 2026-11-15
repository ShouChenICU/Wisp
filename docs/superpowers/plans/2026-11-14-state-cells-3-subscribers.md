# State Cells 3: State Subscribers Move to Cells — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every component that follows another component's state (spec 3.8) subscribes to that component's cell instead of a bus topic, and the private copies folded from bus payloads (spec 1.4) are deleted with their subscriptions. After this plan, a lost or late bus event cannot leave any of them wrong; the periodic compensations still exist (plan 4 removes them) but no longer carry anything.

**Architecture:** One consumer per task, one commit per task (spec 4 step 3). Non-UI consumers subscribe with the default dispatcher (`wisp.bus`); UI consumers pass the `UiRuntime` and are called on the UI thread, with no `post()` hop. Owners built after a consumer hand it their cell through a setter or a start parameter. A subscriber that reacts to an edge inside a cell (a counter or a flag that rises) keeps the value it last acted on, initialised from `get()` when it subscribes, so the first delivery does not replay an old edge.

**Tech Stack:** C++20 (`-fno-exceptions -fno-rtti`), GoogleTest, LVGL 9.5, the simulator build (`build/sim`).

**Spec:** `docs/superpowers/specs/2026-11-14-state-cells-design.md`, sections 1.4, 3.8, 4 step 3, 5 and 7. Builds on plans 1 and 2.

## Global Constraints

- Plan 1's Global Constraints apply (branch, AGENTS.md including the new section 3 rule, locks, `misuse()`, tests without sleeps, clean output, commits, device checks never reported as done).
- Every task's filter includes `ForwardedStream` (plan 2 Task 1); its frames do not change in this plan except where Task 17 says so.
- Tests that drove a subscriber by publishing its topic (`power.state`, `system.settings`, `scene.state`, `voice.state`, `agent.state`, `net.*`, `system.time` in `tests/`) now drive the owner: through its real API (`PowerManager::enter_standby()`, `SystemSettings::set()`, FakeWifi, FakeSntp ...) in App-level tests, or through a cell the test constructs and owns in unit tests (`core::StateCell<hal::PowerLevel> power{bus, hal::PowerLevel::Active};` handed to the consumer as its cell). Name every test you change this way in the commit body; an assertion is never weakened. A test that publishes a stale event to show it is ignored (`AppNet.AStalePowerStateEventDoesNotTurnWifiOff`) stays as it is: bus events are now ignored altogether.
- "Flooded" in a test name means `wisp::test::BusFlood` holds `wisp.bus` while the action happens and is released afterwards (plan 1 Task 5): events published meanwhile are dropped; cell callbacks on `wisp.bus` run after the release; UI-dispatcher callbacks run at once.
- Each consumer task states what the consumer may now observe that it could not before (the current value at subscription, a coalesced update that skips values, a different thread) and why its logic stays right. A reviewer checks that paragraph against the code.

## Re-verify before starting

1. Plans 1-2 as merged: every accessor named below (`PowerManager::level_cell()`, `PowerMonitor::supply()`, `WifiService::wifi_cell()`/`ap_cell()`, `PairingService::pairing_cell()`, `SystemSettings::cell()`, `TimeSyncService::result_cell()`, `StorageManager::sd_cell()`, `FirmwareUpdateService::progress_cell()`, `app::reset_status()`, `SceneRuntime::overview()`, `NotificationService::summary()`, `AgentService::status_cell()`, `VoiceService::status_cell()`/`level()`), and the snapshot field names.
2. `App::start()`'s build order (owners after consumers: PowerManager after SceneWiring and the sensors; NetWiring after the agent, the voice and the scene runtime; PowerMonitor after the notifications) and `App::stop()`'s order against the table below.
3. Where `TalkKey`, `ConversationController` and `SceneToolIndexObserver` are constructed (`voice_wiring.cpp`, `agent_wiring.cpp`), and how `Shell::set_conversation()` reaches Home.
4. That `StatusShell::panel_model()` exposes the clock text (Task 2's assertion), or the nearest public observable of the time zone.

## Shutdown order (`App::stop()`, checked against `app.cpp` at `85a99aa`)

| Subscriber (released in) | Cells it follows | Owner destroyed in |
|---|---|---|
| StatusShell (`status_shell->stop()`, first) | scenes, notices, power level, Wi-Fi | `scenes.reset()`, `notifications.reset()`, `power_manager.reset()`, `net.reset()` — all later |
| Home, Shell (voice: `shell->set_conversation(nullptr)` before `voice.reset()`; the rest: `shell->stop()`) | voice; agent, power level, scenes | `voice.reset()` (after the release), `agent.reset()`, `power_manager.reset()`, `scenes.reset()` |
| ConversationController (`voice->stop()`) | voice | `voice.reset()` |
| AgentService (`agent->stop()`) | Wi-Fi | `net.reset()` |
| ReminderScheduler (`reminders->stop()`) | time sync | `net.reset()` |
| SceneRuntime, SceneWiring power (`scenes->stop_runtime()`) | supply, settings, time sync; power level | `power_monitor.reset()`, `~Impl`, `net.reset()`, `power_manager.reset()` |
| SystemDuties, SelfCheck (`duties->stop()`, `stop_self_check()`) | settings, Wi-Fi, hotspot, pairing, reset, firmware; power level, scenes | `~Impl`, `net.reset()`, `firmware.reset()`, `power_manager.reset()`, `scenes.reset()` |
| NetWiring (`net->stop()`, before its own resets) | power level, supply, its Wi-Fi, hotspot, time sync | `power_manager.reset()`, `power_monitor.reset()`; its own members after the release |
| App supply notices (where `battery_sub` is released today) | supply | `power_monitor.reset()` |
| SceneWiring SD (`scenes->stop()`) | SD | `~Impl` (`storage`) |
| App sensors (where `sensor_sub` is released today) | power level | `power_manager.reset()` |
| PowerManager (`~PowerManager`) | settings | `~Impl` (`system_settings`, declared after `bus`) |

UI-dispatcher subscriptions are all released before `ui->stop()`; `~UiRuntime` and `~EventBus` would report any that is not (plan 1 deviation 2). Task 18 pins this with a test.

## Review Focus

1. A subscriber built after the state changed shows it at once: SystemDuties started after a factory reset was requested, StatusShell started in standby, the scene runtime started after a language change. Tests in Tasks 6, 11, 13.
2. Coalescing that hides a transition: the agent no longer aborts a request on `connected → connecting → connected` between two runs, and the stall check still ends a stream on a dead link; Home still shows the error hint after `error → idle` reaches it as one update. Tests in Tasks 9 and 14.
3. A full scene queue no longer loses a time-zone, language or battery change: the change waits in `pending_device_` and arrives with the next message. Tests in Task 6.
4. A subscriber held while ten updates pass ends at the last one, and acts once per edge it can see (one low-battery notice, one recount per SD change it observes). Tests in Tasks 3, 7, 10.
5. `App::stop()` leaves `bus.cell_subscriptions() == 0` and every dispatcher's `attached() == 0`, and a start-stop-start cycle works. Test in Task 18.

## Deviations from the spec

1. **Home and the Shell follow the voice cell from `set_conversation()`**, not from their constructors or `start()`: `VoiceWiring` (the cell's owner) is built after the shell and destroyed before it stops (`App::stop()` calls `set_conversation(nullptr)` first), so a subscription made at shell start would outlive its cell. `ConversationController` exposes `voice_status()` and `voice_level()` for them.
2. **Cells whose owners are built later arrive through setters or start parameters:** `AgentService::set_wifi()` before `start()` (NetWiring is built after the agent), `ReminderScheduler::start(time)`, `SceneRuntime::attach_time_sync()` before `start()`, `SceneWiring::start_runtime(power)`. Spec 3.8 names the cells, not how they get there.
3. **NetWiring's `wifi_state_` snapshot is replaced in this plan's NetWiring task** (spec 4 lists it under step 5): that task rewrites the same lines, and the snapshot has no other writer.
4. **`voice.state` loses `level` in this plan's last task** (spec 4 lists it under step 5): as soon as Home and the talk overlay read `VoiceService::level()`, nothing reads it, and keeping it would keep the bus's main load (spec 1.2) for nothing.
5. **Battery notices follow the supply cell's edges.** A `battery_low` notice persisted from the last run is now dismissed when the device boots charging (the follower starts from "not charging" and sees the first sample's charging edge). A USB-only change while already charging no longer dismisses it: only `charging` turning true does.

---

### Task 1: PowerManager follows the settings cell

**Files:** Modify `components/wisp_app/include/wisp/app/power_manager.hpp`, `components/wisp_app/src/power_manager.cpp`, `components/wisp_app/src/app.cpp` (deps); Test `tests/app/power_manager_test.cpp`.

**Interfaces:**
- `PowerManagerDeps` gains `const core::StateCell<core::SystemSettingsValues>& system_settings;` (App passes `system_settings.cell()`; tests own a cell).
- Member `core::CellSubscription settings_sub_` (replaces the `SubscriptionId`), on `wisp.bus`: stores `screen_off_ms_`/`deep_after_ms_` from `screen_off_s`/`deep_after_s` (`valid_or()` as `read_settings()` does). The constructor also applies `get()` at once (the first `tick()` may come before the first delivery). `settings_dirty_` is no longer set; `tick()`'s periodic re-read stays until plan 4.
- May now observe: the current value at subscription (equal to what the constructor applied); only the latest of several batches (only the latest timeouts matter); the same thread as today's handler.

- [ ] **Step 1: Write the failing tests:** `PowerManager.ScreenOffFollowsTheSettingsCellFlooded` (bus held; the test's cell set ten times ending at `screen_off_s = 15`; release; `wait_idle`; `tick(last_activity + 15 s)` → Standby), `PowerManager.UsesTheSettingsStoredBeforeItWasBuilt` (cell at 20 s before construction; a tick at 20 s → Standby without any delivery having run: `ManualDispatcher` cell).
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'PowerManager|ForwardedStream' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same filter plus `-R 'AppNet|AppBoot'`. Expected: PASS.
- [ ] **Step 5: Commit** `refactor(app): PowerManager follows the settings cell`.

---

### Task 2: App reads settings from the cell; its copies go

**Files:** Modify `components/wisp_app/src/app_impl.hpp`, `components/wisp_app/src/app.cpp`; Test `tests/app/app_boot_test.cpp`, `tests/app/status_shell_test.cpp`.

**Interfaces:**
- Deleted: `merge_settings()`, `Impl::notify_vol`, `utc_offset_min`, `time_24h`, `lang`, `settings_mu`, `settings_now`, `settings_sub` and its unsubscribe in `stop()`.
- Readers become `impl->system_settings.cell().read(...)`: the router's `notify_volume` (clamped 0-100), the reminders' `utc_offset_min` and `missed_title` language, the status shell's `utc_offset_min` and `time_24h`, the shell's `settings` (`cell().get()`), the agent's language in `build_agent()`. Each is a short read under the cell's lock on the caller's thread (scene task, UI, `wisp.agent`, `wisp.fs`); none touches flash.
- May now observe: always the stored value, never a value one lost event behind.
- [ ] **Step 1: Write the failing tests:** `AppBoot.SettingsReadersFollowStoredChangesFlooded` (bus held; `set({"utc_offset_min", 60}, {"time_24h", false})`; without releasing: the status panel's clock text (re-verify item 4) uses UTC+1 and a 12-hour clock), `AppBoot.NotificationVolumeFollowsStoredSettingsFlooded` (bus held; `set({"notify_vol", 10})`; a posted system notice plays at volume 10 on the fake speaker's tone player).
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'AppBoot|StatusShell|ForwardedStream' --output-on-failure`. Expected: FAIL (the copies only follow the event).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same filter plus `-R 'NotificationRouter|Reminders|ShellSettings|AppAgent'`. Expected: PASS.
- [ ] **Step 5: Commit** `refactor(app): read settings from the cell, not from copies`.

---

### Task 3: App's sensors and battery notices follow the power cells

**Files:** Modify `components/wisp_app/src/app_impl.hpp`, `components/wisp_app/src/app.cpp`; Test `tests/app/app_boot_test.cpp`.

**Interfaces:**
- `Impl::sensor_sub` becomes `core::CellSubscription sensors_power_sub`, subscribed on `wisp.bus` right after `power_manager` is built: `sensors->set_standby(level >= hal::PowerLevel::Standby)`; released where `sensor_sub` is today.
- `Impl::battery_sub`/`charging_sub` become `core::CellSubscription supply_sub`, subscribed on `wisp.bus` right after `power_monitor->start()`, with a captured `std::shared_ptr<SupplyEdges>{bool low, bool charging}`: `charging` initialised from `supply().get()`, `low` initialised to `false`, so a battery already low when App subscribes posts the notice once (today the first sample's `power.battery.low` does that; the first sample runs in `PowerMonitor::start()`, before this subscription). On `low` rising: `dismiss_code(kBatteryLow)` then `post_system(kBatteryLow, percent)`; on `charging` rising: `dismiss_code(kBatteryLow)`. Released where `battery_sub` is today.
- May now observe: the level and the supply at subscription; one update for several (`low` cannot rise twice between two runs: the hysteresis is 5 points); the same thread.
- [ ] **Step 1: Write the failing tests:** `AppBoot.LowBatteryNoticeFollowsTheSupplyFlooded` (power device at 10 % while the bus is held; release; one `battery_low` notice with "10"), `AppBoot.BatteryLowAtBootPostsOneNotice`, `AppBoot.ChargingDismissesTheNotice`, `AppBoot.SensorsPauseInStandbyFlooded` (`SensorService` standby flag after release).
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'AppBoot|ForwardedStream' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same filter plus `-R 'NotificationRouter|SensorService'`. Expected: PASS.
- [ ] **Step 5: Commit** `refactor(app): sensors and battery notices follow the power cells`.

---

### Task 4: AgentWiring follows the settings cell

**Files:** Modify `components/wisp_app/src/agent_wiring.hpp`, `components/wisp_app/src/agent_wiring.cpp`; Test `tests/app/agent_wiring_test.cpp`.

**Interfaces:** `settings_sub_` becomes a `core::CellSubscription` on `wisp.bus` calling `apply_turn_limits(*service, v.agent_max_steps, v.agent_turn_min)`; the constructor keeps its synchronous `apply_turn_limits(get())`. May now observe: the current limits at subscription (the same as the synchronous apply); only the latest of several changes (limits apply from the next turn anyway).
- [ ] Steps as above. Tests: `AgentWiring.TurnLimitsFollowTheSettingsCellFlooded`, `AgentWiring.LimitsStoredBeforeConstructionApply`. Filter `-R 'AgentWiring|AppAgent|ForwardedStream'`. Commit `refactor(app): agent limits follow the settings cell`.

---

### Task 5: VoiceWiring reads the settings cell

**Files:** Modify `components/wisp_app/src/voice_wiring.hpp`, `components/wisp_app/src/voice_wiring.cpp`; Test `tests/app/voice_wiring_test.cpp`.

**Interfaces:** `tts_mute_`, `volume_`, `settings_sub_` deleted; where they are read (on `wisp.voice`), `deps_.settings.cell().read(...)` (volume through the existing `clamp_volume()`). May now observe: the stored value at every read.
- [ ] Steps as above. Tests: `VoiceWiring.MuteFollowsStoredSettingsFlooded` (bus held; `set({"tts_mute", true})`; the next reply is not spoken), `VoiceWiring.VolumeFollowsStoredSettings`. Filter `-R 'VoiceWiring|ForwardedStream'`. Commit `refactor(app): voice reads mute and volume from the settings cell`.

---

### Task 6: The scene runtime follows the supply, settings and time cells

**Files:** Modify `components/wisp_scene/include/wisp/scene/scene_runtime.hpp`, `components/wisp_scene/src/scene_runtime.cpp`, `components/wisp_app/src/scene_wiring.cpp` (deps; `seed_*` calls removed in `app.cpp`), `components/wisp_app/src/app.cpp`; Test `tests/scene/scene_runtime_test.cpp`, `tests/support/scene_runtime_fixture.hpp`.

**Interfaces:**
- `SceneRuntimeDeps` gains `const core::StateCell<core::PowerSupply>* supply = nullptr;` and `const core::StateCell<core::SystemSettingsValues>* settings = nullptr;`. New `void SceneRuntime::attach_time_sync(const core::StateCell<core::TimeSyncResult>* cell);` (before `start()`; App calls it with NetWiring's cell). Deleted: `seed_utc_offset()`, `seed_time_24h()`, the three bus subscriptions, `utc_offset_min_`/`time_24h_`/`lang_` atomics (the scene task reads the settings cell where it read them; without a cell it uses `deps_.utc_offset_min` and the defaults as today).
- New `std::atomic<uint8_t> pending_device_` with bits `kBattery`, `kMinute`, `kLang`, `kClockSet`. Callbacks on `wisp.bus` (each with its captured last value, initialised from `get()` at subscription): supply `charging`/`usb_present`/`battery_percent` changed → `kBattery`; settings `utc_offset_min` or `time_24h` changed → `kMinute`, `language` changed → `kLang`; time sync `attempts` moved with `outcome == Synced` → `kClockSet`. Each sets its bit, then pushes a wake message (a failed push needs no log: the bits are read after every message). The scene task takes the bits (`exchange(0)`) at the top of each loop step and runs the existing `msg::Device{...}`/`msg::ClockSet` handling for each.
- May now observe: the values at subscription (equal to the captured ones: no event at start); one update for several (one minute redraw for two time-zone changes); a full queue delays a change instead of losing it.
- [ ] **Step 1: Write the failing tests:** `SceneRuntimeTest.TimeZoneChangeSurvivesAFullQueue` (block the scene task, fill its queue, set the time zone, unblock: the scene's `minute` handler sees the new offset), `SceneRuntimeTest.LanguageChangeReachesScenes`, `SceneRuntimeTest.ChargingChangeReachesScenes`, `SceneRuntimeTest.NoDeviceEventAtStart`, `SceneRuntimeTest.SyncedTimeRedrawsOnceFailedDoesNot`.
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SceneRuntimeTest|SceneApi|ForwardedStream' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.** Update `scene_runtime.hpp`'s file header (device events come from cells).
- [ ] **Step 4: Run** `-R 'Scene|Examples'`. Expected: PASS.
- [ ] **Step 5: Commit** `refactor(scene): device events follow the supply, settings and time cells`.

---

### Task 7: SceneWiring follows the power level and SD cells

**Files:** Modify `components/wisp_app/src/scene_wiring.hpp`, `components/wisp_app/src/scene_wiring.cpp`, `components/wisp_app/src/app.cpp`; Test `tests/app/app_scenes_test.cpp`.

**Interfaces:**
- `void SceneWiring::start_runtime(const core::StateCell<hal::PowerLevel>* power);` (replaces the `standby` function): a `wisp.bus` subscription calls `rt->set_visible(level < hal::PowerLevel::Standby)`; with no cell the runtime stays visible. The value comes from the cell, which is the latest by construction, so the "read `standby()` instead of the payload" reasoning is kept.
- `sd_subs_` becomes one `core::CellSubscription` on `storage.sd_cell()` (`wisp.bus`), with a captured `changes` initialised from `get()`: when `changes` moved, `sd_changes_ += 1` and submit `files->on_sd_changed()` to the worker as today (a busy worker logs as today).
- May now observe: one update for two SD changes in a row (one recount covers both: `on_sd_changed()` scans what is there).
- [ ] **Step 1: Write the failing tests:** `AppScenes.SceneHidesInStandbyFlooded`, `AppScenes.CardChangeRecountsSceneFiles`, `AppScenes.TwoQuickCardChangesRecountAtLeastOnce`.
- [ ] Steps 2-5 as above. Filter `-R 'AppScenes|SceneFiles|ForwardedStream'`. Commit `refactor(app): scene visibility and card recounts follow cells`.

---

### Task 8: NetWiring follows the cells; its Wi-Fi snapshot goes

**Files:** Modify `components/wisp_app/src/net_wiring.hpp`, `components/wisp_app/src/net_wiring.cpp`; Test `tests/app/net_wiring_test.cpp`, `tests/app/app_net_test.cpp`.

**Interfaces:**
- `subscriptions_` becomes `std::vector<core::CellSubscription, os::LargeAllocator<...>>` on `wisp.bus`, each callback `wake_.give()`: the power level (`deps_.power_manager->level_cell()`), the supply (`deps_.power_monitor->supply()`), `wifi_->wifi_cell()`, `wifi_->ap_cell()`, and with a window the time-sync cell (`note_time_result()` when `attempts` moved, then `wake_.give()`). The loop and its level read stay (phase F).
- `wifi_state_` is deleted; `std::optional<net::WifiState> NetWiring::wifi_state() const` reads `wifi_->wifi_cell()` through an `std::atomic<const core::StateCell<net::WifiSnapshot>*>` set when `wifi_` is built and cleared at the start of `stop()`, so readers after that get `nullopt` (all readers are stopped before `net->stop()`: the shutdown table).
- `reconcile_external_power()` reads `supply().get().usb_present` instead of `last_status()`.
- May now observe: wakes for changes the loop also polls (harmless); `wifi_state()` is the published state, not the last tick's (it can only be newer).
- [ ] **Step 1: Write the failing tests:** `NetWiring.WifiStateReadsTheCell` (a FakeWifi connect: `wifi_state()` is Connected before the next tick), `NetWiring.ExternalPowerFollowsTheSupplyCell`, `AppNet.DeepStandbyReachesWifiServiceFlooded` (replacing the event-publishing part of `DeepStandbyReachesWifiServiceWhenTheEventIsLost`; the assertion is unchanged).
- [ ] Steps 2-5. Filter `-R 'NetWiring|AppNet|NetUi|ForwardedStream'`. Commit `refactor(app): the network task follows cells, not events`.

---

### Task 9: AgentService follows the Wi-Fi cell

**Files:** Modify `components/wisp_agent/include/wisp/agent/agent_service.hpp`, `components/wisp_agent/src/agent_service.cpp`, `components/wisp_app/src/app.cpp`; Test `tests/agent/agent_loop_test.cpp`.

**Interfaces:**
- `void AgentService::set_wifi(const core::StateCell<net::WifiSnapshot>* cell);` — before `start()` (App calls it after NetWiring is built, next to `attach_ws()`). `start()` subscribes on `wisp.bus`: `net_state = state == Connected`; when not up and `in_request`, `trigger(Abort::Network)`. `stop()` resets the subscription where it unsubscribes today. `AgentDeps::network_up` stays for no cell.
- May now observe: the state at `start()` (today unknown until the first event: `network_up()` falls back to `deps.network_up`); a reconnect between two runs is one update (no abort; the stall check, `on_tick()`, ends a stream on a dead link within `stall_ms`).
- [ ] **Step 1: Write the failing tests:** `AgentLoop.LostWifiAbortsTheRequest`, `AgentLoop.KnowsTheNetworkAtStart`, `AgentLoop.ReconnectBetweenRunsDoesNotAbort` (ManualDispatcher as the test's dispatcher: set connecting then connected before `run_all()`), `AgentLoop.DeadLinkStillEndsByStall`.
- [ ] Steps 2-5. Filter `-R 'AgentLoop|AppAgent|ForwardedStream'`. Commit `refactor(agent): follow the Wi-Fi cell for network loss`.

---

### Task 10: Reminders follow the time-sync cell

**Files:** Modify `components/wisp_core/include/wisp/core/reminders.hpp`, `components/wisp_core/src/reminders.cpp`, `components/wisp_app/src/app.cpp`; Test `tests/core/reminders_test.cpp`.

**Interfaces:** `void ReminderScheduler::start(const StateCell<TimeSyncResult>* time = nullptr);` subscribes on `wisp.bus` with a captured `attempts` from `get()`: moved and `outcome == Synced` → `queue_pass(false)`. `stop()` resets it. May now observe: two syncs as one update (one pass covers both).
- [ ] Tests: `Reminders.SyncedTimeQueuesAPassFlooded`, `Reminders.FailedSyncQueuesNothing`, `Reminders.SyncBeforeStartDoesNotQueueTwice`. Filter `-R 'Reminders|ForwardedStream'`. Commit `refactor(core): reminders follow the time-sync cell`.

---

### Task 11: SystemDuties follows its cells on the UI thread

**Files:** Modify `components/wisp_app/src/system_duties.hpp`, `components/wisp_app/src/system_duties.cpp`; Test `tests/app/system_duties_test.cpp`, `tests/app/net_ui_test.cpp`, `tests/app/factory_reset_test.cpp`, `tests/app/firmware_confirm_test.cpp`.

**Interfaces:**
- `settings_sub_`, `net_sub_`, `reset_sub_`, `fw_sub_` become `core::CellSubscription`s with `&ui_` as dispatcher: settings → `sync_system_language()`; Wi-Fi and hotspot → `sync_wifi()`; pairing → `sync_pairing()`; reset → `sync_reset()`; firmware → `sync_firmware()`. `on_settings_event()`, `on_net_event()`, `on_reset_event()`, `on_firmware_event()` and their "post(...) dropped" paths are deleted. The 100 ms timer, its 1 s resync and the pending flags stay (plan 4).
- May now observe: each cell at start (the start-time `sync_*()` calls become redundant; plan 4 removes them with the flags); several changes as one call (each `sync_*()` reads the service's current state).
- [ ] **Step 1: Write the failing tests:** `SystemDuties.HotspotCardFollowsTheCellFlooded` (the card is open within 200 ms, before the 1 s resync could run), `SystemDuties.PairingCardFollowsTheCellFlooded`, `SystemDuties.ResetDialogOpensFlooded`, `SystemDuties.FirmwareDialogOpensFlooded`.
- [ ] Steps 2-5. Filter `-R 'SystemDuties|NetUi|FactoryReset|FirmwareConfirm|ForwardedStream'`. Commit `refactor(app): system duties follow cells on the UI thread`.

---

### Task 12: SelfCheck follows its cells

**Files:** Modify `components/wisp_app/src/self_check_screen.hpp`, `components/wisp_app/src/self_check_screen.cpp`; Test `tests/app/self_check_screen_test.cpp`.

**Interfaces:** `power_sub_` (on `wisp.bus`: `sampler_.set_standby(level >= Standby)`), `net_sub_` (Wi-Fi and hotspot cells on the UI: `sync_net()`), `scene_sub_` (overview on the UI, captured `catalog` and `foreground`: catalog moved → `request_scene_list()`; foreground changed → `follow_foreground()`), and a new settings subscription (UI: `utc_offset_min_`, `time_24h_`, `refresh_time_sync()`) become `core::CellSubscription`s; `on_power_event()`/`on_net_event()` and their dropped paths go. The resync timers and `sync_settings()` stay (plan 4). May now observe: the error-panel close of a faulted scene (it moves `foreground`, plan 2 Task 10).
- [ ] Tests: `SelfCheckScreen.SamplerPausesInStandbyFlooded`, `SelfCheckScreen.NetSectionFollowsTheCell`, `SelfCheckScreen.ClosingAFaultedPanelIsFollowedAtOnce` (within one UI iteration, not 30 s), `SelfCheckScreen.TimeZoneFollowsTheCell`. Filter `-R 'SelfCheck|ForwardedStream'`. Commit `refactor(app): diagnostics follow cells`.

---

### Task 13: StatusShell follows its cells

**Files:** Modify `components/wisp_app/src/status_shell.hpp`, `components/wisp_app/src/status_shell.cpp`, `components/wisp_app/src/app.cpp`; Test `tests/app/status_shell_test.cpp`.

**Interfaces:** `StatusShellDeps` gains `const core::StateCell<scene::SceneOverview>* scenes_cell`, `const core::StateCell<core::NoticeSummary>& notices_cell`, `const core::StateCell<hal::PowerLevel>* power`, `const core::StateCell<net::WifiSnapshot>* wifi`; `bus` goes. Four `core::CellSubscription`s on `&deps.ui`: the first three and Wi-Fi call `refresh()`; the power one calls `layer.close_panel()` at `Standby`/`DeepStandby`, else `refresh()`. `request_refresh()`, `refresh_queued` and the "UI queue full" path go. May now observe: standby at start (the panel cannot open on a dark screen anyway); one refresh for a burst (it reads the services).
- [ ] Tests: `StatusShell.StandbyClosesThePanelFlooded`, `StatusShell.NewNoticeRefreshesTheCapsule`, `StatusShell.StartedInStandbyShowsNoPanel`. Filter `-R 'StatusShell|ForwardedStream'`. Commit `refactor(app): the status shell follows cells`.

---

### Task 14: Home follows the agent, voice and power cells

**Files:** Modify `components/wisp_shell/include/wisp/shell/home.hpp`, `components/wisp_shell/src/home.cpp`, `components/wisp_shell/include/wisp/shell/conversation.hpp`, `components/wisp_shell/src/conversation.cpp` (two accessors), `components/wisp_shell/include/wisp/shell/shell.hpp`, `components/wisp_shell/src/shell.cpp` (deps pass-through), `components/wisp_app/src/app.cpp`; Test `tests/shell/home_test.cpp`.

**Interfaces:**
- `ConversationController`: `const core::StateCell<voice::VoiceStatus>& voice_status() const;`, `uint8_t voice_level() const;` (forwarding to its `VoiceService`).
- `HomeDeps` gains `const core::StateCell<agent::AgentStatus>* agent = nullptr;` and `const core::StateCell<hal::PowerLevel>* power = nullptr;` (`ShellDeps` gains the same two and passes them); `bus` is no longer used by Home.
- `subs_` become `core::CellSubscription`s on `&deps_.ui`: agent (state; `errors` moved since the captured value → `error_at_ = now`), power (`standby_`), and, from `set_conversation(conv)`, voice (state; `outcomes` moved with `outcome == Error` → `error_at_`), released by `set_conversation(nullptr)`. Each calls `request(false)`. `level_` is read from `conv_->voice_level()` where the refresh uses it. `on_bus()` and `payload_str()` go.
- May now observe: an `error → idle` as one update with `errors` moved (the hint still shows: `error_at_` is set from the counter); values at subscription; delivery on the UI thread (the atomics stay for the press logic, written on the same thread now).
- [ ] **Step 1: Write the failing tests:** `HomeTest.ErrorThatEndsInIdleStillShowsTheHint` (the test's agent cell set Error then Idle before one UI iteration: the hint shows), `HomeTest.VoiceErrorShowsTheHint`, `HomeTest.StandbyEndsAPendingPress` (from the power cell), `HomeTest.LevelComesFromTheVoiceService` (a FakeVoice level of 80 animates the wisp while listening, no event).
- [ ] Steps 2-5. Filter `-R 'HomeTest|ShellTalk|ForwardedStream'`. Commit `refactor(shell): home follows the agent, voice and power cells`.

---

### Task 15: The Shell follows the scenes and voice cells

**Files:** Modify `components/wisp_shell/include/wisp/shell/shell.hpp`, `components/wisp_shell/src/shell.cpp`, `components/wisp_app/src/app.cpp`; Test `tests/shell/shell_talk_test.cpp`, `tests/shell/scene_list_test.cpp`.

**Interfaces:** `ShellDeps` gains `const core::StateCell<scene::SceneOverview>* scenes = nullptr;`. `scene_sub_` becomes a `CellSubscription` on `&deps_.ui` (`request_talk()`; `list_scenes()` when the list is open); `talk_voice_sub_` is made in `set_conversation()` from `conv->voice_status()` (`voice_state_`, `request_talk()`), and `voice_level_` reads `conv->voice_level()` in `talk_timer_cb`. `on_voice_bus()` goes. May now observe: the overview at start; several scene changes as one relist.
- [ ] Tests: `ShellTalkTest.OverlayFollowsTheVoiceCell`, `ShellTalkTest.OverlayLevelComesFromTheVoiceService`, `SceneListTest.OpenListRelistsOnCatalogChange`. Filter `-R 'ShellTalk|SceneList|TalkOverlay|ForwardedStream'`. Commit `refactor(shell): the shell follows the scenes and voice cells`.

---

### Task 16: ConversationController follows the voice cell

**Files:** Modify `components/wisp_shell/include/wisp/shell/conversation.hpp`, `components/wisp_shell/src/conversation.cpp`; Test `tests/shell/conversation_test.cpp`.

**Interfaces:** `voice_sub_` becomes a `CellSubscription` on `wisp.bus` with a captured `outcomes` from `get()`: moved with `outcome == Error` → `view_.error = parse_error(reason)`; with `NotHeard` → `view_.not_heard = true`; `state == Idle` → the deferred-hold check. `on_voice_state()` takes a `const voice::VoiceStatus&`. May now observe: an `error` that reached `idle` before the callback ran (the counter keeps it); the state at start.
- [ ] Tests: `ConversationTest.ErrorThatEndsInIdleSetsTheError`, `ConversationTest.NotHeardIsSeenFlooded`, `ConversationTest.DeferredHoldStartsWhenIdleArrives`. Filter `-R 'ConversationTest|ShellTalk|ForwardedStream'`. Commit `refactor(shell): the conversation follows the voice cell`.

---

### Task 17: `voice.state` comes from the mirror, without `level`

**Files:** Modify `components/wisp_voice/include/wisp/voice/voice_service.hpp`, `components/wisp_voice/src/voice_service.cpp`, `docs/architecture.md` (`voice.state` row); Test `tests/voice/voice_service_test.cpp`.

**Interfaces:** the voice cell gets a mirror publishing `voice.state {state, reason (Error), result: "not_heard" (Idle after a not-heard capture)}` with source `"voice"`; `publish()` no longer publishes by hand; `publish_level()` only stores the level (atomic, `level()`), at most every `kLevelIntervalMs` as today. The `WLOG_I` lines stay. `voice.state` is not forwarded, so `ForwardedStream` is unchanged; this is the one bus payload this plan changes (spec 3.6).
- [ ] Tests: `VoiceService.ListeningPublishesNoLevelEvents` (a 500 ms loud capture: `voice.state` events equal the state changes, none has `level`), `VoiceService.ErrorThenIdlePublishesBoth`. Filter `-R 'VoiceService|HomeTest|ShellTalk|ConversationTest|ForwardedStream'`. Commit `refactor(voice): voice.state comes from the status cell, without the level`.

---

### Task 18: Shutdown releases every subscription; full check

**Files:** Test `tests/app/app_boot_test.cpp`.

- [ ] **Step 1: Write the test** `AppBoot.StopReleasesEverySubscription`: a full App (display, Wi-Fi, scenes, voice); `start()`; `stop()`: `bus().cell_subscriptions() == 0` and `bus().attached() == 0`; `start()` again succeeds and the same counts are back to their first-start values; `stop()`.
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R AppBoot --output-on-failure`. Expected: PASS (a failure here is a shutdown-order bug in an earlier task: fix it there).
- [ ] **Step 3:** Run `bash tools/ci.sh`. Expected: exit 0.
- [ ] **Step 4: Commit** `test(app): stop releases every cell subscription`.

**Device checks for the owner (not run by the agent):**
- `WISP_PERF` during a spoken turn on Home and with the talk overlay over a scene: frame rate not below the baseline (the level is read by the UI timers now, not delivered 20 times a second).
- Standby and wake with a scene in front, the status panel open and the diagnostics open: each follows as before.
- `mem`: `cell_subs=` stable over an hour of use (no growth: no leaked subscription).
