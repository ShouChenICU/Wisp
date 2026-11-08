# Porting guide: adding a development board

This document describes what it takes to add an ESP32 development board on top of the Wisp foundation (sub-project 1). The reference implementation is `boards/waveshare_esp32_s3_touch_amoled_2_16/`; for how to write the hardware document, see `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`.

## Overview

A board consists of the following parts:

1. **esp_board_manager YAML** (`boards/<board>/board_info.yaml`, `board_peripherals.yaml`, `board_devices.yaml`): describes the board model, the buses (I2C/I2S/SPI/GPIO) and the devices esp_board_manager already supports (display, touch, audio codec, SD card and so on). It only connects the chips to the buses and assigns pins; it does **not** fill in Wisp's capability list.
2. **`setup_device.c`** (optional; needed only when the board has a device type that esp_board_manager has no built-in factory function for): gives bmgr the constructor entry point for the custom device.
3. **`board.cpp`**: implements `wisp::board::create_board()`, which returns a `wisp::app::Board`. Its `init()` wraps the device handles bmgr has already created (display, touch, audio, SD card) in the `wisp::hal::Device` interface, brings up the devices bmgr does not manage (IMU, PMU, RTC, buttons and so on) with Wisp's own drivers (through the buses bmgr exposes), and registers all of them in `DeviceRegistry`.
4. **`partitions.csv` and `sdkconfig.defaults.board`**: the board's partition table and board-level sdkconfig (see "Partitions and board configuration" and "Firmware and partitions" below).

## Step 1: Write the bmgr YAML

1. Create `boards/<board>/` and write the three YAML files, following the esp_board_manager documentation and the fields of an existing board (`boards/waveshare_esp32_s3_touch_amoled_2_16/`). When you can reuse another public board definition directly (such as ESP-Claw), state its source in the file header and register it in `docs/references.md` and `THIRD_PARTY_NOTICES.md`, as AGENTS.md section 6 requires.
2. Declare in the YAML only device types that esp_board_manager supports natively (`display_lcd`, `lcd_touch`, `audio_codec`, `fs_fat`/SDMMC, bare GPIO/I2C/I2S/SPI buses and so on). Do **not** put custom chips such as the IMU, PMU or RTC into the YAML as `custom` devices: Wisp initialises them with its own drivers in `board.cpp`, on the same bus that bmgr exposes.
3. Generate the code with `idf.py bmgr -c ../boards -b <board>` (the output goes to `firmware/components/gen_bmgr_codes/`, which is git-ignored; rerun this step after changing the YAML; do not edit the generated code by hand).

### Board naming rules

`<board>` is the directory name of `boards/<board>/`. It is also the `board` field of `board_info.yaml`, `WISP_BOARD` in the firmware, the return value of `Board::name()` and the `board` field of `WISP_MANIFEST`. All four must match exactly.

- The format is `<vendor>_<official model>`: all lower case, with every `-`, `.` and space in the official model replaced by `_`. The whole name may contain only `[a-z0-9_]`.
- Keep the official model as it is, including the chip model and size/version numbers. Do not abbreviate.
- Espressif's own boards use the names from their `esp_boards` component directly, with no vendor prefix, for example `esp32_s3_box_3` or `esp32_s31_function_coreboard_1`.
- When the same model has an incompatible hardware revision, add a suffix such as `_v2` to tell them apart.
- At most 39 characters, the limit of the firmware's board name descriptor.
- Example: Waveshare ESP32-S3-Touch-AMOLED-2.16 → `waveshare_esp32_s3_touch_amoled_2_16`.

Renaming a board changes the board name descriptor in the image: a device running under the old name judges a new image uploaded through the web page to be `wrong_board` and rejects it, so the first flash after a rename must go over USB.

## Step 2: Add drivers (`drivers/`)

For a chip that the esp_board_manager component library does not have, create a plain-C ESP-IDF component under `drivers/<chip>/`. Its CMakeLists uses `idf_component_register(...)` directly, not `wisp_component()` from `cmake/wisp_component.cmake`: that macro exists so that the portable layers (`wisp_base`/`os`/`hal`/`core`/`ui`/`app`) work under both the `idf.py` build and the host simulator build, while a driver is compiled only in the `idf.py` firmware build and uses ESP-IDF's `driver/*` headers directly, so it does not need that dual-build switch. If the MyOS project (MIT, by ShouChen) already has a driver for the chip, port it first: keep the register sequences and conversion formulas, and remove MyOS-specific logic that is irrelevant to the board, such as sleep and power-rail control. State "Ported from MyOS (MIT, Copyright (c) 2026 ShouChen)" in the file header and register it in `docs/references.md`. A completely new register configuration (a feature MyOS never drove, such as the gyroscope) must say "newly written, not verified" in the file header, and get an item in the device acceptance checklist that checks it specifically.

## Step 3: Write `board.cpp`

Implement `wisp::app::Board` (`components/wisp_app/include/wisp/app/board.hpp`):

```cpp
class Board : public wisp::app::Board {
public:
    std::string name() const override { return "<board>"; }
    void init(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) override {
        // 1. Take the handles bmgr has already created (display/touch/audio/SD), wrap each in a hal::Device subclass, add() it to registry.
        // 2. With the drivers in drivers/, initialise the IMU/PMU/RTC/buttons on the I2C bus bmgr exposes, add() them to registry.
        // 3. If the chip has built-in Wi-Fi: wisp::port::esp::EspWifi::create() registers net.wifi. Do it last (after the display,
        //    so that the Wi-Fi driver does not first take the internal RAM the display DMA buffers need), and also call it on the
        //    early-return path when bmgr fails: the radio being on is what lets Platform::random (esp_fill_random) give true random numbers.
        // 4. If any step fails: report.record({id, class, DeviceState::Failed, "reason"}), do not add() that device,
        //    and do not let init() itself fail or abort -- a missing device only degrades the system, it does not block boot (AGENTS.md section 3 "Device model").
    }
};

namespace wisp::board {
std::unique_ptr<wisp::app::Board> create_board() { return std::make_unique</* the class above */>(); }
}
```

After `init()`, `App::start()` automatically adds a `DeviceState::Ok` for every device in the registry that has not been reported, so `init()` only needs to record failures and missing devices.

**Device id convention**: `<class>.<name>`, where the class is the lower-case name matching `wisp::hal::DeviceClass` (`display`/`touch`/`imu`/`power`/`clock`/`button`/`audio_out`/`audio_in`/`storage`/`network`). The exception is the Wi-Fi device, which sub-project 2's spec fixes as `net.wifi` (class `network`). The ids used on the reference board: `display.main`, `touch.main`, `imu.main`, `power.main`, `clock.main`, `button.power`/`button.vol_up`/`button.vol_down`, `audio_out.main`, `audio_in.main`, `storage.int`, `storage.sd`. New boards should reuse the same set of ids, unless several instances really need to be told apart (such as a future `display.external`). A key without a volume label gets its own name instead of borrowing a volume id: a BOOT key that is not also a volume key is `button.boot` (as on `waveshare_esp32_s3_touch_amoled_1_75c`). Buttons are found by caps (`role`), not by id, so this costs nothing. A board without an RTC records `clock.main` as `missing` ("no RTC") rather than `failed`; a board without an SD slot registers no `storage.sd`.

**Capability field (caps) convention**: each device fills in its own `DeviceInfo::caps` when it is constructed. This is the only source of capability information (the board YAML handles only pins and buses). Required fields per class:

| Class | Required caps |
|---|---|
| `Display` | `width`, `height`, `shape` (`rect`/`rounded_rect`/`circle`), `corner_radius` (for `rounded_rect`), `panel`, `brightness`, `area_align`; optional `rgb565_be` (true when the panel wants RGB565 high byte first; the UI renders directly in that byte order); optional draw-buffer policy, which defaults to two 24-line buffers in internal DMA RAM with partial refresh: `draw_buf_lines` (lines per buffer, default 24, capped at the screen height), `draw_buf_mem` (`"dma"`, the default, is internal DMA RAM; `"large"` is DMA-readable PSRAM, i.e. `Mem::DmaLarge`, declared only when the chip's DMA can read PSRAM and the display driver can also send from PSRAM; falls back to `"dma"` when the allocation fails), `render_mode` (only `"partial"` is implemented; `"full"`/`"direct"` are reserved names that currently log a warning and are treated as `"partial"`). An invalid value logs a warning and uses the default, without blocking boot (`wisp/hal/draw_buf_policy.hpp`). The ESP port publishes these through the fields of the same names in `EspDisplayConfig` |
| `Touch` | `max_points` |
| `Imu` | `accel`, `gyro`, `magnetometer`, `max_rate_hz` |
| `Power` | `battery`, `charging_status` |
| `Storage` | `removable`, `fs` |
| `Network` | `ap` (bool, whether the hotspot `start_ap()` is supported; when false, `WifiService` does not open the setup hotspot), `bands` (list of bands, such as `["2.4GHz"]`) |
| `Button` | Nothing required; the talk key adds `role: "talk"` (see "Voice and the talk key" below) |
| `AudioIn`/`AudioOut` | Nothing required; those that turn off with the power level add `standby: "off"` |

In addition, every device that switches on and off with the power level declares `standby` (see "Standby and power levels" below).

You do not compute the `Display` `safe_area` yourself: `DeviceRegistry::manifest()` derives it from `shape`/`corner_radius`/`width`/`height` with `wisp::hal::safe_rect()`, and the device only fills in `shape`. A missing or unrecognised `shape` is treated as `circle` (the most conservative) with a warning. For `rounded_rect`/`circle`, `safe_rect()` leaves a fixed margin of 4 px beyond the exact visible boundary (`kSafeCornerMargin`, `wisp/hal/display_geometry.cpp`), so that text placed right against a rounded corner does not lose its first glyph to the real panel.

Do not copy `corner_radius` straight from the vendor datasheet: the measured value is often larger (the waveshare_esp32_s3_touch_amoled_2_16 datasheet says 40; the measured value is about 70). When you bring up a new board with a rounded or round screen, write a calibration scene that draws quarter-circle arcs of a few candidate radii (for example 40/50/60/70/80) in the four corners, centred at `(R,R)`. Check by eye which arcs are visible along the diagonal and which have been clipped, and use the largest radius you can see as `corner_radius`. For the details, see the "Known errata and notes" section of `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`.

## Scene runtime

Bringing a new board onto the scene runtime (scene spec 2.1/2.3) needs no board-level code: the scene layer (`wisp_scene`) and the `wisp_ui`/`wisp_hal` it calls are all portable layers, and the screen shape takes effect only through the Display `shape`/`corner_radius` in "Capability field (caps) convention" above. `wisp.device.info()`/`wisp.device.row_span()` read these two caps directly; they go by the shape, not the board name.

Prerequisites (already in the shared configuration; new boards do not change them):

- PSRAM: the Lua heap (the allocator in `components/wisp_scene/src/vm.cpp`) and the LVGL heap (`components/wisp_ui/src/lv_mem_wisp.cpp`) are both on `os::alloc(Mem::Large)`, and fall back to internal RAM without PSRAM (much smaller; a few example scenes running together probably will not fit; see the internal RAM budget in AGENTS.md section 5).
- `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y` is already in `firmware/sdkconfig.defaults`; putting the 128 KB stack of `wisp.scene` into PSRAM depends on it.

For acceptance, besides the "Scenes card" item of the hardware diagnostics checklist in Step 5 below, also run the scene-related rows of the board hardware document's "Device acceptance checklist" (following `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`).

## Voice and the talk key

The voice pipeline (mvp spec 4, `wisp_voice`) and the talk key (`shell::TalkKey`) are both portable layers. A new board only registers its devices by the conventions below and writes no voice code:

- **Talk key**: add `role: "talk"` to the caps of the button used for push-to-talk (`wisp/hal/button.hpp`). The shell finds the key by this role with `shell::find_talk_button()`; it goes by the caps, not the id or the pin. On the reference board it is the BOOT key (`button.vol_down`); in the simulator it is also `button.vol_down`. Only push-to-talk is supported: holding for 300 ms (`voice::kHoldStartMs`) starts recording, and releasing sends it. No VAD runs while the key is held; anything recorded for more than 300 ms goes to recognition, and an empty recognition result shows "Didn't catch that" (没听清). A short press does nothing. A board that declares no `role: "talk"` still works; the only way to talk is then a long press on the wisp on the home screen.
- **`audio_in.main`**: `open()` must be able to deliver recording at `AudioFormat{16000, 1}` (16 kHz, mono, 16-bit) (`voice::kSampleRate`). If the hardware only does stereo or another sample rate, downmix and resample inside your `hal::AudioIn` implementation; do not leave that to the upper layers. A board without a microphone does not register it, and voice degrades to "Microphone unavailable" (麦克风不可用) while web conversation works as usual.
- **`audio_out.main`**: `open()` must be able to open mono at the sample rate of the TTS profile (the `tts` profile's `sample_rate`, 8000–48000, default 24000). The same output is also used for alert tones (16 kHz), so after each `close()` it must be possible to `open()` it again at another sample rate. `voice::PcmPlayer` and `core::TonePlayer` guarantee they never open it at the same time.
- When the caps of these three devices declare `standby`, recording and playback are impossible while the screen is off. A voice turn holds a `screen` wake lock, so normal use is not affected.

## Standby and power levels

Power has four levels: `active`, `dim`, `standby` and `deep_standby` (scene device spec 4.1/4.2). `app::PowerManager` does not handle devices by name: when the level changes, it walks `DeviceRegistry` and calls `hal::Device::set_power(PowerLevel)` on each device, and each device acts according to the `standby` in its own caps. What a new board's devices need:

- **caps `standby`**: `"off"` (turned off at `standby` and below, for example the display, touch, IMU and audio), `"throttle"` (kept running at a lower rate) or `"wake_source"` (always kept on, for waking, for example the power and volume keys). With no value or any other value, the device is not managed by the level and keeps running. `hal::powered_at(policy, level)` tells whether a policy is powered at a given level.
- **`set_power(PowerLevel)`**: devices that declare `standby` override it. Calling it again with the same state must not resend hardware commands. The base class returns `Unsupported` by default, and `PowerManager` ignores such devices.
- **Wi-Fi radio**: at `deep_standby`, `WifiService` first turns off the hotspot and disconnects, then calls `hal::Wifi::set_radio(false)`; on leaving it calls `set_radio(true)` and reconnects immediately. In the ESP port, `EspWifi` does `esp_wifi_stop()`/`esp_wifi_start()`. A network device whose radio cannot be turned off does not override it (default `Unsupported`), and deep standby then just stops using the network. While the radio is off, `Platform::random` does not give true random numbers; see the "Networking on the device" section of `docs/architecture.md`.
- **Automatic light sleep**: `PowerManager` calls `Platform::set_light_sleep(bool)` with `true` at `deep_standby` when the setting `power/light_sleep` is on and there is no `cpu` wake lock, and with `false` in every other case. In the firmware it only switches tickless light sleep (`firmware/main/main.cpp`); DFS stays on all the time. If a board cannot light-sleep safely, leave it empty; deep standby then only turns off Wi-Fi.

Light sleep must be verified on the device; the simulator cannot test it (it only logs a line `sim: light sleep on|off`). Follow the "Light-sleep compatibility (3b-1 spike)" section of `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`: leave the device on battery for 10 minutes, confirm that it really entered light sleep (`light_sleep_counts` greater than 0; a common blocker is the `APB_FREQ_MAX` lock of an I2S channel that stays open), and check PSRAM, timer accuracy, RTC drift, the display and touch after waking, and USB serial reconnection. Then run the repeated light sleep (item 29) and standby wake with XIP enabled (item 48) from that document's "Device acceptance checklist". If the board is not compatible, do not register `set_light_sleep`, and record the conclusion in the new board's hardware document. GPIOs are isolated during light sleep, so pins that must keep their level (for example the panel's RST/CS) need handling in the board's `init()`; see the "Standby and power saving" section of the same hardware document.

## Partitions and board configuration

`firmware/sdkconfig.defaults` holds only shared configuration that does not depend on the board. Everything that belongs to a particular board goes in the board directory:

- `boards/<board>/partitions.csv`: the board's partition table, and the only copy of it.
- `boards/<board>/sdkconfig.defaults.board`: flash size, mode and frequency, PSRAM, the console, and
  `CONFIG_PARTITION_TABLE_CUSTOM=y` and `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="../boards/<board>/partitions.csv"` (ESP-IDF resolves relative paths against the project directory `firmware/`). `idf.py bmgr -b <board>` merges it into the generated `board_manager.defaults`, which takes priority over `firmware/sdkconfig.defaults`.

sdkconfig defaults take effect only when `firmware/sdkconfig` does not exist. After changing `sdkconfig.defaults.board`, delete `firmware/sdkconfig` first, then run `idf.py bmgr` and `idf.py build`. When `idf.py bmgr -b` names a different board from the last run, bmgr itself deletes `firmware/sdkconfig` (backing it up as `components/gen_bmgr_codes/sdkconfig.bmgr_board.old`, overwritten by the next switch) and `firmware/build/CMakeCache.txt`, so local menuconfig changes there are lost; keep settings you need in the defaults files, or use a per-board build directory ("Building several boards" below).

### Partition conventions (new boards must follow them)

The code looks up partitions only by **partition name**; it does not depend on offsets or sizes.

| Partition name | Required? | Notes |
|---|---|---|
| `nvs` | Required | Configuration |
| `phy_init` | Required | RF calibration |
| `storage` | Required | LittleFS (`/int`) |
| `ota_0`, `ota_1`, `otadata` | Optional (needed for web firmware upload) | Two OTA app partitions of the same size, each big enough for the whole app image (the web asset pack is linked into the image too). Without them, use a single `factory` app partition: `EspFirmwareUpdater::create()` returns empty when it cannot find a second slot, `GET /system/firmware` reports `supported: false`, and the system only degrades |
| `fonts` | Optional (large vector text) | The vector font image used for text at 20 px and above (vector font spec 3.2–3.4). Without this partition, large text falls back to the 16 px bitmap font and `UiRuntime::start()` still succeeds. Do not add it on a board without PSRAM: the glyph cache lives in PSRAM, and without PSRAM that memory would squeeze internal RAM |
| `glyphs` | Optional (16 px body-text bitmap font, glyph partition spec) | The bitmap font image used for body text, kept out of the app image. Without this partition, the firmware has no packaged bitmap body text and falls back to vector 16 px, and failing that only the Montserrat placeholder remains (glyph partition spec 4.3). Like `fonts`, it is read through a mapping, and `UiRuntime::start()` still succeeds |

There is no separate `assets` partition: the web asset pack (WispPack, ≤ 256 KiB) is linked into the app image (see "Firmware and partitions" below). Each board decides its own partition sizes; for example, a 32 MB board can give the extra space to `storage`. On flash larger than 16 MB, everything read through the cache (the app slots and the mapped `fonts`/`glyphs` partitions) must lie entirely below 16 MB: the bootloader and cache address only the first 16 MB unless the experimental `CONFIG_BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH` is on, which Wisp does not use. `storage` (LittleFS, never mapped) can sit above 16 MB, provided ESP-IDF's driver for that flash part reports 32-bit address support (`SPI_FLASH_CHIP_CAP_32MB_SUPPORT` in `spi_flash_chip_<vendor>.c`; otherwise it warns at boot and caps the flash at 16 MB). See `boards/waveshare_esp32_s3_touch_amoled_1_75c/partitions.csv`. For the reference board's partition table, see `boards/waveshare_esp32_s3_touch_amoled_2_16/partitions.csv` and `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`.

**Rules for changing the partition table**: when you change the partition table of a board that has already shipped, keep `nvs` and `storage` at their original offsets and sizes, so that settings and data survive. The partition table and the bootloader are outside OTA; a web upload replaces only the app image. So after changing the partition table (or enabling rollback for the first time, or changing the bootloader), you must do one full flash over USB (`idf.py flash`, which writes the bootloader, the partition table, `otadata` and `ota_0`) before web uploads work. Shrinking `storage` (for example to make room for a new `fonts` partition) also counts as changing the partition table: the LittleFS superblock records the block count, so once the partition size changes the mount fails, `format_if_mount_failed` reformats it, and all data in `/int` (installed scenes, scene data, files) is erased. Such a change must be announced to the user in advance; it must not be treated silently as an ordinary "grow a partition".

## Firmware and partitions

### Board name descriptor

Every Wisp app image carries a 48-byte board name descriptor right after `esp_app_desc_t` (`firmware/main/board_desc.c`, section `.rodata_custom_desc`, so it is always at image offset 288):

| Offset (relative to the descriptor) | Type | Content |
|---|---|---|
| 0 | `u32` | Magic `0x42505357` (little-endian `"WSPB"`) |
| 4 | `u16` | Version, currently 1 |
| 6 | `u16` | Reserved, 0 |
| 8 | `char[40]` | Board name (`WISP_BOARD`, NUL-terminated, at most 39 characters) |

As soon as a web upload has received the first 336 bytes of the image (`core::parse_firmware_head()`), it checks the project name (`wisp`), the chip id (`esp_image_header_t` offset 12, compared with `CONFIG_IDF_FIRMWARE_CHIP_ID`) and this board name. If any of them does not match, it rejects the upload with 400 `wrong_board`, without writing a single byte to flash. An image without the descriptor (not built by Wisp) is rejected too. After the whole image has been written, its segment lengths, checksum and SHA-256 are verified by the bootloader's rules (`invalid_image`). Web upload **never requires a signature**: self-built images must install. A new board needs to do nothing; `WISP_BOARD` is written into the descriptor automatically, and `firmware/main/CMakeLists.txt` also checks that the board selected by `idf.py bmgr` (`CONFIG_ESP_BOARD_NAME`) matches `WISP_BOARD`.

### Version number

At configure time, `cmake/wisp_version.cmake` derives the version number from git and writes it into `esp_app_desc_t` as `PROJECT_VER` (the simulator uses the same function): on or after a `vX.Y.Z` tag it is `X.Y.Z[-N-g<hash>]`; with no tag it is `0.1.0-<commit count>-g<hash>`; with uncommitted changes `-dirty` is appended. The device uses it to tell upgrades from downgrades (a downgrade adds a line of warning to the confirmation dialog). Configuration reruns automatically only after a commit or a branch switch; editing files alone does not refresh `-dirty`.

### Confirmation and rollback

`sdkconfig.defaults` enables `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`: after a web upload is confirmed on the device, the new image boots in the "pending verification" state. `FirmwareUpdater::mark_valid()` is called only after `App::start()` has finished, printed the boot report and started the console (boot flow step 7, see `docs/architecture.md`). A crash, watchdog reset or power loss before that makes the bootloader go back to the old image.

### Font partitions

On a board with a `fonts` partition (partition conventions table, above), `firmware/main/CMakeLists.txt` packs the committed vector font into `build/fonts.bin` (vector font spec 3.3) and adds three flash targets. With a `glyphs` partition, it likewise packs the 16 px body-text bitmap font into `build/glyphs.bin` (glyph partition spec 3.2–3.3), reusing the same three targets rather than adding new ones (a board with only `glyphs` and no `fonts` partition also gets these three targets, which then write only `glyphs.bin`):

- `idf.py -p <port> flash`: writes `fonts.bin` and `glyphs.bin` (each only if its partition exists) together with the bootloader, partition table and app, so a new device is complete with one command and the font partitions always have data from the factory.
- `idf.py -p <port> fonts-flash`: writes only the `fonts` and `glyphs` partitions, for changing fonts or repairing a font partition.
- `idf.py -p <port> app-flash`: writes only the app partition. Use it for everyday code changes; it skips the font writes and the other, idle OTA slot every time.

Adding a `glyphs` partition to a board already in use is a partition table change and requires one full USB flash. If new firmware is installed only through OTA on a board still on the old partition table, the firmware runs normally (`storage` and the other partitions are looked up by name, and data is not affected), but it cannot find the `glyphs` partition, so body text falls back to vector 16 px (slightly blurrier than the bitmap font) until a full flash puts the new table on.

`app_main` maps the whole of both the `fonts` and `glyphs` partitions with `esp_partition_mmap()` into the data bus address space, read-only and permanently (`wisp::port::esp::map_font_partition()`/`map_glyph_partition()`). Each takes as much virtual address space as the partition is big (aligned to `CONFIG_MMU_PAGE_SIZE`, 64 KB pages on the reference board; the reference board's 2.81 MB `fonts` partition and 1.125 MB `glyphs` partition each occupy about as much data address space, until power-off). When a new board decides the sizes of these two partitions, it must leave room for this address space: the data bus virtual address space is shared with flash's other read-only mappings (the web asset pack, and with XIP enabled the executable mapping of the whole flash) and PSRAM's XIP mapping, so partitions that are too large can crowd them out. The exact limit depends on the chip's address-space layout; check that chip's memory-map documentation before making them bigger.

**Boards with XIP enabled must enable flash auto-suspend**: with `CONFIG_SPIRAM_XIP_FROM_PSRAM` enabled, ESP-IDF does not disable the cache while erasing or writing flash, so reading the mapped `fonts`/`glyphs` partitions during an erase or write returns 0xFF, and the UI crashes while drawing text (for the reference board's symptoms and measurements, see "Flash erase/write and mapped reads" in `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`). The same rule applies to both partitions; `glyphs` needs no separate handling. So such boards must add `CONFIG_SPI_FLASH_AUTO_SUSPEND=y` to `boards/<board>/sdkconfig.defaults.board`, and confirm that the board's flash supports suspend:

- The boot log is the final word: `W spi_flash: Flash suspend feature is enabled` means ESP-IDF accepted this flash. If it does not, `esp_flash_init_default_chip()` returns an error and an assertion fails at boot: a USB-flashed device reboots over and over, and an OTA-installed image is rolled back. So verify it with a USB flash the first time you enable it; do not push it straight through OTA to boards that have shipped.
- Before flashing you can check the part: `esptool flash-id` reads the vendor and device ids. For vendor 0x20 (XMC), ESP-IDF reads SFDP byte 0x32 (`esptool read-flash-sfdp 0x32 1`) and treats the chip as the suspend-capable XMC-D series only when bit 3 is 1; when it is 0 (XMC-C) it refuses by default. For other vendors, look at `get_caps` in the matching `spi_flash_chip_<vendor>.c` under ESP-IDF's `components/spi_flash/`.
- A board whose flash does not support suspend cannot have both XIP and the `fonts`/`glyphs` partitions: drop one of them (do not enable XIP, or do not add these two partitions, so that large text and body text fall back to the bitmap/vector fonts built into the firmware). `waveshare_esp32_s3_touch_amoled_1_75c` drops XIP (GigaDevice `0xC84019` is not on ESP-IDF's suspend list); its hardware document records the cost.
- Boards without XIP do not need this item: ESP-IDF disables the cache during erases and writes, so reads never see an intermediate state.

### Build configurations: release and debug

- The default (`sdkconfig.defaults`) is the release build: global `-Os` (`CONFIG_COMPILER_OPTIMIZATION_SIZE`), logging up to INFO, assertions silent.
- **The rendering path is always `-O2`**, whatever the global optimisation level: the `lvgl` component, `wisp_ui`, the `esp_lcd*` components (panel and touch drivers, matched by component name in `firmware/CMakeLists.txt`), and `dev/esp_display.cpp` and `dev/esp_touch.cpp` of `wisp_port_esp` (the LCD refresh path). If a new board's display or touch driver is not named `esp_lcd*`, add it to the matching rule in `firmware/CMakeLists.txt`, or the frame rate drops. At build time CMake prints `Rendering path built with -O2: ...` for checking.
- **Rendering acceleration is chosen per chip** and stays out of the portable layers: `firmware/CMakeLists.txt` defines `WISP_LV_DRAW_SW_ASM_INCLUDE` by `IDF_TARGET` (pointing to the hook header in `wisp_port_esp`), and `lv_conf.h` uses it to switch the LVGL software renderer to `LV_DRAW_SW_ASM_CUSTOM`. Currently only the ESP32-S3 has a set of SIMD fill/copy routines (`components/wisp_port_esp/lv_blend/esp32s3/`); other targets do not define this macro and take LVGL's C path. A new chip that wants its own SIMD or 2D hardware adds a hook header and sources under `lv_blend/<chip>/`, and selects them by target in both CMake files.
- The debug build layers `firmware/sdkconfig.debug` on top (`-Og`, logging up to DEBUG, assertions on), in a separate build directory, without touching the release `sdkconfig`:

```bash
cd firmware && idf.py -B build-debug -D SDKCONFIG=build-debug/sdkconfig \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;components/gen_bmgr_codes/board_manager.defaults;sdkconfig.debug" build
```

Once `SDKCONFIG_DEFAULTS` is given, it replaces the default list, so the `board_manager.defaults` generated by bmgr must be listed explicitly, before `sdkconfig.debug`.

## Chip differences

No chip name appears in the portable layers. Things that vary by chip are concentrated in the places below; check each one when changing chips:

| Difference | Where it is decided |
|---|---|
| SIMD / 2D rendering acceleration | `wisp_port_esp/lv_blend/<chip>/`, selected by `IDF_TARGET` in `firmware/CMakeLists.txt` and `components/wisp_port_esp/CMakeLists.txt` (see "Rendering acceleration is chosen per chip" above); chips without a matching directory take LVGL's C path |
| Task stacks in internal RAM or PSRAM | Tasks only declare `flash_access` and `hot_stack` (AGENTS.md section 5); the ESP port's `wisp/port/esp/stack_placement.hpp` decides by target. Currently every target follows "PSRAM is possible only when both are false", i.e. for tasks that neither access flash nor are on a hot path |
| Whether DMA can read PSRAM directly | The board declares it through the Display caps `draw_buf_mem` (see the capability field table above); on chips where `SOC_PSRAM_DMA_CAPABLE` is false, `Mem::DmaLarge` always fails to allocate and the UI falls back to internal DMA RAM |
| Executing code from PSRAM (XIP) | Board-level sdkconfig: `CONFIG_SPIRAM_XIP_FROM_PSRAM` in `boards/<board>/sdkconfig.defaults.board`. It does not relax the stack rules. With XIP on, you must also enable `CONFIG_SPI_FLASH_AUTO_SUSPEND` and confirm that the flash supports suspend: the `fonts`/`glyphs` partitions, the final OTA verification and otadata are all read through run-time mappings ("Font partitions" above) |
| Minimum ESP-IDF version | One range for the whole project, in `idf:` of `firmware/main/idf_component.yml` (currently `>=6.0.3,<6.1`); a chip that needs a newer IDF must first get this range and `dependencies.lock` upgraded on their own |
| JPEG decoding | esp_new_jpeg links the matching precompiled library by `IDF_TARGET` (1.0.2 provides esp32, esp32s2, esp32s3, esp32p4, esp32c2/c3/c5/c6/c61, esp32h4, esp32s31); `firmware/main/esp_new_jpeg.lf` moves the assembly it places in IRAM to flash for all of them. A chip with JPEG hardware can switch to a hardware decoder in `wisp_port_esp/ui/`, behind the same `install_image_decoders()` interface |
| Internal SRAM budget | Measured on each board with the `mem` command and recorded in `docs/hardware/<board>.md`; the budget minimums are in `wisp/app/ram_budget.hpp` (AGENTS.md section 5) |

**Planned target: ESP32-S31** (not verified; to be checked once hardware is available). What is known: RISC-V cores (the SIMD routines in `lv_blend/esp32s3/` cannot be used; they need rewriting, or the C path first); flash and PSRAM are on separate buses, with PSRAM as 250 MHz octal DDR; it has PPA, 2D-DMA and JPEG codec hardware; it needs ESP-IDF ≥ 6.1, beyond the currently pinned range. When executing from PSRAM with XIP, flash operations no longer stop the cache, but the ESP-IDF documentation still says external RAM cannot be accessed while the cache is disabled and task stacks default to internal RAM, so the stack placement policy stays as it is until measured.

## Step 4: Build

```bash
bash -c '. ~/esp/esp-idf-v6.0.3/export.sh && cd firmware \
    && idf.py set-target <chip, e.g. esp32s3> \
    && idf.py bmgr -c ../boards -b <board> \
    && idf.py build -DWISP_BOARD=<board>'
```

The `WISP_BOARD` cache variable of `firmware/CMakeLists.txt` decides which `boards/<name>/board.cpp` is compiled (default `waveshare_esp32_s3_touch_amoled_2_16`); override it with `-DWISP_BOARD=<board>`. As long as the board YAML has not changed, later `idf.py build` runs do not need set-target/bmgr again. For the debug build command, see "Build configurations: release and debug" above.

A board whose bmgr YAML declares no `fs_fat` device (no SD slot) gets no `dev_fs_fat.h`; `wisp_port_esp`'s SD code compiles without it (`CONFIG_ESP_BOARD_DEV_FS_FAT_SUPPORT`), and the board simply never constructs an `EspSdStorage`.

### Building several boards

Give each board its own build directory and sdkconfig, so that switching does not clobber another board's build:

```bash
cd firmware
idf.py bmgr -c ../boards -b <board>
idf.py -B build-<board> -D SDKCONFIG=build-<board>/sdkconfig \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;components/gen_bmgr_codes/board_manager.defaults" \
    -DWISP_BOARD=<board> build          # or: -p <port> flash / app-flash / fonts-flash
```

- `components/gen_bmgr_codes/` is shared by every build directory and holds only the board of the last bmgr run, so rerun `idf.py bmgr -b <board>` whenever you switch. Building with the wrong one fails at configure time (`idf.py bmgr selected board ... but WISP_BOARD is ...`, `firmware/main/CMakeLists.txt`), since `CONFIG_ESP_BOARD_NAME` has no prompt and always follows the generated Kconfig.
- `SDKCONFIG_DEFAULTS` replaces the default list, so `board_manager.defaults` must be named explicitly (as for the debug build).
- `build-<board>/sdkconfig` is seeded from the defaults and bmgr never deletes it. Values that came from a defaults file stick in it, so delete it after changing that board's `sdkconfig.defaults.board` or the set of device types in its YAML, and after any configure of that directory while bmgr had another board selected: that configure fails with the mismatch error above, but only after merging the other board's `board_manager.defaults` into it (for example an SD board's `CONFIG_ESP_BOARD_DEV_FS_FAT_SUPPORT=y`). The error message says so.
- `tools/ci.sh`'s format check already skips `firmware/build-*`. The default command (`idf.py build`, using `firmware/build` and `firmware/sdkconfig`) still builds the reference board, `WISP_BOARD`'s default, once bmgr has selected it again; for another board it needs `-DWISP_BOARD=<board>` each time, because bmgr deletes `firmware/build/CMakeCache.txt` on a switch.

For the first flash, use `idf.py -p <port> flash` (it writes the bootloader, partition table, app and font partitions together). After that, for everyday code changes use `idf.py -p <port> app-flash`, or upload `build/wisp.bin` on the admin console's "System" page and confirm on the device, which then restarts into the new version. Neither `app-flash` nor the web upload writes the `fonts` and `glyphs` partitions: after changing fonts or the partition table, run `idf.py -p <port> fonts-flash` again (see "Font partitions" above).

## Step 5: Acceptance

Work from the serial output and the hardware diagnostics screen; no extra tools are needed. The device boots into the home screen (black background, wisp in the centre); first confirm that the home screen appears and the wisp is centred in the safe area. Hardware diagnostics is the former self-check screen. Open it by typing `diag` on the serial console, or by swiping right on the home screen into Settings and tapping "Hardware diagnostics" under "About"; close it with `diag close`, or by swiping right from the left edge of the screen back to the home screen. `bench scroll` scrolls the diagnostics list, so run `diag` before measuring too:

1. **Boot report** (`WISP_BOOT_REPORT`): every device should be `ok` (or an expected `degraded`/`missing`, with a `reason`); there should be no unexpected `failed`.
2. **Capability list** (`WISP_MANIFEST`): check that the `board` field is the board name and that each device's `id`/`class`/`model`/`caps` follow the conventions above; the `Display` `safe_area` does not clip content on a rounded or round screen.
3. **Hardware diagnostics** (spec section 8, opened with `diag`): the screen is in English every time it opens (the page has its own language; it neither reads nor changes the system language), so the labels below are the English ones. All content is inside the safe area. Check from top to bottom:
   - **Title and summary labels**: the large title at the top is "Self Check". The "Devices n/m OK" below it is green (n = m). If it is yellow (something degraded), red (something failed) or grey (only missing devices), tapping it scrolls to the first device that is not OK.
   - **Board card**: the four rows Board (board name, wrapping when too long, not clipped), Target (chip target), Firmware (version number) and Boot (boot time) match reality.
   - **Language switch**: in the Board card, tap "中文" and then "日本語" once each. All the page text changes to that language, with complete glyphs (no boxes) and nothing clipped; the scroll position and test results stay the same. Then tap "EN" to switch back to English.
   - **Devices card**: the dot in front of every device is green. Devices that are not green are listed first, with the state and reason on the second line, and the reason must be expected (for example no SD card inserted).
   - **Interactive tests**: press in the Touch test area (the coordinates follow your finger), press any button, and tap "Mic test" and hear the sound. All three must change from "Not tested" to "✓ Passed", and the summary label "Tests 3/3 done" turns green.
   - **Scenes card** (after "Interactive tests" and before "Performance"): lists the installed scenes, each row a name plus state (version number, green "Running" or red "Error"); with no scenes it says "No scenes installed". Each row is at least 48 px high. Tapping a row opens that scene, and swiping right from the left edge of the screen returns to hardware diagnostics.
   - The remaining cards (Network, Sensors, Power & clock, Storage, Performance): the IMU values change with tilt and the ball moves with them; the battery level and RTC time are correct; the storage capacities are correct.
4. **Voice and the home screen**: set up the three model profile kinds `llm`, `asr` and `tts` in the admin console and connect to Wi-Fi. Hold the talk key (the one with `role: "talk"`), say a sentence and release it. The home screen shows, in order, what it heard, the agent's steps and the reply, and reads the `<say>` part aloud. A long press on the wisp also completes a turn. On the home screen, dragging right, left and down enters Settings, the scene list and the conversation history respectively, and dragging the opposite way returns to the home screen.
5. List the device acceptance items specific to the new board in the "Device acceptance checklist" section of `docs/hardware/<board>.md` (new), following the style of `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`: one item per row, with how to test it and the expected result, plus an empty "Result" column.
