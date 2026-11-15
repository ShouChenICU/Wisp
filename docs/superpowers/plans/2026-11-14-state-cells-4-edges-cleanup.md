# State Cells 4: Edges, Compensations Removed, Documents — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Button edges and scene lifecycle edges reach their consumers through listeners and `EdgeInbox`es, never lost silently; the periodic work that exists only because the bus may drop (C1-C8 of spec 1.3) is deleted, one compensation per task, each with a test that its consumer converges without it; the documents describe the result. After this plan `WsHub` is the only device-side bus subscriber.

**Architecture:** `ButtonService` calls listeners on its own thread, in order, for every `down`/`up`/`short`/`long`, with the edge's own timestamp; listeners only push into an inbox, store an atomic or schedule a slot. The scene runtime reports every state it publishes through `SceneRuntimeDeps::on_lifecycle`; App drains them from one inbox on `wisp.bus` into the recents, the notices and the agent's tool index. The bus keeps publishing `button.*` and `scene.state` unchanged.

**Tech Stack:** C++20 (`-fno-exceptions -fno-rtti`), GoogleTest, LVGL 9.5, the simulator build (`build/sim`).

**Spec:** `docs/superpowers/specs/2026-11-14-state-cells-design.md`, sections 1.3, 3.5, 3.8 (the button and `scene.state` rows), 3.9, 4 steps 4-6, 5, 6 and 7. Builds on plans 1-3.

## Global Constraints

- Plan 1's Global Constraints apply; plan 3's rules on tests that drove a subscriber by publishing apply to `button.*` too: a test that published `button.power.*` drives `ButtonService::emit_for_test()` (Task 1) instead. Name each changed test in the commit body; no assertion is weakened.
- Every task's filter includes `ForwardedStream`; its frames do not change in this plan.
- A compensation is removed only in its own task, after the task (plan 3) that moved its consumer to the cell, and each removal task has two tests: a consumer that starts after the change shows it, and a consumer held while ten updates pass (its dispatcher blocked by a posted job or `BusFlood`) ends at the last one, with the compensation's code gone.
- AGENTS.md's comment rules: code comments and `docs/` (outside `docs/superpowers/`) never name "C1"..."C8" (the CI guard rejects them); they say what the code does.

## Re-verify before starting

1. Plans 1-3 as merged, in particular `EdgeInbox`'s constructor, `App::dispatch_report()`, the cell accessors, `SystemDuties`' and `SelfCheck`'s subscriptions, and `PowerManagerDeps`.
2. `ButtonService`: every `publish_event()` call site, the lock it holds there (`state_mutex_` on the worker, the fallback timer's thread), and the clock of its timestamps (`os::now_us()` base expected; `TalkKey`'s `now_ms_` is `os::now_ms()`).
3. Where `TalkKey` is built and started (`voice_wiring.cpp` expected) and whether it can reach `ButtonService` (App's `buttons`).
4. `SceneToolIndexObserver` (in `agent_wiring.cpp`): its bus subscription, the states it handles (`headless`, `error`, `deleted`, `running`), and who owns it.
5. The tests that publish `button.power*` (about 140 references), `kSettingsRefreshTicks`, `kNetResyncMs`, `kSceneResyncMs`, `revision()`, `take_factory_reset_request`, `take_firmware_request`, `request_confirm` (`grep -rn` in `tests/` and `components/`).
6. The largest number of background scenes the runtime restores at boot (its memory budget), against the lifecycle inbox's 32.

## Review Focus

1. The talk key's `down`/`up` with `wisp.bus` flooded: the capture starts and ends; a hold is timed from the edge's timestamp, so a late delivery does not stretch or shorten it. Tests in Task 4.
2. A button press in standby while the UI's post queue is full and `wisp.bus` is flooded: the screen wakes (the wake latch and its UI slot), the power key still toggles the screen (its inbox), and no edge is counted as an overflow. Tests in Tasks 2 and 3.
3. A scene deleted while `wisp.bus` is flooded: its notices are dismissed, its tools leave the index and it leaves the recents, in that order relative to its `stopped`; restoring the background scenes at boot overflows nothing. Tests in Task 7.
4. Each compensation removal: a late consumer and a held consumer converge, and the removed periodic work no longer runs (a counting backend or a probe shows zero reads). Tests in Tasks 8-15.
5. An inbox that does overflow is visible: `mem` prints `inbox <name> overflows=<n>`, and the warning is logged. Tests in Tasks 3, 4 and 7.

## Deviations from the spec

1. **PowerManager's activity hook runs in its UI slot, not in the listener.** Spec 3.5 has the listener "note activity (an atomic, as today)". Today's button handler also calls `on_activity()` (which silences an alarm through `TonePlayer`); a listener runs on `wisp.button` under `ButtonService`'s lock and may only push, store or schedule, so the listener sets an `activity_pending_` flag and the PowerManager slot (on the UI dispatcher, `wisp.bus` without a screen) runs `on_activity()` and the undim. The idle clock is still stamped in the listener.
2. **`ButtonService::emit_for_test()`** is added as the "listener seam" spec 4 step 4 asks for: it runs the same path as a real edge (listeners, then the bus event) without a button device.
3. **Inbox names** in `mem`: `power_key` (SystemDuties), `talk_key` (TalkKey), `scene_lifecycle` (App). The spec leaves them unnamed.
4. **The scenes overview's `catalog` also moves on an SD card change and on a dev-mode change.** Plan 2's deviation 9 said "dev-mode file edits and a card coming or going do not move `catalog`"; that no longer holds. `SceneWiring` calls `SceneRuntime::note_catalog_changed()` once per card change it sees, at the end of its recount job on `wisp.fs` (after the card's scenes are marked; also from the `sd_cell()` callback when that job could not be queued), and once per `poll_scenes()` that reports a change; only once the runtime has started (nothing follows the overview before, and there is none without a screen). Spec 3.8's deletion of the diagnostics' 30 s scene relist needs it: the relist was the only trigger for those two cases. The Shell's open scene list now also lists again on a card change (it already relisted on its own `sd_changes_` at its next listing).

---

### Task 1: Button listeners

**Files:** Modify `components/wisp_core/include/wisp/core/button_service.hpp`, `components/wisp_core/src/button_service.cpp`; Test `tests/core/button_service_test.cpp`.

**Interfaces:**

```cpp
namespace wisp::core {
struct ButtonEdge {
    enum class Kind : uint8_t { Down, Up, Short, Long };
    FixedString<31> id;  // the button's device id, e.g. "button.power"
    Kind kind = Kind::Down;
    uint64_t at_us = 0;  // os::now_us() base: the confirmed edge (Down, Up, Short) or the long deadline (Long)
};
using ButtonListener = os::LargeFunction<void(const ButtonEdge&)>;
// ButtonService:
uint32_t add_listener(ButtonListener fn);   // any thread; ids start at 1
void remove_listener(uint32_t id);          // after it returns `fn` is not running and never runs again
void emit_for_test(std::string_view id, ButtonEdge::Kind kind, uint64_t at_us);  // as a real edge
}
```

`publish_event(state, suffix)` becomes `emit(state, kind, at_us)`: it calls every listener in registration order under a leaf `listeners_mutex_` (taken inside `state_mutex_`; the order is `state_mutex_ → listeners_mutex_ → an inbox's lock`), then publishes the `button.<id>.<edge>` event exactly as today. Listener contract (header comment): push into an `EdgeInbox`, store an atomic or `schedule()` a slot; nothing else.
- [ ] **Step 1: Write the failing tests:** `ButtonService.ListenerGetsEveryEdgeInOrderWithItsTime` (a press of 1.2 s on a FakeButton: Down, Long, Up in order; `Long.at_us == Down.at_us + 1 000 000`; a short press: Down, Up, Short), `ButtonService.RemovedListenerIsNeverCalledAgain` (remove while the worker is emitting: no call after return), `ButtonService.BusEventsAreUnchanged`, `ButtonService.EmitForTestReachesListenersAndTheBus`.
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'ButtonService|ForwardedStream' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.** Update `docs/architecture.md`'s "Button input and standby" section (listeners first, then the bus copy).
- [ ] **Step 4: Run** the same filter plus `-R 'PowerManager|SystemDuties|TalkKey|SelfCheck'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): let button consumers listen to every edge`.

---

### Task 2: PowerManager wakes through a listener and a slot

**Files:** Modify `components/wisp_app/include/wisp/app/power_manager.hpp`, `components/wisp_app/src/power_manager.cpp`; Test `tests/app/power_manager_test.cpp`, `tests/app/app_net_test.cpp` (tests that published `button.*`).

**Interfaces:** `button_sub_` becomes `uint32_t button_listener_` on `deps.buttons` (none without buttons). The listener stamps `last_activity_ms_`, sets `activity_pending_`, sets `wake_latch_` when `standby()` and the edge is not the owned power key (`power_key_owned_` and `id == "button.power"`), and schedules `wake_slot_` (a `core::DispatchSlot` member) on `ui_` (or the bus without a screen). The slot, on that thread: `activity_pending_.exchange(false)` → `on_activity_()` and the undim; `wake_latch_.exchange(false) && standby()` → `exit_standby()`. `~PowerManager()` removes the listener, then cancels the slot (it is not called on the UI task: existing precondition) and detaches. `run_on_ui("exit_standby", ...)` for buttons goes. Deviation 1.
- [ ] **Step 1: Write the failing tests:** `PowerManager.ButtonWakesFromStandbyWithAFullUiQueue` (UI paused in standby, post queue filled, `wisp.bus` flooded; `emit_for_test("button.vol_up", Down)`: the screen wakes), `PowerManager.OwnedPowerKeyIsLeftToItsOwner`, `PowerManager.ButtonActivitySilencesAlarmsOnTheUi` (the `on_activity` hook ran on the UI thread).
- [ ] Steps 2-5. Filter `-R 'PowerManager|AppNet|ForwardedStream'`. Commit `fix(app): a button wakes the screen even when every queue is full`.

---

### Task 3: SystemDuties' power key through an inbox

**Files:** Modify `components/wisp_app/src/system_duties.hpp`, `components/wisp_app/src/system_duties.cpp`, `components/wisp_app/src/app.cpp` (`dispatch_report()`); Test `tests/app/system_duties_test.cpp`, `tests/app/app_boot_test.cpp`.

**Interfaces:** `button_sub_` becomes a listener on App's `ButtonService` that pushes `button.power` edges into `os::LargeUnique<core::EdgeInbox<core::ButtonEdge, 8>> power_key_` (`"power_key"`, dispatcher `&ui_`); the drain maps `Short` → `power_key_.on_short(screen_on)`, `Long` → `on_long(screen_on)` with `screen_on` read from `PowerManager::standby()` as today. `on_button_event()` and its dropped path go. `App::dispatch_report()` appends `inbox power_key overflows=<n>` while the duties exist.
- [ ] **Step 1: Write the failing tests:** `SystemDuties.PowerKeyWorksFlooded` (bus held; short press via `emit_for_test`: the screen goes off), `SystemDuties.PowerKeyEdgesKeepTheirOrder` (short then long within one UI iteration: off, then the power-off dialog), `AppBoot.DispatchReportListsThePowerKeyInbox`.
- [ ] Steps 2-5. Filter `-R 'SystemDuties|AppBoot|PowerKey|ForwardedStream'`. Commit `fix(app): the power key never loses a press`.

---

### Task 4: The talk key through an inbox

**Files:** Modify `components/wisp_shell/include/wisp/shell/talk_key.hpp`, `components/wisp_shell/src/talk_key.cpp`, its builder (re-verify item 3), `components/wisp_app/src/app.cpp` (`dispatch_report()`); Test `tests/shell/talk_key_test.cpp`.

**Interfaces:** TalkKey takes `core::ButtonService&` in place of the bus; `start()` adds a listener pushing its own id's `Down`/`Up` into `core::EdgeInbox<core::ButtonEdge, 8>` (`"talk_key"`, on `wisp.bus`, where its logic runs today); the drain calls `on_down(at_us)`/`on_up()`. `down_ms_ = at_us / 1000` (the edge's time, not the dispatch time), so the hold threshold counts from the press. `stop()` removes the listener before resetting the inbox. `dispatch_report()` appends `inbox talk_key overflows=<n>`.
- [ ] **Step 1: Write the failing tests:** `TalkKey.UpIsNeverLostFlooded` (Down, Up while the bus is held: after release the capture began and ended), `TalkKey.HoldIsTimedFromTheEdge` (a Down whose `at_us` is 600 ms old: the first tick starts the hold), `TalkKey.ShortPressDoesNotHold`.
- [ ] Steps 2-5. Filter `-R 'TalkKey|ShellTalk|ConversationTest|ForwardedStream'`. Commit `fix(shell): the talk key never loses its release`.

---

### Task 5: SelfCheck's button test listens

**Files:** Modify `components/wisp_app/src/self_check_screen.hpp`, `components/wisp_app/src/self_check_screen.cpp`; Test `tests/app/self_check_screen_test.cpp`.

**Interfaces:** `button_sub_` becomes a listener that posts `tests_section_.set_button_event(<id>.<edge>)` and `pass_test(Button)` (a plain post; a lost diagnostics display is harmless, spec 3.5). After this task no device-side component subscribes to `button.*`.
- [ ] Tests: `SelfCheckScreen.ButtonTestPassesOnAPress`. Filter `-R 'SelfCheck|ForwardedStream'`. Commit `refactor(app): the button test listens to the button service`.

---

### Task 6: The scene runtime reports lifecycle edges

**Files:** Modify `components/wisp_scene/include/wisp/scene/scene_runtime.hpp`, `components/wisp_scene/src/scene_runtime.cpp`, `components/wisp_scene/src/scene_runtime_tools.cpp`, `components/wisp_app/src/scene_wiring.hpp`, `components/wisp_app/src/scene_wiring.cpp`; Test `tests/scene/scene_runtime_test.cpp`, `tests/app/app_scenes_test.cpp`.

**Interfaces:**

```cpp
namespace wisp::scene {
struct SceneTransition {
    enum class State : uint8_t { Stopped, Running, Background, Error, Headless, Deleted };
    FixedString<32> id;
    State state = State::Stopped;
};
// SceneRuntimeDeps: called on the scene task right after each scene.state the runtime publishes,
// in the same order; must not block (App pushes into an inbox). Empty: none.
std::function<void(const SceneTransition&)> on_lifecycle;
}
```

`SceneWiringDeps` gains the same callback, passes it to the runtime and calls it with `Deleted` right after publishing `deleted`.
- [ ] Tests: `SceneRuntimeTest.LifecycleCallbackSeesEveryPublishedState` (open, background, front, close: Running, Background, Running, Stopped; a fault: Error; a headless call: Headless), `AppScenes.DeleteReportsDeletedAfterStopped`. Filter `-R 'SceneRuntimeTest|SceneBackground|SceneTools|AppScenes|ForwardedStream'`. Commit `feat(scene): report every scene lifecycle edge`.

---

### Task 7: App and the tool index follow lifecycle edges from one inbox

**Files:** Modify `components/wisp_app/src/app_impl.hpp`, `components/wisp_app/src/app.cpp`, `components/wisp_app/src/agent_wiring.hpp`, `components/wisp_app/src/agent_wiring.cpp` (`SceneToolIndexObserver`); Test `tests/app/app_scenes_test.cpp`, `tests/app/scene_tool_index_observer_test.cpp`, `tests/app/app_boot_test.cpp`.

**Interfaces:**
- `App::Impl` gains `os::LargeUnique<core::EdgeInbox<scene::SceneTransition, 32>> lifecycle` (`"scene_lifecycle"`, on `bus`), built right before the scene runtime and passed as `on_lifecycle = [inbox](const auto& t) { inbox->push(t); }`. Its handler: `Running` → `recents->opened(id)`; `Deleted` → `recents->forget(id)` and `notes->dismiss_source(id, Dismiss)`; every transition → `agent->tool_index()->on_transition(t)` when there is an agent. `scene_state_sub` goes. Destroyed after `scenes->stop()` (no more pushes) and before `recents`/`notifications` are reset.
- `SceneToolIndexObserver::on_transition(const scene::SceneTransition&)` replaces its bus subscription (same handling of `Running`, `Headless`, `Error`, `Deleted`).
- `dispatch_report()` appends `inbox scene_lifecycle overflows=<n>`.
- [ ] **Step 1: Write the failing tests:** `AppScenes.DeleteDismissesNoticesAndForgetsFlooded`, `AppScenes.OpenMovesTheSceneToTheFrontOfTheRecentsFlooded`, `SceneToolIndexObserver.TransitionsRefreshAndRemove` (the existing bus-driven cases rewritten on `on_transition`), `AppScenes.RestoringBackgroundScenesOverflowsNothing` (the most the budget allows, re-verify item 6: `overflows() == 0`).
- [ ] Steps 2-5. Filter `-R 'AppScenes|SceneToolIndex|AppBoot|ForwardedStream'`. Commit `fix(app): scene lifecycle edges reach the recents, notices and tools`.

---

### Task 8: PowerManager no longer re-reads Settings (C1)

**Files:** Modify `components/wisp_app/include/wisp/app/power_manager.hpp`, `components/wisp_app/src/power_manager.cpp`, `components/wisp_app/src/app.cpp`; Test `tests/app/power_manager_test.cpp`.

**Interfaces:** deleted: `kSettingsRefreshTicks`, `settings_dirty_`, `ticks_since_read_`, `read_settings()`, `PowerManagerDeps::settings` (the `"power"` namespace; PowerMonitor keeps its own). The constructor takes the timeouts from `system_settings.get()`.
- [ ] Tests: `PowerManager.TickReadsNoSettings` (a counting `SettingsBackend` behind the cell's owner: zero `get()` calls over 120 ticks), `PowerManager.LateConstructionUsesTheCurrentTimeouts`, `PowerManager.HeldBusEndsAtTheTenthChange` (ten changes while the bus is held; release; the tenth applies). Rewrite or delete the tests that proved the 60-tick catch-up (re-verify item 5), naming them in the commit body. Filter `-R 'PowerManager|AppNet|ForwardedStream'`. Commit `refactor(app): drop PowerManager's periodic settings read`.

---

### Task 9: SystemDuties' one-second resync goes (C2)

**Files:** Modify `components/wisp_app/src/system_duties.hpp`, `components/wisp_app/src/system_duties.cpp`; Test `tests/app/system_duties_test.cpp`, `tests/app/net_ui_test.cpp`.

**Interfaces:** deleted: `kNetResyncMs`, `last_net_resync_ms_`, the resync block of `refresh_timer_cb()` and the start-time `sync_wifi()`/`sync_pairing()` calls (the cells deliver at subscription). The 100 ms timer stays for the cards' countdowns (`pair_card_.tick()`, `setup_card_.tick()`).
- [ ] Tests: `SystemDuties.HotspotOpenedBeforeStartShowsTheCard`, `SystemDuties.HotspotCardEndsAtTheLastOfTenChangesWithTheUiHeld` (a posted job blocks the UI while the hotspot opens and closes ten times; release: the card matches the last state), `SystemDuties.PairingCodeIssuedBeforeStartShowsTheCard`. Rewrite the tests that proved "a dropped event is caught up within a second" (re-verify item 5) into these. Filter `-R 'SystemDuties|NetUi|ForwardedStream'`. Commit `refactor(app): drop the system duties' network resync`.

---

### Task 10: The 100 ms language poll goes (C3)

**Files:** Modify `components/wisp_app/src/system_duties.hpp`, `components/wisp_app/src/system_duties.cpp`; Test `tests/app/system_duties_test.cpp`.

**Interfaces:** `sync_system_language()` takes the language from the settings callback's value (no `revision()`, no `get()`); `language_revision_` and the per-tick call go.
- [ ] Tests: `SystemDuties.LanguageChangedBeforeStartIsShown`, `SystemDuties.LanguageEndsAtTheLastOfTenChangesWithTheUiHeld`, `SystemDuties.NoSettingsReadOnTimerTicks` (a counting backend: zero reads across 50 ticks). Filter `-R 'SystemDuties|ForwardedStream'`. Commit `refactor(app): follow the language from the settings cell only`.

---

### Task 11: The factory-reset pending flag goes (C4)

**Files:** Modify `components/wisp_app/src/factory_reset.hpp`, `components/wisp_app/src/factory_reset.cpp`, `components/wisp_app/src/app_impl.hpp`, `components/wisp_app/src/system_duties.hpp`, `components/wisp_app/src/system_duties.cpp`; Test `tests/app/factory_reset_test.cpp`, `tests/app/system_duties_test.cpp`.

**Interfaces:** deleted: `Impl::reset_pending`, `take_factory_reset_request()`, `sync_reset()`'s flag. SystemDuties keeps `uint32_t handled_reset_seq_`: a `Requested` status with `seq != handled_reset_seq_` opens the dialog (ignored while a confirm dialog is open, as today) and records the seq; any other phase closes the reset dialog if open. The web events are unchanged (`ForwardedStream`).
- [ ] Tests: `SystemDuties.ResetRequestedBeforeStartOpensTheDialogAtStart`, `SystemDuties.ResetRequestedWhileFloodedOpensOnTheNextIteration`, `SystemDuties.RepeatedRequestWhileOpenIsIgnored`, `FactoryReset.WebEventsUnchanged` (requested, cancelled, done frames). Filter `-R 'FactoryReset|SystemDuties|NetUi|ForwardedStream'`. Commit `refactor(app): hand a factory-reset request to the dialog through its cell`.

---

### Task 12: The firmware pending flag goes (C5)

**Files:** Modify `components/wisp_core/include/wisp/core/firmware_update.hpp`, `components/wisp_core/src/firmware_update.cpp`, `components/wisp_app/src/firmware_confirm.hpp`, `components/wisp_app/src/firmware_confirm.cpp`, `components/wisp_app/src/app_impl.hpp`, `components/wisp_app/src/app.cpp`, `components/wisp_app/src/system_duties.cpp`; Test `tests/core/firmware_update_test.cpp`, `tests/app/firmware_confirm_test.cpp`, `tests/app/system_duties_test.cpp`.

**Interfaces:** deleted: `Impl::firmware_pending`, `request_firmware_confirm()`, `take_firmware_request()`, `FirmwareUpdateService::Deps::request_confirm`. `Deps` gains `bool confirmable = false;` (App sets it with a screen); `begin()` refuses with `Unavailable` when it is false (today's "no screen" refusal). SystemDuties opens the firmware dialog on `Received` with an unhandled `seq` and closes it on any other phase.
- [ ] Tests: `FirmwareUpdate.UploadRefusedWithoutAScreen`, `SystemDuties.FirmwareReceivedBeforeStartOpensTheDialog`, `SystemDuties.FirmwareTimeoutClosesTheDialog` (2 min fake clock: `Rejected` closes it), `FirmwareConfirm.WebEventsUnchanged`. Filter `-R 'Firmware|SystemDuties|ForwardedStream'`. Commit `refactor(app): hand a firmware image to the dialog through its cell`.

---

### Task 13: SelfCheck's network and scene resyncs go (C6)

**Files:** Modify `components/wisp_app/src/self_check_screen.hpp`, `components/wisp_app/src/self_check_screen.cpp`; Test `tests/app/self_check_screen_test.cpp`.

**Interfaces:** deleted: `kNetResyncMs`, `kSceneResyncMs`, `last_net_resync_ms_`, `last_scene_list_ms_` and their blocks in `refresh_timer_cb()` (the storage measurement stays: spec 2.2), and the build-time `sync_net()`/`follow_foreground()`/`request_scene_list()` calls the first deliveries replace.
- [ ] Tests: `SelfCheckScreen.ErrorPanelCloseIsFollowedWithoutResync` (fault the front scene, close its panel: followed within one UI iteration; no 30 s timer exists), `SelfCheckScreen.NetSectionEndsAtTheLastOfTenChangesWithTheUiHeld`, `SelfCheckScreen.OpenedAfterAnInstallListsIt`. Filter `-R 'SelfCheck|ForwardedStream'`. Commit `refactor(app): drop the diagnostics' network and scene resyncs`.

---

### Task 14: SelfCheck's settings poll goes (C7)

**Files:** Modify `components/wisp_app/src/self_check_screen.hpp`, `components/wisp_app/src/self_check_screen.cpp`; Test `tests/app/self_check_screen_test.cpp`.

**Interfaces:** deleted: `sync_settings()`, `settings_revision_` and its per-tick call; the settings callback (plan 3 Task 12) is the only writer of `utc_offset_min_`/`time_24h_`.
- [ ] Tests: `SelfCheckScreen.TimeZoneSetBeforeOpeningIsShown`, `SelfCheckScreen.NoSettingsReadOnTimerTicks`. Filter `-R 'SelfCheck|ForwardedStream'`. Commit `refactor(app): diagnostics follow settings from the cell only`.

---

### Task 15: `SystemSettings::revision()` goes (C8)

**Files:** Modify `components/wisp_core/include/wisp/core/system_settings.hpp`, `components/wisp_core/src/system_settings.cpp`; Test `tests/core/system_settings_test.cpp`.

**Interfaces:** deleted: `revision()`, `revision_`; the file header's "with revision() as the fallback when that event is lost" becomes "consumers follow `cell()`". Tests that used `revision()` use `cell().revision()` (which moves only on a change; a test that relied on an unchanged batch moving it asserts the event instead, as plan 2 Task 6 pinned).
- [ ] Tests: the rewritten ones; `grep -rn "revision()" components | grep -v cell` is empty. Filter `-R 'SystemSettings|SystemDuties|SelfCheck|ForwardedStream'`. Commit `refactor(core): drop SystemSettings::revision()`.

---

### Task 16: Documents and full check

**Files:**
- Modify: `docs/architecture.md`:
  - "Event topics" table: a `Kind` column (cell mirror, notification, telemetry, input edge, from `topics::kAll`) and, in the notes of each cell-mirror topic, the cell that owns the state (`PowerManager::level_cell()`, ...); the `system.factory_reset` and `system.firmware` notes lose the pending flag and the once-a-second resync (the dialog follows the cell); `scene.state` gains `headless` and says consumers use `SceneRuntime::overview()` and the lifecycle inbox; `system.settings`'s note loses `revision()`; `button.*`'s note says consumers listen to `ButtonService`.
  - "State cells and edge inboxes" (plan 1): the cells of spec 3.4 in one table (owner, type, dispatcher of each subscriber), the inboxes and their sizes, and "after this phase `WsHub` is the only device-side bus subscriber".
  - "Stop order": subscriptions are released before their owners, and `~UiRuntime`/`~EventBus` report any left.
  - "Serial console": `mem`'s `inbox <name> overflows=<n>` lines.
  - The `wisp.tone` row ("the `audio/notify_vol` that App caches from `system.settings`" becomes "read from the settings cell").
- Modify: `docs/superpowers/specs/2026-11-03-connectivity-admin-design.md` line 267's publisher (`SystemSettings` 的状态单元（state cell）镜像发布) and line 449 (the device UI follows the settings cell).
- Modify: `CHANGELOG.md` (an `Unreleased` entry: state cells, inboxes, no lost button edges, compensations removed; the version is set at merge, AGENTS.md section 7).
- Modify: `docs/backlog.md`: add the device checks below; remove any entry this phase closed (lost power-key presses, lost talk-key releases, the self-check resync), if present.

- [ ] **Step 1:** Edit the documents; `python3 tools/check_topics.py` still passes (the table's first column is unchanged).
- [ ] **Step 2:** `grep -rnE "bus(_|\(\))?\.subscribe\(" components simulator` lists only `components/wisp_admin/src/ws_hub.cpp` (spec 8: `WsHub` is the only device-side bus subscriber).
- [ ] **Step 3:** Run `bash tools/ci.sh --full`. Expected: exit 0, no warnings in test output.
- [ ] **Step 4:** Confirm in the `--full` output that the simulator smoke scenarios for the agent conversation, scene lifecycle variations and navigation on each screen shape passed; record their names in the commit body.
- [ ] **Step 5: Commit** `docs: record state cells, edge inboxes and the topic kinds`.

**Device checks for the owner (not run by the agent):**
1. `mem` at idle and in the worst case of AGENTS.md section 5 (STA+AP, two httpd, the WebSocket log stream, a TLS handshake, an OTA upload) on both boards: internal free and largest block against `ram_budget.hpp`; `wisp.bus`, `wisp_ui` and `wisp.button` stack high-water marks; record them in `docs/hardware/<board>.md`.
2. `WISP_PERF` during a spoken turn on Home and over a scene: frame rate not below the baseline.
3. The power key, the talk key and a button wake from standby and from deep standby, including during a voice turn and with a background scene busy.
4. Factory reset and firmware update from the web page: the dialog on the device, the page following the events, the restart.
5. After a day of normal use: `mem`'s `bus dropped=` and every `inbox ... overflows=` line.
