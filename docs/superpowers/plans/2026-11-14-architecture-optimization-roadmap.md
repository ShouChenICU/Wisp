# Architecture Optimization Roadmap

> Source: the 2026-11-14 architecture review (agent core, scene runtime, foundation/HAL/ports, shell/voice UX, connectivity). This file fixes the scope, order and key decisions of the follow-up work. Each phase gets its own detailed plan under `docs/superpowers/plans/`; phases marked **spec first** get a design spec under `docs/superpowers/specs/` before their plan (AGENTS.md section 7).

## Scope

In scope (owner's ruling, 2026-11-14):

- Review section 1, items 1-3: live tool results cut to 4 KB; turn failures invisible on the device; a hold stopping the running turn.
- Review section 2 (implementation defects), except crash logging.
- Review section 3 (extensibility), all of it. Target hardware stays ESP32-S3 now, ESP32-S31 next.
- Review section 4 (structural debt), all of it. System settings get one authoritative source; standby power is optimised in depth.

Out of scope:

- Restricting the agent's outbound HTTP, LAN access or adding confirmations. The agent is meant to have as much control as possible and to ask the user as rarely as possible.
- Crash logs / coredump (debugging happens over the serial port).
- Review section 5 (release artifacts, data forward-compatibility, frontend tests, release cadence).

## Global Constraints

- AGENTS.md applies in full: layering (`tools/check_layering.py`), C++20 with `-fno-exceptions -fno-rtti`, `wisp::Result<T>`, GoogleTest + TDD, `Mem::Large` for anything beyond a few KB, only `wisp.fs` touches flash, settings keys of at most 15 characters, user-facing text localised in zh/ja/en.
- Work on feature branches, one per phase; Conventional Commits; `bash tools/ci.sh` green before each commit.
- Device-only checks are written into each plan as explicit steps for the owner; they are never reported as verified.
- A phase that changes internal-RAM use, task stacks or the rendering path re-measures with `mem` / `WISP_PERF` on the device (owner).
- A phase that changes a rule or convention updates AGENTS.md, `docs/architecture.md` and the affected spec in the same commit.
- Remove the stale backlog entries (`wisp.tools.register`, headless runs, firmware build in CI) in phase A.

## Phases

Order follows two rules: fix user-visible defects first; then build the primitives that later phases depend on (state cells and the settings schema before power; the device model before scene capabilities).

| # | Phase | Kind | Depends on | Size |
|---|---|---|---|---|
| A | Agent turn reliability | plan ready: `2026-11-14-agent-turn-reliability.md` | - | M |
| B | Voice and wake responsiveness | plan | A | M |
| C | Scene runtime scheduling and install safety | plan | - | M |
| D | State cells and event bus semantics | spec first | - | M |
| E | Settings schema: single source of truth | spec first | D | M |
| F | Standby power, in depth | spec first | D, E | L |
| G | Storage worker lanes | plan | - | M |
| H | Device model, driver adapters and board kit (S3 → S31) | spec first | - | L |
| I | Scene capability modules, permissions and new capabilities | spec first | H | L |
| J | LLM provider and tool provider interfaces | spec first | A | M |
| K | Voice protocol interfaces | spec first | B | M |
| L | Composition root, layering DAG and core split | spec first | D, E | L |
| M | Shell page stack and SceneRuntime split | plan | I | M |

A, C, G and H have no mutual dependencies and can run in parallel worktrees.

### A. Agent turn reliability

Detailed plan: `docs/superpowers/plans/2026-11-14-agent-turn-reliability.md`.
Covers: tool results of the running turn sent whole; `docs.read` paging; old-turn call arguments elided; context-overflow recovery and estimate calibration; transport retry and `Retry-After`; turn failures shown on Home; a hold no longer stops the turn, plus an explicit stop control; the talk overlay on every non-Home screen.

Left for phase J: carrying a summary across the 1 MiB rollover (needs a new message kind in `ConversationStore`).

### B. Voice and wake responsiveness

- **Speech pacing.** In `VoiceService::pump_speech()` (`voice_service.cpp:598`), sentences after the first wait for `!llm_busy_`. Change the gate to "the `<say>` block has closed": `ConversationController` calls a new `VoiceService::say_closed()` when `SayParser` sees `</say>`; after that, pending sentences may run while the model streams. Still at most two TLS connections (one LLM, one TTS). Device check: `mem` during a spoken multi-step turn.
- **Wake from deep standby.** `Readiness` gains `Connecting` (a saved network exists, Wi-Fi not yet connected). `hold_begin()` records in `Connecting`; `VoiceService::capture()` waits up to `kNetWaitMs = 8000` for the network before the ASR request, publishing `voice.state` `transcribing`. Home shows a new string `HomeConnecting` instead of `HomeNoWifi`.
- **Screen-off talk.** In `TalkKey`, a press held `kHoldStartMs` while the screen is off wakes the screen and starts the capture in one gesture (today the first press only wakes).
- **Pre-roll.** `VoiceService` opens the microphone on press (`CaptureMode::Hold` gets a `prime()` step called at button-down, not after `kHoldStartMs`), recording into a `kPrerollMs = 500` ring in `Mem::Large`; a confirmed hold prepends it, a short press discards it. `listening` is published after the microphone is open. The cue tone moves to after the pre-roll is armed and is excluded from the buffer by time.
- **Readiness checklist.** `Readiness` becomes a bit set (`Wifi`, `Llm`, `Asr`, `Tts`); Home lists what is missing with the admin URL. `ConversationController::tap()` shows the same hint for `kFadeAfterMs`.

Decision needed from the owner: none.

### C. Scene runtime scheduling and install safety

- **Completions are never dropped.** HTTP and files completions (`scene_runtime.cpp:1426`, `:1477`) go to a per-instance completion list in `Mem::Large` (bounded by the instance's in-flight limits, so it cannot grow without limit); the queue carries only a wake-up.
- **Priorities.** The scene task drains three lanes in order: foreground UI events, foreground callbacks/completions, background scenes and agent Lua. `kLuaRunSliceMs` drops from 1000 to 200.
- **Two HTTP lanes.** `net::SceneHttp` runs two workers: `wisp.shttp` (interactive: agent and ordinary scene requests) and `wisp.shttp.dl` (downloads). Both pass `TlsGate`; the gate's limit stays as measured. Device check: `mem` worst case with a download and an agent fetch together.
- **Install baseline.** Within one draft session, `.prev/<id>` is written only by the first install (the version from before the session); later installs of the same session leave it alone. `scene.rollback` therefore returns to the last version the user had. The session ends when the draft is discarded or the turn that created it ends.
- **List reset without a blank frame.** `apply_list_reset` keeps the old rows until the first `ListRows` of the new generation arrives.

### D. State cells and event bus semantics (spec first)

Problem: one lossy primitive (queue of 64, drop-oldest) carries state, commands and telemetry; consumers carry their own resync code.

Decisions for the spec:

- Add `core::StateCell<T>`: holds the latest value and a revision; `get()` from any thread; `subscribe(cb)` delivers the current value at once and then every change, coalescing (a slow subscriber sees the latest value, never a stale one, and nothing is "dropped"). Dispatch on `wisp.bus`.
- State topics move to cells: `power.state`, `net.wifi.state`, `net.ap.state`, `system.settings` (phase E), `scene.state` per scene, `agent.state`, `voice.state` (state only), `model.profiles`.
- Commands stop using the bus: power key, factory-reset request and firmware confirmation call service APIs directly (their `pending` flags and one-second resyncs go away).
- Telemetry stays on the lossy bus; `voice.state`'s `level` moves to its own topic `voice.level`.
- A topic registry header `wisp/core/topics.hpp` with constants and payload builders/parsers; `docs/architecture.md`'s topic table is generated from or checked against it.
- Remove the compensations once their source is a cell: `kSettingsRefreshTicks`, `self_check_screen` periodic resync, `reset_pending`.

The admin WebSocket keeps its `events` wire format (cells publish a bus event on change for forwarding).

### E. Settings schema: single source of truth (spec first)

Problem: one key touches about 8 places; defaults are repeated and already disagree (`app.cpp:357` brightness, `loopback.cpp:28` / `esp_audio.cpp:24` volume).

Decisions for the spec:

- One constexpr table in `wisp/core/settings_schema.hpp`: per key `{ns, key, type, min, max, default, i18n label id, flags (agent | scene | web | device_page), unit}`. Everything else derives from it:
  - `SystemSettings` values, `to_map()`, `get()`, validation;
  - the agent's `settings.set` JSON schema and `device.status` fields;
  - the Lua `wisp.device` settings fields;
  - `GET /api/v1/settings/schema`, from which the web System page builds its form;
  - the Shell settings page rows for keys flagged `device_page`.
- The current values live in a `StateCell<SettingsSnapshot>` (phase D); `merge_settings()` and every private cache (`tts_mute`, `notify_vol`, `utc_offset_min`, ...) are replaced by reads of the cell.
- No default or range literal for a setting appears outside the table; a CI grep guard enforces this for the known keys.
- A unit test walks the table: keys at most 15 characters, labels present in zh/ja/en, defaults inside their ranges.

### F. Standby power, in depth (spec first)

Decisions for the spec:

- **No periodic wake-ups in standby.** `wisp.net` blocks without a timeout while the radio is off (woken by the power cell, a wake lock or a command); `wisp.power`'s 1 s tick stops in `standby`/`deep_standby` and is replaced by one-shot deadlines (deep-standby entry, next reminder, wake-lock reminder); the 2 s SD probe stops in standby (card-detect interrupt where the board has one, else re-probe on wake). Every remaining periodic source is listed in the spec with its justification.
- **Light sleep becomes a board capability** (`power.main` caps `light_sleep: true`), not the global `WISP_ENABLE_LIGHT_SLEEP` build option.
- **New level `hibernate`** below `deep_standby`: flush storage, arm the RTC alarm for the next reminder (new `hal::Clock::set_alarm(int64_t utc_s)` / `clear_alarm()`; PCF85063 driver gains its alarm registers), then ESP deep sleep with the power key and RTC alarm as wake sources. Entered after `power/hibernate_s` in `deep_standby` with no wake lock (default and range fixed in the spec). Boot after hibernate is a cold boot; the boot report notes the wake cause.
- **Flush off the UI thread.** `before_deep_standby → flush_storage()` (up to 7 s on the UI task today) runs on its own short task; the level switch does not wait for it.
- **Measurement first.** The spec starts from the owner's current-draw numbers per level on the 2.16 board (backlog device check 2), and sets a target per level.

Decision needed from the owner: whether `hibernate` may drop Wi-Fi-dependent background scenes and wake locks (it must, to reach deep sleep), and the default delay.

### G. Storage worker lanes

- SD-card file operations move to their own worker `wisp.sd.io` (PSRAM stack; the flash-thread rule concerns internal flash only). `StorageManager` routes by mount point.
- CPU work leaves `wisp.fs`: PNG icon decoding and manifest parsing run on the caller's task after the bytes are read.
- A third queue, Interactive (conversation appends, settings, memory, todos), is drained before Normal; Normal jobs longer than one file operation are split.
- `mem` prints each queue's longest wait and longest job since boot.

### H. Device model, driver adapters and board kit (spec first)

The spec `docs/superpowers/specs/2026-11-14-device-model-design.md` is approved (2026-11-14) and splits the phase into H1 (device identity and the generic access path), H2 (board kit, driver adapters, target readiness) and H3 (keypad input for system UI and the pixel-format cap).

Decisions for the spec:

- **Interface tags.** Each HAL interface has its own `kInterface` id; `DeviceRegistry::get<T>` checks the interface, not the class. `DeviceClass` stays as a display category only.
- **Roles instead of fixed ids.** Boards pass ids and a caps `role` (`main`, `talk`, ...); portable code looks devices up with `registry.by_role<T>("main")`. The `*.main` literals in 13 portable files go away.
- **Generic access path.** `hal::Device` gains optional `describe()` (properties and commands with types, in caps), `get(prop)` and `invoke(cmd, Value)`. One agent tool (`device.call`) and one Lua binding (`wisp.device.call`) expose every device that describes itself, so a new simple peripheral (LED, haptics, encoder) needs a driver and a board line only.
- **Driver adapters leave the port.** `EspPower`/`EspClock`/`EspImu` (AXP2101, PCF85063, QMI8658 adapters) move next to their drivers as `drivers/<chip>/wisp_adapter.*`; `wisp_port_esp` no longer requires chip drivers.
- **Board kit.** `boards/common/` holds the shared init code (335 duplicated lines today) driven by a declarative `BoardSpec`; pins come from the bmgr YAML only. A `board.cpp` keeps only real differences.
- **S31 readiness.** Chip-specific sdkconfig moves to `sdkconfig.defaults.<target>`; PSRAM of 8 MB or more is required; no fallback is designed (the boot report records a smaller or missing PSRAM); the support matrix (minimum PSRAM, screen or voice) goes into `docs/porting-guide.md`. S31 specifics are filled in when its ESP-IDF support is pinned.
- **Input and display generality.** `UiRuntime` registers an LVGL keypad/encoder group when the board has such devices; `Display` declares its pixel format in caps (RGB565 stays the only implemented one).
- A device without a display is not a supported configuration for now.
- `AudioIn` gains channel roles (mic, reference) in caps, without changing the mono read path yet.

### I. Scene capability modules, permissions and new capabilities (spec first)

- **`BindingModule` registry** in `wisp_scene`: `{name, install, on_background, on_foreground, on_close, on_fault, contexts (scene | headless | agent), permission}`. Contexts are an allow-list; `strip_bindings` is deleted. `SceneRuntime` iterates modules instead of wiring each service.
- **Permissions.** `scene.json` `permissions` gains `mic`, `audio`, `secrets`; the install flow asks once per new permission, as it does for HTTP hosts today. The agent's own `lua.run` context is granted by the module's `contexts`, with no prompt (owner's ruling on agent autonomy).
- **`"api": N`** in `scene.json`; install refuses a higher N with a clear error.
- **New capabilities**, each its own task group: `wisp.audio.play` / `record` (arbitrated through `OccupancyRegistry` with voice and tones); pointer events with coordinates (`on_press(x, y)`, `on_drag`) for `ui.canvas`; `ui.image` with a bindable `src` and `files:<name>`; a declarative `transition` property backed by LVGL animations; `wisp.input.on("key", ...)`; `wisp.device.call` (phase H).
- **Authoring help.** `pad = "safe"` / `wisp.layout.inset(y1, y2)`, `wisp.i18n.pick{}`, firmware-provided `wisp.import("sys/<name>")` libraries; examples rewritten to use them.
- **Composition.** `wisp.scene.open(id, args)`; the agent's `scene.open` gains `args`.
- **Secrets.** Per-scene write-only secrets; `wisp.http` headers accept `{{secret:name}}`, substituted on `wisp.shttp`.
- **Structure.** Keyed list rows (`ui.each(items, key_fn, row_fn)`) and conditional subtrees (`ui.when`).

### J. LLM provider and tool provider interfaces (spec first)

- `net::LlmProvider` interface (`complete(const LlmCall&) -> LlmResult`); `LlmClient` becomes the chat-completions implementation. `AgentDeps` holds the interface.
- Profile options select a dialect: token-limit field name, `reasoning_content` echo, where the dynamic info goes (trailing system message or merged into the last user message), parallel tool calls.
- `LlmMessage` carries content parts (text, image); the profile's `vision` option becomes real.
- `finish=length` continues the reply once instead of ending the turn as done.
- `agent::ToolProvider` interface (`search`, `describe`, `call`); scene tools are one provider, an MCP client (streamable HTTP through `SceneHttp`'s agent principal) is the second.
- `ToolThread::Io` gets its documented meaning (a one-shot task), not the flash worker.
- Rollover carries a summary into the new conversation.
- `TurnRunner` is split out of `AgentService::Impl`.

### K. Voice protocol interfaces (spec first)

- `voice::AsrSession` (`begin`, `push(frames)`, `finish` → partial/final callbacks) and `voice::TtsStream` (`synthesize(text)` → PCM callback, decoding inside). `VoiceService::capture()` pushes while recording; the OpenAI adapter buffers and uploads as today.
- Profiles gain `protocol`; the first adapters are the existing request shapes. Streaming vendors and non-PCM codecs are separate follow-ups.

### L. Composition root, layering DAG and core split (spec first)

- **Lifecycle.** A `ServiceStack` in `wisp_app`: each start step pushes its stop closure; `App::stop()` unwinds it. The 46 hand-ordered stops go away.
- **`Platform`** splits into domain sub-structs (`NetPlatform`, `StoragePlatform`, `DiagPlatform`, ...).
- **`PowerManager`** depends on an `os::Executor` interface instead of `ui::UiRuntime*` and moves to `wisp_core`.
- **UI code leaves `wisp_app`**: self-check, net cards, scene section and the system-duties dialogs move to `wisp_shell`.
- **Layering becomes an explicit DAG** in `tools/check_layering.py` (allowed edges instead of a linear order). `wisp_core` splits into infrastructure and domain components (`wisp_scene_pkg`, `wisp_memory`, `wisp_models`, `wisp_notify`); `wisp_admin` sits above agent and scene, so `admin::AgentApi` and similar indirections can go. AGENTS.md section 3 and `docs/architecture.md` change in the same commit.

### M. Shell page stack and SceneRuntime split

- `shell::PageStack`: pages implement `build`, `root`, `can_swipe(dir)`, `on_back`; drags run between the top two entries. Home and the scene view are the bottom entries. `ConversationController`'s booleans become an explicit phase enum.
- `SceneRuntime` loses `AgentLuaHost` and `SceneToolHost` to their own classes behind a `SlotHost` interface (service wiring already moved to modules in phase I).

## Open decisions for the owner

1. Phase F: may `hibernate` end background scenes and drop wake locks, and what default delay (proposal: 30 minutes in `deep_standby`)?
2. Phase H: is `device.call` allowed for every self-describing device without confirmation (proposal: yes, consistent with the autonomy ruling)?
3. Phase I: should scene permissions (`mic`, `audio`, `secrets`) prompt at install like HTTP hosts (proposal: yes, once per permission, since scenes can come from outside the agent)?
4. Phase L: the component split changes AGENTS.md section 3. Confirm the DAG direction before the spec is written.

Proposals are used unless the owner rules otherwise before the phase's spec.
