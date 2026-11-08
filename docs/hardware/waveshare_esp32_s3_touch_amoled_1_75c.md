# Waveshare ESP32-S3-Touch-AMOLED-1.75C

Board name (`WISP_BOARD` / bmgr): `waveshare_esp32_s3_touch_amoled_1_75c`. Board code: `boards/waveshare_esp32_s3_touch_amoled_1_75c/`.

Sources: the official demo repository [waveshareteam/ESP32-S3-Touch-AMOLED-1.75C](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C) (Apache-2.0; commit `4bfdc80eedbf95031e384610796bed15e0240312` read), its schematic `hardware/schematic/ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf`, Waveshare's ESP-IDF BSP [`waveshare/esp32_s3_touch_amoled_1_75c`](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_75c) 3.0.0 (Apache-2.0; from Waveshare-ESP32-components commit `010c5fb39ea7f0e996efd1f8e162cc70e569e57f`), the component drivers it pulls in (`espressif/esp_lcd_co5300`, `waveshare/esp_lcd_touch_cst9217`, `espressif/esp_codec_dev`), the [Waveshare wiki page](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C) and facts measured on the device by the user. **Code and measurements are the authority; the product page contradicts them in several places** (see "Known errata and notes"). Waveshare also sells a 1.75 board without the "C" (round display, PCF85063 RTC, microSD); its documentation is easy to confuse with this one.

Most of the hardware matches the reference board (`docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`): the same CO5300 panel controller, CST9217 touch, AXP2101 PMU, QMI8658 IMU and ES8311/ES7210 audio on one I2C bus. What differs is listed in "Known errata and notes".

## SoC and storage

- ESP32-S3R8: 8 MB octal PSRAM (80 MHz, in the package), 32 MB QIO flash. The flash reads JEDEC id `C84019` with `esptool flash-id` (GigaDevice, 256 Mbit), read on the device by the user; the schematic's BOM part `XM25QH128` (16 MB) is out of date.
- No XIP from PSRAM (`sdkconfig.defaults.board`): see "Flash: no suspend, no XIP" below.
- For the partition table, see `boards/waveshare_esp32_s3_touch_amoled_1_75c/partitions.csv` (for the board-level sdkconfig of flash, PSRAM and the console, see `sdkconfig.defaults.board` in the same directory):

  | Partition | Offset | Size |
  |---|---|---|
  | `nvs` | 0x9000 | 24 KB |
  | `otadata` | 0xF000 | 8 KB |
  | `phy_init` | 0x11000 | 4 KB |
  | `ota_0` | 0x20000 | 5.94 MB (0x5F0000) |
  | `ota_1` | 0x610000 | 5.94 MB (0x5F0000) |
  | `glyphs` (16 px body-text bitmap font image, glyph partition spec 2, data subtype 0x41) | 0xC00000 | 1.125 MB |
  | `fonts` (vector font image, vector font spec 3.4, data subtype 0x40) | 0xD20000 | 2.81 MB |
  | (free) | 0xFF0000 | 64 KB |
  | `storage` (LittleFS) | 0x1000000 | 16 MB |

  Same partition names as the reference board; `glyphs` and `fonts` have the reference board's sizes, so the same `glyphs.bin`/`fonts.bin` fit. Everything the cache maps (both app slots, `glyphs`, `fonts`) lies below 16 MB, and the two OTA slots share all the room left there. `storage` takes the whole upper 16 MB: LittleFS reads and writes it through `esp_flash` with 4-byte addresses and never maps it (`CONFIG_LITTLEFS_MMAP_PARTITION` is off, and nothing in Wisp calls `esp_partition_mmap` on it). ESP-IDF v6.0.3's GigaDevice driver reports 32 MB support for ids whose low byte is 0x19 or more (`spi_flash_chip_gd.c` `get_caps`), and its read/program/erase paths switch to the 4-byte commands above 16 MB (`spi_flash_chip_winbond.c`, which the GigaDevice driver reuses). Writing above 16 MB still has to be confirmed on the device (checklist).
- Image size (ESP-IDF v6.0.3, release build, 2026-11-08): `wisp.bin` 3,516,656 B (0x35A8F0), 56% of one OTA slot (0x5F0000); `glyphs.bin` 1,122,585 B, `fonts.bin` 2,431,464 B. The budget is 80% of one OTA slot.

## Peripherals and pins

| Peripheral | Chip | Interface and pins | Address (7-bit) |
|---|---|---|---|
| Display | CO5300, 1.75" 466×466 round AMOLED | QSPI 40 MHz: CS 12, SCLK 38, D0–D3 = 4/5/6/7, RST 1; no TE, no backlight pin (brightness is DCS 0x51); visible columns start at controller column 6 (gap x=6, y=0) | — |
| Touch | CST9217 | I2C; INT 11, RST 2 (both active low); BSP orientation: mirror X and Y, no swap | 0x5A |
| Power management | AXP2101 | I2C; the PWR key goes only to the AXP2101's PWRON pin, so its state is read from the PMU's IRQ status registers over I2C | 0x34 |
| IMU | QMI8658 (no magnetometer) | I2C; INT1/INT2 not wired to a known GPIO (polled) | **0x6B** expected, 0x6A also probed |
| RTC | none found (see errata) | — board.cpp probes 0x51 for a PCF85063 | (0x51) |
| Audio DAC | ES8311 | I2S + I2C | 0x18 |
| Audio ADC | ES7210 (two microphones) | I2S + I2C | 0x40 |
| Amplifier | — | PA_EN GPIO46, active high | — |
| SD card | **none** | — | — |
| Buttons | — | BOOT GPIO0 (active low, internal pull-up); PWR via the AXP2101 | — |

- I2C bus: SDA 15, SCL 14, 400 kHz; every I2C device shares this one bus.
- I2S: MCLK **16**, BCLK 9, LRCK/WS 45, DOUT 8, DIN 10.
- Device ids: `display.main`, `touch.main`, `power.main`, `clock.main` (only if an RTC answers), `imu.main`, `button.power`, `button.boot` (caps `role: "talk"`), `audio_out.main`, `audio_in.main`, `storage.int`, `net.wifi`. The BOOT key gets `button.boot` rather than the reference board's `button.vol_down`: it is the only GPIO key and carries no volume label, and the shell finds the talk key by its `role`, not its id (`docs/porting-guide.md`, "Voice and the talk key").

## Known errata and notes

- **Display shape**: the screen is round (466×466; `shape: "circle"`, no `corner_radius`), confirmed by the user. The demo repository's README calls it "square", and the research based on it was wrong. The exact visible circle has not been measured yet: draw arcs of a few candidate radii around the centre and compare by eye, as for the reference board's corners (`docs/porting-guide.md`), and record the result here.
- **CO5300 init table**: `setup_device.c` uses the BSP's own table verbatim (`lcd_init_cmds` in `esp32_s3_touch_amoled_1_75c.c`), not the `esp_lcd_co5300` default: it adds three page-2 registers (`0xFE=0x20`, `0x19=0x10`, `0x1C=0xA0`), a 466-column CASET starting at column 6 and 600 ms waits after RASET and SLPOUT. The driver may warn that the table's `0x3A`/`0x35` overlap its own COLMOD/TE writes; that is expected. Partial refresh areas must be aligned to 2 pixels (`area_align = 2`), as on the reference board.
- **PWR key**: no GPIO follows the key (the reference board's GPIO16 is this board's I2S MCLK). `button.power` is `EspPmuPowerKey` (`wisp/port/esp/esp_button.hpp`): `ButtonService` polls it every 20 ms (50 ms in standby), each poll reads REG 0x49 over I2C, and `axp2101_pwrkey_update()` rebuilds the key level from the latched PWRON negative (press) and positive (release) edge bits, so short and long presses are timed exactly as for a GPIO key. The bit layout is taken from XPowersLib; which edge is the press has not been checked on the device yet (checklist). If this chip revision latches no edge bits, the short-press bit alone still gives one short press per press, and long presses are lost. The AXP2101's IRQ output is not used. Holding the key for several seconds still triggers the PMU's hardware power-off, independently of software.
- **RTC**: the product page says "RTC: yes", but the BSP, the examples and the schematic show no RTC chip (the schematic has only a `VCC-RTC` net at the AXP2101). `board.cpp` probes 0x51 with `i2c_master_probe()`: with no answer it records `clock.main` as `missing` ("no RTC") and the system clock is set only by SNTP once Wi-Fi connects (`wisp_net` time sync works without an RTC); if a PCF85063 does answer, it is brought up as on the reference board.
- **SD card**: the product page says "SD card: yes"; the board has no slot (BSP `BSP_CAPS_SDCARD 0`, no SD pins, confirmed by the user). No `storage.sd` is registered. bmgr then generates no `fs_fat` device type, so `wisp_port_esp/dev/esp_storage.cpp` compiles its SD code without `dev_fs_fat.h`.
- **IMU address**: Waveshare's examples pass SensorLib's `QMI8658_L_SLAVE_ADDRESS`, which SensorLib defines as 0x6B (its L/H names are the reverse of the datasheet's SA0 levels). A first build that used 0x6A got no answer (`ESP_ERR_INVALID_RESPONSE`), so `board.cpp` probes 0x6B, then 0x6A, and uses whichever answers. `drivers/qmi8658` takes the address as a parameter.
- **Product page vs this board**: the product page's "round", "RTC: yes" and "SD card: yes" all match the 1.75 (non-C) board; treat that page with care.
- When initialising the AXP2101, do not change the voltage or on/off state of any power rail (same rule as the reference board). The BSP never touches the AXP2101 either; the rails' power-on defaults run everything.

### Flash: no suspend, no XIP

The reference board runs code from PSRAM (`CONFIG_SPIRAM_XIP_FROM_PSRAM`), which keeps the cache on during flash erases and writes and therefore needs flash auto-suspend for the mapped `fonts`/`glyphs` partitions ("Font partitions" in `docs/porting-guide.md`). ESP-IDF v6.0.3's GigaDevice driver lists only `0xC84016/17/18` and `0xC84319` as suspend-capable (`spi_flash_chip_gd.c`), not this board's `0xC84019`, so this board enables neither XIP nor auto-suspend and keeps both font partitions. Without XIP, ESP-IDF disables the cache and stalls the other core for every flash erase and write, so mapped reads can never see a half-written state. The costs:

- The UI stalls while flash is being written: OTA upload, LittleFS writes (`/int`), NVS. Each erase or program command holds the cache off until it finishes, so scrolling during an OTA upload will stutter.
- Code runs from flash through the 16 KB instruction cache: on the reference board XIP raised the diagnostics-list scroll from 14 to 20 fps, so expect roughly the reference board's pre-XIP frame rate here (to be measured; checklist).
- The task stack rules do not change (AGENTS.md section 5): without XIP, a task that touches flash with its stack in PSRAM aborts in the flash driver at once, which is what `flash_access` already prevents. Nothing in the shared code assumes XIP: `stack_placement.hpp` keeps flash-touching stacks internal on every target, and `firmware/main/CMakeLists.txt` only checks that XIP is not on without auto-suspend.

The 32 MB flash is used without the experimental `BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH`: the bootloader and the cache address only the first 16 MB, which is why the partition table keeps everything mapped there.

## First flash

From the `firmware/` directory, with the pinned ESP-IDF environment active (`. ~/esp/esp-idf-v6.0.3/export.sh`). This board builds in its own build directory and sdkconfig, so the reference board's `firmware/build` and `firmware/sdkconfig` are not touched (see "Building several boards" in `docs/porting-guide.md`):

```bash
idf.py set-target esp32s3          # only on a fresh checkout
idf.py bmgr -c ../boards -b waveshare_esp32_s3_touch_amoled_1_75c
idf.py -B build-waveshare_esp32_s3_touch_amoled_1_75c \
    -D SDKCONFIG=build-waveshare_esp32_s3_touch_amoled_1_75c/sdkconfig \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;components/gen_bmgr_codes/board_manager.defaults" \
    -DWISP_BOARD=waveshare_esp32_s3_touch_amoled_1_75c -p <PORT> flash monitor
```

`flash` writes the bootloader, partition table, `otadata`, `ota_0`, `glyphs.bin` and `fonts.bin`. Afterwards use the same command with `app-flash` for code changes and `fonts-flash` after font changes, or upload `build-waveshare_esp32_s3_touch_amoled_1_75c/wisp.bin` through the admin console. The device's vendor firmware is overwritten; LittleFS formats `storage` on the first boot.

## Device acceptance checklist

The board has not been brought up yet; every item is still to be tested. Fill in the "Result" column (pass/fail + a short note) at acceptance; a failing item goes back to its implementation. For every item that looks at the diagnostics screen, open it with `diag` on the serial console first. The flash commands are in "First flash" below.

| # | Item | How to test | Expected result | Result |
|---|---|---|---|---|
| 1 | First full flash | "First flash" below, with the board in download mode if needed | `idf.py ... flash` writes bootloader, partition table, `otadata`, `ota_0`, `glyphs` and `fonts` without error; the board boots into the home screen (black background, wisp in the centre) | |
| 2 | Flash and PSRAM detected | Boot log | 32 MB flash detected without "Detected size ... larger/smaller" errors; no "access beyond 16 MB is not supported" warning; 8 MB octal PSRAM; no `Flash suspend feature is enabled` line (auto-suspend is off) | |
| 3 | Boot report | `WISP_BOOT_REPORT` on the serial port | Valid single-line JSON; every device `ok` except `clock.main`, which is `missing` ("no RTC") or `ok` if a PCF85063 answered; no `storage.sd`; `boot_ms` < 3000 | |
| 4 | Capability list | `WISP_MANIFEST` | `board` is `waveshare_esp32_s3_touch_amoled_1_75c`; `display.main`: `model` `CO5300`, `width`/`height` 466, `shape` `circle`, `panel` `amoled`, `area_align` 2, `rgb565_be` true, `safe_area` inside the circle; `imu.main` `magnetometer` false; `button.boot` has `role: "talk"`; `button.power` model `axp2101-pwrkey`, `standby` `wake_source` | |
| 5 | Picture position and colours | Home screen and `diag` | The picture is centred, no column of garbage at the left or right edge (column offset 6), not mirrored; red and blue not swapped | |
| 6 | Touch in all four quadrants | `diag`, press in the Touch test area and then near the top-left, top-right, bottom-left and bottom-right of the circle | The reported point follows the finger in every quadrant (catches a mirrored or swapped axis). If one axis is mirrored or X/Y are swapped, flip `TOUCH_MIRROR_X`/`TOUCH_MIRROR_Y`/`TOUCH_SWAP_XY` in `setup_device.c`, rerun bmgr and rebuild | |
| 7 | Visible-circle calibration | Calibration scene: concentric circles of radius 233, 229, 225, 221 around (233,233), plus marks at the four `safe_area` corners | Record which circles are fully visible; the `safe_area` corners are visible. Record the result in "Known errata and notes" | |
| 8 | Partial refresh | `diag`, scroll the device list quickly up and down | No stripes, misalignment or stale sections | |
| 9 | IMU tilt direction | `diag`, tilt the device left/right/forward/back and watch the ball; lay it flat | The ball moves the way the device tilts (not mirrored); flat, `az` ≈ 1.0 g; gyro ≈ 0 dps at rest | |
| 10 | BOOT key | `diag`, short-press and long-press BOOT | The diagnostics screen shows `button.boot` down/up/short/long | |
| 11 | PWR short press | `diag`, short-press PWR; then on the home screen short-press PWR twice | Diagnostics shows `button.power` down/up/short (press reported on the press, not the release: confirms negative edge = press); on the home screen the first press turns the screen off, the second on | |
| 12 | PWR long press | Hold PWR for about 1 s with the screen on | The power-off confirmation appears while held; the press held to power the board on does not count as a press after boot | |
| 13 | Mic test | `diag`, tap "Mic test" | Records 3 s and plays back clearly, not silent or distorted | |
| 14 | Speaker tone | Trigger an alert tone (for example a scene or agent reminder) and a TTS reply | Audible, no pop at boot or when the PA switches | |
| 15 | Battery | `diag` with a battery fitted, then plug/unplug USB | Battery percentage plausible; charging state follows USB | |
| 16 | RTC present or missing | Boot report and boot log | Either `clock.main` `missing` ("no RTC") and the log line `clock.main: no RTC at 0x51; time comes from SNTP`, with the clock correct after Wi-Fi connects; or a PCF85063 found, `clock.main` `ok`, and the time kept across a power cycle | |
| 17 | LittleFS above 16 MB | `diag` storage card shows ~16 MB internal; install or write a scene, power-cycle the board (not just reset), open the scene list | Capacity ≈ 16 MB; the scene is still there after the power cycle; no flash errors in the log | |
| 18 | OTA upload | Upload `build-waveshare_esp32_s3_touch_amoled_1_75c/wisp.bin` on the admin console's "System" page and confirm on the device | Accepted (not `wrong_board`), the device restarts into the new image and marks it valid; the UI may stutter during the upload (no XIP) but must not crash | |
| 19 | Wake from standby | Short-press PWR to enter standby, wait more than 10 minutes, then press PWR (and separately BOOT) | Wakes on either key; panel, touch and IMU work after waking; brightness not reset | |
| 20 | Frame rate baseline | `diag`, then `bench scroll 10` with the hotspot off and on; note `WISP_PERF` fps/cpu/flush_ms | Recorded here as this board's baseline (no XIP: expected below the reference board's XIP numbers); `internal free` from `mem` recorded too | |
