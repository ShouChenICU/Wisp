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
