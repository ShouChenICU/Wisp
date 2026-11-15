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
- The rest of 4b:
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
- Tests: several negative assertions are sleep-based.

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
  - a power cut after `remove_tree(prev)` and before `rename(dir, prev)` loses the previous version (`recover()` runs with `keep_prev=false`); rename to a temporary name first, or document the window. Only a turn's first install of a scene (and `scene.rollback`, and installs outside a turn) still pass through this window: later installs of the same turn leave `.prev` alone (agent-core spec 4.1);
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
- Firmware builds with `-Werror=missing-field-initializers`, the host build does not, so `tools/ci.sh` misses designated initializers that omit fields until the firmware build in CI catches them; enable the warning on the host.

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

- After the agent dequeues a message and before `run_turn()` sets `running_turn`, `view()` shows neither running nor queued. A hold's transcript that arrives in that gap sees nothing to stop and waits behind the turn instead of replacing it (documented in `conversation.hpp`). The gap is an instant, except when the turn starts with a conversation rollover (`archive()` on flash). Fix if it shows: AgentService exposes a busy flag that covers the slot/active state, and `on_transcript()` stops on it.
- Neither `voice.state` events nor the voice status cell carry a capture id. An `idle`/`not_heard` or `error` that arrives late from capture N can mark capture N+1 as "没听清": a new capture started before the previous capture's "not heard" reaches the conversation controller shows "not heard" while listening. Needs a capture id in the voice status.
- `notify()` copies the whole `TurnView` once per listener on every Delta, which is O(n²) over a turn. Revisit if long replies show up in WISP_PERF.
- Test gaps in the shell tests: TalkKey when the screen turns off mid-hold; settings changes reaching the cached tts_mute and volume.
- ESP `esp_http_client.cpp` attaches the socket to `RequestWatch` only after `esp_http_client_open()` returns, so a stop pressed during DNS/TCP connect/TLS handshake waits for the connection to succeed or fail (worst case: the open timeout). Needs an interruptible connect in the ESP port; not reproducible in the simulator. (A stop before the LLM request is honoured at the agent layer; verified.)

## Home screen (MVP shell)

- Shell home: without a `ConversationController` (voice off), the readiness checklist ("no Wi-Fi", "connecting", the missing models) never shows: `Home::readiness()` falls back to a complete `Readiness`. Pass a readiness callback through ShellDeps → HomeDeps from the app wiring so the hint does not depend on voice.

## Shell sub-pages (MVP spec 6.4)

- Device checks not run: `bench scroll` with the conversation history open and with the scene list open (with icons), against the "性能基准" section of the board doc; the cost of the first icon decode (now on `wisp.shell.list`; its stack is measured under "Storage worker lanes: device checks not run"); touch feel of swipe versus scroll on the sub-pages.
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

- The overlay shows turn-end reasons and voice errors (Note phase), but not "没听清" (`TurnView::not_heard`): a hold over a scene that heard nothing just hides the card. Home shows it. Decide whether the overlay should too.

## Agent turn reliability: device checks not run

Source: `docs/superpowers/plans/2026-11-14-agent-turn-reliability.md`. Simulator and unit tests only so far.

- `mem` after a turn with a 64 KB `scene.read_file` result: the `Mem::Large` peak and the `wisp.agent` stack high-water mark.
- A real turn over weak Wi-Fi that stays associated (requests fail at the TCP/TLS level): the retry is invisible apart from the delay; no doubled text or speech. A Wi-Fi disconnect is not retried: the turn ends `network_lost` (during a request) or `network` (between steps), and Home shows the matching short reason.
- On the 1.75C round AMOLED panel: the Stop button's touch target and spacing, and the taller talk overlay card with no clipping; with a voice turn's card up, a sub-page scrolled to its end shows its last row above the card. The overlay card on e-paper too.
- A provider 400 for an over-long context on the owner's gateway: the turn recovers (note which `error.code` the gateway sends).
- The Stop button's colours on a two-colour panel.
- A provider's real `Retry-After` and 4xx bodies over TLS.

## Agent turn reliability parked review findings

- Deferred: wait for Wi-Fi to reconnect and resume the turn. Today a Wi-Fi disconnect ends a running turn (`Abort::Network` during a request, the `network_up()` check between steps; agent-core spec 3.6); the retries only cover TCP/TLS failures while the link stays up.
- `docs.read` has no tests for an `offset` in the middle of a character or exactly at the end of the text.
- The agent loop has no test for the keep-1 compression step on its own, nor for the 50/300 clamp of `est_scale_pct`.
- The estimate scale is keyed on the model name only: two profiles with the same model name on different providers share it.
- A voice text refused "busy" in the window between the stop and the submit shows no outcome on screen.
- `Home::fitted_url()` and `TalkOverlay::fit()` redo layout work on every refresh.
- The talk overlay's tap tests send `LV_EVENT_CLICKED` directly and bypass hit-testing.
- No boundary test for the elided call arguments at exactly `kCallArgsKeepBytes` and one byte over.
- No test for a `docs.read` page of a long topic that has no newline to break at (the cut falls back to a character boundary).
- A summary request's reported usage does not calibrate the estimate scale, and no test pins that down.
- The same voice error twice in a row is not shown again (Home and the talk overlay only show a change of `TurnView::error`).
- With a small context window, the keep-1 compression runs before the current turn's live tool results are cut, so one large read can cost the earlier turns' detail; consider cutting the live results first.

## Voice and wake responsiveness: device checks not run

Source: `docs/superpowers/plans/2026-11-14-voice-wake-responsiveness.md`. Simulator and unit tests only so far.

- Recognition accuracy with the start tone inside a primed recording, on both boards; if it suffers, set `VoiceConfig::preroll_ms = 0` for that board and report.
- Opening the microphone at the press: no pop in the speaker, no I2S reconfiguration glitch; `mem` for the `wisp.voice` stack high-water mark.
- A spoken multi-sentence reply during a long body: no pause after the first sentence; `mem` worst case with the LLM stream and a TTS request open together.
- Deep standby, then press and speak at once: the sentence is transcribed after Wi-Fi reconnects; time from release to transcript.
- Standby current with the changed `TalkKey` (the press tracked while the screen is off): unchanged.
- Readiness right after a standby wake: does a hold record while Wi-Fi reconnects (Home shows "正在连接 Wi-Fi…"), and how long until the transcript.
- The microphone opening about 300 ms after a dark-screen press (an unprimed capture) against the audio path powering up on wake: no lost or garbled start.
- The readiness checklist and the tap hint on the round 466 panel (1.75C): fully visible, wrapped as in the simulator.

## Voice and wake responsiveness parked review findings

- The talk key and the wisp share one `press_held_` in `ConversationController`: a key tap during a pending wisp press ends that press's priming, so the wisp's hold loses its pre-roll.
- Some negative voice tests rely on short real sleeps (`PrimeIsSkippedWhileSpeechPlays`, `PrerollZeroDisablesPriming`, `UnprimeBeforeTheTaskSawThePrime`): they could pass by luck or flake under heavy load.
- No app-level test of a hold while Wi-Fi reconnects: `HoldRecordsWhileConnecting` checks only the controller's gate, and the voice tests the wait.
- A tap hint can interrupt an error hint's window: a tap during a voice error's or turn reason's `kFadeAfterMs` shows `HomeHoldToTalk` in its place for that time.
- "." is not a sentence terminator in `SentenceSplitter`, so an English say is synthesized as one piece: its first audio waits for the whole say, up to the say's close.
- The English checklist wording ("Needed: chat, listening") is terse, shortened to fit the 360 px round shape; the owner may want to reword it (the ja `HomeNeedLlm` "対話" likewise).

## Scene scheduling: device checks not run

Source: `docs/superpowers/plans/2026-11-14-scene-scheduling-install-safety.md`. Simulator and unit tests only so far.

- `mem` at the worst case: a large `wisp.http.download` in flight and an agent `http.fetch` of an HTTPS page at the same time (two TLS sessions from the proxy at once), with STA+AP and both httpd instances up. Internal free and largest block against `kWorstInternalFreeMin`/`kWorstInternalLargestMin` (`ram_budget.hpp`), and the stack high-water marks of `wisp.shttp` and `wisp.shttp.dl`. Record them in `docs/hardware/<board>.md`.
- `mem` idle after boot: the cost of the second HTTP worker (TCB, watch timer, semaphore) against `kIdleInternalFreeMin`.
- The agent's usual `lua.run` scripts (JSON over a fetched page, a loop over a 64 KB result) on the S3 at the 200 ms slice: none ends in `timeout`; also `wisp.json.decode` of a full agent HTTP response at the 200 ms slice.
- Tap latency on the front scene while a background scene runs a 50 ms timer and the agent waits on fetches: no visible lag.
- `WISP_PERF` while scrolling and while replacing the items of a long list (ledger example) against the baseline, and no blank frame on the reset, on the 2.16 and the 1.75C round panel; and how long a large list reset takes to rebuild on the device.
- Two `scene.install` calls of one scene in one spoken turn, then "undo" (`scene.rollback`): the version from before the turn comes back after a restart too (the `.prev` on flash).

## Scene scheduling parked review findings

- After a list reset the old rows' image and canvas pixels count toward the view's 4 MB media budget until the new rows arrive, so a media growth in the same batch can be refused where it used to fit.
- A narrow race between the install session check (`SceneWorkspace::in_session()`) and the start of staging: an admin upload of the same id that completes in between is replaced without becoming `.prev`.
- No test for `SceneHttp::start()` failing on only the second worker (the simulator's spawn hook fails every spawn).
- No integration test of the `no_memory` result path (a box item with a null payload) through the lanes' `run_item`, nor of `Link::complete`'s `wake_failed()` branch with a live runtime.
- A VM whose callbacks keep producing immediate results (a refused call made from each callback) delays its own due timers: within one VM a waiting result runs before a taken timer.
- In the simulator, a request stuck in its TLS handshake (a server that accepts and never answers) is not stopped by `cancel_scene()` or `SceneHttp::stop()`: libcurl's progress callback did not run during that wait, so `wisp_sim` took 13.6 s to exit on SIGTERM with a scene `wisp.http.get` in that state (its 15 s deadline) and would wait up to the 5-minute deadline for a download. The worker and `SimHttpClient` code path is the same as before the two lanes; check whether the device's client stops at an abort there. With two lanes, two requests can be stuck at once; `stop()` aborts both before joining, so the waits overlap and the worst case is the longer one.
- `queue_load` still drops a loaded-package message when the scene queue is full (`<id>: loaded, but the scene queue is full`); call results no longer go through the queue, but this receipt does.

## State cells primitives: device checks not run

Source: `docs/superpowers/plans/2026-11-14-state-cells-1-primitives.md`. Simulator and unit tests only so far.

- Build the firmware first: the ESP-only branch of `dispatch_misuse()`, the bounded wait in the cell's teardown and the `mem` change in `firmware/main/main.cpp` have never been compiled.
- `mem` at idle on both boards: the line `bus dropped=0 cell_subs=0`; internal free and largest block unchanged within noise against the last values in `docs/hardware/<board>.md` (the plan adds two `os::Mutex` objects, about 200 bytes of internal RAM); the `wisp.bus` and `wisp_ui` stack high-water marks.
- `WISP_PERF` on Home and in a scene: frame rate not below the baseline (the UI loop now checks an empty ready list once per iteration).
- Deep standby for 10 minutes or more on both boards: light-sleep residency and wake counts against `docs/hardware/<board>.md` (the bus loop and the paused UI branch changed).
- Factory reset and firmware confirm on the device: the admin page still shows `system.factory_reset` `done` and `system.firmware` `confirmed` before the reboot, now that `wait_idle()` also covers slots.
- Parked: LeakSanitizer reports the glyph slot table in `a8_font.cpp` (kept for the whole process) in every UI test binary, so ASan runs of `ui_runtime_test` and `ui_cells_test` need `detect_leaks=0` or a suppression file.
- Parked: `tools/check_topics.py` would report a dotted code identifier or file name (`` `wisp.fs` ``) in the first column of another table inside the "Event topics" section as a topic row; no such table exists there today.
- Parked: no test fails if `UiRuntime`'s final drain after `stop()` were to run dispatch slots.
- For the plan that moves subscribers onto the cells: the power supply cell changes on every battery-percent step (up to once a second), so a subscriber that only follows charging or low filters in its callback, or the cell gets a coarser twin; after a failed read the cell keeps the last good supply while `PowerMonitor::last_status()` is empty, so the cell alone does not show that the reading is stale; on a boot that is already low a subscriber may briefly see the first percent with `low` still false before the next delivery.

## State cells beside the events: device checks not run

Source: `docs/superpowers/plans/2026-11-14-state-cells-2-cells.md`. Simulator and unit tests only so far.

- NOT RUN: `mem` at idle and in the worst case of AGENTS.md section 5 on both boards: internal free and largest block against `ram_budget.hpp`; this plan adds 14 cells, about 2.5 KB of internal RAM (14 locks of about 100 bytes plus about 1.1 KB of cell bodies in the 12 internally allocated owners), not the 1.4 KB the spec counted.
- NOT RUN: the admin page's Overview, Network and System pages follow Wi-Fi, the hotspot, the SD card, charging, a time sync, a factory-reset request and a firmware upload exactly as before.
- NOT RUN: a factory reset on the device ends with the settings at their defaults and the device rebooting (the settings cell is reseeded after the wipe job returns).
- NOT RUN: the firmware for both boards has not been built on this branch; the ESP-side code was compiled on the host only where it is shared.

## State cells beside the events parked review findings

- Parked: `exit()`/`close()` of a scene can return before its slot leaves the background list: `close_slot()` answers `done` on the worker before the scene task erases the slot (`erase_slot()`), so a caller that reads `background_scenes()` or the scenes overview right after can still see the scene. Likely cause of the load-flaky `SceneBackground.ExitClosesFromAnyState`.
- The reseed after a factory-reset wipe moves the settings cell to the defaults just before the reboot, so PowerManager adopts the default timeouts in those moments (the system duties keep the screen language: they ignore the language once the reset is confirmed or `done`).
- No test pins the `agent.state` payload for `waiting_confirm`.

## State cell subscribers: device checks not run

Source: `docs/superpowers/plans/2026-11-14-state-cells-3-subscribers.md`. Simulator and unit tests only so far.

- NOT RUN: the firmware for both boards has not been built on this branch since these consumers moved to the cells; the ESP-side code was compiled on the host only where it is shared.
- NOT RUN: standby and wake with a scene in front, with the status panel open and with the hardware diagnostics open: each follows as before (the scene is hidden and shown again, the panel closes at standby, the diagnostics' sampler pauses and resumes).
- NOT RUN: `mem` over an hour of use (turns, scenes opened and closed, the diagnostics and the panel opened and closed, standby and wake): `cell_subs=` stays stable, with no growth (no leaked subscription).
- NOT RUN: `WISP_PERF` on Home while listening and while speaking: the wisp's level animation is smooth at about 20 Hz (Home's timer runs every 50 ms then and passes only the level on between full refreshes), and the frame rate is not below the baseline in `docs/hardware/<board>.md`.
- NOT RUN: `WISP_PERF` with the talk overlay up over a sub-page or a scene while listening and while speaking: its level bar animates at about 20 Hz while it shows Listening (the shell's timer runs every 50 ms then and moves only the bar, no full `update_talk()`), and the frame rate is not below the baseline in `docs/hardware/<board>.md`.

## State cell subscribers parked review findings

Notes for the next state-cell plan (removing the periodic compensations).

- `PowerSupply` has no `known` flag: after a failed read the supply cell keeps the last good sample, so NetWiring's powered-Wi-Fi check reads `PowerMonitor::last_status()` instead of the cell. A `known` flag (false after a failed read) would let followers read only the cell and be woken by a failed read.
- The UI-thread `sync_wifi()`/`sync_pairing()` of the system duties take the Wi-Fi and pairing services' mutexes, which are held across driver calls and persistence; read the snapshot cells instead.
- Untested abort paths: the agent failing to start followed by `App::stop()`, and a failed `VoiceWiring::start()`. Both release every subscription by reading the code; a leak would abort in `~StateCell`, but no test drives either path.
- At start the system duties call `sync_wifi()` twice (the Wi-Fi and hotspot cells' first deliveries).
- Home can show the idle hint for one frame before a voice error's hint: its mood follows the voice cell's `outcomes` on the UI thread, while `ConversationController` sets the error on `wisp.bus`.
- A standby and a wake handled inside one UI loop iteration coalesce to `Active` for the status shell, which then leaves the pull-down panel open across the blink. Power key presses are seconds apart, so this needs two posted jobs drained together.

## State cell edges and cleanup: device checks not run

Source: `docs/superpowers/plans/2026-11-14-state-cells-4-edges-cleanup.md`. Simulator and unit tests only so far.

- NOT RUN: the firmware for both boards has not been built on this branch (it now also contains the storage lanes and the device model part 1 from `main`); the ESP-side code was compiled on the host only where it is shared.
- NOT RUN: `mem` at idle and in the worst case of AGENTS.md section 5 (STA+AP, two httpd, the WebSocket log stream, a TLS handshake, an OTA upload) on both boards: internal free and largest block against `ram_budget.hpp` (three inboxes add an `os::Mutex` each in internal RAM); the `wisp.bus`, `wisp_ui` and `wisp.button` stack high-water marks. Record them in `docs/hardware/<board>.md`.
- NOT RUN: `WISP_PERF` during a spoken turn on Home and over a scene: the frame rate not below the baseline.
- NOT RUN: the power key, the talk key and a button wake from standby and from deep standby, including during a voice turn and with a background scene busy; also a burst of power-key and talk-key presses in standby, then the `inbox ... overflows=` lines of `mem`.
- NOT RUN: factory reset and firmware update from the web page: the dialog on the device, the page following the events, the restart.
- NOT RUN: after a day of normal use, `mem`'s `bus dropped=` and every `inbox ... overflows=` line.

## State cell edges and cleanup parked review findings

- `SelfCheckScreen.SamplerPausesInStandbyFlooded` proves the sampler's pause with real sleeps (1.1 s and 1.5 s) rather than a gate on the sampler's own clock reads.
- `AdminFirmware.AcceptedVersionSurvivesAnImmediateAnswer` does not decide whether the decline lands before or after the route answers 202: making it deterministic needs a hook between the upload's end and the route's response.
- After a settings batch whose rollback failed, the reseeded settings cell follows storage, but a brightness that reached storage that way is not applied to the panel and no `system.settings` event goes out, until the next stored batch or the reboot.

## Storage worker lanes: device checks not run

Source: `docs/superpowers/plans/2026-11-14-storage-worker-lanes.md`. Simulator and unit tests only so far; the ESP-only branches of `WISP_ASSERT_SD_IO_THREAD`/`WISP_ASSERT_MEDIUM_THREAD` and of `storage_manager.cpp` were not compiled in that environment, so the firmware build itself is the first check: build the firmware for both boards first.

1. `mem` at idle and in the AGENTS.md section 5 worst case: `wisp.sd.io` and `wisp.shell.list` stacks in PSRAM, `wisp.fs` in internal RAM; the high-water marks of the three (`wisp.shell.list` while it decodes a 256×256 icon); internal free and largest block against `wisp/app/ram_budget.hpp`, compared with the numbers before this branch. Record them in `docs/hardware/<board>.md`.
2. Card file I/O from `wisp.sd.io`'s PSRAM stack: a scene with `"files": "sd"` writes, appends, reads and downloads several MB; the bytes match; FATFS/sdmmc reads and writes and the unmount in `StorageManager::refresh()` after a card pull run there without an abort and without a `sd access off the sd i/o thread` or `flash access off the flash thread` line. If it aborts, set `kSdOwnWorker` (`components/wisp_app/src/scene_wiring.hpp`) to false and record it in `docs/hardware/<board>.md`.
3. An OTA upload and a card download at the same time, both on the Bulk lane: both complete, no sdmmc timeout in the log; a setting changed meanwhile applies at once.
4. `mem` after a normal session (scene list opened, scenes opened, a voice turn, settings changed): each lane's longest wait and job, recorded in `docs/hardware/<board>.md`; the Interactive lane's longest wait should not exceed the longest job of the other lanes. Also a worst-case to-do or memory file rewrite on the Interactive lane (200 to-dos, 64 notes) and its `job_max_ms`.
5. The scene list opened cold with the example scenes: cards first, icons fill in, no stutter while scrolling; the `scenes ms_list=... ms_icons=...` line compared with before; the warm-open time.
6. The port-8900 httpd stack high-water mark while listing many scenes and during a scene upload (the manifests now parse on `wisp.fs` for those routes, `ParseOn::Worker`).
7. Card pulled during a download and put back: the download fails, the scene keeps running, its files are counted again. Expect either code: the running append fails with `io_error` until the 2 s `refresh()` marks the card offline, and with `unavailable` after that.
8. Factory reset with a card in: `<sd>/wisp/library/agent` is gone after the restart.
9. Deep standby entered right after changing a setting and writing a card file: both are on disk after waking (the drain covers both workers).

## Storage worker lanes parked review findings

- `SceneRepository::install()` opens the zip, inflates it, parses `scene.json` and validates the tool schemas on the admin httpd task's internal-RAM stack before any job (as before this branch); that is heavier than the manifest parse moved off that stack.
- `SceneWiring::list_async` (the hardware diagnostics' scene list) still runs a whole listing in one worker job.
- `Shell::stop()` is bounded by one storage job plus one file-area probe (`kFilesProbeMs`), not strictly by one job; and probes that timed out pile up behind a wedged worker.
- On the own-worker abandon path a `FileDownload` stays in its gate until the scene closes.
- A stale card job that lands on `wisp.fs` creates an empty `SceneFiles` record before answering `Busy`.
- No test for a partial start of the SD worker in the style of `SceneHttp`'s: a failed start of `wisp.sd.io` shares the no-card-slot path and is not tested on its own.
- The first download of a scene with no files location yet runs entirely on `wisp.fs`'s Bulk lane (it stays on the worker it began on), even once the location is the card.
- A scene that always has an Internal-routed job in flight never moves to `wisp.sd.io` (its calls stay on `wisp.fs` to keep their order).
- `ReminderScheduler::stop()` waits at most 5 s for queued and running actions; an action still running after that bound can reach the scheduler while it is destroyed. The fallback wait (`drain_io`) also waits for the whole worker to go idle, so a busy `wisp.fs` at shutdown can cost the full 5 s.
- `StorageWorker::stop()` called a second time while the first call drains returns before the join. No current path does this (`App::stop` joins `wisp.reset` before it stops the scenes).
- `StorageManager::status()`'s `online()` call may deinitialise the card without updating the cached state (from before this branch).
- Wall-clock tests that fail occasionally under parallel load: `PowerManager.TickEntersStandbyAfterIdleTimeoutAndActivityResetsIt`.

## CI speed

- `tools/ci.sh` runs ctest serially (about 390 s; `sim_smoke` alone takes 92 s). `ctest -j 20` finished in 92 s on a 20-core host, but under that load `AppScenes.DeleteAfterStopRuntimeIsRefused` aborted once (it passes alone, three runs out of three). Find its timing assumption first, then run ctest in parallel, keeping the smoke tests `RUN_SERIAL` if they share ports.

## Agent core tool enhancements

- `lua.run` failures now carry their full payload in `ToolOutput.result_json`, while `AgentService::execute_tool` still reads the diagnostic error code only from `ToolOutput.result`. Runtime/compile/timeout/memory/cancelled failures therefore log `error:failed`; the model result and ToolEnd summary remain correct. Preserve a small structured error code or expose explicit metadata without parsing the large JSON payload. Add a regression at the production consumer boundary.

## Device model: device checks not run

Source: `docs/superpowers/plans/2026-11-14-device-model-1-identity-and-generic-path.md`. Simulator and unit tests only so far; the ESP port and board edits (caps `role`, `EspPmuPowerKey`'s role) are not compiled in that environment.

- Both boards build (`idf.py bmgr -c ../boards -b <board> && idf.py build`).
- `WISP_MANIFEST` on both boards: the same devices, ids, models and caps as before, plus `role` (`main`, `internal`/`removable`, `power`, `talk`) on every device except keys without one; no new boot-report failure and no entry "role ... already taken"; no boot warning about a missing `main` role. Then record the manifest lines with `role` in `docs/hardware/*.md`.
- Self-check: every device green; the SD row on the 2.16 follows card insert and remove; the display, battery and clock readouts present.
- Voice: hold the talk key (2.16 Vol-, 1.75C BOOT), speak, hear the answer.
- Standby and wake with the power key, short and long press; the power key still wakes from deep standby (role-based polling); on the 1.75C both the GPIO3 and the PMU fallback paths if switchable.
- Net: Wi-Fi connects, the RTC is set after SNTP, the admin device page lists caps `role` and shows nothing new missing.
- `mem` idle: expect about 1–1.5 KB less free internal RAM (one caps map node of about 90 bytes per device that has a role, plus `core::DeviceService`); re-measure against `ram_budget.hpp`.
- SD card hot-plug on the 2.16 with a scene that uses `files: "sd"` open, and the admin overview's storage card (the SD row follows insert and remove, the labels read "SD card" and "Internal storage").
- An IMU scene receives samples; IMU polling stops in standby and resumes on wake.
- Notification tone, TTS playback and the self-check mic loopback.
- Battery and charging shown (status bar, self-check, admin); a long press on the power key opens the power-off dialog.
- The talk key from a dark screen wakes it and, held, records.
- A 1.75C without an RTC shows no clock warning and still gets the time from SNTP.
- The light-sleep statistics are still reported when the screen goes off and on.
- On a board without a self-describing device (both current boards), the agent's request carries neither `device.list` nor `device.call`.
- On the first board with a self-describing device: the serial monitor and the admin device page with its larger manifest (about 650 bytes more per device); the self-check screen on the round and the rounded shape scrolled to the end (such a device adds a row); driver calls from the agent thread and from a scene task at once over a shared I2C bus; `busy` in standby; `min_interval_ms` holding; the internal-RAM peak while `device.list` and the manifest are rendered.

## Device model

- A key that holds both roles `power` and `talk` (device-model spec 2.3) gets no gesture arbitration: holding it to talk also triggers the power key's long press (the power-off dialog after a second). It needs arbitration where talk owns the hold and the power key keeps only a short press; that belongs to the input-generality work of the device-model spec's part 3. Until then `App` warns about such a board at boot (`core::power_key_also_talks()`).
- Next parts of the device-model work: the ESP devices' ids move into the board configs (`BoardSpec` slots, spec part 2); the install-time prompt for `permissions.device` comes with phase I's general permission mechanism (until then a declared permission is honoured).
- Parked: the device routes and `DeviceRegistry::manifest()` build their JSON as a `Value` tree and a `std::string` in internal RAM (about 10 KB transient for a dozen devices of which three describe themselves); render into `os::LargeString` instead.
- Parked: the admin device page shows `caps.schema` as one JSON blob.
- Parked: a per-holder call budget in `DeviceService` for scenes: a scene can call a granted device's `get()`, or a command without `min_interval_ms`, as fast as its callbacks run, which contends for the bus and writes one log line per state-changing call.
- Parked: the agent's `lua.run` reaches devices as holder `agent` without leaving a conversation note, unlike `device.call`.
- Parked: the `device.call` activity line carries no device identity (it would need a per-tool key preference in `activity_target()`).
- Parked: `tools/check_layering.py`'s comment stripper can report a false positive on a line with a digit separator and an apostrophe in its comment.
