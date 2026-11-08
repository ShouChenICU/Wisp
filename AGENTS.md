# AGENTS.md — Wisp project instructions

> For every coding agent (Claude Code, Codex and others) and every human contributor working in this repository.
> This file holds only global, long-lived content: project goals, rules, conventions and workflow. Hardware facts about boards and chips go in `docs/hardware/`; progress goes in the plan documents.
> This is a living document. Update it whenever a rule or convention changes (see "Maintaining this file" at the end).

## 1. Project goals

Wisp (小鬼火, "little will-o'-the-wisp") is a handheld personal agent system that runs on ESP32-series chips and is driven entirely by AI.

- **No apps.** There is no traditional desktop, lock screen or app. Every interface is a "scene", which the agent creates, edits, saves and reuses on the fly. Out of the box the device has only one "main scene", for conversation.
- **The agent controls the device.** The LLM uses only the OpenAI-compatible protocol (directly or through a gateway) and controls the whole device through full tool calling.
- **Interaction.** Voice is the main input and on-screen text is the main output, with TTS as a supplement.
- **Long-term memory.** The agent remembers preferences, to-dos and similar information.
- **Lightweight, customisable, general-purpose.** The framework provides infrastructure and peripheral abstractions. Drivers are pluggable, and nothing is tied to one particular development board.

The roadmap has 8 sub-projects. Each one goes through "design → plan → implementation" on its own:
1. Foundation
2. Connectivity and admin console
3. Scene runtime (Lua)
4. Agent core
5. Voice pipeline
6. Memory and scheduling
7. Security and evolution
8. Main scene and system shell (the wisp character and navigation; the renderer prototype can be developed early in the simulator)

## 2. Documents and order of authority

| Document | Purpose |
|---|---|
| `docs/superpowers/specs/*-design.md` | Design spec of each sub-project, **highest authority** |
| `docs/superpowers/plans/*.md` | Implementation plans, which expand the specs; when the two conflict, the spec wins |
| `docs/architecture.md` | Layering, boot flow, list of event topics |
| `docs/porting-guide.md` | How to add a development board |
| `CONTRIBUTING.md` | Contribution process, code requirements and PR checklist (for outside contributors, consistent with this file) |
| `docs/hardware/<board>.md` | Hardware facts for each development board (pins, chips, known errata, performance baselines) |
| `docs/references.md`, `THIRD_PARTY_NOTICES.md` | Reference sources and third-party licences |

When a change touches a sub-project, read its spec first, then the matching plan. When a change touches a board, read its hardware document first.

## 3. Architecture rules

- **Layering**:
  - Portable layers: `wisp_base` → `wisp_os` → `wisp_hal` → `wisp_core` → `wisp_net` → `wisp_voice` → `wisp_admin` → `wisp_ui` → `wisp_scene` → `wisp_agent` → `wisp_shell` → `wisp_app`.
  - Third-party source lives unmodified in `third_party/` (for example Lua, brought in by `tools/vendor_lua.sh`). Do not change that source. Only `wisp_scene` may use the Lua headers and the `lua` component.
  - Platform layers: `wisp_port_sim` (PC simulator), `wisp_port_esp` (ESP-IDF).
  - Board adaptation goes in `boards/<board>/`; chip drivers go in `drivers/`.
- **No platform headers in the portable layers**: the portable layers must not include `esp_*`, `freertos/*`, `driver/*`, IDF's `hal/`, `soc/*`, `rom/*`, `lwip/*`, `mbedtls/*`, `curl/*`, `sdkconfig.h`, `nvs.h`/`nvs_flash.h`, `sdmmc_cmd.h`, `SDL*`, or the platform layers' `wisp/port/*`. In addition, each layer may depend only on layers that come before it in the order above, never on a later one (for example `wisp_net` must not include `wisp/admin/`, `wisp/ui/` or `wisp/app/`). `tools/check_layering.py` enforces this rule.
- **Board independence**: no board-specific pins, chip models or constants may appear in the portable layers or the platform layers. They may appear only in `boards/` and `drivers/`, and are exposed through the devices' caps. Exception: a platform layer may choose an implementation by SoC target (`IDF_TARGET`/`CONFIG_IDF_TARGET_*`), for example the rendering acceleration in `wisp_port_esp/lv_blend/<chip>/`. Such code depends only on the chip itself, not on a board; board-level constants are still forbidden. No chip name appears in the portable layers (including `lv_conf.h`).
- **Service layer**: agent tools, REST endpoints and Lua bindings are all thin adapters over the same set of service APIs. Nothing may bypass the service layer to call a driver directly.
- **Screen shape**: a screen is not necessarily a full rectangle (rounded-corner screens, round screens). No interface may hard-code margins or assume the whole screen is visible (base spec 5.6).
  - System interfaces (the self-check screen, `ui::SystemLayer` dialogs, the on-screen keyboard, the error panel, the back-gesture zone) must lay out by the Display caps `shape` and `hal::safe_rect()`/`row_span()`, so that their content stays visible.
  - A scene owns the whole screen and does its own layout: its root container fills the screen with no default padding, and the runtime does not pull the scene into the safe area for it (scene spec 5.2 "Screen shape").
  - The runtime gives scenes (and, later, the agent) the screen shape, corner radius, safe area and `row_span` (`shape`/`corner_radius`/`safe_area` of `wisp.device.info()`, and `wisp.device.row_span(y)`).
  - Scrolling content may pass through invisible areas, but when scrolled to either end, the first and last items must be fully visible. The example scenes (`scenes/examples/`) demonstrate these techniques, and their tests check them on all three shapes.
- **Single UI thread**: only the UI thread may call LVGL. Every other thread hands its work to the UI thread through `UiRuntime::post()`. Scripts do not call LVGL directly either.
- **Device model**:
  - Devices are registered in `DeviceRegistry`, with ids of the form `<class>.<name>`.
  - Device capabilities are declared through caps and exposed by the manifest.
  - A device declares its standby policy through the caps `standby` (`off`/`throttle`/`wake_source`, see `wisp/hal/device.hpp`). A device that declares one must implement `set_power(PowerLevel)`. New code drives devices uniformly by their caps `standby`, not one by one by name.
  - A missing device or a device that fails to initialise only degrades the system; it must never block boot.
- **Storage**: data is split into three logical areas, System / Library / Backup. Every persistent write goes through `StorageManager::write_atomic`.

## 4. Build and verification

```bash
# Host side: unit tests + simulator (the main tool for everyday development)
cmake -S simulator -B build/sim -G Ninja && cmake --build build/sim
ctest --test-dir build/sim --output-on-failure

# Full check before committing: formatting, layering, build, tests
bash tools/ci.sh

# Firmware: first activate the ESP-IDF environment at the version the plan pins (export.sh is a bash script)
# On a fresh checkout run set-target first: the bmgr subcommand comes from esp_board_manager, which the component manager fetches
cd firmware && idf.py set-target esp32s3 && idf.py bmgr -c ../boards -b <board> && idf.py build
```

- Whatever can be verified in the simulator must not depend on the device. Verification steps that need the device must be written out explicitly and handed to the user to run. Never claim they have been verified.
- Machine-readable output (`WISP_BOOT_REPORT`, `WISP_MANIFEST`, `WISP_PERF`) is always "prefix + single-line JSON". When you change the format of this output, update the smoke tests and the documentation with it.
- When you change the rendering path, compare the `WISP_PERF` numbers. The frame rate must not drop below the baseline agreed in the spec and the plan.

## 5. Coding conventions

- **Language and error handling**: C++20, compiled with `-fno-exceptions -fno-rtti`. Do not use `dynamic_cast` or `typeid`. All errors are returned through `wisp::Result<T>`.
- **Naming**: component prefix `wisp_`, namespace `wisp::<layer>`, header path `wisp/<layer>/<name>.hpp`, log macros `WLOG_D/I/W/E`.
- **Component CMake**: use `wisp_component(...)`, so that the same CMakeLists works with both idf.py and plain CMake.
- **Formatting and language**:
  - Formatting follows `.clang-format`.
  - Code identifiers, code comments, commit messages and the documentation are in English. The documentation means `README.md`, `AGENTS.md`, `CONTRIBUTING.md` and `docs/`, apart from the design specs and plans.
  - `README.md` (English) is the source. `README.zh-CN.md` (Chinese) and `README.ja.md` (Japanese) are translations of it; update them together with `README.md` whenever it changes.
  - Design specs and plans (`docs/superpowers/specs/`, `docs/superpowers/plans/`) may be written in Chinese.
  - User-facing UI text is localised (zh/ja/en).
- **Testing**: use GoogleTest and follow TDD (write the failing test first, then the implementation). Test output must be clean, with no warnings.
- **Dependencies**: external dependencies are pinned to exact versions; the firmware commits `dependencies.lock`.
- **Settings keys**: Settings namespaces and keys must not exceed 15 characters, for NVS compatibility.
- **Memory allocation**: use `os::alloc(Mem::Large)` for large buffers and `Mem::Dma` for DMA buffers. Do not call the platform's allocation functions directly.
- **Internal RAM budget** (the ESP32 has only a few hundred KB of internal SRAM, and when it runs out, the error often appears far from the cause, for example httpd failing to get a stack):
  - Internal SRAM is reserved for: DMA buffers (`Mem::Dma`); the stack of the flash thread `wisp.fs`; ISR and IRAM code/data; ESP-IDF's own tasks and drivers; task stacks declared as hot-path (`os::TaskConfig::hot_stack`, default false; currently only the UI thread `wisp_ui` declares true, because of the rendering hot path rather than flash safety; any new hot-stack task must justify itself in its spec). Everything else larger than a few KB uses `Mem::Large` (PSRAM), for example the Lua heap, LLM context and JSON, audio ring buffers and caches of all kinds.
  - **Only `wisp.fs` accesses flash**: `os::TaskConfig::flash_access` (for an httpd instance, `EspHttpServerConfig::flash_access`) defaults to false, and only `wisp.fs` (`core::StorageWorker`) declares true. The port decides where a stack goes, per chip, from the `flash_access`/`hot_stack` declarations; callers do not choose the location directly. When another task needs flash (Settings/NVS, StorageManager/LittleFS, OTA, partition reads, writes and mapping; services called through it and callbacks it runs on behalf of others all count; reading constants and the web asset pack that are already mapped into the address space does not), it hands the work asynchronously to `wisp.fs` through `StorageWorker::submit()`/`run_sync()`. There are two queues: Normal (settings, JSON, scene data) and Bulk (firmware chunks, file download chunks). `WISP_ASSERT_FLASH_THREAD` (`wisp/os/flash_guard.hpp`) enforces this rule at run time: in the simulator and unit tests a violation prints the call site and then `abort()`s; on the device each call site logs `WLOG_E` once and does not abort on its own (a task whose stack is in internal RAM keeps running; a task whose stack is in PSRAM is aborted by ESP-IDF's own cache check in hardware). With XIP enabled (`CONFIG_SPIRAM_XIP_FROM_PSRAM`), reading flash that is mapped at run time (such as the `fonts` partition) during an erase or write returns 0xFF, so such boards must enable `CONFIG_SPI_FLASH_AUTO_SUSPEND` (`docs/porting-guide.md` "Font partitions"). Size stacks from the high-water mark measured with the `mem` command, plus a margin.
  - Budget: the minimum free internal RAM and minimum largest block, for the worst case (STA+AP, two httpd instances, the WebSocket log stream, a TLS handshake and an OTA upload all at once) and for idle, are defined in `wisp/app/ram_budget.hpp`. Any change that consumes internal RAM must be re-measured on the device.
  - Diagnostics: the firmware serial command `mem` (each heap and each task's stack); every time net starts its services it logs one `internal RAM: ...` line, which is a warning when below budget. Measured data for each board is recorded in `docs/hardware/<board>.md`.
- **Comments**: explain only why the current code is the way it is (constraints, invariants, pitfalls you hit). Do not write "which review round or which fix changed what": change history belongs in commit messages, not in comments. Do not reference any file under `.superpowers/` (`task-*-brief/report/review.md`, `global-constraints.md` and so on; these are not committed to git and are dead links on a fresh checkout). Do not use review-id references ("Fix round N", "Re-review N", "R2-N", "Task N", bare finding ids such as "M8"/"N3"/"I1"). When you need to point at context, point at a committed authoritative source instead: a section of a spec ("spec 6.7" is fine), a section of AGENTS.md, `docs/hardware/*.md`, or another function or file in the repository. `tools/ci.sh` has a grep guard for this.

## 6. References and licensing

- The project licence is Apache-2.0.
- **No forks, no hard dependency on upstream.** Other open-source projects serve only as references; reimplement what is needed, in a targeted way.
- When porting or adapting outside code, state its source and licence in the file header, and register it in `docs/references.md` and `THIRD_PARTY_NOTICES.md`.
- Never leak the user's personal information or secrets in code, requests or logs. API keys are write-only; they can never be read back.

## 7. Workflow

- **Process**: a new feature or sub-project starts with a design spec. After the user approves it, write the implementation plan. Implementation starts only after the user has reviewed the plan (brainstorming → writing-plans → subagent-driven-development).
- **Branches**: do not develop directly on `main`; use feature branches. Ask the user before you push, merge or release.
- **Commits**: use the Conventional Commits style (`feat(core): ...`, `docs: ...`). Commits created by an agent carry the `Co-Authored-By` line required by the current environment's attribution rules.
- **Communication**: always talk with the user in Chinese. Give projects and modules English names, and explain their meaning in Chinese.
- **Deviations**: when you find a mistake in a spec or plan, never work around it silently. For a small issue, write the ruling in the plan or execution record and carry on; for a large issue, stop and ask the user.

## 8. Common pitfalls

- **Atomic writes on FAT**: on the FAT file system, `rename` cannot overwrite an existing file. Atomic writes use two-stage naming: write `.tmp` → fsync → rename to `.new` (at this point `.new` is always complete) → delete the target → rename `.new` to the target. On read, if the target is missing and `.new` exists, recover from `.new`. `.tmp` may be half-written and is always discarded as garbage.
- **LVGL configuration**: LVGL must use `components/wisp_ui/config/lv_conf.h` (guarded by the `WISP_LV_CONF` macro).
- **Small mallocs land in internal RAM**: with PSRAM enabled, a plain `malloc`/`new` smaller than `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` (16 KB) is served from internal RAM first, and so are the intermediate buffers of a growing `std::string`/`std::vector`. So allocate large data with `os::alloc(Mem::Large)` from the start; do not rely on malloc to fall through to PSRAM.
- **Generated code**: the code that `idf.py bmgr` generates is git-ignored. Do not edit it by hand.
- **bmgr compiles every source file in the board directory**: the generated `gen_bmgr_codes` scans `boards/<board>/` with `SRC_DIRS`, so it also compiles Wisp's own `board.cpp`; `firmware/CMakeLists.txt` takes it back out. Any new `.c`/`.cpp` in a board directory goes into `gen_bmgr_codes` by default.

## 9. Maintaining this file

- **When to update it**:
  - when project goals, architecture rules, conventions or the workflow change (update the related spec or `docs/architecture.md` at the same time);
  - when you find a pitfall that applies across boards and sub-projects.
- **What does not belong here**: hardware facts (they go in `docs/hardware/`), progress and status (they go in the plan documents), details of one particular task (they go in the code or the spec).
- **Keep it short**: delete stale entries outright; do not pile up history.
- **Commits**: put a change to this file in the same commit as the change that triggered it, with a `docs(agents): ...` commit message.
