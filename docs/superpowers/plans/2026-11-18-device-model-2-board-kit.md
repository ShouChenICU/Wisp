# Device Model H2: Board Kit, Driver Adapters and Target Readiness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A third ESP32-S3 board is a `BoardSpec` plus the bmgr YAML with pins written once; `wisp_port_esp` names no chip driver and no device id; everything target-specific sits in places chosen by `IDF_TARGET`, so a future SoC port only adds files.

**Architecture:** The AXP2101/PCF85063/QMI8658 Wisp adapters move into their driver components (`drivers/<chip>/wisp/`), and each board names its drivers in `boards/<board>/board.cmake`. Every ESP device takes its id and role from its config, filled from the board kit's `Slot`s. `boards/common/board_kit.{hpp,cpp}` runs today's shared bring-up sequence from a declarative `BoardSpec`, reading every pin from bmgr's device and peripheral configs (buttons become bmgr `gpio` peripherals). S3-only sdkconfig moves to `firmware/sdkconfig.defaults.esp32s3`; the DFS minimum and Wi-Fi model/bands come from a per-target fact table in the port; partition tables are shared by flash size; the boot report records a PSRAM below 8 MB; `AudioIn` declares channel roles.

**Tech Stack:** C++20 (`-fno-exceptions -fno-rtti`), ESP-IDF v6.0.3, esp_board_manager 0.7.5 (`firmware/dependencies.lock`), GoogleTest (host), Python (`tools/check_layering.py` and its self-test).

**Spec:** `docs/superpowers/specs/2026-11-14-device-model-design.md` (approved 2026-11-14), sections 6, 7, 8, 11, the H2 part of section 13, 14.4 and 17.3. Roadmap: `docs/superpowers/plans/2026-11-14-architecture-optimization-roadmap.md` section H. H1 (`docs/superpowers/plans/2026-11-14-device-model-1-identity-and-generic-path.md`) is merged; its deviation 1 (ESP ids stay in the port) is closed here. H3 (keypad, pixel format) is a separate plan.

**Owner decisions (2026-11-18):** rulings 7 (drop the 1.75C boot-time GPIO3 warning; electrical checks stay in its hardware doc) and 8 (touch levels/flags move into the YAML, checked by touching all four corners) are accepted; `CHANGELOG.md` gains an `Unreleased` section.

## Global Constraints

- Branch `feat/board-kit`; no commits on `main`; nothing pushed.
- AGENTS.md applies in full: layering, board independence, `wisp::Result<T>`, no exceptions/RTTI, comments explain why (no review ids, no change history, no references to files under `.superpowers/`), Conventional Commits ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. AGENTS.md edits are their own `docs(agents): ...` commits inside the task that makes the rule true.
- `bash tools/ci.sh` is green (exit 0, no warnings in test output) before every commit.
- **Firmware build** (every task that touches `boards/`, `drivers/`, `firmware/` or `components/wisp_port_esp/`), for `B` in `waveshare_esp32_s3_touch_amoled_2_16` and `waveshare_esp32_s3_touch_amoled_1_75c`:
  ```bash
  cd firmware && bash -c ". ~/esp/esp-idf-v6.0.3/export.sh >/dev/null 2>&1 && idf.py bmgr -c ../boards -b $B && idf.py -B build-$B -D SDKCONFIG=build-$B/sdkconfig -D SDKCONFIG_DEFAULTS='sdkconfig.defaults;components/gen_bmgr_codes/board_manager.defaults' -DWISP_BOARD=$B build"
  ```
  Expected: exit 0 and the line `wisp.bin binary size 0x… bytes. Smallest app partition is 0x… bytes. 0x… bytes (N%) free.` Record, per board, the image size and `100 - N` (% used) in the task's commit body, next to Task 1's baseline. Budget: at most 80 % used (the 2.16 is at about 78.9 %); any growth over 4 KiB against the baseline is explained in the commit body or removed. The `fonts`/`glyphs` images are unaffected by this plan.
- Deletions only on literal absolute paths inside the worktree; never a bare `git stash`; never add `.claude/` or `.playwright-mcp/`.
- **No internal-RAM change**: no new task or stack, no new hot path, no buffer growth (`EspAudioIn`'s scratch keeps its size, Task 5). The kit runs once inside `Board::init()` on the task that already runs it.
- **No behaviour change on either board**: the same device ids, classes, models, caps and roles in `WISP_MANIFEST`, plus one additive cap (`channels` on `audio_in.main`, Task 5); the same boot-report devices and states; the same bring-up order; the same pins held through light sleep. `WISP_BOOT_REPORT` gains only the `psram` note (Task 9), absent on both boards.
- Board code (`boards/**`) and drivers may name device ids; after Task 3 `components/wisp_port_esp/` may not, and `tools/check_layering.py` enforces it.
- Device-only checks are listed at the end for the owner and are never reported as verified.

## Review Focus

1. **A PSRAM of exactly 8 MiB gives no note; one byte less does** (`"7MB"`, truncated), 0 gives `"none"`, and the simulator (unmeasured) none. Tests in Task 9.
2. **A pin the YAML leaves at -1, or names twice, is not held** (a board without touch reset, or with PA and CS on one pin): the hold list skips negatives and duplicates and keeps today's order. Tests in Task 6 (`HeldPins.*`).
3. **A capture layout other than two mics** (`{mic, ref}`, three mics, four channels): the mono output averages only `mic` channels with truncation toward zero and a read never overruns the fixed scratch buffer; an empty list, a list without `mic` or more than four channels is refused at `create()`. Tests in Task 5 (`AudioDownmix.*`, `CaptureChunk.*`).
4. **A power key whose GPIO path is unavailable** (peripheral failed, or the edge service cannot be installed) gets its board's fallback device registered exactly once; a fallback that fails is recorded failed with its own reason; a key with no fallback is recorded failed. The selection goes through the tested `power_button_use_gpio()`; review step in Task 6, device check 9.
5. **A fresh sdkconfig is unchanged by moving options to `sdkconfig.defaults.esp32s3`** (an existing `build-*/sdkconfig` hides the change): Task 4 regenerates both boards' sdkconfig from scratch and diffs them against Task 1's baseline.

## Verified before writing (spec 17.3: "verified first in H2")

Read from the pinned sources; Task 1 re-checks them on a real `bmgr` run before any code moves.

- **V1, spec 7.1 (`boards/common/` beside the boards).** esp_board_manager 0.7.5 treats a directory as a board only when it holds all of `board_info.yaml`, `board_peripherals.yaml`, `board_devices.yaml` (`generators/config_generator.py`, `_is_board_directory`); a directory with none of them is recursed into silently (`_scan_all_directories_for_board_infos`, depth limit 3), so `boards/common/` and `boards/common/partitions/` are ignored. `gen_bmgr_codes` compiles sources only from the selected board's directory (`generators/source_scanner.py`), so `boards/common/*.cpp` is never compiled by bmgr. The kit stays in `boards/common/`; no fallback.
- **V2, spec 7.3 (peripheral configs).** `esp_board_manager_get_periph_config(name, &cfg)` exists (`include/esp_board_manager.h:174`, `src/esp_board_manager.c:142`). A `gpio` peripheral's config is `periph_gpio_config_t` with `gpio_num` (`peripherals/periph_gpio/periph_gpio.h`); its YAML keys are `pin`, `mode`, `pull_up`, `pull_down` (`periph_gpio.py`). `esp_board_manager_init()` initialises every declared peripheral whether a device uses it or not and only logs a peripheral failure (`src/esp_board_periph.c:627-649`), so a button's peripheral handle exists exactly when its `gpio_config()` succeeded. The other pins are in device configs bmgr already returns: display reset and CS (`dev_display_lcd_config_t::sub_cfg.spi.panel_config.reset_gpio_num`, `.io_spi_config.cs_gpio_num`), touch reset (`dev_lcd_touch_config_t::touch_config.rst_gpio_num`), PA enable (`dev_audio_codec_config_t::pa_peripheral.port`, the resolved pin of `pa_name`), SD mount point (`dev_fs_fat_config_t::mount_point`). Touch `flags` (`swap_xy`, `mirror_x`, `mirror_y`) and `levels` are YAML keys too (`devices/dev_lcd_touch/dev_lcd_touch.py:150-164`). No 7.3 fallback is needed.
- **V3, spec 8.1 (`sdkconfig.defaults.<target>` with an explicit `SDKCONFIG_DEFAULTS`).** ESP-IDF v6.0.3 resolves each listed file to an absolute path (`tools/cmake/project.cmake:701-702`) and appends `<file>.<IDF_TARGET>` right after it when it exists (`tools/cmake/kconfig.cmake:180-184`). The multi-board command therefore reads `sdkconfig.defaults`, `sdkconfig.defaults.esp32s3`, `board_manager.defaults` in that order; the build command and the porting guide need no change.
- **V4 (new, spec 6.3).** Component requirements are expanded in a separate `cmake -P` run (`tools/cmake/component.cmake:209-226`) that sees build properties but not cache variables, so `firmware/main/CMakeLists.txt` cannot `include(boards/${WISP_BOARD}/board.cmake)` (`WISP_BOARD` is empty there). Build properties set before `project()` are exported to that run (`tools/cmake/build.cmake:46-51`).
- **V5 (new, spec 8.3).** `heap_caps_get_total_size(MALLOC_CAP_SPIRAM)` is the PSRAM *heap*, smaller than the chip (the 2.16's diagnostics show a PSRAM "total" of about 5 MB, `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md` item 62), so it would flag both 8 MB boards. `esp_psram_get_size()` (`esp_psram.h:43`) is the detected chip size, 0 when PSRAM did not initialise.

## Deviations from the spec

Small rulings, recorded per AGENTS.md section 7; none changes the design's intent.

1. **Board requirements travel as a build property** (V4). `firmware/CMakeLists.txt` includes `boards/${WISP_BOARD}/board.cmake` before `project()` and publishes its list with `idf_build_set_property(WISP_BOARD_REQUIRES ...)`; `firmware/main/CMakeLists.txt` reads it with `idf_build_get_property()`. Spec 6.3 had `firmware/main` include the file.
2. **PSRAM size from `esp_psram_get_size()`**, not `heap_caps_get_total_size()` (V5). The note's text is spec 8.3's: `"<N>MB"` with N the size in whole MiB (truncated), `"none"` for 0.
3. **Drivers also require `wisp_core`** (for `WLOG_*`), and `axp2101` requires `wisp_port_esp` for `kButtonStandbyPollMs` (`wisp/port/esp/button_input.hpp`), the standby read interval it shares with `EspGpioButton`. Spec 6.1 listed `wisp_hal wisp_os`. The dependency points from the driver to the port, never back; `tools/check_layering.py` rejects the reverse (Task 2).
4. **No new GPIO helpers in the port.** Spec 6.2 proposed `gpio_hold_in_light_sleep(pins)` and `configure_input(pin, active_low)`. bmgr now configures button inputs (V2) and the kit is the only code holding outputs, so the loop stays in the kit.
5. **Chip model strings default in the adapter configs** (`"AXP2101"`, `"PCF85063"`, `"QMI8658"`, `"axp2101-pwrkey"`): drivers may name their chip (AGENTS.md section 3); boards stop repeating them.
6. **`DisplaySpec` carries no draw-buffer fields.** Neither board sets `draw_buf_lines`/`draw_buf_mem`/`render_mode`; the first board that needs them adds the fields (they map one to one onto `EspDisplayConfig`).
7. **A button may name a fallback.** `ButtonSpec::fallback` (a function the board supplies) replaces the 1.75C's hand-written PMU fallback: when set, the GPIO path also needs `EspGpioButton::prepare_edge_input()`, exactly as today's `power_button_use_gpio(kUseSysOut, configured, edge_ready)`. The 1.75C's success-path warning ("GPIO3 active-high selected; verify …") is dropped: the pending electrical checks are tracked in its hardware document, not in every boot log.
8. **Touch orientation and levels move into the YAML too.** Spec 7.3 removes only the `rst_gpio_num`/`int_gpio_num` overrides from `setup_device.c`; the same factories also override `levels` and `flags`, which bmgr reads from YAML (V2). Moving them leaves both touch factories as plain pass-throughs to `esp_lcd_touch_new_i2c_cst9217()`, so `setup_device.c` holds only the panel init tables (spec 7.4).
9. **The id-literal guard now covers `wisp_port_esp`** (Task 3): once the port takes ids from configs it has none, and the guard keeps it so. Spec 2.4 excluded the port.
10. **The section numbers in the brief.** The plan split is spec section 13 (not 16); section 16 is the cost estimate.

---

### Task 1: Confirm the bmgr and IDF facts on a real run; record baselines

No code change. Produces the reference every later task compares against, in `firmware/build-baseline/` (git-ignored by `firmware/build-*/`).

**Files:**
- Modify: this plan (an "Execution record" section at the end, filled in this task and kept up to date by later tasks)

**Interfaces:**
- Produces: `firmware/build-baseline/<B>.sdkconfig` (fresh sdkconfig per board), `<B>.partition-table.sha256`, `<B>.size` (the `binary size … free` line), `<B>.gen_device_config.c` and `<B>.gen_periph_config.c` (copies of `firmware/components/gen_bmgr_codes/gen_board_{device,periph}_config.c`).

- [ ] **Step 1: Build both boards from main's code** with the firmware build command, each in a build directory that has no `sdkconfig` yet. Expected: both succeed. Copy the five baseline files per board into `firmware/build-baseline/` (`sha256sum firmware/build-$B/partition_table/partition-table.bin`).
- [ ] **Step 2: V1.** Create `boards/common/partitions/` containing an empty `.probe` file, rerun `idf.py bmgr -c ../boards -b waveshare_esp32_s3_touch_amoled_2_16`. Expected: exit 0, no `Skipping invalid board "common"` warning, `components/gen_bmgr_codes/CMakeLists.txt` lists no file under `boards/common/`. Delete `boards/common/` (absolute path inside the worktree).
- [ ] **Step 3: V2.** In the baseline `gen_board_device_config.c`/`gen_board_periph_config.c` of each board, find the values the kit will read and compare them with today's constants: 2.16 `reset_gpio_num` 39, `cs_gpio_num` 12, touch `rst_gpio_num` 40, `int_gpio_num` 11, `pa_peripheral` port 46, `mount_point` `"/sd"`; 1.75C 1, 12, 2, 11, 46. Expected: all present and equal. A missing field is a large deviation: stop and report.
- [ ] **Step 4: V3.** `grep -E 'CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB|CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240' firmware/build-baseline/*.sdkconfig`. Expected: both `=y` in both files (the starting point Task 4 must preserve).
- [ ] **Step 5:** Write the "Execution record" section (V1-V3 confirmed or not, the baseline sizes and % used) and commit `docs(plans): record the H2 bmgr checks and firmware baselines`.

---

### Task 2: Chip adapters live with their drivers

**Files:**
- Create: `drivers/axp2101/include/wisp/drivers/axp2101_power.hpp`, `drivers/axp2101/wisp/axp2101_power.cpp` (from `components/wisp_port_esp/{include/wisp/port/esp/esp_power.hpp,dev/esp_power.cpp}` and `EspPmuPowerKey` from `esp_button.{hpp,cpp}`)
- Create: `drivers/pcf85063/include/wisp/drivers/pcf85063_clock.hpp`, `drivers/pcf85063/wisp/pcf85063_clock.cpp` (from `esp_clock.{hpp,cpp}`)
- Create: `drivers/qmi8658/include/wisp/drivers/qmi8658_imu.hpp`, `drivers/qmi8658/wisp/qmi8658_imu.cpp` (from `esp_imu.{hpp,cpp}`)
- Delete: `components/wisp_port_esp/dev/esp_{power,clock,imu}.cpp`, `components/wisp_port_esp/include/wisp/port/esp/esp_{power,clock,imu}.hpp`
- Modify: `components/wisp_port_esp/include/wisp/port/esp/esp_button.hpp`, `dev/esp_button.cpp` (remove `EspPmuPowerKey`, the `axp2101_conv.h` and `esp_power.hpp` includes)
- Modify: `components/wisp_port_esp/CMakeLists.txt` (SRCS lose the three files; `IDF_REQUIRES` loses `axp2101 pcf85063 qmi8658`; header comment)
- Modify: `drivers/{axp2101,pcf85063,qmi8658}/CMakeLists.txt`
- Create: `boards/waveshare_esp32_s3_touch_amoled_2_16/board.cmake`, `boards/waveshare_esp32_s3_touch_amoled_1_75c/board.cmake`
- Modify: `firmware/CMakeLists.txt`, `firmware/main/CMakeLists.txt`
- Modify: both `boards/*/board.cpp` (new types and includes only; bring-up unchanged)
- Modify: `tools/check_layering.py`, `tools/check_layering_test.py`
- Modify: `AGENTS.md` sections 3 and 5 (separate commit), `docs/porting-guide.md` "Step 2: Add drivers"

**Interfaces:**
- Produces (namespace `wisp::drivers`; each config is an aggregate, boards use designated initialisers):
  ```cpp
  struct Axp2101PowerConfig { std::string id; std::string role; std::string model = "AXP2101"; };
  class Axp2101Power : public hal::Power {
  public:
      static Result<std::shared_ptr<Axp2101Power>> create(i2c_master_bus_handle_t bus, Axp2101PowerConfig config);
      // status(), power_off(), charging_config_status(), enable_power_key_irqs(),
      // take_power_key_irqs(uint8_t*) exactly as EspPower today
  };
  struct Axp2101PowerKeyConfig { std::string id; std::string role; std::string model = "axp2101-pwrkey"; };
  class Axp2101PowerKey : public hal::Button {   // EspPmuPowerKey's behaviour unchanged
  public:
      Axp2101PowerKey(std::shared_ptr<Axp2101Power> power, Axp2101PowerKeyConfig config);
  };
  struct Pcf85063ClockConfig { std::string id; std::string role; std::string model = "PCF85063"; };
  class Pcf85063Clock : public hal::Clock {
  public:
      static Result<std::shared_ptr<Pcf85063Clock>> create(i2c_master_bus_handle_t bus, Pcf85063ClockConfig config);
  };
  struct Qmi8658ImuConfig {
      std::string id; std::string role; std::string model = "QMI8658";
      uint8_t addr = QMI8658_I2C_ADDR_HIGH; hal::ImuAxisMap mount{};
  };
  class Qmi8658Imu : public hal::Imu {
  public:
      static Result<std::shared_ptr<Qmi8658Imu>> create(i2c_master_bus_handle_t bus, Qmi8658ImuConfig config);
  };
  ```
  Each writes `info_.id = config.id` and caps `role` only when `config.role` is non-empty; every other cap stays as today.
- Produces: `boards/<board>/board.cmake` sets `WISP_BOARD_REQUIRES` (both boards: `axp2101 pcf85063 qmi8658`). `firmware/CMakeLists.txt`, after `include(project.cmake)` and before `project()`: `include(${CMAKE_CURRENT_LIST_DIR}/../boards/${WISP_BOARD}/board.cmake)` then `idf_build_set_property(WISP_BOARD_REQUIRES "${WISP_BOARD_REQUIRES}")`. `firmware/main/CMakeLists.txt`: `idf_build_get_property(_wisp_board_requires WISP_BOARD_REQUIRES)` appended to `REQUIRES` (deviation 1).
- Produces in `tools/check_layering.py`: `check_port_requires_no_drivers(root, errors)`, called from `run()`: every name in `components/wisp_port_esp/CMakeLists.txt`'s `REQUIRES` and `IDF_REQUIRES` (parsed with `_extract_wisp_component_call`/`_parse_keyword_lists`) that is a directory under `drivers/` is an error `"<path>: wisp_port_esp must not require the chip driver <name> (AGENTS.md section 3)"`.
- Driver CMake: `SRCS` add `wisp/<chip>_<role>.cpp`; `REQUIRES esp_driver_i2c wisp_hal wisp_os wisp_core` (`axp2101` also `wisp_port_esp`, deviation 3); `target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)` as `firmware/main` does. The C sources and `*_conv.h` stay untouched and usable on their own.

- [ ] **Step 1: Write the failing self-tests** in `tools/check_layering_test.py`:
  ```python
  def test_port_requiring_a_chip_driver_is_flagged(self):
      # scratch tree: drivers/axp2101/, components/wisp_port_esp/CMakeLists.txt with
      # IDF_REQUIRES freertos axp2101 -> one error naming axp2101 and AGENTS.md section 3
  def test_port_requires_without_drivers_pass(self):
      # same tree, IDF_REQUIRES freertos esp_lcd -> no error
  def test_a_requires_name_that_is_not_a_driver_dir_passes(self):
      # drivers/ has only qmi8658; port requires "qmi" -> no error
  ```
- [ ] **Step 2: Run** `python3 tools/check_layering_test.py`. Expected: the first test FAILS (no such check yet).
- [ ] **Step 3: Implement** the check, then move the adapters, update the CMake files and the boards (`Axp2101Power::create(bus, {.id = "power.main", .role = wisp::hal::role::kMain})` and so on; the 1.75C passes `.addr`/`.mount`; its PMU fallback builds an `Axp2101PowerKey`). Update `components/wisp_port_esp/CMakeLists.txt`'s header comment (no chip adapters) and `docs/porting-guide.md` "Step 2" (a driver component carries its Wisp adapter under `wisp/`, header `include/wisp/drivers/<chip>_<role>.hpp`, namespace `wisp::drivers`, `REQUIRES wisp_hal wisp_os wisp_core`; the board lists it in `board.cmake`).
- [ ] **Step 4: Run** `python3 tools/check_layering_test.py && python3 tools/check_layering.py`. Expected: PASS, no errors. Then `grep -rn 'axp2101\|pcf85063\|qmi8658' components/wisp_port_esp`: only comments may remain, none naming a header or component.
- [ ] **Step 5: Firmware build**, both boards. Expected: success; sizes recorded.
- [ ] **Step 6:** `bash tools/ci.sh`; commit `refactor(drivers): chip adapters live with their drivers` (body: sizes).
- [ ] **Step 7:** AGENTS.md section 3 "Layering": after "chip drivers go in `drivers/`" add "; a chip's Wisp adapter lives in its driver component (`drivers/<chip>/wisp/`), and a board names its drivers in `boards/<board>/board.cmake`. `wisp_port_esp` requires no component under `drivers/` (`tools/check_layering.py`)." Section 8 "Common pitfalls": "**Component requirements see no cache variables**: ESP-IDF expands requirements in a separate CMake run, so a component's `REQUIRES` cannot depend on `WISP_BOARD`; publish such lists as build properties before `project()` (`firmware/CMakeLists.txt`, `WISP_BOARD_REQUIRES`)." Commit `docs(agents): drivers carry their adapters; board requirements as a build property`.

---

### Task 3: ESP devices take their ids and roles from the board

**Files:**
- Modify: `components/wisp_port_esp/include/wisp/port/esp/{esp_display,esp_touch,esp_audio,esp_storage,esp_wifi}.hpp` and `dev/esp_{display,touch,audio,storage}.cpp`, `net/esp_wifi.cpp`
- Modify: both `boards/*/board.cpp` (pass today's ids and roles)
- Modify: `tools/check_layering.py`, `tools/check_layering_test.py` (the guard covers `wisp_port_esp`, deviation 9)
- Modify: `docs/porting-guide.md` "Step 3" (the ESP devices' configs carry `id` and `role`)

**Interfaces:**
- Produces (no defaults for `id`/`role`; an empty role writes no caps `role`):
  - `EspDisplayConfig`, `EspTouchConfig`, `EspWifiConfig` gain `std::string id; std::string role;` (`EspWifiConfig::id` loses its `"net.wifi"` default).
  - `struct EspAudioOutConfig { std::string id; std::string role; std::string model; };` and `EspAudioOut::create(esp_codec_dev_handle_t, i2s_chan_handle_t, EspAudioOutConfig)`.
  - `struct EspAudioInConfig { std::string id; std::string role; std::string model; };` and `EspAudioIn::create(esp_codec_dev_handle_t, i2s_chan_handle_t, EspAudioInConfig)` (Task 5 adds `channels`).
  - `EspInternalStorage::create(std::string id, std::string role)`.
  - `struct EspSdStorageConfig { std::string id; std::string role; std::string mount_point; std::string bmgr_device; std::string model; };` and `EspSdStorage(EspSdStorageConfig config, bool already_mounted, sdmmc_card_t* card)`.
- The `wisp/hal/roles.hpp` includes in these port files go; `EspGpioButton` already takes id and role.

- [ ] **Step 1: Write the failing self-test** `test_device_id_literal_in_esp_port_is_flagged` (a scratch `components/wisp_port_esp/dev/x.cpp` with `info_.id = "display.main";` → one error; the same literal in a comment → none).
- [ ] **Step 2: Run** `python3 tools/check_layering_test.py`. Expected: FAIL.
- [ ] **Step 3: Implement**: `check_device_id_literals()` also scans `iter_source_files(root, "wisp_port_esp")`; change the port's devices and the boards. The 2.16 passes `{"display.main", main}`, `{"touch.main", main}`, `{"audio_out.main", main}`, `{"audio_in.main", main}`, `("storage.int", internal)`, `{"storage.sd", removable, "/sd", "fs_sdcard", "sdcard-sdmmc"}`, `{"net.wifi", main}`; the 1.75C the same without the SD card.
- [ ] **Step 4: Run** `python3 tools/check_layering_test.py && python3 tools/check_layering.py`. Expected: PASS, no errors (the port is clean).
- [ ] **Step 5: Firmware build**, both boards; sizes recorded.
- [ ] **Step 6:** `bash tools/ci.sh`; commit `refactor(port): ESP devices take ids and roles from the board`.

---

### Task 4: Target facts; S3 options in `sdkconfig.defaults.esp32s3`

**Files:**
- Create: `components/wisp_port_esp/include/wisp/port/esp/target_facts.hpp` (pure, host-tested), `components/wisp_port_esp/include/wisp/port/esp/target.hpp` (ESP-only)
- Create: `tests/port_esp/target_facts_test.cpp`; Modify: `tests/CMakeLists.txt` (`wisp_test(NAME target_facts_test ...)` with the port's `include/`, as `button_input_test`)
- Create: `firmware/sdkconfig.defaults.esp32s3`; Modify: `firmware/sdkconfig.defaults` (lines 8-16 move out; header says it names no chip)
- Modify: `firmware/main/main.cpp` (`apply_pm_config`), `components/wisp_port_esp/include/wisp/port/esp/esp_wifi.hpp`, `net/esp_wifi.cpp`, both `boards/*/board.cpp` (`init_wifi` stops setting model and bands)
- Modify: `docs/porting-guide.md` "Chip differences" (instruction cache and CPU frequency rows point to `firmware/sdkconfig.defaults.<target>`; new rows for the DFS minimum and the Wi-Fi model/bands in `target_facts.hpp`)

**Interfaces:**
- Produces `target_facts.hpp` (namespace `wisp::port::esp`, no ESP-IDF include):
  ```cpp
  struct TargetFacts {
      std::string_view idf_target;  // CONFIG_IDF_TARGET
      const char* model;            // net.wifi's DeviceInfo::model
      uint32_t dfs_min_mhz;         // esp_pm_config_t::min_freq_mhz
  };
  inline constexpr TargetFacts kTargetFacts[] = {{"esp32s3", "ESP32-S3", 80}};
  constexpr const TargetFacts* find_target_facts(std::string_view idf_target);  // nullptr if unknown
  ```
- Produces `target.hpp`: `inline constexpr const TargetFacts& kTarget = *find_target_facts(CONFIG_IDF_TARGET);` preceded by `static_assert(find_target_facts(CONFIG_IDF_TARGET) != nullptr, "add this target to kTargetFacts in wisp/port/esp/target_facts.hpp")`.
- `EspWifiConfig` loses `model` and `bands`; `EspWifi::create()` sets model `kTarget.model` and caps `bands` `{"2.4GHz"}`, plus `"5GHz"` when `SOC_WIFI_SUPPORT_5G` is defined (`soc/soc_caps.h`).
- `apply_pm_config()`: `cfg.min_freq_mhz = wisp::port::esp::kTarget.dfs_min_mhz;` (the maximum stays `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ`).
- `sdkconfig.defaults.esp32s3` holds `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` and `CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y` with their existing comments, and a header saying ESP-IDF reads it right after `sdkconfig.defaults` for this target, also with an explicit `SDKCONFIG_DEFAULTS` (V3).

- [ ] **Step 1: Write the failing tests**:
  ```cpp
  TEST(TargetFacts, Esp32s3)              // find_target_facts("esp32s3"): model "ESP32-S3", dfs_min_mhz 80
  TEST(TargetFacts, UnknownTargetIsNull)  // "esp32xx" and "" -> nullptr
  TEST(TargetFacts, LookupIsConstexpr)    // static_assert(find_target_facts("esp32s3")->dfs_min_mhz == 80)
  ```
- [ ] **Step 2: Run** `cmake --build build/sim && ctest --test-dir build/sim -R TargetFacts --output-on-failure`. Expected: compile failure, then FAIL.
- [ ] **Step 3: Implement** the headers, Wi-Fi, `main.cpp`, the sdkconfig split and the board edits.
- [ ] **Step 4: Run** the same command. Expected: PASS.
- [ ] **Step 5: Firmware build**, both boards, each in a build directory whose `sdkconfig` was deleted first (absolute path). Then `diff <(grep -v '^#' firmware/build-baseline/$B.sdkconfig) <(grep -v '^#' firmware/build-$B/sdkconfig)`. Expected: no difference for either board (Review Focus 5). `grep -rn 'ESP32S3\|ESP32-S3\|esp32s3' firmware/sdkconfig.defaults` prints nothing.
- [ ] **Step 6:** `bash tools/ci.sh`; commit `feat(port): target facts and S3 options by target` (body: sizes, "fresh sdkconfig identical").

---

### Task 5: Channel roles for audio capture

**Files:**
- Modify: `components/wisp_port_esp/include/wisp/port/esp/audio_downmix.hpp` (replace `downmix_stereo_to_mono`)
- Modify: `tests/port_esp/audio_downmix_test.cpp`
- Modify: `components/wisp_port_esp/include/wisp/port/esp/esp_audio.hpp`, `dev/esp_audio.cpp`
- Modify: both `boards/*/board.cpp` (`.channels = {AudioChannel::Mic, AudioChannel::Mic}`), the two `board_devices.yaml` comments on `audio_adc`
- Modify: `components/wisp_hal/include/wisp/hal/audio.hpp` (AudioIn caps comment), `docs/porting-guide.md` caps table (`AudioIn`: optional `channels`)

**Interfaces:**
- Produces in `audio_downmix.hpp` (still free of ESP-IDF includes):
  ```cpp
  enum class AudioChannel : uint8_t { Mic, Ref };    // Ref: the speaker signal looped back
  const char* to_string(AudioChannel c);             // "mic", "ref" (inline)
  inline constexpr size_t kMaxCaptureChannels = 4;
  // Mean of the Mic channels of each interleaved frame, int32 sum, truncated toward zero.
  // Requires 1..kMaxCaptureChannels roles with at least one Mic; `in` holds frames * roles.size().
  void downmix(const int16_t* in, int16_t* mono, size_t frames, std::span<const AudioChannel> roles);
  // Frames of one esp_codec_dev_read(): min(remaining, scratch_samples / channels).
  constexpr size_t capture_chunk_frames(size_t remaining, size_t scratch_samples, size_t channels);
  ```
- `EspAudioInConfig` gains `std::vector<AudioChannel> channels;`. `create()` returns `InvalidArgument` for an empty list, more than `kMaxCaptureChannels`, or no `Mic`. `open()` uses `channel = n`, `channel_mask = (1u << n) - 1`. `read()` reads `capture_chunk_frames(count - done, kScratchSamples, n)` frames per call into the unchanged `int16_t scratch_[kScratchSamples]` (`kScratchSamples = 960`, today's `kScratchFrames * 2`). Caps gain `channels`: a list of `to_string()` names.

- [ ] **Step 1: Write the failing tests** (the existing stereo cases move to `downmix` with `{Mic, Mic}`, same expected values):
  ```cpp
  TEST(AudioDownmix, TwoMicsKeepTheStereoRule)        // the four existing vectors, unchanged results
  TEST(AudioDownmix, RefChannelsAreSkipped)           // {Mic, Ref}, frames {100, 9999, -50, 7} -> {100, -50}
  TEST(AudioDownmix, ThreeMicsTruncateTowardZero)     // {-1,-1,-2} -> -1; {1,1,2} -> 1
  TEST(AudioDownmix, FourMicsAtTheExtremes)           // 4 x 32767 -> 32767; 4 x -32768 -> -32768
  TEST(AudioDownmix, OneMicIsCopied)                  // {Mic}: {5, -6} -> {5, -6}
  TEST(CaptureChunk, NeverExceedsTheScratch)          // (480, 960, 2) -> 480; (1000, 960, 3) -> 320; (7, 960, 4) -> 7
  ```
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'AudioDownmix|CaptureChunk' --output-on-failure`. Expected: compile failure, then FAIL.
- [ ] **Step 3: Implement** the header, `EspAudioIn` and the board lines.
- [ ] **Step 4: Run** the same filter. Expected: PASS.
- [ ] **Step 5: Firmware build**, both boards; sizes recorded. Read `esp_audio.hpp` once more: `scratch_` is still 960 samples (no internal-RAM change).
- [ ] **Step 6:** `bash tools/ci.sh`; commit `feat(port): audio capture declares channel roles`.

---

### Task 6: The board kit; the 2.16 on a `BoardSpec` with pins from YAML

**Files:**
- Create: `boards/common/board_kit.hpp`, `boards/common/board_kit.cpp`, `boards/common/held_pins.hpp` (pure)
- Create: `tests/boards/held_pins_test.cpp`; Modify: `tests/CMakeLists.txt` (`wisp_test(NAME held_pins_test ...)`, include dir `${WISP_ROOT}/boards/common`)
- Modify: `firmware/main/CMakeLists.txt` (SRCS add `../../boards/common/board_kit.cpp`; `PRIV_INCLUDE_DIRS ../../boards/common`; header comment)
- Rewrite: `boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp` (a `BoardSpec` constant, `bring_up_chips()`, `create_board()`)
- Modify: `boards/waveshare_esp32_s3_touch_amoled_2_16/board_peripherals.yaml` (buttons), `board_devices.yaml` (touch `levels`/`flags`), `setup_device.c` (touch factory passes the config through), `README.md`
- Modify: `docs/porting-guide.md` "Overview" and "Step 3" (the kit, `BoardSpec`, buttons as bmgr `gpio` peripherals, `bring_up_chips`, what stays in `setup_device.c`), `docs/architecture.md` (boot flow step 2: the board's `init()` runs the kit's sequence)
- Modify: `AGENTS.md` (separate commit)

**Interfaces:**
- Produces `held_pins.hpp` (namespace `wisp::board_kit`, standard headers only):
  ```cpp
  inline constexpr size_t kMaxHeldPins = 8;
  struct HeldPins {
      std::array<int, kMaxHeldPins> pins{};
      size_t count = 0;
      bool add(int pin);  // false and unchanged for pin < 0, a pin already held, or a full list
  };
  ```
- Produces `board_kit.hpp` (namespace `wisp::board_kit`; ESP-only, may include bmgr and port headers):
  ```cpp
  struct Slot { const char* id; hal::DeviceClass cls; const char* role; };
  class Kit;
  struct DisplaySpec {
      Slot slot{"display.main", hal::DeviceClass::Display, hal::role::kMain};
      const char* bmgr = "display_lcd";
      const char* model; const char* shape; int corner_radius = 0; const char* panel;
      int area_align = 1;
      port::esp::EspDisplayConfig::PanelSleep sleep = port::esp::EspDisplayConfig::PanelSleep::VendorDispSleep;
      uint32_t sleep_out_settle_ms = 120;
  };
  struct TouchSpec    { Slot slot{"touch.main", hal::DeviceClass::Touch, hal::role::kMain}; const char* bmgr = "lcd_touch"; const char* model; };
  struct AudioOutSpec { Slot slot{"audio_out.main", hal::DeviceClass::AudioOut, hal::role::kMain}; const char* bmgr = "audio_dac"; const char* model; };
  struct AudioInSpec  { Slot slot{"audio_in.main", hal::DeviceClass::AudioIn, hal::role::kMain}; const char* bmgr = "audio_adc"; const char* model;
                        std::span<const port::esp::AudioChannel> channels; };
  struct SdSpec       { Slot slot{"storage.sd", hal::DeviceClass::Storage, hal::role::kRemovable}; const char* bmgr = "fs_sdcard"; const char* model; };
  struct ButtonSpec {
      Slot slot; const char* bmgr_gpio; bool active_low; const char* model = "gpio-button";
      hal::ButtonInputMode mode = hal::ButtonInputMode::EdgeEvent;
      // Used when the GPIO path is unavailable; when set, that path also needs the edge service.
      Result<std::shared_ptr<hal::Device>> (*fallback)(Kit&, const ButtonSpec&) = nullptr;
  };
  struct BoardSpec {
      const char* name;
      std::optional<DisplaySpec> display; std::optional<TouchSpec> touch;
      std::optional<AudioOutSpec> audio_out; std::optional<AudioInSpec> audio_in;
      std::optional<SdSpec> sd;
      std::span<const ButtonSpec> buttons;
      std::span<const Slot> chips;           // devices bring_up_chips() registers, for the bmgr-failure records
      void (*bring_up_chips)(Kit&) = nullptr;
      Slot internal_storage{"storage.int", hal::DeviceClass::Storage, hal::role::kInternal};
      Slot wifi{"net.wifi", hal::DeviceClass::Network, hal::role::kMain};
  };
  class Kit {
  public:
      Kit(core::DeviceRegistry& registry, core::BootReport& report);
      i2c_master_bus_handle_t i2c(const char* bmgr_periph);   // null (logged) when bmgr has none
      template <class D> bool add(const Slot& slot, Result<std::shared_ptr<D>> made);  // false: recorded failed
      void missing(const Slot& slot, std::string reason);     // DeviceState::Missing
      void failed(const Slot& slot, std::string reason);      // DeviceState::Failed + WLOG_E
      core::DeviceRegistry& registry();
  };
  void init(const BoardSpec& spec, core::DeviceRegistry& registry, core::BootReport& report);
  std::unique_ptr<app::Board> make_board(const BoardSpec& spec);  // name() = spec.name, init() = init(spec, ...)
  ```
- `init()` sequence (today's, `2_16/board.cpp:565-618`): internal storage (before bmgr); `esp_board_manager_init()`, on failure record every declared slot (display, touch, chips, buttons, audio out/in, sd) failed with `"esp_board_manager_init: <err>"`, start Wi-Fi and return; display; touch; `bring_up_chips`; RTC sync (`by_role<hal::Clock>(role::kMain)`, the body of today's `sync_system_clock_from_rtc`); buttons; audio out, audio in; SD card; hold outputs; Wi-Fi. Failure messages keep today's wording (`"bmgr display_lcd handle: <err>"`, `"registry.add: <msg>"`, …).
- Pins: display reset and CS from the display's device config (SPI sub-type only; other sub-types hold nothing for the panel), touch reset from the touch config, PA from the `audio_out` device config's `pa_peripheral.port`, collected in that order into a `HeldPins`, each held with `gpio_sleep_sel_dis()`; one `WLOG_I(kTag, "held through light sleep: GPIO39 GPIO40 GPIO46 GPIO12")` line lists them. Device configs are bmgr's static tables, so the list does not depend on which devices came up. SD mount point from `dev_fs_fat_config_t::mount_point`.
- Buttons: the pin is the `gpio_num` of `esp_board_manager_get_periph_config(bmgr_gpio)`; `configured` is "`esp_board_manager_get_periph_handle(bmgr_gpio)` succeeded"; with a `fallback`, `edge_ready` is `EspGpioButton::prepare_edge_input()` and the choice is `power_button_use_gpio(true, configured, edge_ready)`; without one, a key that is not configured is recorded failed `"bmgr <name>: <err>"`. A fallback's error is recorded failed with its message.
- The 2.16's spec: display `{.model = "CO5300", .shape = "rounded_rect", .corner_radius = 70, .panel = "amoled", .area_align = 2, .sleep = DcsSleep, .sleep_out_settle_ms = 120}` (the comments on 70 and DCS sleep move with the values), touch `"CST9217"`, audio `"ES8311"`/`"ES7210"` with `{Mic, Mic}`, SD `"sdcard-sdmmc"`, buttons `{button.power, btn_power, active_low=false, model "axp2101-pwrkey", role power}`, `{button.vol_up, btn_vol_up, true}`, `{button.vol_down, btn_vol_down, true, role talk}`, chips `power.main`, `clock.main`, `imu.main`; `bring_up_chips` = PMU (with today's charging-config warning), RTC, IMU through the Task 2 adapters on `kit.i2c("i2c_master")`.
- YAML: `btn_power` (pin 16, input, `pull_down: true`), `btn_vol_up` (18, `pull_up: true`), `btn_vol_down` (0, `pull_up: true`), each `type: gpio`, `role: io`, `mode: GPIO_MODE_INPUT`. `lcd_touch.config.touch_config` gains `levels: {reset: 0, interrupt: 0}` and `flags: {swap_xy: true, mirror_y: true}`.

- [ ] **Step 1: Write the failing tests** `tests/boards/held_pins_test.cpp`:
  ```cpp
  TEST(HeldPins, KeepsOrderAndSkipsNegativesAndDuplicates)
  // add 39, 40, -1, 46, 12, 39 -> pins {39, 40, 46, 12}, count 4; add(-1) and the second add(39) return false
  TEST(HeldPins, TheOneSeventyFiveCList)   // 1, 2, 46, 12 -> {1, 2, 46, 12}
  TEST(HeldPins, IsBounded)                // 9 distinct pins -> count 8, the 9th add returns false
  ```
- [ ] **Step 2: Run** `cmake -S simulator -B build/sim && cmake --build build/sim && ctest --test-dir build/sim -R HeldPins --output-on-failure`. Expected: compile failure, then FAIL.
- [ ] **Step 3: Implement** `held_pins.hpp`; run the filter. Expected: PASS.
- [ ] **Step 4: Implement** the kit, the 2.16's `board.cpp`, YAML and `setup_device.c`; delete the constants `kPin*`, `kSdMountPoint` and the per-device functions from the 2.16's file. Review the kit against the old file side by side: every `record_failed` reason and every `report.record(... Missing ...)` of the old file has its counterpart; the bmgr-failure path records exactly the slots the old block did (`2_16/board.cpp:580-592`).
- [ ] **Step 5: Firmware build**, 2.16 (the 1.75C still builds with its old `board.cpp`: build it too). Then check the generated tables: `grep -n 'btn_power\|btn_vol' firmware/components/gen_bmgr_codes/gen_board_periph_config.c` shows pins 16, 18, 0 with the pulls above, and `gen_board_device_config.c`'s touch config has `swap_xy = true`, `mirror_y = true`. Expected: as stated; sizes recorded (the 2.16 should not grow).
- [ ] **Step 6:** `bash tools/ci.sh`; commit `feat(boards): board kit; the 2.16 on a BoardSpec with pins from YAML`.
- [ ] **Step 7:** AGENTS.md section 3 "Layering": "Shared board bring-up is the board kit (`boards/common/`, a `BoardSpec` per board); pins live only in the bmgr YAML." Section 8, the bmgr pitfall: "`boards/common/` is not a board (no YAML), so bmgr neither treats it as one nor compiles it; `firmware/main` compiles the kit." Commit `docs(agents): the board kit and pins from YAML`.

---

### Task 7: The 1.75C on the board kit

**Files:**
- Rewrite: `boards/waveshare_esp32_s3_touch_amoled_1_75c/board.cpp`
- Modify: its `board_peripherals.yaml` (buttons), `board_devices.yaml` (touch `levels`/`flags`), `setup_device.c` (drop `TOUCH_*` overrides; keep the panel table and the column gap), `README.md`
- Modify: `docs/hardware/waveshare_esp32_s3_touch_amoled_1_75c.md` (buttons are bmgr peripherals; PWR selection now through `ButtonSpec::fallback`)

**Interfaces:**
- Consumes: Task 6's `BoardSpec`, `Kit`, `ButtonSpec::fallback`; Task 2's adapters.
- The spec: display `{.model = "CO5300", .shape = "circle", .panel = "amoled", .area_align = 2, .sleep = DcsSleep, .sleep_out_settle_ms = 120}`, touch `"CST9217"`, audio as the 2.16, no SD; buttons `{button.power, btn_power, active_low=false, "gpio-button", role power, .fallback = pmu_power_key}`, `{button.boot, btn_boot, true, role talk}`; chips `power.main`, `clock.main`, `imu.main`.
- `bring_up_chips`: PMU (kept in a file-scope `std::shared_ptr<wisp::drivers::Axp2101Power>` for the fallback); RTC probed at 0x51 (`ESP_ERR_NOT_FOUND` → `kit.missing(slot, "no RTC")`, other errors failed `"i2c probe: <err>"`); IMU probed at 0x6B then 0x6A (none → failed `"no answer at 0x6B or 0x6A"`), created with `kImuMount`.
- `pmu_power_key(Kit&, const ButtonSpec&)`: without the PMU, `Result::err(DeviceFailure, "power.main unavailable")`; otherwise logs today's PMU-fallback warning and returns an `Axp2101PowerKey` with the slot's id and role.
- YAML: `btn_power` (pin 3, input, no pulls: SYS_OUT has its external network), `btn_boot` (pin 0, `pull_up: true`); touch `levels: {reset: 0, interrupt: 0}`, `flags: {swap_xy: false, mirror_x: true, mirror_y: true}`.

- [ ] **Step 1: Implement**; review the old file against the new one as in Task 6 Step 4 (the bmgr-failure path records the old block's slots, `1_75c/board.cpp:480-493`).
- [ ] **Step 2: Firmware build**, both boards. Generated tables: pins 3 and 0 with the pulls above; touch `mirror_x = true`, `mirror_y = true`. `wc -l boards/*/board.cpp`: both well under today's 629/522 lines (spec 7.2 estimates about 60 and 120). Sizes recorded.
- [ ] **Step 3:** `bash tools/ci.sh`; commit `refactor(boards): the 1.75C on the board kit`.

---

### Task 8: Partition tables shared by flash size

**Files:**
- Move: `boards/waveshare_esp32_s3_touch_amoled_2_16/partitions.csv` → `boards/common/partitions/16mb.csv`, `boards/waveshare_esp32_s3_touch_amoled_1_75c/partitions.csv` → `boards/common/partitions/32mb.csv` (`git mv`, contents byte for byte)
- Modify: both `sdkconfig.defaults.board` (`CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="../boards/common/partitions/16mb.csv"` / `32mb.csv`), both board `README.md`
- Modify: `docs/porting-guide.md` "Partitions and board configuration" (shared tables by flash size; a shipped table is frozen, a change is a new file; a board with special needs keeps its own `partitions.csv`), both `docs/hardware/*.md` partition lines, `tools/pack_font_image_test.py:23`, `tools/pack_glyph_image_test.py:28` (comments), `components/wisp_port_esp/dev/esp_storage.cpp:32`, `include/wisp/port/esp/esp_storage.hpp:7`, `esp_font_partition.hpp:7` (comments naming the table's place)

- [ ] **Step 1:** Move and edit. `git diff -M --stat` shows two renames at 100 % similarity.
- [ ] **Step 2: Firmware build**, both boards, with each `build-$B/sdkconfig` deleted first (the defaults changed). Expected: `sha256sum firmware/build-$B/partition_table/partition-table.bin` equals Task 1's for both boards; sizes unchanged.
- [ ] **Step 3:** `bash tools/ci.sh` (the font/glyph tool tests still pass); commit `refactor(boards): partition tables shared by flash size`.

---

### Task 9: The boot report records a PSRAM below 8 MB

**Files:**
- Modify: `components/wisp_app/include/wisp/app/app.hpp` (`Platform::psram_bytes`), `components/wisp_app/include/wisp/app/ram_budget.hpp` (`kMinPsramBytes`), `components/wisp_app/src/app.cpp`
- Modify: `firmware/main/main.cpp`
- Test: `tests/app/app_boot_test.cpp` (`MakeAppOptions::psram_bytes`)
- Modify: `docs/architecture.md` "Machine-readable output" (the `psram` note), `docs/porting-guide.md` "Scene runtime" (replace the wrong "fall back to internal RAM" sentence: PSRAM of 8 MB or more is required, the Lua heap uses `Mem::Large` only, with less the boot report records `psram` and nothing else is promised)

**Interfaces:**
- `Platform`: `int64_t psram_bytes = -1;` — the PSRAM chip's size; -1 not measured (the simulator), 0 none.
- `ram_budget.hpp`: `inline constexpr int64_t kMinPsramBytes = 8 * 1024 * 1024;`
- `App::start()`, right after the board's devices are recorded (step 2): when `0 <= psram_bytes < kMinPsramBytes`, `report.set_note("psram", psram_bytes == 0 ? "none" : std::to_string(psram_bytes >> 20) + "MB")` and `WLOG_E("app", "PSRAM %s: Wisp needs 8 MB or more", <same text>)`. Boot continues.
- `main.cpp`: `platform.psram_bytes = esp_psram_is_initialized() ? static_cast<int64_t>(esp_psram_get_size()) : 0;` (`esp_psram.h`; `firmware/main` adds `esp_psram` to `REQUIRES`).

- [ ] **Step 1: Write the failing tests**:
  ```cpp
  TEST(AppBoot, NoPsramIsNoted)            // psram_bytes 0 -> notes.psram == "none"; boot ok
  TEST(AppBoot, SmallPsramIsNoted)         // 4 MiB -> "4MB"; 8 MiB - 1 -> "7MB"
  TEST(AppBoot, EightMegabytesHasNoNote)   // 8 MiB and 16 MiB -> no "psram" key
  TEST(AppBoot, UnmeasuredPsramHasNoNote)  // default -1 -> no "psram" key
  ```
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R AppBoot --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the filter, then the whole suite. Expected: PASS; `tests/smoke/sim_smoke.py` (via `ci.sh`) unchanged.
- [ ] **Step 5: Firmware build**, both boards; sizes recorded.
- [ ] **Step 6:** `bash tools/ci.sh`; commit `feat(app): the boot report records a PSRAM below 8 MB`.

---

### Task 10: Porting guide, documents and the changelog

**Files:**
- Modify: `docs/porting-guide.md`: "Overview" lists what a board directory holds after this phase (spec 7.4); a "Support matrix" section with spec 8.4's table (image 3.6 MB → the current measured size from Task 1, 80 % slot budget); "Chip differences" ends with spec 8.5's list of what a new target touches (`firmware/sdkconfig.defaults.<target>`, `lv_blend/<target>/`, `stack_placement.hpp`, `target_facts.hpp`, the IDF range, this section); "Step 4: Build" mentions `board.cmake`; the actuator guideline (H1) is checked to still match spec 3.5.
- Modify: `docs/architecture.md` (layering: drivers carry adapters, the kit in `boards/common/`), `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md` (pins in YAML, buttons as peripherals)
- Modify: `docs/superpowers/specs/2026-11-14-device-model-design.md`: "In the code" notes at 6.1 (deviation 3), 6.2 (deviation 4), 6.3 (deviation 1), 7.2 (deviations 6, 7), 7.3 (deviation 8), 8.3 (deviation 2), 2.4 (deviation 9), 13 (H2 done)
- Modify: `CHANGELOG.md`: an `## Unreleased` section at the top (create it if absent) with: chip adapters in `drivers/`; the board kit and `BoardSpec`; pins only in YAML; S3 options in `sdkconfig.defaults.esp32s3`; shared partition tables; the `psram` boot note; `AudioIn` caps `channels`
- Modify: `docs/backlog.md`: the device checks below until the owner has run them; the S31 facts of spec 17.1

- [ ] **Step 1:** Edit the documents.
- [ ] **Step 2:** `bash tools/ci.sh`. Expected: exit 0.
- [ ] **Step 3:** Commit `docs: board kit, target readiness and the support matrix`.

---

## Device checks for the owner (not run by the agent)

On both boards, flashed with `idf.py -p <port> flash` from the last commit (the partition table file moved but is byte-identical, so `app-flash` also works); check the MAC before flashing, one board connected at a time.

1. `WISP_BOOT_REPORT` and `WISP_MANIFEST`: the same devices, ids, classes, models and caps as before H2, plus `channels: ["mic","mic"]` on `audio_in.main`; no new `failed`; no `psram` note; on the 1.75C `clock.main` still `missing` ("no RTC").
2. Hardware diagnostics (`diag`): every device green; touch, button and microphone tests pass; touch follows the finger in all four corners (orientation now from YAML).
3. Voice: hold the talk key (2.16 Vol-, 1.75C BOOT), speak, hear the answer.
4. Standby and wake: power key short press turns the screen off and on; the power key wakes from deep standby.
5. Light sleep: the boot log shows `held through light sleep: GPIO39 GPIO40 GPIO46 GPIO12` (2.16) / `GPIO1 GPIO2 GPIO46 GPIO12` (1.75C); after 10 minutes of standby, wake shows no panel re-init and the speaker does not pop (2.16 checklist item 29).
6. `mem` idle and the worst case of AGENTS.md section 5: unchanged against `wisp/app/ram_budget.hpp`.
7. `diag` then `bench scroll 10`: `WISP_PERF` frame rate at the boards' baselines (the 32 KB instruction cache still applies).
8. OTA: upload `build-$B/wisp.bin` on the admin "System" page, confirm; the device runs it.
9. 1.75C: `button.power` is `gpio-button` on GPIO3; IMU orientation unchanged (tilt test in diagnostics).

## Execution record

(Filled in by Task 1 and kept current by later tasks: V1-V3 results, per-board image size and % of the smallest app slot after each firmware-touching task.)

### Task 1: bmgr/IDF checks and baselines (main's code)

Both boards were built from a fresh `build-<B>/sdkconfig`; the baseline files are in `firmware/build-baseline/` (git-ignored). A fresh checkout needs `idf.py set-target esp32s3` before `idf.py bmgr` (the `bmgr` subcommand only exists once the component manager has fetched esp_board_manager); that command rewrites `firmware/dependencies.lock` (esp_boards 0.7.2 to 0.7.3, drops esp_io_expander), so the lock was restored with `git checkout` and is not part of any commit.

- **V1 confirmed.** With `boards/common/partitions/.probe` present, `idf.py bmgr -c ../boards -b waveshare_esp32_s3_touch_amoled_2_16` exits 0, prints no `Skipping invalid board` (or any "invalid board") warning, and `components/gen_bmgr_codes/CMakeLists.txt` lists nothing under `boards/common/`.
- **V2 confirmed.** The generated configs hold every value the kit will read:
  - 2.16: display `reset_gpio_num = 39`, `cs_gpio_num = 12`; touch `rst_gpio_num = 40`, `int_gpio_num = 11`; audio `pa_peripheral.port = 46`; SD `mount_point = "/sd"`; the periph config also has a `gpio` peripheral with `gpio_num = 46`.
  - 1.75C: display `reset_gpio_num = 1`, `cs_gpio_num = 12`; touch `rst_gpio_num = 2`, `int_gpio_num = 11`; `pa_peripheral.port = 46`; no SD device (as expected).
- **V3 confirmed.** `CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y` and `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` in both baseline sdkconfigs.
- **Baselines.**

| Board | Image (`wisp.bin`) | Smallest app slot | Used | Partition table sha256 |
|---|---|---|---|---|
| 2.16 | 0x38d580 (3,724,672 B) | 0x480000 | 78.9% (21% free) | `b36c4bd8a170942f37688a3bf5cee8c1420f32b52ae4949b9e56696c2f1c659a` |
| 1.75C | 0x37cb70 (3,656,560 B) | 0x5f0000 | 58.7% (41% free) | `5fa9b1c82c2cfe7fc2dc8643b7a9dd8247d4f1c75d94459fc631c90bc9f2cba2` |

