# Backlog

Deferred work, collected in fast mode from 2026-11-06: Minor review findings, items the MVP spec leaves out, and device checks that were not run. Each entry points to where it came from. When an item is picked up, it goes through the normal design → plan → implement flow, or into the nearest plan.

## Out of the MVP (MVP spec 1.7)

- Voice:
  - wake word;
  - streaming ASR with live captions;
  - saying "确认" to confirm.
- Wisp:
  - colour presets;
  - charging, low-battery and dozing faces;
  - the mini wisp as a Lua widget inside scenes.
- 4a:
  - daily token budget and usage stats;
  - `ui.check`;
  - screenshots and vision;
  - simulated input (`ui.tap` / `ui.swipe` / `ui.type`).
- Scene list: pin and export.
- Settings page: wisp colour, storage management.
- All of 4b:
  - `wisp.tools.register`;
  - headless runs;
  - `wisp.agent.ask` / `notify`;
  - dynamic injection;
  - peripheral pre-emption.
- A scene's `screen` lock: burn-in protection, low-battery fallback.
- Volume on a hardware key once BOOT becomes the talk key (MVP spec 7.3).
- A monospace font for code blocks (MVP plan, ruling M13).

## 3b-1 parked review findings

Source: `feat/scene-device` review records.

- Panel occlusion on rect screens (the hide-screen approach crashes LVGL 9.5). Decide from the panel `WISP_PERF` reading on the device.
- `StatusShell`:
  - the scene-name cache is not invalidated on delete or reinstall;
  - `read_name` sticks if `ui.post` fails;
  - the UTC offset is cached twice.
- `power.state` recovery after a dropped event takes about 50 s.
- `IMU set_power` is not synchronised.
- There is no counter for dropped sensor batches.
- `SceneFiles`:
  - the narrow adoption-vs-remove race;
  - `count_dir` is unbounded at scan;
  - the install-vs-queued-write race.
- Migration:
  - several scenes migrating at boot can take up to about 6 s;
  - a firmware downgrade drops a pending migration journal.
- `restore_background` only logs when the worker is busy.
- On power loss during a reinstall, a background scene can drop out of the list.
- Tests:
  - several negative assertions are sleep-based;
  - `AppScenes.SelfCheckResyncsAfterAFaultedPanelCloses` takes about 30 s.

## 3b-1 device checks not run in the smoke pass

The full list is the 30-row device acceptance checklist kept with the `feat/scene-device` review records. The smoke pass runs rows 1, 10, 11, 17, 19, 20, 22 and 26. On 2026-11-06 rows 10a, 11, 17, 19, 20 and 26a passed; rows 1, 10b/c, 22 and 26b/c are still to run. The rows outside the smoke pass are:

- 2: standby drain.
- 3: audio and standby.
- 4: Wi-Fi radio cycles.
- 5: network window (40 minutes).
- 6: queued requests on wake.
- 7: half-open admin WebSocket.
- 8: wake-lock table. Note that scene `screen` locks no longer get the 30-minute reminder.
- 9: `/system/power` in every level.
- 12: rebuild time.
- 13: migration.
- 14: a confirmed delete that cannot go ahead.
- 15: capsule.
- 16: top-edge swipe.
- 18: panel `WISP_PERF`.
- 21: `wisp.imu`.
- 23: SD pulled mid-download.
- 24: power cut mid-download.
- 25: admin download of tens of MB.
- 27: `wisp.tone` stack.
- 28: scene file list with 256 or more files.
- 29: panel with 50 notices.
- 30: smaller allocations.

## MVP parked review findings

Source: `feat/agent-core` review records.

- `net::LlmClient`:
  - a stored profile whose free-form `vision`/`context_window` option fails the new check is dropped at load, key included; drop only the bad option there;
  - an abort on an old ticket between retry attempts is a no-op, so callers must also make `cancelled()` true;
  - a transport error after `finish_reason` but before `[DONE]` fails a complete reply;
  - per-event `from_json` strings under 16 KB land in internal RAM briefly; measure with `mem` once `wisp.agent` exists.
- `ConversationStore`: after a failed append leaves half a line, the next good line joins it and is skipped on load; truncate or start a fresh line before the next append.
- `AgentService`:
  - `remove_listener` can deadlock when a listener removes itself from its own callback before `start()`, or blocks on the UI thread while the UI thread removes it; listeners must not block on another thread;
  - the `TurnBegin` text and serialized tool results are `std::string`, briefly in internal RAM; include them in the device `mem` measurement;
  - the done-notice id carries the reply's first line, so each turn adds a notice instead of replacing one.
- Context assembly:
  - `sv()` and `prefix_bytes()` are duplicated between `context.cpp` and `agent_service.cpp`; move them to one internal header;
  - the prompt takes the date from `wall_s` but the weekday name from `DeviceSnapshot::weekday`; derive the name from `weekday_of(wall_s, offset)` and keep `weekday == 0` only as the "clock unset" flag;
  - the compaction summary is a mid-history `system` message; gateways that hoist system messages would move it and break the cached prefix; consider a `user` message;
  - `compress()` appends while the loop still holds the history snapshot, which forces a full PSRAM copy; append after the snapshot scope closes;
  - a summary request that keeps timing out counts as a transport error, so the history never compresses and every request carries it whole;
  - the four new `AgentLoop` tests sit outside the test namespace.
- Scene drafts and install:
  - a power cut after `remove_tree(prev)` and before `rename(dir, prev)` loses the previous version (`recover()` runs with `keep_prev=false`); rename to a temporary name first, or document the window;
  - `remove_here` returns NotFound before cleaning up `.prev/<id>` when the scene is not installed, so an orphaned `.prev` stays;
  - `kMaxDraftBytes` repeats `SceneRepoConfig::max_code_bytes` instead of reading it, and the draft file count is not capped at `kMaxFiles` until install;
  - a `write_file` rejected with `quota_exceeded` still creates the draft (`ensure_draft_here` runs first);
  - `fold_tree` re-renders the whole tree after each fold (O(n·k)); time a 2000-node tree on the device;
  - `install()` never checks `ctx.cancelled`, so a stopped turn still waits through confirm, check, install and settle;
  - `scene_workspace.cpp` re-implements the directory walk from `scene_repository.cpp`;
  - `wait_stable()` watches the whole-screen frame counter, so a system-layer animation during a turn may keep `stable` false on the device.
- Scene-ops and device tools:
  - `docs.read` copies a topic into a `std::string` (the `lua` topic is ~16 KB, just under the internal-RAM malloc threshold) and `widgets_overview()` grows one; return a view or `os::LargeString`;
  - the `lua` docs topic repeats the widgets reference (Value types through Widgets), costing ~4K tokens twice; exclude those sections;
  - `notify.list` builds system-notice text in the device language, not the conversation's;
  - `AppScenes.DeleteSceneWithoutDialog` needs its own `EventBus` because `os::set_flash_thread()` is one global taken over by a second worker;
  - `ThreadsRisksAndTimeouts` asserts `timeout_ms` on agent-thread tools, which the registry ignores;
  - `tr_fill_n` has no direct test in `strings_test.cpp`.
- Agent REST/WebSocket wiring:
  - the conversation `epoch` is bumped by the `Cleared` listener after the store already changed, so a `GET /conversation` in that window pairs new messages with the old epoch; keep a generation counter in the store read under the history lock; `message(seq)` carries no epoch;
  - `POST /agent/messages` copies the up-to-12 KB text twice in internal RAM (`as_string()` then the argument); take a `std::string_view`;
  - `DeltasCoalescedPer100ms` relies on real time; inject `now_ms`;
  - `AppAgent.StopsWithATurnInFlight` does not actually stop mid-turn (no model, the turn ends at once);
  - no test for `DELETE /agent/conversations` without CSRF or any agent route without a token;
  - the WS bridge formats every settled message even with no `agent` subscribers;
  - the `queued` flag in the 202 can be wrong under a concurrent voice submit;
  - `device.status` storage use lags one call and is null on the first call after boot;
  - with no `net.wifi` device (simulator) the context reports the network as up.
- Device confirm and chat page:
  - `AgentService::stop()` clears pending commands, so a queued Clear never answers `cleared` and the web page waits on the device;
  - the comment at `agent_service.cpp` "history gains no extra system or user message" reads as if no message is added, but a `Note` is (not sent to the model);
  - a confirm close dropped on a full UI queue leaves the dialog up, and auto screen-off can blank the screen during the 30 s confirm; hold a wake lock while asking;
  - the chat page hides messages by role rather than the `hidden` field;
  - the `ws.js` typedef comment does not list the `agent` kind;
  - the dropped-frame catch-up and the "allow" branch of Clear all were not exercised live (no scripted LLM traffic, no simulated dialog tap).
- Memory notes:
  - `AgentLoop.MemoryNotesInTheRequest` rebuilds the context lambda itself; add an `agent_wiring_test` case for `memory_path_of`, the load in `start()` and the context wrapper;
  - the notes path is derived from `conversation_dir`; give `AgentWiringDeps` its own `memory_path` (an empty `conversation_dir` yields `/memory/notes.json`);
  - whitespace-only text is accepted as a note;
  - write-failure rollback and `update` returning `empty`/`too_long` have no test;
  - the mutex is held across `write_atomic`, so `list()` waits for a write; matters once the admin HTTP thread calls it;
  - a write that fails after the `.new` file is complete rolls memory back while the next read restores `.new`;
  - a failed `clear()` leaves `load_failed_` cleared.
- Reminders and todos:
  - the default `esp_timer` path (`make_timer` empty) is untested; every arm re-creates a timer from internal RAM, measure with `mem` on the device;
  - no test covers rollback after a failed write in `add`/`update`/`done`/`snooze`/`remove`;
  - the tool output says `snoozed_until`, the stored field is `snooze_until`.
- Wi-Fi pages:
  - the "已保存的网络" card rows on the network page have no connect button; only a pick in the scan list connects directly;
  - the setup wizard's step indicator marks "输入密码" done when a saved network was joined without one;
  - the firmware's app version string is fixed when CMake configures, so an incremental `app-flash` reports an older `git describe`.
- Firmware builds with `-Werror=missing-field-initializers`, the host build does not, so `tools/ci.sh` misses designated initializers that omit fields; enable the warning on the host or build the firmware in CI.

## Glyph partition parked review findings

Source: glyph partition review records.

- Simulator: an explicit but unreadable `--glyph-image <path>` silently falls back to the host built-in font (`simulator/main.cpp`); disable the built-in when a path was given, or warn louder.
- `internal_library_quota` defaults to 3 MiB (`storage_manager.hpp`) while the reference board's `storage` partition is now 2.94 MiB, so the Library quota can never trigger; size it per board or as a fraction of the partition.
- `glyph_image.cpp`: the cmap rejections for a FORMAT0_FULL `list_length` below `range_length` and for a list present or absent against the type have no tests.
- `fonts.cpp` re-decodes the payload length from the raw header; `GlyphFont` could expose it.
- `keep_glyph_font`'s "different bytes -> new font" branch has no test.
- After a different image is installed, the old A8 wrapper's cached glyphs keep using the shared 512 KiB arena (simulator and tests only).
- `a8_set_fallback` silently ignores a font that is not an A8 wrapper; a debug assert would catch misuse.
- Each `UiRuntime::start()` re-validates the image and briefly allocates about 130 KB of PSRAM before dropping it when the old font is reused.

## Flash-thread parked review findings

- The admin page's reboot runs `App::flush_storage()` (settings flush up to 2 s, then a storage drain up to 5 s) on the esp_timer task, which can stall every other esp_timer callback for up to 7 s; move the flush onto its own short-lived task.
- When the storage worker cannot start (out of memory at boot), callers have no fallback path: requests are refused rather than run inline; decide whether boot should fail loudly or degrade.

## Memory page (MVP 5.3)

- `GET /memory` answers 200 with an empty list when `MemoryStore` failed to load, or before `AgentWiring::start()` has loaded it (`net->start()` runs first). The page then says "no notes yet" while the notes are still on flash. Expose `available()` and answer 503 `memory_unavailable`, and give the page an error state. Add a load_failed test.
- `web/app/pages/memory.js`: when the list is already populated, a failed refresh shows nothing, because the alert only renders while the list is empty. Toast the error instead.

## Voice clients (MVP spec 2)

- `AsrClient`: the 15 s timeout applies to each attempt, so with one retry the worst case is 15 + 1 + 15 = 31 s. `LlmClient` keeps one overall deadline across its retries. Decide whether ASR should do the same.
- `ModelProfileService` load: when a stored profile's options fail the newer per-kind checks (e.g. `language` on an llm profile), the whole profile is dropped, key included, without a word. Strip the bad options and keep the profile instead.

## Conversation controller (MVP shell)

- After the agent dequeues a message and before its TurnBegin, `view()` shows neither running nor queued, and while `run_turn()` checks for a rollover that gap can be long. During it, a tap can start a capture, and `hold_begin()` does not stop the just-dequeued web turn. Fix: AgentService exposes a busy flag that covers the slot/active state, and `hold_begin()` always calls `stop_turn()`.
- `voice.state` events carry no capture id. An `idle`/`not_heard` or `error` that arrives late from capture N can mark capture N+1 as "没听清". Fix this when building the shell UI.
- `notify()` copies the whole `TurnView` once per listener on every Delta, which is O(n²) over a turn. Revisit if long replies show up in WISP_PERF.
- Test gaps in the shell tests: TalkKey when the screen turns off mid-hold; settings changes reaching the cached tts_mute and volume.
- ESP `esp_http_client.cpp` attaches the socket to `RequestWatch` only after `esp_http_client_open()` returns, so a stop pressed during DNS/TCP connect/TLS handshake waits for the connection to succeed or fail (worst case: the open timeout). Needs an interruptible connect in the ESP port; not reproducible in the simulator. (A stop before the LLM request is honoured at the agent layer; verified.)

## Home screen (MVP shell)

- Shell home: without a `ConversationController` (voice off), the first-boot "no Wi-Fi / no model" hint never shows. Pass a readiness callback through ShellDeps → HomeDeps from the app wiring so the hint does not depend on voice.

## Shell sub-pages (MVP spec 6.4)

- Device checks not run: `bench scroll` with the conversation history open and with the scene list open (with icons), against the "性能基准" section of the board doc; the cost of the first icon decode on the worker, and the wisp.fs stack high-water mark (`mem`) after opening a scene list with icons, since lodepng now runs on that 8 KB internal stack; touch feel of swipe versus scroll on the sub-pages.
- The settings page reads the settings once when it is built: a change made by voice or from the admin page while it is open shows only after it is reopened, and the screen-off highlight follows the tap even if the write fails. Subscribe to `system.settings` while the page is open.
- Scene list open on the device (9 scenes, no icons, no SD card): the first open after boot lists in 539 ms (was 770–1283 ms), later opens in 8–40 ms (`cached=1`); the build is 38–66 ms. The first open still re-reads each `scene.json` in the wisp.files lookups with the card out; listing once in the background after boot, or keeping the chosen files location with the cached listing, would remove it. Not yet measured: how long the long-press card takes to show "同时删除数据" (one `removal()` probe, about 16 file calls).
- Small long-lived containers in internal RAM: `SceneRecents`' map and the scene list's `scenes_`/`cards_` vectors (one entry per scene), and also:
  - `SceneRepository::changed_` (`std::vector<std::pair<std::string, uint64_t>>`), never pruned: every scene ever installed or removed keeps an entry for the life of the process; drop entries older than every reader's generation, or cap it at 2×`kMaxScenes`;
  - `Shell::scene_cache_` (`std::vector<ShellScene>` with `std::string` id and name);
  - `Shell::IconCache::by_id` (`std::map<std::string, Entry>`).

  Tiny today; move to PSRAM containers (`os::LargeVector`/`LargeString`) if the scene count grows.

## Agent steps and page drags (MVP spec 6.1–6.3)

- Home's step rows are restyled and relaid out on every `refresh()` while a turn is shown (each 250 ms tick plus each controller change, rate-limited to 50 ms): `fill_row()` in `home.cpp` sets the mark's text and colour and measures every label even when nothing changed, so four rows redraw about 4–20 times a second. Small, but it is redraw on the device during a turn; skip a row whose label and state are unchanged, keeping the last state per row.
- Device checks not run: the larger wisp (`kWispIdlePercent` 45 → 55) and the drag bench have simulator numbers only (the simulator caps at about 62 fps). AGENTS.md §4 asks for a `WISP_PERF` comparison and spec 6.1 for at least 20 fps on the device: measure `wisp_fps`/`wisp_ms` idle on Home at 55%, and `bench drag 10` fps/cpu, against the board doc's baseline.
- The activity label table (`activity.cpp` kLabels) and its test (`tests/shell/activity_test.cpp`) both list the registry's tools by hand: a tool added later silently falls back to "调用 <tool>" and the test does not notice. Add an app-level test that owns a full `ToolRegistry` (for example in `agent_wiring_test`), walks the registered tools and asserts `activity_label()` uses no fallback for any of them.
- `hold_off_long_press()` in `shell.cpp` writes LVGL's private `pr_timestamp`/`long_pr_sent`. Correct for the pinned LVGL 9.5.0, but an LVGL bump can rename or repurpose those fields without a compile error; add a `static_assert` on `LVGL_VERSION_MAJOR`/`LVGL_VERSION_MINOR` next to it so an upgrade forces a recheck.
- A skipped call's step row is dropped at the next Thinking or TurnEnd; when the step list was already full, the older row it pushed out of the last four does not come back, so the list may show three rows. Harmless; keep one spare row if it shows.

## Talk overlay (MVP spec 6.5)

- `TalkOverlay::decide()` has no phase for a voice error (`VoiceState::Error`) or `TurnView::error`/`not_heard`: it falls through to Hidden. Confirm whether that is the intended split with Home (which may already surface these) before adding one.

## CI speed

- `tools/ci.sh` runs ctest serially (about 390 s; `sim_smoke` alone takes 92 s). `ctest -j 20` finished in 92 s on a 20-core host, but under that load `AppScenes.DeleteAfterStopRuntimeIsRefused` aborted once (it passes alone, three runs out of three). Find its timing assumption first, then run ctest in parallel, keeping the smoke tests `RUN_SERIAL` if they share ports.

## Agent core tool enhancements

- `lua.run` failures now carry their full payload in `ToolOutput.result_json`, while `AgentService::execute_tool` still reads the diagnostic error code only from `ToolOutput.result`. Runtime/compile/timeout/memory/cancelled failures therefore log `error:failed`; the model result and ToolEnd summary remain correct. Preserve a small structured error code or expose explicit metadata without parsing the large JSON payload. Add a regression at the production consumer boundary.
