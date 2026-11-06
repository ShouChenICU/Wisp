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

The full list is the Task 16 checklist (rows 1-30) in the `feat/scene-device` final review. The smoke pass runs rows 1, 10, 11, 17, 19, 20, 22 and 26. The other rows are:

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
