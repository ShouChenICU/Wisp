# Changelog

Notable changes to Wisp, newest first. Versions follow [Semantic Versioning](https://semver.org/); each one is a `vX.Y.Z` tag on `main`, which is also where the firmware version comes from (`cmake/wisp_version.cmake`). How and when to bump the version is described in `AGENTS.md` section 7.

## 0.5.1 — 2026-11-13

### Features

- Device settings now offer quick connectivity tests and activation for saved chat, speech recognition and speech synthesis profiles. Profile creation and editing remain in the web console.
- Add dedicated Wi-Fi settings with a radio switch, network scanning, saved-network connection and explicit disconnection. Network details group available security, signal, channel, access-point and address information. Password entry uses a modal dialog, and details support scrolling from text and interactive swipe-back navigation.

### Changes

- Enable automatic light sleep by default, with a build-time opt-out through `WISP_ENABLE_LIGHT_SLEEP`; remove the runtime setting from device settings and the web console. Keep Wi-Fi active while externally powered.
- Hide network lists and the status-panel Wi-Fi indicator immediately when Wi-Fi is disabled. Manual disconnection retains credentials and pauses automatic reconnection until an explicit connection, radio off/on cycle or reboot.

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
