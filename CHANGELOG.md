# Changelog

Notable changes to Wisp, newest first. Versions follow [Semantic Versioning](https://semver.org/); each one is a `vX.Y.Z` tag on `main`, which is also where the firmware version comes from (`cmake/wisp_version.cmake`). How and when to bump the version is described in `AGENTS.md` section 7.

## Unreleased

### Fixes

- Starting a new conversation (or deleting all of them) clears Home: the last turn's text and the reason it failed no longer stay on screen.

## 0.6.0 — 2026-11-16

### Features

- Vision for models that take images (the profile's "Sees images" option): the agent can look at the screen with `ui.screenshot` (the visible part of the panel, cropped, at most 240 px wide, JPEG) and fetch an image with `http.fetch` `as:"image"` (JPEG or PNG, at most 128 KiB). Only the newest image of a turn is sent whole; images are never stored in the conversation. Models without vision are not offered `ui.screenshot`, and `http.fetch` refuses images for them.
- MCP servers (the Model Context Protocol, over streamable HTTP): the agent finds and calls their tools through `tools.find` and `tools.call`, beside the scenes' tools, and manages the servers with `mcp.servers`, `mcp.add`, `mcp.remove` and `mcp.enable` (no confirmation, a note in the conversation). The web console has an MCP servers page (under Models: in the sidebar, and linked from the Models page's chat tab on a phone) to add, edit, enable, test and delete servers; header values (such as a bearer token) are write-only and never read back, server messages are scrubbed of them, and a server with stored headers is never followed through a redirect. A factory reset removes the servers.
- Advanced dialect options for a chat model profile, on the web Models page: the output token limit (`out_tokens`) and the field that carries it (`max_tokens` or `max_completion_tokens`), whether the model's reasoning is sent back (`reasoning` `echo`, for providers that require it), whether system messages may only come first (`system_msgs` `first`) and parallel tool calls (`parallel_tools`). A preset can supply its own defaults; an invalid stored option is dropped on its own, with a warning naming the profile.
- A reply cut by the output limit (`finish_reason` `length`) is continued once with a second request; a second cut keeps the text with a note.
- A conversation that rolls over into the archive carries a short summary of itself into the new one, so the agent keeps the thread; the turn shows as running while the summary is written.

- The serial `mem` command prints a line `bus dropped=<n> cell_subs=<n>` (events the bus dropped on overflow and live state-cell subscriptions), then one line per edge inbox, `inbox <name> overflows=<n>` (`scene_lifecycle`, `power_key`, `talk_key`): key presses and scene lifecycle steps dropped because their consumer fell behind.
- Add a Stop button right of the wisp on the home screen while a turn runs, with a 48 px touch target on every screen shape.
- The home screen says why a turn did not finish (stopped, a limit reached, no model, a refused key, no network, a lost connection, a server error, a conversation too long) in one short line, with the admin address where the fix is there. The full explanation stays in the conversation.
- `docs.read` returns long topics in pages the agent reads on with `next_offset`.
- The talk key works from a dark screen: held, it wakes the screen and records; a short press only wakes the screen.
- The serial command `mem` also prints each storage lane's job count, longest wait and longest job (`storage <worker> <lane> jobs=… wait_max_ms=… job_max_ms=…`), for `wisp.fs` and the SD card's worker.
- The home screen lists what is missing to talk instead of one generic hint: "Connecting to Wi-Fi…" while it reconnects, "No models set up yet" on first boot, or the missing items ("Needed: listening, speech"), with the admin address. A muted device does not ask for a speech model. While talking works (say, only speech is missing) why the last try failed shows first, for its usual time, and then the list returns. A tap on the wisp says "Hold the wisp to talk" once everything is set up and no turn runs.
- The agent can read and control peripherals that describe themselves (a vibration motor, a light, a sensor): `device.list` shows each one's properties and commands, and `device.call` reads a property or runs a command (when the board has such a device; otherwise the two tools are not offered). Arguments are checked against the device's ranges, commands that must not repeat quickly are rate-limited, and nothing asks for confirmation. The activity line shows "Check peripherals" / "Use peripheral", and a command that changes something leaves a note in the conversation.
- Scenes reach the same devices through `wisp.device.list()`, `wisp.device.get(id, prop)` and `wisp.device.call(id, cmd, args)`; `get` and `call` need the device's id in `scene.json`'s `permissions.device` (at most 8 ids); the agent's `lua.run` reaches every device.
- The simulator has a vibration motor, `haptic.main` (`pulse`, `effect`, property `active`), that logs what it would do.
- `GET /api/v1/settings/schema` describes every setting the admin page can change: its type, range, default, choices and label. The web System page takes its ranges and choices from it, and a setting the page has no card for appears in a "More settings" card at the end.
- The agent can also mute spoken replies, change when deep standby starts and set the time zone (`settings.set`, without asking); `settings.get` and `settings.set` now cover nine settings.

### Changes

- Holding to talk stops the speech but no longer stops a running turn, such as a scene build started from the web page. The new question replaces the turn only once it has been heard; cancelling the hold or not being heard leaves the turn running.
- The talk overlay now appears over every screen but Home (scenes, settings and other pages, diagnostics), shows why a turn ended or a voice error, and grows to three lines before cutting the text.
- The agent sees its current turn's tool results whole (up to 96 KB) instead of cut to 4 KB, and no longer resends large tool-call arguments from earlier turns.
- A spoken reply no longer pauses after its first sentence while the model writes a long answer: speech continues as soon as the say block closes, its last sentence included, while the text still streams (still one speech request at a time).
- Words said right as you press are no longer lost: the microphone opens at the press of the talk key or the wisp and keeps the last 500 ms before the hold starts (`VoiceConfig::preroll_ms`; 0 turns this off for a board). The start tone is now part of such a recording. "Listening" shows only once the microphone is actually recording.
- Talking right after the device wakes from deep standby works: a hold records while Wi-Fi reconnects to a saved network, and recognition waits up to 8 s for it instead of failing "offline" at once. When no connection is being made (Wi-Fi turned off, no saved network, disconnected in Settings, the only network's password rejected, or a saved network out of range and waiting to retry) a hold is still refused at once and Home says there is no Wi-Fi.
- Taps on the scene in front are answered before background work: the scene task picks each step from three lanes (UI events and control messages in arrival order, then the scene in front, then background and headless scenes and the agent's Lua in turn), with bursts (`kQueueBurst` 8, `kFrontBurst` 4) that keep any lane from starving.
- The agent's `lua.run` CPU slice drops from 1000 ms to 200 ms, the same as a scene's step; a run still lasts its own `timeout_ms`, but a script that computes for more than 200 ms without waiting now ends in `timeout`.
- A scene, or a `lua.run` script, may have at most 16 `wisp.http`, `wisp.http.download` and `wisp.files` calls waiting for results (`kMaxCallsInFlight`); one more raises the Lua error "too many calls waiting for results (at most 16)", which `pcall` catches, and a place frees once its callback has run. An entry file that starts more than 16 at load fails the install check.
- Downloads run on their own worker (`wisp.shttp.dl`): a download that streams for minutes no longer holds up a scene's other requests or the agent's `http.fetch`. At most two downloads wait behind the one running; a download and an ordinary request of one scene may now complete in either order.
- Work on SD card files no longer holds up settings saves and conversation writes: a scene's files on the card are read, written and downloaded on a worker of their own (`wisp.sd.io`), also when the agent's `scene.files_*` tools read or remove them, and settings saves, conversation appends, memory notes and to-dos take a new Interactive lane on `wisp.fs` that runs ahead of other file work (a lower lane still gets a turn after 8 jobs, `kLaneTurnAfter`). `kSdOwnWorker` turns the card worker off for a board where card I/O cannot run from a PSRAM stack.
- The scene list and its icons load on their own task (`wisp.shell.list`) instead of on the storage worker, reading one scene per storage job, so opening the list no longer holds up saves.
- Internal: `StorageManager` checks threads and locks per medium (internal flash, the card), so card I/O never waits for a flash erase; `StorageRouter` picks the worker by medium; `SceneFiles` routes each scene's calls to the worker for where its files live; the scene repository reads one scene per job and parses manifests off the storage worker (on the worker for the admin routes, whose httpd stack is small); the agent's `scene.list` runs on the agent task.
- `scene.rollback` returns to the version from before the agent's turn: within one turn only a scene's first install keeps the installed version as the previous one, so two installs and an "undo" no longer land on the turn's own first attempt.
- Devices are found by role instead of fixed ids: a board names its devices as it likes and marks each one's purpose in caps `role` (`main`, `internal`/`removable` storage, the `power` and `talk` keys). `WISP_MANIFEST` gains caps `role` on every device that has one, caps `schema` on a self-describing device and the class `other` for a device with no typed interface; the admin device page shows them. A board that registers a device without its `main` role, or gives one key both the `power` and `talk` roles, gets a warning at boot.
- Internal: components keep the state others follow in state cells beside the unchanged events (the admin WebSocket and the scenes see the same topics, payloads and order). The simulator's `debug clock-advance` answers `clock-advance=no time sync` when the time sync service is not running, and in-process subscribers now see component names (`power_monitor`, `wifi_service`, `storage_manager`) instead of device ids as the source of mirrored events.
- The device no longer re-reads periodically the settings and service state it already follows: gone are the power manager's settings read every minute, the system duties' network, pairing, factory-reset and firmware re-read every second, the 100 ms checks of the screen language and of the diagnostics' time zone, and the diagnostics' scene relist every 30 s. The timers these reads ran in still run for their other work. The diagnostics still read the signal strength and the saved networks once a second, which nothing else reports, and ask again for a scene list they did not get (with at most one listing waiting, only until one is shown).
- The screen keeps its language after a factory reset is confirmed, until the restart, instead of switching to the default language for a moment.
- After a factory reset that failed, a new reset request from the admin page opens the confirmation dialog again.
- A firmware "Yes" installs only the image its dialog showed: an image declined or timed out meanwhile and replaced by a new upload is not installed by a late tap; the new one asks on its own.
- The hardware diagnostics' scene list follows an SD card being inserted or removed, and dev-mode edits, at once instead of within 30 s; the shell's scene list, while open, now lists again on these too.
- Internal: key presses and scene lifecycle steps reach their consumers through `ButtonService` listeners and edge inboxes instead of the event bus, which now has one subscriber on the device, the admin WebSocket. The `button.*` and `scene.state` events are still published unchanged.
- Internal: the in-process `voice.state` event (not sent to the admin page) no longer carries the recording or speech `level` (read `VoiceService::level()`), and is published only when the voice state or its outcome changes, not for a repeated state.
- The screen-off and deep-standby choices are the same on the device and the web page: the device's settings page gains 2 minutes for both.
- Brightness shows the same percentage on the device and the web page, 0 % at its lowest level.
- The device uses one wording per setting: the agent's notes use the settings page's names (zh: 提醒音量, 自动熄屏, 系统语言, and "语音播报 on → off" when spoken replies are muted).
- A stored setting outside its range still reads as its default, and now logs one warning naming it at boot. A stored low-battery threshold (`power/low_batt_pct`, set only by hand) outside 5-50 % reads as the default, 15 %.
- The agent's `settings.get` description is generated from the settings list, and the keys of `settings.set` follow that list's order (tool definitions are sent fresh each turn, so stored conversations are not affected).
- Internal: every system setting is declared in `wisp/core/settings_schema.hpp`; the settings service, the REST routes, the agent tools, `wisp.device.info()`, the device's settings page and the web System page take keys, ranges, defaults, choices and labels from it. `tools/check_settings.py` (run by `tools/ci.sh`) fails when code outside the schema restates a setting's default or range on the line that names it (tests exempt).
- Internal: host builds now fail on an aggregate member left without a default (`-Werror=missing-field-initializers`), as the firmware build does; the firmware builds for both boards with ESP-IDF v6.0.3.
- Internal: the hardware diagnostics' performance overlay toggle saves through the settings service, so it also publishes `system.settings` (the payload is unchanged; the overlay is not in it).
- The token estimate's calibration is kept per model profile and model, and resets when either changes.
- Internal: the agent reaches its model through `net::LlmProvider` and runs each turn in `TurnRunner`; scene tools and MCP servers are tool providers behind `tools.find`/`tools.call`. Blocking tools such as `ui.screenshot` run on a one-shot task, `wisp.tool.io`. The JPEG encoder is the vendored `stb_image_write` (public domain / MIT).
- A smaller firmware image (about 230 KB less on both boards; 78.6 % of the 2.16 board's app slot): JSON numbers are written and read without libstdc++'s floating-point `to_chars`/`from_chars`, the `docs.read` texts are stored compressed and inflated per read, the codec drivers neither board has are left out (it takes a regenerated `sdkconfig`), and the web UI's scripts and styles are minified when packed.

### Fixes

- Recover from an over-long context: summarize harder, then cut the current turn's results, and retry once when the model refuses the request as too long; the token estimate calibrates itself from the model's reported usage.
- Retry model requests that failed before any reply arrived on the same budget as 429/5xx, honour `Retry-After`, and read providers' error codes without ever logging the error body or keeping any part of the API key. These retries cover connection and TLS failures only: a Wi-Fi disconnect still ends the running turn ("lost connection" during a request, "no network" between steps), unless the link has already recovered by the time the agent sees it.
- A scene no longer loses an HTTP or files result when its message queue is full: results wait in a box of the scene's own, sized for its 16 calls, and the queue only carries a wake-up.
- A brief Wi-Fi drop that recovers before the agent sees it no longer ends a running request; a link that stays dead still ends it through the stall check.
- A low-battery notice left over from the last run is dismissed when the device boots while charging, and plugging in USB power while already charging no longer dismisses it (only charging starting does).
- Screens and services no longer stay out of date after a busy moment drops an event: the status panel, the hotspot and pairing cards, hardware diagnostics, scenes' visibility in standby, the time zone, time format and language that scenes see, and reminders after the clock is set all follow the current state.
- Key presses are no longer lost when the device is busy or the event bus drops events: the power key still turns the screen off and on and opens the power-off dialog, releasing the talk key still ends the recording, and any key still wakes the screen from standby, however full the event queue and the UI's queue are. Only a burst of more than 8 edges waiting for one consumer at once (`power_key`, `talk_key`) loses the oldest, which is counted and shown by `mem`. A short press of the talk key handled late is no longer taken for a hold.
- A deleted scene loses its notifications, its tools and its place in the scene list's order even when the device is busy or the event bus drops events at that moment. Only a burst of more than 32 scene changes waiting at once loses the oldest, which is counted and shown by `mem` (`inbox scene_lifecycle`).
- A failed settings save whose undo also failed no longer leaves screens and services showing values other than the ones stored.
- A shell destroyed without being stopped no longer leaves its event subscriptions and conversation listeners behind.
- Replacing a list's items no longer shows an empty list for a frame: the old rows stay on screen until the new ones arrive, and a tap on an old row in that moment does nothing.
- The screen starts at the stored brightness, and at the default when the stored value is out of range (a stored value below 10 used to light the panel at that level while everything else reported 200).
- Running the hardware diagnostics' mic loopback test no longer makes speech, cue tones and notification sounds quieter until the next restart; the loopback plays at the volume setting and no longer plays the unrecorded end of its buffer after a short recording.
- The device's settings page shows changes made by voice or on the web page while it is open, and puts a row back when its change was not saved.
- A brightness left in storage by a failed settings save reaches the screen.
- Clearing or dismissing a notification takes its banner at the top of the screen down at once, and a cleared notice no longer shows a banner later.
- Pulling the status shade down hides the banners: banners of notices listed in the panel are dropped, and a scene's banner-only notice waits and shows once the shade is closed. Banners no longer leave stale pixels over the moving shade.
- The "kept the device awake for 30 minutes" notice names every system task in words (for example the voice, the agent or Wi-Fi) instead of an internal id, and is no longer raised for keeping Wi-Fi on while the device is on external power, which is intended.

Device checks are pending for the turn reliability, the voice and wake, the scene scheduling and install safety, the storage worker lanes, the state subscriber changes, the key and scene edge changes and the settings schema (listed in `docs/backlog.md`, "Agent turn reliability: device checks not run", "Voice and wake responsiveness: device checks not run", "Scene scheduling: device checks not run", "Storage worker lanes: device checks not run", "State cell subscribers: device checks not run", "State cell edges and cleanup: device checks not run" and "Settings schema: device checks not run"); these changes are verified in the simulator and unit tests only. The device-model changes (devices found by role, self-describing devices) are likewise verified in the simulator and unit tests only; their device checks are listed in `docs/backlog.md`, "Device model: device checks not run". The LLM provider changes (dialect options, length continuation, rollover carry, vision, MCP servers) and the firmware size trim are verified in the simulator, unit tests and firmware builds only; their device checks are in `docs/backlog.md` ("MCP client: device checks not run", "Firmware size trim: device checks not run") and at the end of the plans `docs/superpowers/plans/2026-11-16-llm-provider-seam.md`, `2026-11-16-llm-dialects.md` and `2026-11-16-llm-vision.md`.

## 0.5.2 — 2026-11-14

### Fixes

- Remove the top status capsule while retaining pull-down access to the status panel, reclaim its reserved space, and pad and centre the Wi-Fi empty-network message.
- Pin the USB dependency to 1.6.0 so clean firmware configuration and board switching preserve the committed dependency lock.

### Tooling

- Simplify repeated timed long-press checks in scene-list tests and remove the 2,000-widget wall-clock benchmark. Retain touch interaction, data deletion, asynchronous discovery, tree folding and output-limit coverage.

### Documentation

- Require proactive self-review of implementation details, usability and integration before delivery.

## 0.5.1 — 2026-11-13

### Features

- Device settings now offer quick connectivity tests and activation for saved chat, speech recognition and speech synthesis profiles. Profile creation and editing remain in the web console.
- Add dedicated Wi-Fi settings with a radio switch, network scanning, saved-network connection and explicit disconnection. Network details group available security, signal, channel, access-point and address information. Password entry uses a modal dialog, and details support scrolling from text and interactive swipe-back navigation.

### Changes

- Enable automatic light sleep by default, with a build-time opt-out through `WISP_ENABLE_LIGHT_SLEEP`; remove the runtime setting from device settings and the web console. Keep Wi-Fi active while externally powered.
- Hide network lists and the status-panel Wi-Fi indicator immediately when Wi-Fi is disabled. Manual disconnection retains credentials and pauses automatic reconnection until an explicit connection, radio off/on cycle or reboot.
- Split daily CI from full release checks. Daily simulator smoke covers eight core scenarios; release checks retain all 22. Select tool self-tests from changed tools and inputs with conservative fallback, keep gesture coverage by screen shape, and run screenshot previews manually.

### Fixes

- Improve settings layouts across screen shapes and languages, preserve visible spacing between independent controls, centre trailing icons, and keep password input visible above the keyboard.
- Synchronize scene test startup before installing a second storage worker, avoiding intermittent flash-thread assertions from unfinished diagnostics listings.

### Documentation

- Document shared UI layout conventions, including control spacing, touch targets and icon alignment.

## 0.5.0 — 2026-11-13

### Features

- Agent HTTP fetching with cached chunk reads, temporary Lua execution with waiting HTTP/file calls, and discoverable scene-exported tools with headless execution. Adds localized activity labels and stronger prompt-injection rules for external content. The user reported normal operation on both supported boards; targeted RAM measurements and adversarial-page acceptance remain pending.
- Round-screen weather and 2048 example scenes, available to the agent through the built-in example documentation.

### Documentation

- Refresh the English, Chinese and Japanese README introductions, summarize the core tools in four groups, and reorganize quick start, interface previews and usage instructions. Add round-screen weather and 2048 screenshots with transparent backgrounds.

## 0.4.0 — 2026-11-12

### Changes

- Interrupt-first button input: one deadline-driven worker handles timestamped edges without periodic polling. PollOnly buttons keep 20 ms active sampling; screen-off pauses ordinary keys and keeps a 100 ms power-key fallback. Queue/task creation failure retains an emergency 20 ms Timer tick while device reads still follow those deadlines. PMU battery/charging/VBUS sampling remains 1 s active / 30 s standby. GPIO buttons request independent RTC light-sleep wake; unsupported wake pins or unknown ISR-service ownership degrade to polling. The 1.75C now selects GPIO3 for PWR by default after user-reported button-function tests on both boards. Electrical/strapping, physical light-sleep wake, worst-case RAM and power/stress measurements remain pending.

### Fixes

- GPIO input accepts IRAM-safe C interrupt levels 1–3 rather than requiring a free level-1 input. This resolves the interrupt-allocation failure observed while bringing up GPIO3 on the round board; the failure had also triggered an IPC task stack overflow. The allocation-failure stack-pressure path has not been separately stress-tested.


### Features

- Admin page, models: a **Fetch** button next to the model field lists the platform's models (saving the profile first, like "Test connection"), and the model becomes a dropdown of the models of that tab's kind. Values that can be picked are now picked: the TTS voice from the model's voices (name, language and description), the ASR language from the model's languages or a common list (with "Auto" first), each with a "type it in" option; TTS speed is a slider with a reset to the default; the sample rate moved into a folded "Advanced" section and shows the model's rate without filling it in; picking a chat model fills in its context window and image input. The connectivity test returns these per-model `details` (from speech.cpp's `speech` object and OpenRouter-style `context_length`/`architecture`), read into PSRAM and bounded. The web UI's gzip budget is now 170 KiB.

## 0.3.1 — 2026-11-09

### Changes

- Faster boot: the PSRAM test at boot is off, the panel's long vendor waits after power-up are gone, and a wake from deep sleep skips the bootloader's image check. On the 1.75C board a normal boot's `boot_ms` went from 2410 to 1336 ms, and a wake from deep sleep to the home screen from 3.64 to 1.56 s.

### Fixes

- Speech synthesis: the request no longer carries `sample_rate` unless the TTS profile sets one. It is not part of OpenAI's request, and servers that check strictly, such as speech.cpp, refused every sentence with 400 "Unrecognized request argument supplied: sample_rate".

### Tooling

- New serial command `sleep`: how the chip used light sleep in deep standby (time asleep, number and lengths of sleeps, wake causes, battery drain per hour) for the running and the last session, plus the PM locks and the time in each PM mode. The sessions survive the reset that opening the serial port can cause. `sleep reset` clears them. How to measure standby on battery is in `docs/hardware/waveshare_esp32_s3_touch_amoled_1_75c.md`.

## 0.3.0 — 2026-11-09

### Features

- **Pull-down panel**: it now follows the finger. A swipe down from the top edge pulls the panel down over the screen like a sheet, its bottom edge under the finger, while what is underneath stays in place. Let go past a third of the screen, or flick down, and it slides fully open; otherwise it slides back up. An upward swipe on the panel takes it back up the same way, and a tap on empty space or the back gesture slides it shut instead of making it vanish. A small handle at the bottom of the panel shows where it ends.

### Fixes

- Edge gestures now scale with the screen: the top and left edge zones are 8% of the screen's shorter side (38 px on a 480 px screen, before a fixed 16 px), and the trigger distances, the shell's swipes and the drag slop are shares of it too. This also fixes a swipe down from the top edge sometimes opening the conversation history instead of the pull-down panel: a finger coming in over the bezel is first seen up to about 26 px below the edge.

## 0.2.1 — 2026-11-08

### Fixes

- Settings page: the admin console address was cut off on round screens. It now has its own line and wraps when needed.
- Power: an open admin console page no longer keeps the device awake. With the screen off the device goes into deep standby and turns Wi-Fi off as usual, and the page reconnects when it wakes, so a forgotten browser tab cannot drain the battery. Uploads, OTA and downloads still keep it awake while they run.

## 0.2.0 — 2026-11-08

The first public release.

### Features

- **Shell**: the wisp home screen, drawn in code with its own expressions for idle, listening, thinking and speaking. Finger-following navigation leads to Settings, the scene list and history. Long tool output such as Lua code is folded so it does not slow the screen down.
- **Settings page**: grouped list, with choice sub-pages for screen-off, deep standby, language and time zone. Language changes apply at once, without a reboot. New 24/12-hour time format (`time_24h`). Restart and power off, each confirmed first. Sub-page titles scroll with their lists.
- **Scenes**: Lua 5.5 sandbox per scene with a declarative UI, background scenes, notifications and scene tools for the agent. New `on_swipe(dir)` event for layout widgets and the canvas.
- **Agent**: any OpenAI-compatible model, with the device as its toolbox, plus context compaction. The step limit and turn time limit can be set in the admin console.
- **Voice**: hold to talk, recognition, `<say>` speech and interruption.
- **Memory and reminders**: preference notes and to-dos with due times.
- **Admin console**: Wi-Fi and QR setup, LAN console, model profiles and OTA from the browser.
- **Icons**: a larger built-in icon set, including a microphone mark for voice prompts in history.

### Boards

- Waveshare ESP32-S3-Touch-AMOLED-2.16 (reference board).
- Waveshare ESP32-S3-Touch-AMOLED-1.75C (round 466 × 466 screen, 32 MB flash), including its IMU orientation. The wisp is drawn larger on round screens.

### Performance

- Code runs from flash with a 32 KB instruction cache on every board instead of XIP from PSRAM. Scrolling stays at 20–22 fps on both boards, and about 3.5 MB more PSRAM is free (6.7 MB in all). The 2.16 keeps flash auto-suspend, so the UI stays smooth during an OTA upload. The 1.75C's flash cannot use auto-suspend efficiently, so its UI stutters during an upload (`docs/hardware/waveshare_esp32_s3_touch_amoled_1_75c.md`).
