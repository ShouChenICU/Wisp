<div align="center">

# 🔥 Wisp

**A personal agent on ESP32 that builds the apps you ask for.**

🇺🇸 **English** | 🇨🇳 [简体中文](README.zh-CN.md) | 🇯🇵 [日本語](README.ja.md)

![License](https://img.shields.io/badge/license-Apache--2.0-blue?style=flat-square)
![Platform](https://img.shields.io/badge/platform-ESP32--S3-E7352C?style=flat-square&logo=espressif&logoColor=white)
![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0.3-E7352C?style=flat-square)
![C++](https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=cplusplus&logoColor=white)
![LVGL](https://img.shields.io/badge/LVGL-9.5-343839?style=flat-square)
![Lua](https://img.shields.io/badge/Lua-5.5-2C2D72?style=flat-square&logo=lua&logoColor=white)
![Status](https://img.shields.io/badge/status-active-brightgreen?style=flat-square)

</div>

Wisp is your personal handheld agent. It can chat with you, has a memory of its own and, most importantly, can create and modify apps to suit your needs through an extremely lightweight, custom Lua app framework. Say “make me a Pomodoro timer,” for example, and it writes the interface and logic, installs the app and opens it. The whole process takes place on the device, with no recompilation or computer needed. You can reopen the app whenever you like or ask Wisp to keep refining it, all running on ESP32. Let your creativity and imagination run free, and bring whatever you have in mind to life.

## ✨ Highlights

- **Create apps on demand**: Turn a request into a Lua scene you can use and refine immediately.
- **Control the device**: Tools for scenes, networking, scripts and settings, with progress on screen.
- **Remember and remind**: Persistent preferences, to-dos and reminders that wake the screen.
- **Choose your models**: OpenAI-compatible chat, ASR and TTS, with switchable profiles.
- **Manage locally**: LAN admin console, write-only API keys and on-device confirmation for sensitive actions.
- **Build across boards**: Pluggable hardware and a PC simulator for everyday development.

## 🚀 Quick start

### 💻 Simulator (no hardware)

```bash
# Arch: sudo pacman -S cmake ninja sdl2 python curl zlib
# Debian/Ubuntu: sudo apt-get install cmake ninja-build libsdl2-dev python3 libcurl4-openssl-dev zlib1g-dev
git clone https://github.com/ShouChenICU/Wisp.git wisp && cd wisp
cmake -S simulator -B build/sim -G Ninja && cmake --build build/sim
./build/sim/wisp_sim
```

The mouse stands in for your finger. <kbd>P</kbd> is the power key, <kbd>+</kbd> and <kbd>-</kbd> are the volume keys, and <kbd>S</kbd> inserts or removes the SD card. The terminal is the serial console; type `help` to list its commands. The admin console is at `http://127.0.0.1:8900`.

<details>
<summary>Common options</summary>

| Option | What it does |
| --- | --- |
| `--shape rect\|rounded_rect\|circle` | Simulate a different screen shape |
| `--headless` / `--screenshot <png>` / `--exit-after-ms <n>` | Run without a window, save a screenshot, exit after a delay |
| `--data-dir <dir>` | Data directory, `./sim-data` by default |
| `--web-dir <dir>` | Serve the console straight from the source tree; just reload after edits |
| `--no-sd` / `--disable <id>` / `--wifi-fail` | Simulate no SD card, a failed device, or an unreachable router |
| `--tap X,Y@MS` / `--swipe X1,Y1,X2,Y2@MS` | Tap or swipe at a given time |

</details>

### 📟 Device

Supported now: **Waveshare ESP32-S3-Touch-AMOLED-2.16**. It has a 480×480 AMOLED touchscreen, two microphones, a speaker, an IMU, an RTC, battery management and an SD slot, with no soldering needed. Also supported, pending acceptance on the device: **Waveshare ESP32-S3-Touch-AMOLED-1.75C** (466×466 round AMOLED, 32 MB flash, no SD slot); build it with the per-board commands in [its README](boards/waveshare_esp32_s3_touch_amoled_1_75c/README.md).

```bash
. ~/esp/esp-idf-v6.0.3/export.sh        # in fish and other non-bash shells, wrap in bash -c '...'
cd firmware
idf.py set-target esp32s3
idf.py bmgr -c ../boards -b waveshare_esp32_s3_touch_amoled_2_16
idf.py build && idf.py -p <port> flash
```

- **Wi-Fi:** on first boot, tap "Set up Wi-Fi" on the screen and scan the QR code with your phone, or type `wifi add <ssid> <password>` on the serial console.
- **Models:** on the admin console's Models page, add a profile for chat, one for recognition and one for synthesis, and make each one active.
- **Updates:** after a code change, run `idf.py app-flash`, or upload `wisp.bin` in the admin console (OTA).

> [!TIP]
> After a font change, also run `idf.py fonts-flash`. Fonts live in their own partitions, which an app update does not write.

## 📸 Interface preview

The on-device UI supports Chinese, Japanese and English. These screenshots show Chinese.

### Conversation & agent activity

<div align="center">

<table>
  <tr>
    <td align="center"><img alt="Idle" src="docs/images/home.png" width="200"><br><sub>Idle</sub></td>
    <td align="center"><img alt="The agent at work" src="docs/images/agent-steps.png" width="200"><br><sub>The agent at work</sub></td>
    <td align="center"><img alt="Done" src="docs/images/agent-done.png" width="200"><br><sub>Done</sub></td>
  </tr>
</table>

</div>

### Example scenes

<div align="center">

<table>
  <tr>
    <td align="center"><img alt="Clock" src="docs/images/scene-clock.png" width="200"><br><sub>Clock</sub></td>
    <td align="center"><img alt="Pomodoro" src="docs/images/scene-pomodoro.png" width="200"><br><sub>Pomodoro</sub></td>
    <td align="center"><img alt="Weather" src="docs/images/scene-weather-round.png" width="200"><br><sub>Weather</sub></td>
  </tr>
  <tr>
    <td align="center"><img alt="Spirit level" src="docs/images/scene-level.png" width="200"><br><sub>Spirit level</sub></td>
    <td align="center"><img alt="Colour balls" src="docs/images/scene-balls.png" width="200"><br><sub>Colour balls</sub></td>
    <td align="center"><img alt="2048" src="docs/images/scene-game2048-round.png" width="200"><br><sub>2048</sub></td>
  </tr>
</table>

</div>

<sub>Example source: <a href="scenes/examples/clock/">clock</a>, <a href="scenes/examples/pomodoro/">Pomodoro</a>, <a href="scenes/examples/weather-round/">weather</a>, <a href="scenes/examples/level/">spirit level</a> and <a href="scenes/examples/game2048/">2048</a>.</sub>

## 👆 How to use it

| Action | What it does |
| --- | --- |
| Hold <kbd>BOOT</kbd>, or long-press the wisp | Talk; release to send. Hold again while it speaks to interrupt it |
| Swipe right / left / down | Settings / scene list / conversation history. Pages follow your finger |
| Hold <kbd>BOOT</kbd> inside a scene | A listening bar slides up at the bottom, so you stay in the scene |
| Settings → About → Hardware diagnostics | Self-test results for every peripheral |

Replies come in two layers. The sentence to be spoken is shown in large type, and the details appear below in small type. While the agent calls tools or writes files, the steps it is taking are listed under the wisp.

## 🏠 Self-hosting the AI services

Wisp talks to its models over OpenAI-compatible APIs, so chat, speech recognition and speech synthesis can each run on a machine of your own instead of a cloud provider.

**Voice: [speech.cpp](https://github.com/nyosegawa/speech.cpp) is recommended.** It is a C++ server (MIT) that runs speech recognition and synthesis locally on CPU, Metal or Vulkan, with no Python, and serves OpenAI's `/v1/audio/transcriptions` and `/v1/audio/speech`. Qwen3-ASR and Qwen3-TTS understand and speak Chinese, English, Japanese and other languages, and Qwen3-TTS comes with several voices.

```bash
# on the machine that will serve the models (macOS or Linux; Windows has install.ps1)
curl -fsSL https://raw.githubusercontent.com/nyosegawa/speech.cpp/main/install.sh | sh
speech models                      # list the models it can download
speech serve qwen3-tts-0.6b qwen3-asr-0.6b --host 0.0.0.0
```

The first run downloads the models. Then, on the admin console's Models page:

- **Speech recognition:** add a profile with the preset "OpenAI-compatible (custom)", the address `http://<server-ip>:8080/v1` and no API key. "Test connection" lists the models; pick the Qwen3-ASR one.
- **Speech synthesis:** the same address; pick the Qwen3-TTS model and a voice such as `serena`. Leave the sample rate empty: Qwen3-TTS speaks at 24 kHz, which is what Wisp plays by default.

> [!WARNING]
> speech.cpp has no authentication and no TLS. Listen on `0.0.0.0` only on a network you trust, or put it behind a proxy that adds them.

**Chat:** any OpenAI-compatible server works, such as [Ollama](https://ollama.com) (it has its own preset) or llama.cpp's `llama-server`. The agent drives the device through tool calling, so choose a model that handles tool calls well; small models often do not.

## 🧩 Core modules

| Module | Description |
| --- | --- |
| 🔥 **Shell** | A native home screen that no broken scene can take away. The wisp is drawn live in code and has its own expressions for idle, listening, thinking and speaking. Swipe to Settings, scenes or history, and the pages follow your finger. During a turn, the spoken sentence shows in large type with the agent's steps underneath. |
| 🎭 **Scenes** | Every feature is a scene: one folder with one `main.lua`. The UI is declared as a Lua table bound to state. Each scene runs in its own Lua 5.5 sandbox with memory and time limits, and it can reach only the HTTPS hosts it declares. Scenes can run in the background, post notifications and register tools for the agent. |
| 🤖 **Agent** | Works with any OpenAI-compatible model, with the whole device as its toolbox. It creates, edits, installs, opens and deletes scenes, fetches web content and APIs, runs Lua scripts, calls tools exported by scenes, manages memory and to-dos, and changes settings. Long conversations are compacted to fit the context. |
| 🎙️ **Voice** | Hold <kbd>BOOT</kbd> or long-press the wisp to talk, and release to send. The sentence marked `<say>` is spoken while the rest streams to the screen. Hold again to interrupt; alarms take the speaker first. Recognition and synthesis can each use their own provider, and SiliconFlow works well. |
| 🧠 **Memory & reminders** | Short notes about your preferences, plus to-dos with due times. When a to-do comes due, the screen lights up and it rings. You can review both in the admin console. |
| 🌐 **Admin console** | Open `http://wisp.local:8900` on your LAN and sign in with the 6-digit code on the screen. From there you can chat, manage Wi-Fi, models, memory and scenes, watch the logs and upload firmware. It is about 100 KB of plain JavaScript inside the firmware, in Chinese, Japanese and English. |

> [!IMPORTANT]
> Tool results are treated as data, never as instructions. API keys are write-only. Deleting a scene, a factory reset and a firmware update must all be confirmed on the device.

## 🤖 What the agent can do

The model reasons through your configured OpenAI-compatible service; Wisp executes its tool calls on the device. The core tools fall into **four groups by purpose**:

| Group | Tools | What they enable |
| --- | --- | --- |
| Scene development & runtime | `scene.*`, `ui.tree`, `docs.read`, `tools.find`, `tools.call` | Read the built-in API reference, create and edit scenes, install or roll back versions, manage their lifecycle, data and files, and inspect widgets and logs for debugging. Discover and call tools exported by installed scenes to add reusable capabilities without changing the firmware. |
| Web & scripting | `http.fetch`, `lua.run` | Access websites, APIs and LAN services over HTTP/HTTPS, extract text from HTML and read long responses in pages. Run sandboxed Lua 5.5 scripts to calculate, process JSON, chain requests, download files and read or write the agent's own files. HTTP and file operations can wait for results; memory and execution time are bounded. |
| Memory & to-dos | `memory.*`, `todo.*` | Remember preferences, maintain task lists, and set due times or recurring reminders that wake the screen and ring. |
| Device & notifications | `device.status`, `settings.*`, `notify.list`, `notify.dismiss` | Check battery, network, storage and peripherals; adjust brightness, volume, screen timeout, language and clock format; review or dismiss notifications. |

These tools can be combined in a conversation. For example:

- **“Make me a Pomodoro timer, then make the digits bigger.”** Read the API reference, create and install a Lua scene, open it, then edit its layout. Roll back if the previous version worked better.
- **“Read this page and summarize it; fetch this API and calculate the totals.”** Use `http.fetch` to retrieve content and `lua.run` to parse JSON, calculate or save results. API access depends on the service and any credentials it requires.
- **“Use my weather scene to check tomorrow's forecast.”** If that scene exports a forecast tool, discover and call it. A stopped scene can run without a UI for the call, leaving the current screen in place; it closes after 60 seconds idle.
- **“Remember that I prefer tea, and remind me tomorrow at nine to buy some.”** Save a preference and create a timed to-do; the device lights up and rings when it is due.

Core tools are available directly; scene tools are discovered when needed rather than all being added to every model request. This lets the agent grow its capabilities by creating and reusing scenes. See the [scene API reference](docs/scene-api.md) for scene development and tool exports.

## 🔧 Porting to a new board

Wisp is not tied to one board. Everything board-specific lives in `boards/<board>/` and `drivers/`, and nothing above them changes. The reference port is [`boards/waveshare_esp32_s3_touch_amoled_2_16/`](boards/waveshare_esp32_s3_touch_amoled_2_16/).

| Step | What to do |
| --- | --- |
| 1. Describe the hardware | Write the board, buses and pins in esp_board_manager YAML; the display, touch, audio and SD card are declared here |
| 2. Add drivers | For chips esp_board_manager does not support (IMU, PMU, RTC, …), write a C component under `drivers/<chip>/` |
| 3. Write `board.cpp` | Wrap each device as a `hal::Device`, register it with `DeviceRegistry`, and declare its capabilities as caps: screen shape, safe area, whether there is a microphone, and so on |
| 4. Partitions and config | Write `partitions.csv` to the conventions (OTA, `fonts`, `glyphs`, storage) and the board's sdkconfig |
| 5. Build and verify | Build with `-DWISP_BOARD=<board>`, then check the boot report, the device manifest and the diagnostics page item by item |

> [!TIP]
> A missing or failed device only degrades the system; it never stops the boot. So you can get the screen up first and add the other peripherals one at a time.

> 📖 **The full walkthrough is the [porting guide](docs/porting-guide.md).** It covers board naming, capability fields, partition conventions, chip differences and the acceptance checklist. Whether you are a person or an AI coding assistant, read it and [`AGENTS.md`](AGENTS.md) before you start. Then write a `docs/hardware/<board>.md` for the new board, modelled on the [existing hardware notes](docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md).

## 🧱 Architecture

One C++20 code base builds two targets: the ESP32-S3 firmware and an SDL2 simulator.

```
 wisp_app     boot wiring, serial console
 wisp_shell   home screen, navigation, sub-pages, conversation control
 wisp_agent   conversation loop, tool registry, context management
 wisp_scene   Lua scene runtime
 wisp_ui      LVGL, wisp renderer, system layer, i18n          portable layers
 wisp_admin   admin console REST / WebSocket                   (each depends only
 wisp_voice   capture, ASR, TTS, playback                       on the ones below)
 wisp_net     Wi-Fi, pairing, HTTP
 wisp_core    event bus, device registry, storage, memory, reminders
 wisp_hal · wisp_os · wisp_base
 ───────────────────────────────────────────────
 wisp_port_esp (FreeRTOS + ESP-IDF) │ wisp_port_sim (std::thread + SDL2)
 boards/ board definitions · drivers/ chip drivers
```

A few principles:

- **Layering.** Portable layers include no platform headers and never depend on a layer above them. `tools/check_layering.py` enforces this.
- **Services.** Agent tools, REST routes and Lua bindings are thin adapters over one set of services. Nothing bypasses the services to reach a driver.
- **Screen shape.** Screens may be round or have rounded corners, so every UI lays itself out in the safe area that the Display caps give.
- **Memory.** Internal SRAM is spent against a budget. Large buffers go to PSRAM, and changes are measured on the device with the `mem` serial command.

More in the [architecture notes](docs/architecture.md), the [scene API](docs/scene-api.md) and the [hardware notes](docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md).

## 🗺️ Roadmap

| | Feature | Scope | Status |
| --- | --- | --- | --- |
| 🧱 | Foundation | Device abstraction, storage, power and standby | ✅ |
| 🌐 | Admin console | Wi-Fi and QR setup, LAN console, model profiles, OTA from the browser | ✅ |
| 🎭 | Scene runtime | Lua sandbox, declarative UI, background scenes, notifications, scene files | ✅ |
| 🤖 | Agent | Scene editing and control, HTTP requests, Lua scripts, scene tool discovery, context compaction, live step display | ✅ |
| 🎙️ | Voice | Hold to talk, recognition, `<say>` speech, interruption | ✅ |
| 🧠 | Memory & reminders | Preference notes, to-dos with due times, reminders | ✅ |
| 🔥 | Shell | Wisp home screen, finger-following navigation, settings, scene list, history | ✅ |
| 🔌 | MCP client | Connect to remote MCP servers, giving the agent outside tools and data sources | 📋 |
| 🗣️ | More natural voice | Wake word, streaming recognition with live captions, streaming providers such as Bailian, Volcano and iFlytek | 📋 |
| 🐾 | Wisp interaction | Touch play such as patting its head, more expressions, colour presets, a mini wisp inside scenes | 📋 |
| ⏰ | Scheduling & automation | Timed jobs and event rules that the agent runs on its own | 📋 |
| 📦 | Scene ecosystem | Export and sharing, version rollback, pinned scenes, a scene editor in the console | 📋 |
| 🛡️ | Safety & self-evolution | Permission tiers and audit logs; improving its own tools and prompts, always with rollback | 📋 |

✅ done · 📋 planned. The detailed list is in [`docs/backlog.md`](docs/backlog.md).

## 🤝 Contributing

Issues and pull requests are welcome. People and AI coding assistants follow the same process:

1. **Read the rules first.** The project's rules and conventions are in [`AGENTS.md`](AGENTS.md). Read it, then the design spec your change touches.
2. **Match the process to the change.** Small fixes can go straight to a PR. New features and architectural changes start with a spec, which is discussed and agreed before the plan and the code.
3. **Work on a feature branch.** Write the test first, and make sure `bash tools/ci.sh` passes before you commit.
4. **Write commit messages in the Conventional Commits style.**
5. **Say how it was verified.** The PR says plainly what was verified in the simulator and what on a device.

📖 **The full process, code requirements and PR checklist are in the [contributing guide](CONTRIBUTING.md).**

## 📄 License

Wisp is open source under the [Apache-2.0](LICENSE) license. Third-party projects used or referenced are listed in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) and [`docs/references.md`](docs/references.md).
