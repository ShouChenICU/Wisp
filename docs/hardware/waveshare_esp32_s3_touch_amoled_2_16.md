# Waveshare ESP32-S3-Touch-AMOLED-2.16

Board name (`WISP_BOARD` / bmgr): `waveshare_esp32_s3_touch_amoled_2_16`. Board code: `boards/waveshare_esp32_s3_touch_amoled_2_16/`.

Sources: the MyOS project (MIT, by ShouChen, read-only reference; **its code is the authority, its documentation has errors**), Waveshare's official BSP, the board definition of the same board in ESP-Claw (Apache-2.0), and the [official product page](https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-2.16/).

## SoC and storage

- ESP32-S3R8: 16 MB QIO flash, 8 MB octal PSRAM (80 MHz)
- For the partition table, see `boards/waveshare_esp32_s3_touch_amoled_2_16/partitions.csv` (for the board-level sdkconfig of flash, PSRAM, the console and so on, see `sdkconfig.defaults.board` in the same directory):

  | Partition | Offset | Size |
  |---|---|---|
  | `nvs` | 0x9000 | 24 KB |
  | `otadata` | 0xF000 | 8 KB |
  | `phy_init` | 0x11000 | 4 KB |
  | `ota_0` | 0x20000 | 4.5 MB |
  | `ota_1` | 0x4A0000 | 4.5 MB |
  | `glyphs` (16 px body-text bitmap font image, glyph partition spec 2, data subtype 0x41) | 0x920000 | 1.125 MB |
  | `storage` (LittleFS) | 0xA40000 | 2.94 MB |
  | `fonts` (vector font image, vector font spec 3.4) | 0xD30000 | 2.81 MB |

  The web asset pack is linked into the app image; there is no `assets` partition. The offset and size of `nvs` have not changed across any of the table changes, so settings stored in NVS, such as brightness and language, are kept. But saved Wi-Fi networks (`/int/net/wifi.json`) and paired clients (`/int/net/clients.json`) live in `/int` and are lost when the table changes, so Wi-Fi must be set up and clients paired again. The offset and size of `fonts` also stay the same. This is the second table change: the first (vector font spec 3.4) shrank `storage` from 5.875 MB to 3.06 MB to make room for the new `fonts` partition; this one (glyph partition spec 2) shrank the two OTA slots from 5 MB to a round 4.5 MB to make room for the new `glyphs` partition, and `storage` shrank with them from 3.06 MB to 2.94 MB (its offset moved from 0xA20000 to 0xA40000). The reasoning is the same both times: the LittleFS superblock records the block count, so once the partition size changes the mount fails and `format_if_mount_failed` immediately reformats it. **Every time a new table goes on, all data in `/int` (saved Wi-Fi networks, pairing records, installed scenes, scene data, files) is erased.** The user approved this table change (glyph partition spec 2). When coming from an old partition table (two 4 MB app slots plus a 2 MB `assets` partition; the previous version with `storage` at 0x5E0000 and no `fonts` partition; or the previous version with two 5 MB app slots, no `glyphs` partition and `storage` at 0xA20000), and the first time a bootloader with rollback enabled is used, you must do one full USB flash (`idf.py -p <PORT> flash`, which writes the bootloader, the partition table, `otadata`, `ota_0`, `fonts` and `glyphs`) before firmware can be uploaded through the web page. To change only the fonts, use `idf.py -p <PORT> fonts-flash` (which writes both `fonts.bin` and `glyphs.bin`); for everyday code changes, use `idf.py -p <PORT> app-flash` (see "Font partitions" in `docs/porting-guide.md`).
  On a board still on the previous partition table (two 5 MB app slots, no `glyphs` partition) that has had new firmware uploaded through the web page without a full flash, the new firmware runs normally, `storage` is looked up by name, and `/int` is not affected; but there is no `glyphs` partition, so body text falls back to vector 16 px (slightly blurrier than the bitmap font) until one full USB flash (`idf.py -p <PORT> flash`) puts the new table on.
- Image size (ESP-IDF v6.0.3, `idf.py size`): release build 3,114,019 B (66.0% of the new `ota_0`, 0x480000, after the glyph partition table change on 2026-11-06, with `glyphs.bin` at 1,122,464 B; the debug build was not re-measured). The budget is 80% of one OTA slot. The earlier 3,967,023 B (75.7% of the old 5 MB `ota_0`, at the vector font acceptance on 2026-11-05, including 3a and the vector font) is from before the table change; once `lv_font_cjk_16` moved into the `glyphs` partition it is no longer linked into the image.

## Peripherals and pins

| Peripheral | Chip | Interface and pins | Address (7-bit) |
|---|---|---|---|
| Display | CO5300, 480×480 AMOLED, corner radius 70 (measured, see "Known errata and notes" below) | QSPI: CS 12, SCLK 38, D0–D3 = 4/5/6/7, RST 39 | — |
| Touch | **CST9217** (the MyOS documentation wrongly says CST9220) | I2C; INT 11, RST 40 | 0x5A |
| Power management | AXP2101 | I2C; the PWR key state is read from GPIO16 (**high while pressed**) | 0x34 |
| IMU | QMI8658 (no magnetometer) | I2C; INT1 17, INT2 21 | 0x6B |
| RTC | PCF85063 | I2C; INT 13 | 0x51 |
| Audio DAC | ES8311 | I2S + I2C | 0x18 |
| Audio ADC | ES7210 | I2S + I2C | 0x40 |
| Amplifier | — | PA_EN GPIO46 | — |
| SD card | — | **1-bit SDMMC**: CLK 2, CMD 1, D0 3 | — |
| Buttons | — | Volume+ GPIO18; Volume−/BOOT GPIO0 (both active low) | — |

- I2C bus: SDA 15, SCL 14, 400 kHz; every I2C device shares this one bus.
- I2S: MCLK 42, BCLK 9, LRCK/WS 45, DOUT 8, DIN 10.

## Known errata and notes

- The touch chip is actually the CST9217, and the driver is `waveshare/esp_lcd_touch_cst9217`.
- The SD card uses SDMMC, not SPI.
- The display's partial refresh areas must be aligned to 2 pixels (`area_align = 2`).
- The panel's actual corner radius is much larger than the 40 in the vendor's documentation. Separately from the self-check screen, we wrote a calibration scene that draws quarter-circle arcs of radius 40, 50, 60, 70 and 80 in the four corners of the screen (centred at `(R,R)`), and checked by eye, one by one, which arcs could still be seen along the diagonal and which had been clipped by the real rounded corner. The result: the 60 px arc is completely invisible along the diagonal and the 70 px arc is only partly visible, from which the visible boundary on the diagonal works out at about 20 px from the corner, corresponding to a radius of about 70 (`kCornerRadius` in `board.cpp`). Computing the safe area from the datasheet's 40 clips the corners of text or controls. On a new board, always measure with this arc comparison; do not use the datasheet value directly (see `docs/porting-guide.md`).
- When initialising the AXP2101, do not change the voltage or on/off state of any power rail.
- GPIO16 reflects the PWR key state: it is driven low when released and high when held (measured; the official documentation only labels it `SYS_OUT`). It is not the AXP2101's interrupt output; read it as an ordinary button, with short and long presses timed by `ButtonService`, and there is no need to read the AXP2101's IRQ registers. In the measurements, the AXP2101's PWRON long-press IRQ also happened to set at 1 s of holding, consistent with the level of GPIO16.
- Holding the PWR key for several seconds (the AXP2101's OFFLEVEL) triggers the chip's hardware forced power-off, independently of software.
- This board must enable flash auto-suspend (`CONFIG_SPI_FLASH_AUTO_SUSPEND`, `sdkconfig.defaults.board`): with XIP on, reading the `fonts` partition, which is mapped at run time, during a flash erase or write returns 0xFF, and the vector font crashes as a result. For details, see "Flash erase/write and mapped reads" below.
- Known limitations of uploading firmware through the web page (httpd has a single task; accepted as they are, see "Networking on the device" in `docs/architecture.md`): a file over 8 MiB gets a connection reset rather than a 413 (as does any upload rejected early with more than 8 MiB left to send); when two browsers upload at the same time, the second has to wait until the first has finished before it gets 409 `busy`; the logs page pauses its push during an upload.

## Code from flash (2026-11-08)

This board no longer runs code from PSRAM: `sdkconfig.defaults.board` turns off `CONFIG_SPIRAM_XIP_FROM_PSRAM` and `CONFIG_SPI_FLASH_AUTO_SUSPEND`, and the shared `firmware/sdkconfig.defaults` doubles the instruction cache to 32 KB. Every instruction-cache miss is then a QSPI flash read (about 1 µs for a 32-byte line at 80 MHz, against about 0.4 µs from octal PSRAM), and one frame's rendering code is far larger than the cache, so the cache size matters more than where the code lives. Measured with `diag` and `bench scroll 10`, hotspot off, firmware `934adf6` plus the configuration change:

| Configuration | fps | `internal free` (idle) | `psram free` (idle) |
|---|---|---|---|
| XIP, 16 KB instruction cache (the configuration before) | 18.0–19.0 | 94.9 KB | 3.23 MB |
| No XIP, 16 KB instruction cache | 11.1 | — | — |
| XIP, 32 KB instruction cache | 24.8–24.9 | 76.8 KB | 3.23 MB |
| **No XIP, 32 KB instruction cache** (now) | 20.2–21.9 | 77.0–78.8 KB | 6.76 MB |

With the hotspot on (Wi-Fi setup card showing), the current configuration scrolls at 31.2–32.7 fps, with 70.8 KB of internal RAM free. Both are above the acceptance lines of "Current baseline" below. XIP with the 32 KB cache is about 3 fps faster, but it costs the ~3.5 MB image copy in PSRAM and depends on flash auto-suspend, which depends on the flash part (see "Risk" below).

Without XIP, ESP-IDF disables the cache and stalls the other core for every flash erase and write. Mapped reads (`fonts`, `glyphs`, otadata, the OTA image check) therefore never see a half-written state, and the 0xFF problem of the next section cannot happen. The cost is that the UI stalls while flash is written; it shows during an OTA upload (checklist item 67). The 32 KB cache takes 16 KB of internal RAM, so the worst case has to be measured again (checklist item 68).

## Flash erase/write and mapped reads (2026-11-05)

This section describes the board while it still ran code from PSRAM (XIP). Since 2026-11-08 it does not (see "Code from flash" above), and auto-suspend is off; the measurements stay here because any board that turns XIP back on meets the same problem.


**Symptom**: while uploading firmware through the web page (OTA), the UI task crashed with LoadProhibited on core 0, EXCVADDR `0xbc804b19`, with the stack `font_image_has_glyph` ← `vector_glyph_dsc` (`components/wisp_ui/src/fonts.cpp`) ← `lv_font_get_glyph_dsc`. Any flash write (OTA, LittleFS `/int`, NVS, including IDF's own Wi-Fi/PHY NVS writes) that coincides with the UI drawing an uncached vector glyph can trigger it.

**Root cause**: this board has XIP enabled (`CONFIG_SPIRAM_XIP_FROM_PSRAM`), so ESP-IDF does not disable the cache while SPI1 erases or writes flash, and only takes a mutex (`SPI_FLASH_CACHE_NO_DISABLE` in `spi_flash/spi_flash_os_func_app.c`). While an erase or write is in progress the flash is busy, and a cache miss that lands on the `fonts` partition mapped with `esp_partition_mmap` reads back all 0xFF. `font_image_has_glyph()` at the time re-read the format and counts of the cmap subtable from the mapped area every time; reading 0xFFFF sent it down the format 12 branch with a group count of 0xFFFFFFFF, and `12 * mid` wrapped around in 32 bits, so the pointer ended up outside the mapped area. Code and read-only data had already been copied into PSRAM at XIP startup, as had the web asset pack, so they are not affected. What is affected is flash mapped at run time: the `fonts` partition, plus the new image that ESP-IDF itself maps to read (`esp_image_verify()` inside `esp_ota_end()` and `esp_ota_set_boot_partition()`) and `otadata` (`read_otadata()`). Under concurrent writes, the latter two can misjudge an OTA as failed, or read both `otadata` records as invalid. Boards without XIP are not affected: IDF disables the cache and stalls the other core during erases and writes.

**Fix**:
- The board enables `CONFIG_SPI_FLASH_AUTO_SUSPEND`: when a mapped read coincides with an erase or write, the hardware first suspends the erase or write, finishes the read and then resumes, so the data read is correct.
- Hardening (not a fix): when `open_font_image()` validates the cmap, it copies the subtable format and segment/group counts into `FontImage`, and glyph lookups use only that copy; the one offset computed from a value read at run time (the glyphIdArray position) is compared once against the subtable length. Glyph data and stb_truetype's reads still come from the mapped area, and only auto-suspend keeps them correct.

**Flash part**: `esptool flash-id` reads vendor 0x20, device 0x4018, i.e. XMC 128 Mbit. For vendor 0x20, ESP-IDF v6.0.3 reads SFDP byte 0x32 and treats the chip as the suspend-capable XMC-D series when bit 3 is 1 (`get_caps` in `spi_flash_chip_generic.c`); this chip reads 0xF9 (`esptool read-flash-sfdp 0x32 1`), so bit 3 is 1. The boot log has `W spi_flash: Flash suspend feature is enabled`.

**Measurements** (`fprobe` was a temporary command added only for this measurement and is not in the firmware: it copies the mapped area into PSRAM as a reference and memcmps it repeatedly, or puts a 128 px label on the top layer and changes its 6 Chinese characters every 20 ms so that the glyph cache keeps missing; meanwhile another task loops doing 64 KB erases + 64 KB writes in `ota_1`):

| Test | Without suspend | With suspend |
|---|---|---|
| Compare the mapped area while writing flash | 29 passes read 41,817,016 wrong bytes, all 0xFF (about 59% of the bytes read) | 19 and 24 passes, 0 wrong bytes |
| Change 128 px glyphs while writing flash | Crashes within 5 s of the writes starting, with the same stack as the OTA crash | 180 s writing 56.6 MB, no crash; tested again on the fix commit, 150 s writing 46.9 MB with 1587 updates, no crash |
| Change glyphs while uploading 3.97 MB of firmware through the web page | This is the original crash | No crash, 24.0 s, returned 202 |
| Web upload of 3,971,952 B (idle self-check screen) | 20.4–20.7 s | 20.6 s (twice) |
| Pure write throughput (no readers) | 338.5–340.9 KB/s | 355.8–365.6 KB/s |
| `bench scroll 10` (same screen, hotspot off) | 18.8–19.1 fps | 18.8–19.0 fps; 18.72 / 18.75 on the fix commit `352ad8f` |
| `boot_ms` | 2416 | 2411 |

None of the suspend tests produced a watchdog alarm or a flash error. Only in the extreme combination "compare the mapped area at full speed + write flash at the same time" (no real code reads like this) did `power_monitor` report `axp2101_read_status: ESP_ERR_TIMEOUT` every few seconds, recovering on the next read; it did not appear with full-speed comparison alone, writing alone, or changing glyphs while writing.

**Risk**: auto-suspend depends on the flash part. If a batch of this board ships with flash that ESP-IDF does not accept for suspend (for example the XMC-C series, with bit 3 of SFDP[0x32] at 0), `esp_flash_init_default_chip()` returns an error and `assert(flash_ret == ESP_OK)` fires at boot (`esp_flash_spi_init.c`): firmware installed through OTA is rolled back, and a USB-flashed one reboots over and over. The failure is obvious and does not go wrong silently. If it happens, first confirm the part with `esptool flash-id` and `read-flash-sfdp`. The ESP-IDF documentation also warns that erases get slower when interrupts are very frequent; in the measurements this was not seen with Wi-Fi and asynchronous flushes working at the same time.

## Device acceptance checklist

This gathers the manual verification items recorded during the implementation of each sub-task as "needs the user to run on the device", plus the acceptance criteria of spec section 10. This board has been on the device; items without a result are still to be tested. The "Result" column is left empty, to be filled in with the measured result at acceptance (pass/fail + a short note); a failing item goes back to its implementation to be fixed.

The source in front of each item tells which part of the implementation the check comes from; items marked "spec 10.n" are the acceptance criteria quoted from section 10 of the design document.

The device boots into the home screen (black background, wisp in the centre). "Hardware diagnostics" below is the former self-check screen: open it by typing `diag` on the serial console, or by swiping right on the home screen into Settings and tapping "Hardware diagnostics" under "About"; close it with `diag close`, or by swiping right from the left edge of the screen back to the home screen. For every item that looks at the diagnostics screen, open it with `diag` first.

| # | Item (from) | How to test | Expected result | Result |
|---|---|---|---|---|
| 1 | Boot time (spec 10.1) | `idf.py -p <PORT> flash monitor`; time from power-on until the home screen (black background, wisp in the centre) appears, by eye or with serial timestamps; also look at `boot_ms` in `WISP_BOOT_REPORT` | `boot_ms` < 3000 | |
| 2 | Boot report/capability list format (spec 10.4, wisp_core::BootReport/DeviceRegistry::manifest()) | Look at the two lines `WISP_BOOT_REPORT <json>` and `WISP_MANIFEST <json>` on the serial port | Both are valid single-line JSON; device states are as expected (`storage.sd` reported as missing with no SD card inserted is normal) |  |
| 3 | IMU has no magnetometer (spec 10.4) | Look at `imu.main.caps` in `WISP_MANIFEST` | `magnetometer` is `false` |  |
| 4 | Display capability list (display driver: esp_display) | Look at `display.main` in `WISP_MANIFEST` | `model` is `CO5300`; `caps` contains `width:480 height:480 shape:"rounded_rect" corner_radius:70 panel:"amoled" brightness:true area_align:2 rgb565_be:true`; `safe_area` is `{24,24,455,455}` |  |
| 5 | Touch model (touch driver: bmgr dev_lcd_touch) | Look at `touch.main` in `WISP_MANIFEST` | `model` is `CST9217` |  |
| 6 | Display colours (byte order: esp_display publishes the caps `rgb565_be`, and wisp_ui renders high byte first directly from it) | Open hardware diagnostics with `diag`, show solid colour blocks or backgrounds, and check pure red and pure blue by eye | Red and blue are not swapped and not speckled; if they are swapped, the byte order was judged wrong |  |
| 7 | No clipping in the four corners (spec 8, 10.2) | Look at the four corners of the home screen and hardware diagnostics (`diag`) | Content is not clipped by the rounded corners |  |
| 8 | Touch coordinate orientation (touch driver: esp_display_touch coordinate mapping) | Tap the four corners of the screen in turn | The top-left corner is near (0,0) and the bottom-right near (479,479); not mirrored, X and Y not swapped |  |
| 9 | Partial refresh without misalignment (display driver: esp_display flush_async()/area alignment, two buffers transferred in turn) | Open hardware diagnostics with `diag` and scroll the device list up and down quickly | No stripes, misalignment, or a section showing the previous frame or another section's content |  |
| 10 | Brightness initialisation (display driver: esp_display set_brightness()) | Compare the brightness right at power-on with the brightness once the home screen has settled | Set to 200 after boot, so it should be slightly darker than the 0xFF at initialisation |  |
| 11 | IMU follows tilt (spec 10.2) | Open hardware diagnostics with `diag`, tilt the device and watch the ball; lay it flat and look at `az` | The ball follows the tilt smoothly; when flat, `az` is close to 1.0g |  |
| 12 | Gyroscope values are sensible (drivers/qmi8658, newly written register configuration, check carefully) | Open hardware diagnostics with `diag`, keep the device still and then rotate it, and look at `gx/gy/gz` | Close to 0 dps when still; direction and magnitude match intuition when rotating; if an axis is reversed or the range is off, check the CTRL3 FS/ODR encoding in `drivers/qmi8658/qmi8658.c` |  |
| 13 | Battery level and charging state updates (spec 10.2, drivers/axp2101) | Open hardware diagnostics with `diag` and look at the battery percentage and the "Charging"/"Not charging" (充电中/未充电) state; plug and unplug USB power; you can also look at the `power.charging.changed` log line of `power_monitor` on the serial port (see item 35 below) | The percentage is in a sensible range; after plugging or unplugging, the charging state updates within 1 second |  |
| 14 | Events from the three buttons (spec 10.2, wisp_core::ButtonService) | Open hardware diagnostics with `diag` and short-press/long-press power, vol_up and vol_down in turn | The diagnostics screen shows the matching event for each |  |
| 15 | Short press of the power key (spec 10.3) | With the screen on, short-press the power key, then short-press it again | The first press turns the screen off (enters standby), the second turns it on again |  |
| 16 | Long press of the power key to power off (spec 10.3, drivers/axp2101 power_off, wisp_app::PowerKeyController) | With the screen on, hold the power key for about 1 second (without releasing) | The power-off confirmation dialog appears while it is held; the press held to power on at boot does not trigger it; after confirming, the whole device loses power (judge with an ammeter or an indicator light, not just by the screen going dark) |  |
| 17 | Power on from the off state (spec 10.3) | Long-press the power key while powered off | It powers on again |  |
| 18 | RTC keeps time (spec 10.2, drivers/pcf85063) | Check the boot log for "system clock set from clock.main RTC"; remove power (unplug USB and the battery), restart and check the time again. If the RTC has never been set (the chip's first power-up), `get_unix()` fails, the log has only one `WLOG_W`, and the system time is not set. In that case, connect to Wi-Fi first: once network time sync (connectivity spec 5.6) succeeds, the log shows `time: synced from <server>: unix <seconds>` and the RTC is written at the same time; then restart to verify this item | The log shows "system clock set from clock.main RTC"; after restarting the time is continuous and does not jump back to 1970 or a fixed value |  |
| 19 | Charge current profile (drivers/axp2101 charge parameter configuration) | With a clearly low battery, power the device through a USB power meter and watch the current during charging; also check whether the boot log shows `power.main: charging configuration failed: ...` (the `WLOG_W` in `board.cpp`) | The constant-current phase holds at about 400mA; the current tapers off near 4.2V; once full, `charging` flips to `false` and the current drops to a maintenance level, without a sustained high-current overcharge; no `charging configuration failed` log (if it appears, `axp2101_charging_config_status()` returned something other than `ESP_OK`, and the charge parameters may have stayed at the chip's factory defaults) |  |
| 20 | Audio/storage device states (wisp_port_esp::EspAudio/EspStorage) | Look at `audio_out.main`/`audio_in.main`/`storage.int`/`storage.sd` in `WISP_BOOT_REPORT` | The first three are `ok`; `storage.sd` is `ok` or `missing` ("no card") depending on whether a card is inserted |  |
| 21 | No pop at boot (display driver power-up sequence, wisp_port_esp::EspAudio initialisation) | With a speaker connected, listen at the moment of boot | No audible click or pop before the home screen appears |  |
| 22 | Mic test (spec 10.2, wisp_app::loopback) | Open hardware diagnostics with `diag` and tap the "Mic test" button ("录音回放" in the Chinese UI) | It records for 3 seconds and then plays back, at a suitable volume, with the correct sound, not distorted, not stuttering, not silent |  |
| 23 | Storage capacity and SD hot-plug (spec 10.2, wisp_port_esp::EspStorage/sd_retry) | Open hardware diagnostics with `diag` and look at the internal flash/SD card capacities; insert and remove the SD card while running | The capacity figures are correct; after insertion the card comes online within about 2 seconds with no restart; after removal it is detected as offline within a few seconds |  |
| 24 | NVS settings persistence (wisp_port_esp::NvsSettingsBackend) | Change a setting that is read at boot (such as brightness), then remove power from the whole device and restart (not a soft restart) | The changed value still applies after the restart |  |
| 25 | Internal storage degraded path (optional, wisp_port_esp::EspStorage degraded path) | If convenient, make `storage.int` fail to mount on purpose | The system still boots in degraded/read-only mode, without aborting |  |
| 26 | Entering/leaving standby and waking (spec 6.7/10.3, wisp_app::PowerManager/PowerKeyController) | With the screen on, short-press the power key to enter standby; press any key to wake. Then also test: with the power key confirmation dialog open, wait for the automatic screen-off to trigger, then wake with a short press and with a long press | Any key wakes from standby; with the dialog open both kinds of press wake it (the dialog may still be on screen), with no misbehaviour from the "wake" and "confirmation dialog" logic clashing |  |
| 27 | Wake latency (spec 10.5) | Time from pressing the power key until the panel actually lights again (stopwatch or logic analyser) | ≤ 300 ms (see the `DcsSleep` estimate in the "Standby and power saving" section below, about 200 ms, only an estimate) |  |
| 28 | Standby current (spec 10.5) | See the detailed measurement method in "Acceptance item 5" below | Average standby current ≤ 1/10 of the average current with the screen on and idle |  |
| 29 | No problems after repeated light sleep (wisp_app::PowerManager light sleep) | Wake after more than 10 minutes in standby (making sure several automatic light sleeps have happened) | No garbled panel and the brightness has not been reset unexpectedly; touch responds normally; no unusual pops or noise floor from the speaker (see item 1 of "Acceptance item 5" below) | Pass (2026-11-05, ruling A, see the "Light-sleep compatibility (3b-1 spike)" section above): run 2 was in standby for 10 minutes with `sleeps` at 751366, and the panel and touch were normal after waking; this assumes the audio I2S channels are closed during standby |
| 30 | IMU recovery after power-down (wisp_app::PowerManager standby, drivers/qmi8658) | Note an IMU reading before standby, and look again after waking | The readings update normally again; they are neither the last reading before standby nor all zeros/clearly wrong values (see item 4 of "Acceptance item 5") |  |
| 31 | SD probing pauses (wisp_port_esp::sd_retry pauses probing during standby) | With a card inserted, stay in standby for a few minutes and wake; optionally insert/remove the SD card physically during standby | After waking, `online()` is still correct; insertions and removals during standby are noticed only after waking (expected behaviour, not a bug, see item 5 of "Acceptance item 5") |  |
| 32 | Power use of the touch reset pin (optional measurement, standby power evaluation) | During standby, try pulling the touch reset pin (GPIO40) low and compare the current before and after | Only for evaluating whether a later power-saving item is worthwhile; it does not mean the code needs to change now (see item 6 of "Acceptance item 5") |  |
| 33 | UI frame rate baseline (spec 10 performance baseline) | Open hardware diagnostics with `diag`, keep scrolling the diagnostics list with a finger for 10 seconds, and look at `WISP_PERF` on the serial port | Target fps ≥ 30. Acceptance line (ruled by the user on 2026-11-04, see "Recorded deviations" in section 10 of the foundation spec): not below the current baseline recorded in the "Performance baselines" section below (on 2026-11-05 the user agreed to re-measure on the current hardware diagnostics screen (the former self-check screen), see "Current baseline" in that section): 18.3 fps with the hotspot off, 29.8 fps with the hotspot on (while the Wi-Fi setup card shows); measure with `bench scroll`, with finger scrolling only for reference; record the three values fps/cpu/flush_ms in that section |  |
| 34 | DFS does not hurt the frame rate (firmware/main.cpp apply_pm_config, DFS/light sleep) | Compare `WISP_PERF` with `CONFIG_PM_ENABLE` on and off | With DFS on, the frame rate is still not below the previous item's baseline |  |
| 35 | `power.charging.changed` event (spec 6.3, wisp_core::PowerMonitor) | Plug and unplug USB power and look at the `power.charging.changed: charging=... usb_present=... battery_percent=...` log line of `power_monitor` on the serial port | The log line appears within 1 second of plugging or unplugging, and the values of `charging`/`usb_present` match the real state |  |
| 36 | `power.battery.low` event (spec 6.3, wisp_core::PowerMonitor) | Watch the serial log when the battery is clearly low. Sub-project 1 has no CLI/REST yet to change Settings; to trigger it quickly, you can temporarily add a line `power_settings.set("low_batt_pct", int64_t{100});` after `core::Settings power_settings(*impl_->platform.settings, "power");` (around line 102) in `components/wisp_app/src/app.cpp`, rebuild the firmware, and delete the line once verified (do not commit it) | The `power.battery.low: battery_percent=... (threshold=...)` log line of `power_monitor` appears only once (not repeated continuously); after the level recovers above the threshold + 5 and then drops below the threshold again, it appears once more; with no battery and only USB power, the log does not appear |  |
| 37 | First full flash with the new partition table (connectivity spec 3.2) | On a board that already has settings and data (Wi-Fi set up, a browser paired, brightness changed, files in `/int`), run `idf.py -p <PORT> flash` | Boots normally; no partition-related errors on the serial port; the Wi-Fi, pairing, brightness and other settings and the `/int` data are all still there; the firmware card on the admin console's "System" page shows the version, `waveshare_esp32_s3_touch_amoled_2_16` and the running partition `ota_0` |  |
| 38 | Web upload and confirm (connectivity spec 6.5) | Change a little code and build, then upload `firmware/build/wisp.bin` on the "System" page; when the confirmation dialog appears on the device, tap "Yes" | After the progress bar finishes, the web page says "Confirm on the device"; the confirmation dialog shows old version → new version; after "Yes", the device restarts and the web page says the new version is running; the running partition on the firmware card changes to `ota_1`; no rollback log on the serial port |  |
| 39 | Rejecting after a web upload | Upload again and tap "No" on the device; upload once more and do nothing for 2 minutes | Both times the web page says the upload was declined on the device ("Declined on the device. Nothing was installed."); the device does not restart, and the version and running partition do not change |  |
| 40 | Rejecting another board's image | Build with `-DWISP_BOARD=<another board>` (or take any non-Wisp ESP-IDF app `.bin`) and upload it | The web page immediately says "This firmware is for another board or chip." (400 `wrong_board`); no confirmation dialog appears on the device, and flash is not written (uploading a correct image afterwards still works) |  |
| 41 | Automatic rollback on boot failure (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) | Temporarily add a line `abort();` at the start of `app_main()` in `firmware/main/main.cpp` and build (do not commit it), upload it and confirm on the device | After the new image crashes and resets, the bootloader goes back to the old image; the web page says the device came back running the old version and the new firmware was rolled back; uploading a normal image afterwards still installs |  |
| 42 | Release-build frame rate (global `-Os`, rendering path `-O2`) | Build and flash with the default (release) configuration and repeat item 33 | The fps of `WISP_PERF` is not below the acceptance line of item 33 (18.3 fps with the hotspot off, 29.8 with it on; the target is still 30). `-O2` for `esp_driver_spi` has been measured to bring no gain (see that section); no need to try it again |  |
| 43 | Log push during an upload | Open the "Logs" page while uploading firmware | Log push pauses during the upload (httpd has only one task, and the upload request occupies it), resumes after the upload ends, and the WebSocket does not disconnect |  |
| 44 | Internal RAM worst case (the budget of AGENTS.md section 5) | With the device connected to the home Wi-Fi, run `wifi ap on` on the serial port and open the setup page on a phone connected to the hotspot; on a computer, open the admin page's "Logs" page (WebSocket) through the STA address, run a connection test against a real HTTPS endpoint on the "Model" page, then upload firmware once; run `mem` every few seconds throughout | Both `admin server listening on port 8900` and `captive redirect listening on port 80` appear in the log, and the later `internal RAM: ...` lines are not warnings; the `internal free` of `mem` stays ≥ 40 KB and `largest` ≥ 16 KB (`min` is the sum of each memory region's own low point, which is too low, so only treat it as a lower bound); the stacks of `wisp.ws`, `wisp.dns` and the port-80 `httpd` are in `psram`, and the stack of the port-8900 `httpd` is in `internal`; STA never drops throughout and the log shows no repeated reconnects (the RX BA window is only 10, so take a look when changing routers too) | |
| 45 | Occlusion clipping for opaque cards (`wisp/ui/overlay.hpp`) | Show the Wi-Fi setup card with `wifi ap on`, open hardware diagnostics with `diag`, and scroll the diagnostics list up and down with a finger; then close the card with `wifi ap off` | No ghosting, tearing or missing blocks around or on the card while scrolling; after the card closes, the list content where the card used to be is complete and consistent with its surroundings | The user confirmed on 2026-11-04 that scrolling with the hotspot on shows no ghosting (the probing during rendering and the handling of hidden layers were fixed later; worth a second look) |
| 46 | Wi-Fi setup QR code (stored in the screen's pixel format, `flatten_qr()` in `net_cards.cpp`) | After `wifi ap on`, scan the QR code on the Wi-Fi setup card with a phone camera | Black and white are not inverted, and the phone recognises it and offers to join the hotspot | The user confirmed on 2026-11-04 that it scans |
| 47 | 8-bit glyph cache of the default font (`components/wisp_ui/src/a8_font.cpp`) | Look at the Chinese, Japanese and English text in hardware diagnostics (`diag`) and on the Wi-Fi setup card | Same as before the change: no missing or wrong characters, no blur or misalignment |  |
| 48 | Standby and wake with XIP on (`CONFIG_SPIRAM_XIP_FROM_PSRAM`) | On firmware with XIP on, repeat items 26–29 (entering standby and waking, wake latency, standby current, repeated light sleep) | Results match the expectations of items 26–29; no garbled screen, reset or hang after waking from light sleep | Pass (2026-11-05): the probe firmware kept this board's default configuration (XIP already on), see the "Light-sleep compatibility (3b-1 spike)" section above; same conclusion as item 29 |
| 49 | Network time sync (connectivity spec 5.6, `net::TimeSyncService`, `EspSntp` in `wisp_port_esp`) | After connecting to Wi-Fi, look at the serial log and the "Power & clock" card of hardware diagnostics (`diag`); then look at the `wisp.ntp` task with `mem`; tap "Sync now" on the admin console's "System" page; change `ntp_servers` to `down.invalid,ntp.aliyun.com` and sync once more | Within a few seconds of getting an IP, `time: synced from pool.ntp.org` appears (or it falls back to `ntp.aliyun.com`); the card shows "Synced HH:MM" (local time); the `wisp.ntp` stack is in PSRAM with enough high-water-mark margin (4096-byte stack), and `NetWiring::kNtpStackBytes` is adjusted from the measurement; idle internal RAM is not below budget; the second sync falls back to `ntp.aliyun.com` |  |
| 50 | Both install methods (scene spec 9.4.1) | Upload the `counter` zip on the web "Scenes" page; `python3 tools/wisp_scene.py --host <IP>:8900 push scenes/examples/todo --open`; open hardware diagnostics with `diag` and open both from the "Scenes" card | Both install successfully, and the push output shows the declared domains; tapping in the list opens them | 2026-11-05: pass. Both the API push and the web zip upload installed successfully (after the log line `admin: scene upload started`, the scene installed, opened and closed normally). |
| 51 | Example features and persistence across power loss (scene spec 9.4.2) | Open the 7 examples one by one; after adding 3 to-dos and 2 ledger entries, remove the battery and USB to cut power and restart | Features work; the data is still there after the restart |  |
| 52 | Back gesture and error panel (scene spec 9.4.3) | Start within 16 px of the rounded screen's left edge and swipe right by more than 24 px (at any angle, `ui::SystemLayer::kBackZoneWidth`/`kBackTriggerDx`, see `components/wisp_ui/include/wisp/ui/system_layer.hpp`); push `tests/scene/fixtures/broken` and open it, tap "Back", then open it again and tap "Reload" | The gesture works; the error panel shows the file and line number (`main.lua:5`), and Back and Reload work |  |
| 53 | Long-list frame rate and memory (scene spec 9.4.4) | Put 200 to-dos in, keep scrolling with a finger for 10 s and look at `WISP_PERF`; run `mem` at the same time; note the longest `slow message` duration in the log (GC pauses) | fps not below the hotspot-off baseline of the hardware diagnostics screen, 18.3 ("Current baseline" in the "Performance baselines" section below); `internal free` ≥ 60 KB, `largest` ≥ 20 KB (`kIdleInternalFreeMin`/`kIdleInternalLargestMin` in `ram_budget.hpp`) | 2026-11-05: with todo200 scrolled continuously, fps 22–26, occasionally dropping to 14; `internal free` ≥ 84 KB, above the 60 KB minimum; `slow message open` 115–196 ms. The brief drop to 14 fps is below the baseline and was not investigated further this time. |
| 54 | Recovery after screen-off and standby power (scene spec 9.4.5) | With the pomodoro and clock running, turn the screen off for 3 minutes and then on; measure the standby current by the method in the "Acceptance item 5: measured standby power" section and compare with no scene | Remaining time and dial are correct, and `minute` catches up only once; standby current is comparable to having no scene |  |
| 55 | Undeclared domains are refused (scene spec 9.4.6) | Temporarily change the weather scene's URL to an undeclared host, then push and open it | The callback receives `permission_denied`, and there is no outgoing connection |  |
| 56 | Frame rate with the LVGL heap in PSRAM | With no scene open, on the final firmware, first open hardware diagnostics with `diag` and repeat the "UI frame rate baseline" row, once with the hotspot off and once with it on (measured once on the device already after moving the LVGL heap to PSRAM, see the "LVGL heap" subsection of the "Performance baselines" section below) | `bench scroll` fps ≥ 18.3 with the hotspot off and ≥ 29.8 with it on ("Current baseline" in the "Performance baselines" section below); if lower, stop and report; do not change it back on your own | 2026-11-05: 22–24 fps with the hotspot off, meeting the line (baseline 22.7); 23–24 fps with the hotspot on, recorded at the time as below the 28.2 baseline and to be investigated. After the acceptance line was changed to ≥ 22.7 with the hotspot on, this result meets the line. |
| 57 | Internal RAM worst case re-measured (after all mbedTLS allocations moved to PSRAM, `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` in `firmware/sdkconfig.defaults`; the UI task stack has also grown from 8 KB to 10 KB) | Flash firmware that includes this change (after changing `sdkconfig.defaults`, delete `firmware/sdkconfig` before building, or it does not take effect; after building, `firmware/sdkconfig` should contain `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`). Use the same scenario as item 44: STA on the home Wi-Fi, `wifi ap on`, a phone on the hotspot with the setup page open, a computer with the admin page's "Logs" page (WebSocket) open through STA; then, at the same time: open the weather scene (HTTPS), run a connection test against a real HTTPS endpoint on the "Model" page, and upload firmware once. Run `mem` every 1–2 seconds throughout | Every `mem` shows `internal free` ≥ 40 KB (`kWorstInternalFreeMin`) and `largest` ≥ 16 KB; note the lowest `internal free` of all the samples and the final `min`. `internal free` is a snapshot at one moment and is the authority; `min` is the sum of each memory region's own low point, which is too low, and only for reference. If all samples are ≥ 40 KB but `min` < 40 KB, note both; this does not count as failing (when the exact low point is needed, you can temporarily wrap this sequence in the firmware with `heap_caps_monitor_local_minimum_free_size_start/stop(MALLOC_CAP_INTERNAL)`). Also confirm that the weather scene and the connection test both succeed (the handshake is slower but does not time out), and that STA does not drop. If the result is below 40 KB, stop and report | 2026-11-05 met (firmware `0.1.0-463-g9b24c06`, the merged theme branch, including 3a, the vector font and flash auto-suspend): STA on the home Wi-Fi, `wifi ap on`, a computer's wireless card on the hotspot fetching `setup.html` every 2 s (all 200), the computer subscribed to scene logs (WebSocket) through STA; at the same time, the weather scene was pushed and opened (HTTPS fetch succeeded), a connection test ran against a DeepSeek endpoint (with a fake key: a full TLS handshake and then HTTP 401, 2.2 s; the profile was deleted after the test), and 3.97 MB of firmware was uploaded once (202, then not confirmed and discarded on timeout). `mem` every 1.5 s, 51 samples in all: lowest `internal free` 62635 B, `largest` always 31744 B, both above 40 KB / 16 KB; the final `min` was 39732 B (the sum of each memory region's low point, too low, only for reference). The stacks of `wisp.ws`, `wisp.dns` and the port-80 `httpd` were in `psram`, and the port-8900 `httpd` in `internal` (lowest high-water mark remaining 3220 B); STA never disconnected. The connection test and the upload ran one after the other (the upload started 11 ms after the test ended), so they did not fully overlap. |
| 58 | Rendering vector glyphs during flash writes (see "Flash erase/write and mapped reads" above) | Confirm the boot log has `Flash suspend feature is enabled`; open a screen with large Chinese characters (or keep switching between scenes with large text), and upload firmware once through the admin console at the same time | No crash; the upload succeeds (202) and the new firmware runs after the restart; large text displays normally with no garbled glyphs | 2026-11-05: device OTA pass: while the "大字测试" (large-text test) scene was showing, `0.1.0-449-gc0be5e3` was uploaded through the admin console (202); no crash during the upload or the restart; after the restart the boot log had `Flash suspend feature is enabled` and `fonts: image ok`, and the user confirmed that the title was large vector text. Earlier stress tests with the temporary `fprobe` command (changing 128 px characters for 150 s + erasing and writing 46.9 MB in `ota_1` at the same time) also did not crash, and 24 passes of comparing the mapped area found 0 wrong bytes |
| 59 | A `wisp.sd` stack in PSRAM does not trigger the flash guard (flash-thread spec 3.9: `wisp.sd` only does SD card mount probing and declares `flash_access = false`) | Insert an SD card, and remove and insert it repeatedly to trigger the mount/unmount retries of `EspSdStorage` (see the "Storage capacity and SD hot-plug" item above); check the stack location of the `wisp.sd` task with `mem` at the same time | The `wisp.sd` stack is in `psram`; mounting, unmounting and retrying do not abort or trigger "flash access off the flash thread" | |
| 60 | Admin console httpd (8900) stack moved to PSRAM (flash-thread spec 3.6/3.9: OTA and Settings are both handed to `wisp.fs` through `run_sync`, httpd itself no longer touches flash, and the default of `EspHttpServerConfig::flash_access` changed to false). This changes the baseline recorded in items 44 and 57 of the "Internal RAM" section above (where the 8900 httpd stack was in `internal`) | Repeat the worst-case combination of items 44/57 (STA+AP, two httpd instances, WebSocket logs, a TLS handshake and an OTA upload all at once), check the location of the 8900 `httpd` stack with `mem`, and record the new `internal free`/`largest` | The 8900 `httpd` stack is in `psram` (no longer `internal`); uploads, settings reads and writes, and the Wi-Fi setup flow never trigger "flash access off the flash thread"; `internal free` should improve on item 57's baseline (by about 10 KB, as `stack_bytes` moves from internal RAM to PSRAM) | |
| 61 | Flashing the new partition table (glyph partition spec 5.1) | On a board that already has settings and large vector text (already on the table from item 37), run `idf.py -p <PORT> flash`, writing the new partition table, the app, `fonts.bin` and `glyphs.bin` | Boots normally; the Wi-Fi, pairing, brightness and other settings and the large vector text are all still there; `/int` has been reformatted (the scene list is empty); pushing an example scene again installs and opens it normally; the boot log has a line saying the `glyphs` image passed validation |  |
| 62 | Image size and total PSRAM (glyph partition spec 5.2) | Look at the Total image size from `idf.py size`; look at the PSRAM "total" (共) in hardware diagnostics (`diag`); record `boot_ms` of `WISP_BOOT_REPORT` before and after the table change (glyph partition spec 4.4: the CRC check of the new `glyphs` image is measured by it) | Image about 3.13 MB; PSRAM "total" about 5 MB; note both `boot_ms` values and their difference |  |
| 63 | Scrolling frame rate with the hotspot off and on (glyph partition spec 5.3) | First open hardware diagnostics with `diag`, then measure `bench scroll 10` once with the hotspot off and once with it on (Wi-Fi setup card showing) | fps not below the current baseline recorded in the "Performance baselines" section below |  |
| 64 | Body text unchanged (glyph partition spec 5.4) | Look at the Chinese body text on hardware diagnostics, the pomodoro, the to-dos and other screens | Same as before the table change: no missing or wrong characters, no blur or misalignment |  |
| 65 | Degradation with only the `glyphs` partition erased (glyph partition spec 5.5) | Flash only the app (`idf.py app-flash`), then `parttool.py erase_partition --partition-name=glyphs` | Boots normally; Chinese body text shows in vector 16 px; the boot log has a warning; after `idf.py fonts-flash` it goes back to bitmap body text |  |
| 66 | Scrolling body text during OTA (glyph partition spec 5.6) | Open a screen with Chinese body text and keep scrolling it, while uploading firmware once through the admin console | Displays normally with no crash; the upload succeeds and the new firmware runs after the restart |  |
| 67 | Stalls during flash writes without XIP (see "Code from flash" above) | Open hardware diagnostics with `diag`, start a firmware upload on the admin console's "System" page, keep scrolling with a finger during the upload, and do not confirm the new firmware; time the upload | Scrolling may stutter but the UI does not freeze for more than about a second at a time; no crash or watchdog; the upload returns 202; note the upload time against the 20.6 s of "Flash erase/write and mapped reads" | |
| 68 | Internal RAM worst case with the 32 KB instruction cache (16 KB more internal RAM) | Same scenario as item 57 | `internal free` stays ≥ 40 KB and `largest` ≥ 16 KB in every `mem` sample | |

## Standby and power saving (spec 6.7)

- **Display sleep (CO5300): standard MIPI DCS sleep instead of the vendor's deep standby (adopted with the user's confirmation)**:
  - The first implementation called `esp_lcd_panel_disp_on_off()`/`esp_lcd_panel_disp_sleep()` directly, in turn.
    But this board's `board_devices.yaml` gives `display_lcd` a `reset_gpio_num: 39` (the panel really is wired to a
    hardware RST), which makes the `espressif__esp_lcd_co5300` driver (`esp_lcd_co5300_spi.c`) send `DSTBON`
    (deep standby) **in addition** on `disp_sleep(true)`; on wake, `disp_sleep(false)` first hard-resets the panel
    (`panel_co5300_reset()`, 10 ms + 150 ms) and sets `needs_init` to `true`, forcing the whole initialisation
    sequence to run again. The first and last commands of `s_lcd_init_cmds[]` in this board's `setup_device.c`
    (`0x11` SLPOUT, `0x29` DISPON) each carry a 600 ms delay, so in total (160 + 600 + 600 = 1360 ms) one panel
    wake was expected to take **~1.36 s**, far beyond the acceptance item 5 target of "screen on within 300 ms of
    pressing the power key". Worse, the hard reset clears GRAM, and the initialisation sequence sends DISPON on
    line 45 (and then still waits 600 ms), so uninitialised GRAM is shown first (garbled or black screen).
  - It now uses `EspDisplayConfig::PanelSleep::DcsSleep` (`components/wisp_port_esp/dev/esp_display.cpp`, enabled by
    `kDisplaySleepOutSettleMs` in `boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp`): it never calls
    `esp_lcd_panel_disp_sleep()`/`disp_on_off()`, and instead uses `esp_lcd_panel_io_tx_param()` (the same path as
    `set_brightness()`) to send standard MIPI DCS commands directly. To sleep: `DISPOFF`(0x28) +
    `SLPIN`(0x10); to wake: `SLPOUT`(0x11) + a wait of `sleep_out_settle_ms` (120 ms on this board, the lower
    bound the MIPI DCS specification sets for this wait) + `DISPON`(0x29). These are standard DCS commands, with
    nothing private to the CO5300. Ordinary sleep does not clear GRAM, so after waking the last picture shows
    immediately, followed by a normal redraw, and no hard reset is triggered.
  - **Wake time estimate (not verified on the device)**: button polling delay (50 ms in standby) + `sleep_out_settle_ms`
    (120 ms) + a little command transfer time, expected to be **around 200 ms**, which should meet the 300 ms target.
    This is an estimate and **has not been measured on the device**; see the device verification steps in
    "Acceptance item 5" below.
  - Once `DcsSleep` is selected, `EspDisplay` never calls `esp_lcd_panel_disp_sleep()`/
    `disp_on_off()` again (that would conflict with the `sleep_mode` state they record internally). Deep standby
    (DSTB) is left for the "deep standby" level of spec 1.6, outside the scope of sub-project 1.
  - **Lost brightness**: whichever way the panel sleeps, its brightness register may be reset after waking (this
    board's init sequence writes `0x51` as `0xFF`; the `DcsSleep` path does not go through that init, but the
    safeguard is kept to be safe). `EspDisplay::set_brightness()` now remembers the last value set,
    and resends that brightness automatically once `set_power(true)` succeeds, so the user's setting is not reset
    to maximum brightness after every standby.
- **Touch sleep (CST9217)**: the `waveshare__esp_lcd_touch_cst9217` driver (see
  `esp_lcd_touch_cst9217.c`) assigns only three fields of `esp_lcd_touch_s`, `read_data`/`get_xy`/`del`;
  `enter_sleep`/`exit_sleep` are always null pointers. `esp_lcd_touch_enter_sleep()`/
  `exit_sleep()` themselves log `ESP_LOGE` and return `ESP_FAIL` when the field is null. To avoid an error log
  on every standby switch, `EspTouch::set_power()` checks these two function pointers directly, returns
  `ErrorCode::Unsupported` straight away when they are null, and never calls the two functions (see
  `components/wisp_port_esp/dev/esp_touch.cpp`). In other words, this board's touch chip has **no** hardware sleep:
  touch really does produce no more touch points in standby (because `UiRuntime` no longer calls `Touch::read()`
  once paused), but the CST9217's own power draw does not drop during standby.
- **GPIOs are isolated during automatic light sleep**: on the ESP32-S3, `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`
  automatically selects `CONFIG_PM_SLP_DISABLE_GPIO` (the Kconfig default dependency of
  `ESP_SLEEP_GPIO_RESET_WORKAROUND`; the generated `firmware/sdkconfig` really does have it as `y`). On every
  automatic light sleep, the direction, level and pull-ups/pull-downs of **all** GPIOs are disabled (floating) until
  the light sleep ends. Four outputs must keep their level during sleep: the panel RST (GPIO39), the touch RST
  (GPIO40), the amplifier enable (GPIO46) and **the panel SPI's CS (GPIO12, `io_spi_config.cs_gpio_num` in
  `board_devices.yaml`)**. If they float, the panel or touch may be reset unexpectedly (together with `DcsSleep`
  above, that would directly undo the very reinitialisation it is meant to avoid); a floating CS may also make the
  panel sample random levels on the bus during light sleep and take unrelated SPI data as commands meant for it;
  and a floating amplifier enable risks pops or leakage.
  `hold_outputs_during_light_sleep()` in `boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp` (called at the
  end of the board's `init()`) calls `gpio_sleep_sel_dis()` on each of these four pins, so they keep their
  configured level and direction during light sleep. **To be verified**: after many consecutive automatic
  light-sleep/wake cycles on the device (for example more than 10 minutes in standby, making sure dozens of
  automatic light-sleep/wake cycles have happened), confirm that the panel picture is normal (no garbling, no
  signs of an unexpected reset such as the brightness being reset; see "Lost brightness" above), that touch still
  responds normally, and that the speaker has no unusual pops or noise floor; see the verification steps in
  "Acceptance item 5" below.
- **USB-Serial-JTAG and automatic light sleep**: this board uses USB-Serial-JTAG as the console (there is no external
  UART-to-USB chip), and on the ESP32-S3 `SOC_USB_SERIAL_JTAG_SUPPORT_LIGHT_SLEEP` is not defined
  (`components/soc/esp32s3/include/soc/soc_caps.h`: so far this macro appears only in Kconfig comments of models
  such as `esp32h2`/`esp32c6` that have not landed yet; the ESP32-S3 does not support "keeping minimal USB
  communication during light sleep" at all). The `USJ_NO_AUTO_LS_ON_CONNECTION` option of
  `esp_driver_usb_serial_jtag`/Kconfig exists for exactly this kind of chip: with it on, as long as the
  USB-Serial-JTAG port is connected (the host has completed USB enumeration, judged from the SOF signal,
  regardless of whether a virtual serial monitor is open), the system automatically holds an
  `ESP_PM_NO_LIGHT_SLEEP` lock that prevents automatic light sleep; once the USB data cable is unplugged, the lock
  is released and automatic light sleep can really happen. `firmware/sdkconfig.defaults` already has the three
  items `CONFIG_PM_ENABLE=y`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` and `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y`.
  The conclusion: **while connected over USB for debugging, it does not need to and will not enter light sleep
  (whether the serial monitor is open or not makes no difference to this lock); only on battery power, or with
  USB power but no data connection (even a charge-only USB cable with no data pins), is there no SOF, so the lock
  is released and it really sleeps**. The current measurement of acceptance item 5 must really have the data
  cable disconnected (the data cable itself unplugged, not "just no serial monitor open").
- **Tasks that keep running in the background and limit the depth/frequency of light sleep** (grouped below by
  "behaviour during standby"; this records the current state and does not mean they need changing now):
  - **Fully paused during standby, waking only when really woken by `give()`**:
    - The IMU-reading branch of the self-check screen's sampling task (`SelfCheck::sample_loop()`, which originally
      read the IMU with a period of `kSampleTaskPeriodMs` = 20 ms): it subscribes to `power.state` and stops
      reading the IMU completely in standby. This is the fastest periodic wake source in the system (20 ms,
      faster even than ButtonService's 50 ms), and it is eliminated completely in standby.
      `PowerManager::exit_standby()` calls `give()` on waking to resume sampling immediately. The "slow sampling"
      branch for the power state/RTC does **not** stop completely; see the next group.
    - `wisp::hal::Imu::set_power(false)`: `PowerManager::enter_standby()` clears the QMI8658's accelerometer/
      gyroscope enable bits (`CTRL7`), so the chip itself stops sampling during standby, and
      `exit_standby()` restores the previously selected ODR.
    - The SD card probing worker of `EspSdStorage` (`components/wisp_port_esp/dev/esp_storage.cpp`) originally
      polled at a fixed granularity of `kWorkerPollMs` = 100 ms (in the `Mounted` state it probed whether the card
      was still there about every 1 s, and in the `Unmounted` state it retried the mount every 2 s, but the loop
      itself woke every 100 ms to check the state/`worker_running_`, i.e. it woke the CPU at 10 Hz). It is shut
      off during standby through a new `set_standby()` (forwarded by `StorageManager::set_standby()`, which
      `PowerManager` calls in `enter_standby()`/`exit_standby()`): the worker blocks completely on its own
      semaphore (an `os::kForever` wait with no timeout; `set_standby(false)` and the destructor both guarantee a
      `give()`, so no signal can be missed), and no longer probes or retries. `online()`/`capacity()` are not
      affected and keep returning the cached last state, so they never hang or report errors during standby. In
      the `Mounted` state, the wait between probes also checks the standby flag, so if standby starts during the
      wait, that probe is skipped outright and no extra CMD13 is sent.
  - **Slowed to about once every 30 s rather than stopped completely (as spec 6.7 literally requires)**:
    - The power state/RTC "slow sampling" branch of the self-check screen's sampling task (originally once every
      `kSlowSampleIntervalMs` = 1000 ms): during standby it reads once every `kStandbySlowSampleMs` = 30000 ms
      (blocking on `standby_wake_`, reading once on timeout, and skipping straight on when woken by `give()`); on
      leaving standby it likewise calls `give()` immediately to resume the original once-a-second rhythm. The
      values this branch reads are used only for the self-check screen display (and the screen itself has
      already stopped rendering).
    - The sampling timer of `wisp::core::PowerMonitor` itself (`"wisp.pwrmon"`, see spec 6.3/6.7):
      polls `power.main` once every 1 s with the screen on and every 30 s in standby (at most 5 I2C reads each
      time; the `low_batt_pct` threshold is cached and NVS is reread only once every 20 samples (or on every
      `start()`), not on every sample), and publishes `power.charging.changed`/
      `power.battery.low`. This is a real consumer: `power.battery.low` is published once when the battery drops
      below Settings `power`/`low_batt_pct` (default 15) (re-arming with hysteresis), and both events log a line
      with `WLOG_I` when published, so they can be watched directly on the serial port.
  - **Still waking the CPU on their original periods during standby (kept on purpose, see the explanation below)**:
    - Under ESP, `wisp::os::Timer`s all run on the same esp_timer task: `App`'s `wisp.storage` (once every
      2000 ms, `StorageManager::refresh()`) and `wisp.power` (once every 1000 ms, `PowerManager::tick()`) keep
      waking periodically even during standby. This is a deliberate ruling, not an oversight: the unified
      peripheral power-saving design of `2026-11-01-foundation-design.md` explicitly requires that "StorageManager's
      2 s status refresh stays unchanged, but does not trigger SD detection". `refresh()` itself only reads the
      cached flag `sd_dev_->online()` once (what has been paused above is the real SD probing itself, not this
      `refresh()` call), and changing it would violate that ruling. `wisp.power`'s `tick()` returns on its very
      first line when `standby_` is true (see `components/wisp_app/src/power_manager.cpp`), so during standby the
      cost of this 1 Hz timer waking the CPU is already the smallest of these background sources; lengthening its
      period further is expected to bring a negligible gain in measured standby current, so under the "minimal
      change" principle it was left alone.
    - The self-check screen's performance report timer (`"wisp.perf"`, `kPerfReportMs` = 5000 ms,
      `SelfCheck::emit_perf_line()`) is **not** paused by the `power.state` subscription above: it and the
      sampling task are two separate `wisp::os::Timer`/tasks. During standby it still wakes once every 5 s and
      prints a `WISP_PERF` line as usual, only with `fps`/`frames`/`flush_ms` at 0 (because no frames are rendered
      once `UiRuntime` is paused). This is expected behaviour, not a bug: the `WISP_PERF` log during standby is
      itself one of the signals that confirm "nothing is really being rendered"; if at device acceptance the
      standby log shows a `WISP_PERF` `fps` other than 0, that means the pausing of rendering has regressed.
    - `core::ButtonService` polls the buttons once every 50 ms in standby (the interval widened by
      `PowerManager::enter_standby()`); once paused, `ui::UiRuntime` **no longer** wakes up periodically on its
      own: its standby loop now waits indefinitely for `post()` jobs, and when there is nothing to do it never
      wakes up of its own accord.
  - At present, the **50 ms** polling of `core::ButtonService` is the shortest periodic wake source in the system
    (the 20 ms IMU branch of the self-check sampling task has been eliminated; the power/RTC slow sampling branch
    and `wisp.pwrmon` have both dropped to 30 s; the SD worker does not wake on its own at all during standby).
    All of these only "wake the CPU briefly on a period and then go back to sleep"; they do not prevent automatic
    light sleep itself from happening (there is no resident `ESP_PM_NO_LIGHT_SLEEP`/
    `ESP_PM_CPU_FREQ_MAX` lock), but they limit the longest continuous stretch of light sleep to the order of
    `ButtonService`'s 50 ms, slightly raising the average power. If the measured standby current on the device is
    clearly higher than expected, these are still the first sources to investigate.

### Light-sleep compatibility (3b-1 spike)

Date: 2026-11-05. The probe firmware is on the throwaway branch `spike/light-sleep` (not merged), with HEAD at
`3cc43ec`; run 1 used the branch's earlier commit `64ce0dd`, and runs 2 and 3 used `3cc43ec` (i.e. with
"close the I2S channels during standby" added).

**Run 1** (I2S kept open, on battery for 10 minutes): `light_sleep_counts` was 0, so it never
entered light sleep at all. `pm_locks` had two `i2s_driver APB_FREQ_MAX` locks, held 100% of the time:
esp_board_manager's `periph_i2s` opens the TX and RX channels of I2S0 at boot and keeps them open,
and these two locks blocked all automatic light sleep. Apart from this one item, everything passed: `psram_ok` was
true, `timer_missed` 0, maximum lateness 0.05 ms, `rtc_drift_ms` 1758, `wifi_reconnect_ms`
5501; the picture and touch were normal after waking.

**Run 2** (audio I2S channels closed during standby): raw JSON (one `WISP_SPIKE` line):

```
WISP_SPIKE {"battery_end":100,"battery_start":100,"i2s_off_err":"rx=ESP_OK tx=ESP_OK","i2s_on_err":"tx=ESP_OK rx=ESP_OK","interrupted":false,"minutes":10,"pm_locks":"Time since boot up: 645307580 us\nLock stats:\nName            Type            Arg    Active    Total_count    Time(us)        Time(%) \nmpi_cpu         CPU_FREQ_MAX    0      0         14             5166            1  %\nmpi_sleep       NO_LIGHT_SLEEP  0      0         14             5164            1  %\nwifi            APB_FREQ_MAX    0      0         337            8552606         2  %\nsdmmc           APB_FREQ_MAX    0      0         83             37382           1  %\nspi_master      APB_FREQ_MAX    0      0         1766           771117          1  %\ni2s_driver      APB_FREQ_MAX    0      0         1              45106293        7  %\ni2s_driver      APB_FREQ_MAX    0      0         1              45109157        7  %\nI2C0            NO_LIGHT_SLEEP  0      0         7753           3333635         1  %\nusb_serial_jtag NO_LIGHT_SLEEP  0      0         48             26287575        5  %\nrtos1           CPU_FREQ_MAX    0      0         135180         26275935        5  %\nrtos0           CPU_FREQ_MAX    0      1         142031         30117165        5  %\n\nMode stats:\nMode      CPU_freq    Time(us)    Time(%)   \nSLEEP     80 M        568030455   88%\nAPB_MIN   80 M        28290       0 %\nAPB_MAX   80 M        23033955    3 %\nCPU_MAX   240M        54213633    8 %\n\nSleep stats:\nlight_sleep_counts:15880  light_sleep_reject_counts:3\n","psram_ok":true,"reset_reason":"usb","rtc_drift_ms":639,"run_s":600,"sleep_ratio":0.879084954,"sleep_ratio_reported":0.869085396,"sleeps":751366,"standby_entered":true,"timer_fires":600,"timer_max_late_ms":0.21,"timer_mean_late_ms":0.16914,"timer_missed":0,"uptime_end_s":651,"wifi_reconnect_ms":5503,"wifi_stop_err":"ESP_OK"}
```

After waking (after the 10-minute automatic screen-off had triggered), the panel and touch were both normal (checked by eye by the user).

**Run 3** (woken with the power key at about 3 minutes): raw JSON:

```
WISP_SPIKE {"battery_end":100,"battery_start":100,"i2s_off_err":"rx=ESP_OK tx=ESP_OK","i2s_on_err":"tx=ESP_OK rx=ESP_OK","interrupted":true,"minutes":10,"pm_locks":"Time since boot up: 339262644 us\nLock stats:\nName            Type            Arg    Active    Total_count    Time(us)        Time(%) \nmpi_cpu         CPU_FREQ_MAX    0      0         14             5183            1  %\nmpi_sleep       NO_LIGHT_SLEEP  0      0         14             5179            1  %\nwifi            APB_FREQ_MAX    0      0         337            8898276         3  %\nsdmmc           APB_FREQ_MAX    0      0         83             37069           1  %\nspi_master      APB_FREQ_MAX    0      0         209            82976           1  %\ni2s_driver      APB_FREQ_MAX    0      0         1              45105280        14 %\ni2s_driver      APB_FREQ_MAX    0      0         1              45108117        14 %\nI2C0            NO_LIGHT_SLEEP  0      0         7457           3185981         1  %\nusb_serial_jtag NO_LIGHT_SLEEP  0      0         47             28849385        9  %\nrtos1           CPU_FREQ_MAX    0      1         78864          14296463        5  %\nrtos0           CPU_FREQ_MAX    0      1         85722          21982219        7  %\n\nMode stats:\nMode      CPU_freq    Time(us)    Time(%)   \nAPB_MIN   80 M        127265      0 %\nAPB_MAX   80 M        24359624    7 %\nCPU_MAX   240M        34747379    10%\n","psram_ok":true,"reset_reason":"usb","rtc_drift_ms":-77,"run_s":294,"sleep_ratio":0.911800832,"sleep_ratio_reported":0.905153316,"sleeps":245634,"standby_entered":true,"timer_fires":294,"timer_max_late_ms":0.213,"timer_mean_late_ms":0.122802721,"timer_missed":0,"uptime_end_s":349,"wifi_reconnect_ms":10302,"wifi_stop_err":"ESP_OK"}
```

`interrupted` is true (interrupted early by the power key). The user's observation: after a short press of the power key the screen lit up "instantly", and the picture and
touch were both normal.

**Item c (USB serial reconnects without a reset)**: cannot be judged. When the serial tool on the controlling side opens the port it asserts DTR/RTS,
which resets the board, so the `reset_reason` read at that moment is always `usb`. Whether the run itself was
continuous can be judged only indirectly, from the relation of `uptime_end_s` to `run_s` plus the waiting delay, not from
`reset_reason`. Only the observation is recorded; per Step 4 of the plan, this item does not affect the ruling.

**Ruling table** (against the pass lines of Step 4):

| Item | Conclusion |
|---|---|
| Really entered light sleep | Run 2: `sleeps` 751366, `sleep_ratio` 0.879 ≥ 0.5, pass (run 1's 0 was caused by the I2S locks; the cause was found and removed in run 2) |
| PSRAM | `psram_ok` true in both runs 2 and 3, pass |
| Display, touch | b and d both normal by eye; the wake latency was observed as "instant", taken as ≤ 300 ms, pass |
| Timer accuracy | `timer_missed` 0 in both; `timer_max_late_ms` far below 50 ms; `rtc_drift_ms` 639 and -77, both ≤ 2000 ms, pass |
| No reset | `reset_reason` being `usb` was caused by the serial tool reopening the port, not by a reset during the run; the relation of `uptime_end_s` to `run_s` and the waiting delay shows the run was continuous, pass |
| Wi-Fi recovery | `wifi_reconnect_ms` 5503 (run 2) and 10302 (run 3), both ≤ 15000, pass |
| USB serial reconnection | Cannot be judged (see the previous paragraph); does not affect the ruling |

**Final ruling: A**, automatic light sleep is enabled in deep standby.

**Precondition**: the audio I2S channels must be closed during Standby/DeepStandby; otherwise the
`i2s_driver APB_FREQ_MAX` locks are held 100% of the time and prevent any light sleep (run 1 is the counterexample).

## Internal RAM

For the budget and rules, see AGENTS.md section 5. The data below was measured with the serial command `mem` (ESP-IDF v6.0.3, release build). `largest` is the largest allocatable block IDF reports; `min` is the sum of each memory region's own low point, which is too low, so only treat it as a lower bound.

**Problem**: running `wifi ap on` with no saved network, the redirect on port 80 came up, but the admin server on port 8900 did not (`httpd_start: ESP_ERR_HTTPD_TASK`). At that point internal RAM had 13.5 KB free, with a largest block of 7.7 KB, which cannot hold the 10 KB httpd stack. The main users of the internal heap after boot:

| User | Size |
|---|---|
| LVGL partial refresh buffers 2 × 480 × 48 × 2 B (`Mem::Dma`) | 90 KB |
| Wi-Fi static RX/TX buffers, 16 each (IDF's default with PSRAM enabled) | 54 KB |
| `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` reserved pool (only for DMA and task stacks, not wasted) | 32 KB |
| Task stacks (`wisp_ui` 8 KB, `wisp.console`/`wisp.net`/`wisp.bus` 6 KB each, `esp_timer`/`wifi` 6.5 KB each, and so on) | About 75 KB |

At the time `hal::Display::flush()` waited for the transfer to complete before returning, so LVGL could never draw into the second buffer while transferring the first, and the second buffer (46 KB) was completely useless. (Later, flush became asynchronous and it went back to two buffers of 24 lines each, still 46 KB in total; see "Performance baselines" below.)

**Changes**: LVGL uses only one 48-line buffer; the Wi-Fi static RX/TX buffers became 10/10 and the RX BA window 10 (`firmware/sdkconfig.defaults`); `wisp.ws`, `wisp.dns` and the port-80 httpd do not touch flash, so their stacks moved to PSRAM.

**Which task stacks can go in PSRAM** (for the rule, see AGENTS.md section 5): when ESP-IDF does a flash operation, it stops the cache and asserts that the current task's stack is in internal RAM (`spi_flash/cache_utils.c`), for reads and writes alike, and also when called indirectly through NVS, LittleFS or `esp_partition_read`. With XIP enabled on this board (`CONFIG_SPIRAM_XIP_FROM_PSRAM`), `esp_flash` reads and writes only take a mutex and no longer stop the cache (`SPI_FLASH_CACHE_NO_DISABLE` in `spi_flash/spi_flash_os_func_app.c`), but mapping and unmapping flash (`esp_partition_mmap`, verifying the image at the end of an OTA) still freeze the cache and do the same stack check (`esp_mm/esp_cache_utils.c`). So the rule is not relaxed by XIP: XIP is only a board-level configuration, and the rule holds for every board. PSRAM stacks are used only when `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` is on (fixed to y in `firmware/sdkconfig.defaults`); with it off, they automatically fall back to internal stacks.

The three tasks checked: `wisp.ws` only hands queued frames to the admin httpd to send through `httpd_queue_work()`; `wisp.dns` only sends and receives UDP; the port-80 httpd only builds a 302 from the hotspot state in memory. None of the three reads Settings or files, and their logs go only to the in-memory ring buffer and the serial port. The web asset pack is linked into the app image and read through the cache (in PSRAM with XIP on), without triggering a flash operation, and it is served only by the port-8900 admin httpd.

When either httpd instance stops (hotspot closed, STA disconnected), the httpd task deletes itself with `vTaskDeleteWithCaps()`, which needs a temporary helper task with an internal stack (about 2 KB), and aborts if creating it fails (ESP-IDF's `idf_additions.c`). The worst-case minimum of 40 KB already includes this margin.

| State (idle, services started) | Before the changes | After the changes |
|---|---|---|
| After boot, no network | 36.3 KB (largest block 21.5 KB) | 105.0 KB (31.7 KB) |
| Hotspot only (two httpd instances) | 13.5 KB (7.7 KB), admin server fails to start | 75.9 KB (31.7 KB) |
| STA only (admin httpd + mDNS) | 62.6 KB¹ | 85.1 KB (31.7 KB) |
| STA + hotspot | 41.1 KB¹ (26.6 KB) | 70.5–71.9 KB (31.7 KB) |

¹ Measured after only changing LVGL to one buffer.

Lowest internal RAM during various operations with STA + hotspot (`min` lower bound, after the changes): pairing 58 KB; scanning 58 KB; WebSocket log stream plus a TLS handshake 51 KB; WebSocket plus an OTA upload of 3.18 MB about 60 KB; the phone-side setup flow (302 redirect, `setup.html`, `/pair/ap`, scan, WebSocket) 51 KB. TLS was measured only up to the certificate verification failure: the firmware enables only TLS 1.2, and a rejected certificate aborts the handshake before ECDHE and signature verification, so a full handshake is estimated to use a few KB more, still above the 40 KB budget. For the connection test against a real endpoint, see item 44 of the checklist above.

Cost: an OTA upload of 3.18 MB went from 15.1 s to 17.5 s (fewer RX buffers). Scrolling the self-check list by program (6 px per frame) gave 8–9 fps both before and after the changes, for reasons unrelated to this change; see "Performance baselines" below. An RX BA window of 10 is below the 16 that IDF recommends for boards with PSRAM (the help of `CONFIG_ESP_WIFI_RX_BA_WIN` says 16 is better for throughput and for compatibility with various APs); so far it has been tested only on a hotspot opened by a computer's network card, and some routers may see throughput collapse or repeated reconnects with a small window; see item 44 of the checklist.

**After switching to asynchronous double buffering and XIP** (the final configuration of "Performance baselines" below): 103.3 KB after boot (largest block 31.7 KB), 73.5–74.0 KB with the hotspot only (31.7 KB), back to 101.5 KB after closing the hotspot; 102–105 KB while scrolling (`heap_internal_free` of `WISP_PERF`). About 2 KB less than "after the changes" in the table above: `LV_OBJ_STYLE_CACHE` about 0.8 KB, XIP about 0.6 KB, and the remaining 0.6 KB or so not broken down. The `min` lower bound while opening the hotspot is 40.0 KB (41.7 KB before). STA + hotspot was not re-measured (no real network connected); by extrapolation from the table above it is about 68–70 KB.

**After the rendering optimisations** (QR code stored in screen format, skipping areas covered by cards, 8-bit glyph cache, SIMD; the glyph cache and the QR code are in PSRAM): 103.1 KB after boot (largest block 31.7 KB, `min` 56.3 KB), 76.9–79.6 KB with the hotspot only (31.7 KB), and a `min` lower bound of 40.9–41.6 KB while opening the hotspot.

**After adding the scene runtime** (2026-11-04, firmware e9e11a7, STA only, idle, no scene open): 92.6 KB of internal RAM free (`min` 58.3 KB), against 97.5 KB (`min` 62.8 KB) before, a net 4.9 KB less (`min` a net 4.5 KB less). This is the net of two changes: the slots of `os::Queue` moved into PSRAM at the same time (event bus 64 × 80 B, UI post queue 32 × 16 B, about 5.6 KB); taking that into account, the scene subsystem itself uses about 10 KB of internal RAM, broken down in scene spec 2.3. Stacks of the new tasks: `wisp.fs` 4093 B, in internal RAM (it reads and writes flash), with 1532 B of high-water mark remaining; `wisp.shttp` 8181 B and `wisp.scene` 131057 B, both in PSRAM. The worst case (STA + hotspot, two httpd instances, WebSocket logs, a TLS handshake and an OTA upload at the same time) was not re-measured on this version.

**`wisp.fs` stack raised from 4 KB to 6 KB** (2026-11-05, measured on firmware 5fab9ac with a temporary 12 KB stack): with one scene installed, listing the scenes at boot parses each scene's manifest on this thread, and the 4 KB stack overflowed, showing up as a reboot about 3 seconds after boot, over and over (the log shows `stack overflow in task wisp.fs`, or the overflow corrupts a neighbouring queue and it becomes `Interrupt wdt timeout`). Measured with the 7 example scenes installed: listing the scenes at boot used 4.3 KB, and the peak after installing and opening ledger and weather was 4.5 KB, so it was set to 6 KB, leaving about 1.7 KB of margin. In the same measurement, during a scene install plus weather's HTTPS request, the internal RAM `min` dropped to 39.7 KB (with `wisp.fs` on the temporary 12 KB stack at the time; with 6 KB that is equivalent to about 45.7 KB).

**How much making STA and the hotspot mutually exclusive would save**: hotspot only has just 5.4 KB more than STA + hotspot (the 4 KB mDNS task stack plus the DHCP client and association state), and STA only has 14.6 KB more than STA + hotspot. The hotspot part itself: softAP driver, netif and DHCP server about 10.6 KB; the port-80 httpd about 3 KB (its stack has moved to PSRAM; 7.1 KB before the move); the DNS task 0.4 KB (3.4 KB before the move). The hotspot cannot be left off during Wi-Fi setup, so mutual exclusion could lower the peak by only about 5 KB.

**Vector font (2026-11-05, measured on the feasibility branch `spike/vector-font`; the code was discarded and only the conclusions kept)**: after the `wisp_ui` task stack was raised from 8 KB to 10 KB, the high-water-mark margin measured with the `mem` command dropped from 1208 B to 808 B: stb_truetype's rasterisation uses about 0.5 KB of scanline buffer on the stack. After `esp_partition_mmap()` of the whole `fonts` partition, internal RAM grew by only about 80 B (the bookkeeping of the mapping itself, not the 2.81 MB that is mapped, which lives in the data address space and takes no heap). Both numbers are one-off measurements on the spike branch, not standing budgets of this firmware version; the formal measurement with the vector font in place (the worst case of STA + hotspot, two httpd instances, WebSocket logs, a TLS handshake and an OTA upload at the same time) is left to device acceptance (this document's "Device acceptance checklist").

**Vector font device acceptance (2026-11-05, firmware `40d24f4`/`c09a2cc` after merging the theme branch, which includes the vector font and 3a; STA on the home Wi-Fi, 12 scenes installed in internal flash)**:

| State | Internal RAM free | Largest block | `min` | `wisp_ui` stack remaining (10 KB stack) | PSRAM free |
|---|---|---|---|---|---|
| Idle after boot | 86679 B | 31744 B | 51960 B | 3412 B | 4.04 MB |
| Pomodoro has run, "大字测试" (large-text test: 128 px Chinese characters, mixed xxl text, icons) open | 84283 B | 31744 B | 43680 B | 2484 B | 3.89 MB |

Both states meet `ram_budget.hpp`'s free ≥ 60 KB and largest block ≥ 20 KB, with `wisp_ui` remaining above 1024 B. This version runs only one scene at a time, and the pomodoro had already closed when "大字测试" was opened, so the second row is sequential, not simultaneous.

- **Boot**: `boot_ms` 2411–2418; with the font partition erased (image validation fails when reading the magic) it is 2219, so the CRC check of the whole image takes about 195 ms. Mapping the partition itself takes only 42–50 µs.
- **Cold rendering**: the first time "大字测试" was opened, the scene's `open` message took 67 ms, and the second time (glyphs already cached) also 67 ms. The existing logs give no per-frame time, so the time of the first rasterisation of 128 px glyphs was not measured separately; the user saw no pause.
- **Frame rate**: see "Current baseline" in the "Performance baselines" section (this version is the firmware the baseline was measured on).
- **Degradation**: after erasing the `fonts` partition the log says `fonts partition holds no font image; large text uses bitmap fonts`; after writing only the first 1 MB of the image (truncated) it says `font image CRC mismatch; large text uses bitmap fonts`. Neither case crashes; large text falls back to 16 px, and icons are still large (confirmed by the user). After `idf.py fonts-flash` it is back to `fonts: image ok`.
- **OTA leaves the fonts alone**: after uploading an app image through the admin console and confirming on the device, it is still `fonts: image ok` after the restart, and the title is still large vector text. This acceptance also exposed the crash when reading the mapped area during flash erases and writes; see "Flash erase/write and mapped reads" above and item 58 of the checklist.
- **Image size**: the Total image size of `idf.py size` is 3,967,023 B, 75.7% of `ota_0` (0x500000). The vector font itself made the image a net 55 KB smaller: adding Tiny TTF and stb_truetype added 19.5 KB, and removing Montserrat 24/32/48 saved 72.3 KB. The 3,169,136 B the plan compared against is from older firmware; since then 3a and other changes had grown the image to about 4.01 MB.

**After adding the agent and voice** (2026-11-07, feat/agent-core firmware `36ba2a8`, STA only; ASR is SiliconFlow's SenseVoiceSmall, TTS is CosyVoice2, LLM is DeepSeek):

| State | Internal RAM free | Largest block | `min` | `wisp.voice` / `wisp.play` / `wisp.agent` stack remaining |
|---|---|---|---|---|
| Idle after boot | 99527 B | 31744 B | 53696 B | 7304 / 3260 / 15440 B (8 / 4 / 16 KB stacks, all in PSRAM) |
| Speaking the first sentence while the LLM is still streaming (two TLS connections, TTS and LLM, online at once) | 85251 B | 31744 B | 49088 B | 3064 / 2812 / 10112 B |
| Speaking later in the same turn (only TTS left) | 89111 B | 31744 B | 43460 B | 3064 / 2812 / 10112 B |

- All three states are above the worst-case minimums of `ram_budget.hpp` (40 KB / 16 KB), and `overlap_first_sentence` keeps its default of true.
- At boot, registering the agent's tools once took 54 KB of internal RAM (`std::string`, `Value` trees and `std::map` nodes are all small allocations and land in internal RAM, see AGENTS.md section 8), leaving only 43 KB free; after the tool table moved into PSRAM (`36ba2a8`) it went back to 99.5 KB.
- Latency (one measurement, holding to talk for 5.8 s): ASR 3.6 s; first LLM output about 6.3 s; the first sentence's TTS took about 4 s from request to sound, and synthesising a whole 52-character sentence took 11.9 s (streamed, played while receiving).
- When the microphone is opened after the speaker has played 24 kHz TTS, the recording has shown a near-full-scale transient (peaks clipped, single-frame RMS up to 11000–15000, against about 600–1000 for normal speech). Push-to-talk runs no VAD, and the first 100 ms after opening the microphone are discarded (`kMicSettleMs`).

### Scene runtime

Besides item 53 of the device acceptance checklist, there are further measurement steps handed to the user to run on the device (scene spec 2.3): run the serial command `mem` once in each of the following four states, and record `internal free`/`largest`/`min`, plus the stack size and high-water mark of the four tasks `wisp.scene`, `wisp.fs`, `wisp.shttp` and `wisp.ntp`:

(a) idle after boot, no scene open;
(b) the to-do scene open and scrolling 200 entries;
(c) while the weather scene makes an HTTPS request;
(d) STA + hotspot + the web logs page + uploading a scene zip.

Compare the internal RAM increase of (b)–(d) over (a) with the ≤ 2 KB expected in spec 2.3, and with the net value already measured in the "After adding the scene runtime" paragraph above (adding the scene runtime itself is a net 4.9 KB less, because the `os::Queue` slots moved into PSRAM at the same time). After measuring, adjust `*StackBytes`/`stack_bytes` of the three tasks in PSRAM from the high-water mark plus a margin; `wisp.fs` has already been adjusted the same way from 4 KB to the current 6144 B (see the "`wisp.fs` stack raised from 4 KB to 6 KB" paragraph above). After adjusting, update `docs/architecture.md` to match.

Expected locations and configured stack sizes of the four tasks (checked against `task_config()`/`TaskConfig` in the code; tasks with `flash_access = true` have their stacks in internal RAM, those with `false` in PSRAM):

| Task | Expected location | Configured stack size | Basis |
|---|---|---|---|
| `wisp.scene` | PSRAM | 131072 B | `flash_access = false` in `SceneRuntime::task_config()` (`components/wisp_scene/src/scene_runtime.cpp`); the size is `SceneRuntimeConfig::stack_bytes` (`components/wisp_scene/include/wisp/scene/scene_runtime.hpp`), set from the recursion depth of `string.gsub` under Lua's `LUAI_MAXCCALLS` |
| `wisp.fs` | Internal RAM | 6144 B | `flash_access = true` in `StorageWorker::task_config()` (`components/wisp_core/src/storage_worker.cpp`) (it reads and writes flash); the size is `StorageWorkerConfig::stack_bytes` (`components/wisp_core/include/wisp/core/storage_worker.hpp`) |
| `wisp.shttp` | PSRAM | 8192 B | `flash_access = false` in `SceneHttp::task_config()` (`components/wisp_net/src/scene_http.cpp`) |
| `wisp.ntp` | PSRAM | 4096 B | `flash_access = false` of `os::Task::spawn()` in `NetWiring` (`components/wisp_app/src/net_wiring.cpp`); the size is `kNtpStackBytes` (`components/wisp_app/src/net_wiring.hpp`) |

Measured results (2026-11-05, device acceptance, firmware `0.1.0-387-gac2c5d0-dirty`; stack size and high-water mark of each task in the four states (a)–(d), in B; three more tasks, `wisp_ui`/`httpd`/`wisp.ws`, were recorded because this round also caught a new low for `wisp_ui`):

| Task | Configured stack size | (a) HWM | (b) HWM | (c) HWM | (d) HWM |
|---|---|---|---|---|---|
| `wisp.scene` | 131072 | 130140 | 124892 | 124892 | 124892 |
| `wisp.fs` | 6144 | 1752 | 1688 | 1688 | 1688 |
| `wisp.shttp` | 8192 | 7344 | 7344 | 3664 | 3664 |
| `wisp.ntp` | 4096 | 1620 | 1620 | 1620 | 1620 |
| `wisp_ui` | 8192 (at the time of measurement)¹ | 1120 | 480 | 432 | 432 |
| `httpd` (port 8900, admin server) | 10240 | 9064 | 9064 | 9064 | 4280 |
| `wisp.ws` | 4096 | 3264 | 3264 | 3264 | 2208 |

¹ This measurement still used the 8192-byte stack from before the change. In (c)/(d) the high-water mark of `wisp_ui` had only 432 B left, the lowest point measured in this whole acceptance round; the 8 KB stack was nearly full, so `UiConfig::task_stack` (`components/wisp_ui/include/wisp/ui/ui_runtime.hpp`) was raised from 8192 to 10240 on this basis, using 2 KB more internal RAM. In (a), idle, internal free was 88899 B; less these 2 KB, about 86851 B, still far above the `kIdleInternalFreeMin` (60 KB) minimum, so the idle budget does not need to change with it.

Internal RAM (B):

| State | internal free | largest | min |
|---|---|---|---|
| (a) Idle, no scene open | 88899 | 31744 | 54656 |
| (b) To-do scene, scrolling 200 entries | 88615 | 31744 | 48924 |
| (c) Weather scene, during the HTTPS request | 88851 | 31744 | 39772 |
| (d) STA + hotspot + logs page + uploading a scene zip | 86287 | 31744 | 33772 |

The instantaneous drops of `internal free` in (b)/(c) relative to (a) are 284 B and 48 B, both within the ≤ 2 KB given in spec 2.3; the drop in (d) is about 2.6 KB, slightly higher, but (d) stacks up the hotspot, two httpd instances and an upload, so the cost is not just the scene subsystem itself.

`largest` stays at 31744 B throughout, above both `kWorstInternalLargestMin` (16 × 1024 = 16384 B) and `kIdleInternalLargestMin` (20 × 1024 = 20480 B) of `components/wisp_app/include/wisp/app/ram_budget.hpp`, so both are met. The `internal free` of 88899 B in (a) is above `kIdleInternalFreeMin` (60 × 1024 = 61440 B), so it is met.

`min` is the historic low since boot: the historic internal RAM low reached in this acceptance round was 33772 B (appearing around the TLS connection test combined with the hotspot upload, and staying until the end of the test). Against `kWorstInternalFreeMin` (40 × 1024 = 40960 B): 33772 < 40960, **not met**, needs follow-up (the log shows this low appeared in the interval after the hotspot opened and the WebSocket logs connected, and before the upload; the exact trigger has not yet been taken apart further).

2026-11-05, re-measured per item 57 of the checklist (mbedTLS allocations already moved to PSRAM): in the same combination, the lowest sampled `internal free` was 62635 B and `min` 39732 B, about 6 KB higher than the 33772 B above; all `internal free` samples were above 40 KB, so it is met by the criterion of item 57.

Follow-up (2026-11-05, not run on the device): most of a TLS session's internal RAM comes from mbedTLS allocations smaller than 16 KB (the 4 KB send buffer, the SSL context, handshake state, the parsed certificate chain, bignum temporaries), which by the default `malloc` rule all land in internal RAM, estimated at 15–20 KB during a handshake; the TLS gate only serialises handshakes, so a scene's HTTPS session and the connection test's handshake can overlap. Now `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` puts all mbedTLS allocations into PSRAM (for the reasons and the cost, see the comment on that item in `firmware/sdkconfig.defaults`). In addition, after this measurement the UI task stack grew from 8 KB to 10 KB, permanently using 2 KB more internal RAM. This needs re-measuring on the device per item 57 of the checklist, and cannot count as met until then.

**Background scenes, notifications and power (2026-11-06, `feat/scene-device` firmware ae6f55a, STA only)**: idle after boot with no scene open, `internal free` was 79679 B, `largest` 31744 B, `min` 41232 B, and PSRAM free 3.87 MB. Both `internal free` and `largest` are above the idle minimums of `ram_budget.hpp` (60 KB / 20 KB), but about 9 KB below the 88899 B of (a) in the table above; this is the resident cost added by background scenes, the notification centre, power management and `wisp.imu`/`wisp.tone`. With the pomodoro and the weather both in the background, `heap_internal_free` in `WISP_PERF` stayed between 70 and 73 KB. Switching from a panel back to a background scene rebuilt it in 7–41 ms. A full measurement of the three states (including each task's stack high-water mark) has not been done yet.

## Performance baselines

ESP-IDF v6.0.3, release build (global `-Os`, `-O2` for the rendering path of LVGL, esp_lcd and wisp_ui), CPU 240 MHz, QSPI 40 MHz. "Programmatic scrolling" was a test command temporarily added to the firmware (not committed) that scrolls the self-check list by 6 px per frame on the UI thread, back and forth for 10 s:

- **raw**: `lv_obj_scroll_by_raw()`, with one `SCROLL_BEGIN`/`SCROLL_END` before and after, the same as LVGL does for a finger drag.
- **scroll_by**: one call to `lv_obj_scroll_by()` per frame. Every call sends a `SCROLL_BEGIN`/`SCROLL_END` pair, toggles the `LV_STATE_SCROLLED` state and triggers a relayout of the whole tree, adding about 11 ms per frame. Finger scrolling does not do this, so raw is the authority.

fps is counted from LVGL's `REFR_READY` events, and the `WISP_PERF` numbers agree with it. The raw measurement is now the serial command `bench scroll [seconds]`, resident in the firmware (default 10 s, printing one `WISP_BENCH` line, see "Machine-readable output" in `docs/architecture.md`). Before measuring, open hardware diagnostics with `diag` (the device boots into the home screen, which has no scrollable list), and make sure the performance overlay is off and the list is at the top. "Hotspot" means `wifi ap on` (hotspot only, not connected to any network), when the Wi-Fi setup card (with its QR code) covers the list.

| Configuration | raw fps | raw fps (hotspot) | scroll_by fps | `WISP_PERF` (while scrolling) | Internal RAM free after boot |
|---|---|---|---|---|---|
| Original: synchronous flush, one 48-line buffer, CPU byte swapping | 10.6 | — | 8.6–8.8 | fps 9, cpu 100, flush_ms 4.2–4.4 | 105.2 KB |
| Asynchronous flush, two 24-line buffers, rendering in the panel's byte order, XIP, `LV_OBJ_STYLE_CACHE` | 19.8–20.4 | 11.8–12.2 | 14.8 | fps 20, cpu 100, flush_ms 0.22–0.23 | 103.3 KB |
| **Now**: plus the QR code stored in screen format, skipping areas covered by opaque cards, an 8-bit glyph cache for the default font, S3 SIMD fill/copy, and an inlined masked fill | **22.7** | **28.2** | 15.8 | fps 22–23 (hotspot 28), cpu 97–100, flush_ms 0.24–0.27 | 103.1 KB |
| LVGL heap moved to PSRAM (`LV_STDLIB_CUSTOM`, `wisp/ui/lv_heap.hpp`); STA on the home Wi-Fi, and the "hotspot" column is STA + hotspot | 21.8 | 27.7 | — | fps 22 (hotspot 28), cpu 97–100, flush_ms 0.23–0.27 | 98.8–99.8 KB |

At rest the self-check screen is still at 10 fps (it refreshes about 7 labels every 100 ms): `WISP_PERF` cpu 22–23, flush_ms 0.70–0.87; with the hotspot on, cpu 24–25, flush_ms 0.22–0.25. After the LVGL heap moved to PSRAM (STA connected to a network): cpu 22–24, 25–26 with the hotspot on, while the CPU the UI task really uses does not change (see "LVGL heap" below). `boot_ms` 1826–1828. XIP has to copy the image into PSRAM at boot, which pushes the moment the app starts running from 960 ms to 1042 ms (the timestamp of `Pro cpu start user code` in the ESP log), and it also takes 3.1 MB of PSRAM. The default font's 8-bit glyph cache is in PSRAM, capped at 512 KB (`components/wisp_ui/src/a8_font.hpp`).

**Idle cpu is sensitive to code layout**: when the components linked before LVGL (`wisp_port_esp`, `main`, `wisp_app`, `wisp_admin`, `wisp_ui`) grow by 1–3 KB of code, the whole of LVGL moves, and the conflicts in the 16 KB instruction cache change with it. On the same screen with the same buffers, the measured idle cpu jumped between 24 and 33, with the same fps. So re-measure `WISP_PERF` after changing these components, and put new code that is not on the rendering path preferably into components linked after LVGL (such as `wisp_hal` and `wisp_core`).

**Gain of each change** (raw fps, hotspot off / hotspot on):

| Change | Hotspot off | Hotspot on | Notes |
|---|---|---|---|
| QR code stored in screen format (`flatten_qr()` in `net_cards.cpp`) | 19.9 | 11.8 → 14.5 | `lv_qrcode` stores a 1-bit indexed image, so every redraw of the card looked up the palette pixel by pixel |
| Skipping areas covered by opaque cards (`wisp/ui/overlay.hpp`) | 19.8 | 14.5 → 27.2 | When the list scrolls under the card, LVGL redraws the whole invalidated area (including the card); now only the part outside the card is redrawn, and the invalidated pixels per frame drop from about 280,000 to about 120,000 |
| 8-bit glyph cache for the default font (`a8_font.cpp`) | 19.8 → 22.0 | — | LVGL expands a 4-bit glyph every time it draws it; with the cache it takes LVGL's static A8 bitmap path |
| S3 SIMD solid fill and 16-bit copy (`components/wisp_port_esp/lv_blend/esp32s3/`) | 22.0–22.1 | 27.9 | Within the noise on this screen; enabled only in ESP32-S3 builds |
| Masked solid fill inlined (`lv_blend_mask_rgb565.h`) | 22.9 | 28.2 | Blending of glyphs and rounded-corner edges, bit-for-bit identical to LVGL |

These are single measurements taken after each change; repeated measurements of the same firmware vary by about ±0.3 fps. The final baseline is the 22.7 / 28.2 of the overall table above (the difference between the 22.9 of the "inlined" row and the final 22.7 is within the variation, not a regression from a later change).

**Where the time goes in each frame** (the current configuration, hotspot off, raw scrolling):

- **Cycle**: about 44 ms per frame, of which rendering (`REFR_START` to `REFR_READY`) averages 40.2 ms with a maximum of about 52 ms, about 9.6 M cycles. While scrolling, the whole list area is redrawn every frame (about 280,000 invalidated pixels per frame, 480 × 480 transferred).
- **CPU time distribution** (PC sampling on the UI core): text layout and glyph lookup 20%, pixel blending and filling 17%, object tree traversal and refresh scheduling 13%, style lookup 12%, code in ROM 11% (mostly after the critical-section exit `vPortExitCritical`, i.e. the stretch with interrupts off during LVGL memory allocation and SPI submission), event dispatch 8%, SPI submission 6%, rectangles, rounded corners and shadows 4%, other 10%. Shadows take only 0.4%, and rounded corners about 1.5%.
- **Pipeline**: about 4.3 million instructions per frame (IPC about 0.45). About 32% of the cycles stall on instruction fetch (misses in the 16 KB instruction cache, with code fetched from PSRAM), and data cache misses are about 1%.
- **Transfer**: 40 MHz QSPI measures about 17.9 MB/s, so a whole 480 × 480 RGB565 frame takes about 25.7 ms on the wire. Each segment also has to send CASET/RASET first, and submitting a segment takes about 0.15–0.25 ms (20 segments per frame, 3–5 ms in all). The link limit for whole-frame refresh is about 36 fps. Transfer already overlaps the rendering of the next segment.
- **CPU frequency**: with DFS on (`CONFIG_PM_ENABLE`), it stays at 240 MHz throughout rendering; holding an `ESP_PM_CPU_FREQ_MAX` lock while scrolling changed nothing. So DFS does not hurt the frame rate (item 34).
- **Flash and PSRAM**: QIO 80 MHz flash, octal 80 MHz PSRAM. The rendering loop misses the 16 KB instruction cache often, so running code from PSRAM (XIP, `CONFIG_SPIRAM_XIP_FROM_PSRAM`) gives the largest gain: the same code went from 14.9 fps to 20.2 fps.

**Other approaches tried** (raw fps; internal RAM is the free value after boot measured with temporary test code, 2–3 KB lower than the release firmware):

| Approach | raw fps | Internal RAM | Conclusion |
|---|---|---|---|
| Asynchronous 2 × 24 lines, no XIP | 14.2–14.9 | 100.8 KB | The base gain without XIP |
| Asynchronous 2 × 48 lines | 19.1 | 55.2 KB | Over budget |
| Asynchronous 2 × 48 lines + XIP | 25.5 | About 55 KB (extrapolated) | Over budget |
| 2 × 24 lines + XIP + 32 KB instruction cache | 23.4 | 83.3 KB | Uses 16.7 KB more; the worst case of hotspot + STA would fall below 40 KB |
| 2 × 48 lines + XIP + 32 KB instruction cache | 28.1 | 35.7 KB | Closest to 30, but below the 60 KB budget right after boot |
| `LV_OBJ_STYLE_CACHE` off | 19.2 | 0.8 KB more | Keep the style cache (+1 fps) |
| Two LVGL software render units (`LV_USE_OS LV_OS_FREERTOS`, `LV_DRAW_SW_DRAW_UNIT_CNT 2`) | 16.7 | 80.2 KB | Slower, and uses more internal RAM |
| `-O2` for `esp_driver_spi` (item 42) | 20.1 | Unchanged | No gain, not adopted |
| Draw buffers in PSRAM (rendering only, no transfer) | 46.5 ms per frame with 24 lines, 35.4 ms per frame with 240 lines | Saves 46 KB | Rendering is slower. DMA directly from PSRAM would also need esp_lcd's `psram_dma_direct`, and bmgr 0.7.5 generates that field only for IDF ≥ 6.1 |
| UI task pinned to core 1 | 20.1 | Unchanged | No gain (the UI was not competing with other tasks for the CPU anyway) |
| `-O3` for LVGL's `src/draw/sw/**` (29 files) (measured before SIMD) | 19.4–19.9 (control 19.8–20.0) | Unchanged | No gain; app 37 KB larger |
| `-O3` for all of LVGL (measured on the current configuration) | 23.1 (control 22.7) | Unchanged | Less than 1 fps, not adopted; app 65 KB larger, about 4% fewer instructions, but instruction-fetch stalls rose from 31.6% to 33.2%, cancelling most of the gain |
| Simplifying the self-check screen's styles | — | — | Not changed: the button shadows (theme default, 50% grey) and rounded corners are visible, removing them would change the look, and together they are under 2%; there are no gradients, translucent layers or transforms |
| LVGL hot functions in IRAM | — | — | Not done: IRAM and DRAM share the same SRAM, the hot set would take more than 10 KB, and with the hotspot on the `min` lower bound is only about 1 KB above 40 KB |

`-Ofast`/`-ffast-math` was not used: LVGL's software rendering is mostly integer arithmetic, and fast-math affects only floating point, but could change the semantics of floating-point computation in transforms and gradients (NaN, rounding, associativity), for very little gain.

**Why it cannot reach 30 fps**: within budget (≥ 60 KB after boot, ≥ 40 KB in the worst case), rendering alone takes about 40 ms per frame, with the CPU time spread over text layout, style lookup, object traversal and blending, and no single hot spot that could cut 25% on its own. About a third of the cycles are spent waiting on instruction fetch. To reach 30 fps would need one of the following:

- switching to the 32 KB instruction cache plus larger buffers once the internal RAM budget is relaxed;
- waiting for bmgr to support `psram_dma_direct`, putting the buffers in PSRAM and freeing internal RAM for the 32 KB instruction cache;
- fewer objects and labels in the scrolling area (a trade-off at the level of UI design).

It once reached 28.2 fps with the hotspot on: that was measured on the old self-check screen (`b16bfc3`, hotspot only, no STA, with the programmatic scrolling above), where the Wi-Fi setup card covered half the list and fewer pixels had to be drawn per frame; it was not the hotspot itself that made rendering faster. Since then the self-check screen has been redesigned twice (both the content and the section cards changed), the safe area became smaller (corner radius 70, safe area `{24,24,455,455}`), and the Wi-Fi setup card also became larger (large dialog buttons), so the card no longer covers the same proportion, and 28.2 cannot be compared with the current screen. The current hotspot-on baseline was re-measured on the current screen by the same method; see "Current baseline" below. That earlier 23–24 fps, the same as with the hotspot off, may mean that the card's occlusion clipping was not taking effect, for example because the performance overlay (the LVGL sysmon label) was on: it is an unmarked visible object on the system layer, and any invalidated area that touches it turns clipping off (`components/wisp_ui/src/overlay.cpp`).

### LVGL heap

LVGL's heap is in PSRAM (`LV_STDLIB_CUSTOM`, `components/wisp_ui/src/lv_mem_wisp.cpp`), which leaves about 17 KB more internal RAM after boot than with CLIB `malloc`, and raises the STA + hotspot `min` from 39.7 KB to 56.8–57.2 KB (measured on the release firmware). Without PSRAM, or when PSRAM runs out, it falls back to internal RAM, counted separately by `wisp::ui::lv_heap_internal_bytes()`.

After the move to PSRAM, the idle cpu was once measured about 11 points higher and the scrolling frame rate 0.16–0.47 fps lower. Re-measuring showed that neither number was caused by the heap:

- **The cpu of `WISP_PERF` is not CPU time.** It is the wall-clock time spent in `lv_timer_handler()`, including blocking such as waiting for flushes. It differs on every boot of the same firmware and then stays fixed after boot: the firmware with the CLIB heap was between 23 and 29 at rest over 8 boots, while the CPU of the `wisp_ui` task, from FreeRTOS run-time statistics, was 11.6–11.9% every time. In addition, where the list stops at the end of a scroll test depends on the frame count, and different stopping positions redraw different labels at rest, so the idle cpu "after scrolling" is not comparable. With the list scrolled back to the top before measuring, the UI task CPU is the same for every approach (see the table below).
- **The frame-rate difference is within the noise of code layout.** Measuring the same firmware repeatedly, raw fps differs by less than 0.05; but changing only the test command and relinking once took the same CLIB-heap firmware from 22.31 to 21.64 (hotspot 28.51 → 27.97). Code runs from PSRAM, and about a third of the cycles wait on instruction fetch (see above), so when function positions change, the instruction cache conflicts change. The differences between the heap approaches (±0.35 fps) are all smaller than this, and reverse with a different link order.

Method: the temporary programmatic scrolling command (as above, 6 px per frame, 30 s), plus a command that uses `uxTaskGetSystemState()` to total each task's CPU time over 12 s (with `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` temporarily on, so the internal RAM in the table below is about 9 KB lower than the release firmware). STA stayed connected to the home Wi-Fi throughout, and "hotspot" means STA + hotspot. Each approach was measured twice, and within a group the firmware was identical apart from the heap:

| LVGL heap | raw fps | raw fps (hotspot) | UI task CPU: idle / idle (hotspot) | Internal RAM after boot | STA + hotspot `min` | PSRAM after boot |
|---|---|---|---|---|---|---|
| CLIB `malloc` (before the change) | 21.64 / 21.64 | 27.97 / 27.96 | 11.9% / 15.0–15.2% | 72.3–72.9 KB | 39.8 KB | 5,106 KB |
| PSRAM (now) | 21.84 / 21.80 | 27.68 / 27.67 | 12.1–12.2% / 15.3–15.4% | 89.5–90.0 KB | 46.2–46.6 KB | 5,084 KB |
| LVGL's bundled TLSF, 512 KB pool in PSRAM | 21.99 / 21.98 | 27.86 / 27.87 | 11.7–12.0% / 15.2% | 89.3–89.8 KB | 47.0 KB | 4,580 KB |

Hybrid approaches tried on another group of firmware (in this group CLIB was 22.31 / 28.51 and PSRAM 21.83–21.88 / 27.63–27.76):

| Approach | raw fps | raw fps (hotspot) | Internal RAM after boot | STA + hotspot `min` |
|---|---|---|---|---|
| Blocks ≤ 512 bytes in internal RAM during rendering (`RENDER_START` to `RENDER_READY`) | 21.78–21.79 | 27.61–27.62 | 89.4–90.0 KB | 46.6–46.8 KB |
| Blocks ≤ 256 bytes in internal RAM | 21.90–21.92 | 27.79–27.81 | 71.9–72.0 KB | 39.7 KB |
| Blocks ≤ 16 KB in internal RAM (the same as CLIB's rule, single run) | 22.08 | 28.04 | 67.4 KB | 39.7 KB |

Conclusion: keep the PSRAM heap. TLSF is not faster, and LVGL 9.5 does not grow the pool by itself (`LV_MEM_POOL_EXPAND_SIZE` only relaxes the limits for a single pool and a single block), so a fixed pool cannot hold a scene with 2000 widgets (about 300 bytes per widget on the host), and the 690–920 KB draw buffer used for screenshots would need separate handling too. Approaches that put blocks back into internal RAM either bring no gain or eat the 17 KB gain back.

### Storage readings on the self-check screen (2026-11-05)

`bench scroll` (hotspot off, STA on the home Wi-Fi, 12 scenes installed in internal flash, 248 KB used) measured the 3a firmware at only 15.6–15.8 fps, 7 fps below the 22.7 above. There were two causes, neither of them in the scene runtime:

- **The self-check screen read the storage capacity on the UI thread every 100 ms** (`StorageManager::status()`). LittleFS's `esp_littlefs_info()` has to read every block in use (`lfs_fs_size()`, reading blocks and computing CRCs), measured at 18 ms each time, i.e. 18% of the UI thread's time, costing about 4 fps; `lfs_fs_size()` walks every block in use, so the more is stored, the slower it gets (at the 3a acceptance, finger scrolling still gave 22–24 fps, probably because little was stored then, so it did not reproduce). This read had been there since the self-check screen first existed, so on the same device the old firmware (`d7e98d5`, measured at 21.8 at the time) now also gives only 18.2, and goes back to 22.6 without this read. It now reads once a second on the storage worker thread (`wisp.fs`) and hands the result back to the UI thread (`SceneWiring::storage_status_async()`), and the 3a firmware went from 15.6–15.8 back to 19.1–19.4.
- **The self-check screen redesign** (`dbf75f0`, the status summary and cards) itself draws quite a lot more per frame: likewise without the storage reading, 22.5–22.6 before the redesign and 19.5 after. Commits after 3a brought no change beyond the noise (`842d6f3` and `43a2caf` are both 15.6–15.8).

So the 3a firmware runs at 19.1–19.4 fps on the current self-check screen (this screen, this device); the 22.7 was measured on the screen before the redesign, and the two cannot be compared directly.

### Current baseline (2026-11-05)

The 22.7 / 28.2 of the overall table above were measured on the self-check screen before the redesign, and are used only to show the gain of each rendering change. From 2026-11-05 the acceptance lines (checklist items 33, 42, 53, 56) are the values re-measured on the current hardware diagnostics screen (the former self-check screen):

| Firmware | Hotspot off | Hotspot on (hotspot only, Wi-Fi setup card showing) |
|---|---|---|
| Theme + vector font + 3a (`40d24f4`, including the storage reading fix above) | 18.38 / 18.66 / 18.66 | 29.86 / 29.93 / 29.87 |

Method: first open hardware diagnostics with `diag`, then `bench scroll 10`, 3 times for each, with STA on the home Wi-Fi, 12 scenes installed in internal flash, the performance overlay off and the list at the top; `cpu` was 99–100 in all of them. The acceptance lines are 18.3 with the hotspot off and 29.8 with it on: repeated measurements of the same firmware vary by about ±0.3 fps (see above), and the acceptance lines are set slightly below the lowest reading, so that the firmware that set the baseline does not fail it itself. It is faster with the hotspot on because the card covers most of the list, so fewer pixels are drawn per frame (see the "It once reached 28.2 fps with the hotspot on" paragraph above). On the same screen with only 3a it was 19.1–19.4, so the theme and the vector font together cost about 0.5 fps. If the self-check screen is redesigned again, or the size of the Wi-Fi setup card changes, re-measure these two numbers on the same firmware and update this table.

## Acceptance item 5: measured standby power

(To be filled in after device acceptance. Measurement method: unplug the USB data cable and run on battery only, with an ammeter in series between the battery positive and the battery
(or use a power analyser with current sensing); record the average current both "screen on, idle" and "screen off in standby after a short press of the power key";
the standby current should be no more than 1/10 of the screen-on current. Also record the time from a short press of the power key to wake until the screen actually lights,
see the "Standby and power saving" section above: with `DcsSleep` it is estimated at about 200 ms, which should meet the 300 ms
target, but this is only an estimate and needs confirming by measurement.

Further checks:
1. After a few minutes in standby (making sure automatic light sleep has happened), wake with the volume key or the power key, and confirm that touch still responds
   normally and that the speaker has no unusual pops or continuous noise floor. This verifies that the `gpio_sleep_sel_dis()` handling really
   covers the four pins panel RST, touch RST, amplifier enable and panel CS.
2. After an automatic screen-off, wake with a short press of the power key and confirm that the screen does not flash on and then go dark again; after turning the screen off with the power key, wake with the volume key
   and confirm that the next short press of the power key turns the screen off normally. This verifies that PowerManager being the single source of truth for the screen state
   holds on the device too.
3. With the power key confirmation dialog open (long press on the power key), wait for the automatic screen-off to trigger, then wake with a short press and with a long press of
   the power key. Confirm that both kinds of press wake the screen (the dialog may still be on screen), with no misbehaviour from the "wake"
   and "confirmation dialog" logic competing for the same key press (`PowerKeyController::
   on_short()`/`on_long()` always check "is the screen already off" first, so waking takes priority over the confirmation dialog
   logic).
4. IMU power-down/recovery: before standby, open hardware diagnostics with `diag` and read the IMU values on it once, enter standby, and after waking confirm that the IMU values
   update normally again (not stuck at the last reading before power-down, and not all zeros or clearly wrong values). This verifies that
   `qmi8658_set_power()` correctly restores the previously selected ODR on waking, rather than the driver's default.
5. SD probing pause: with an SD card inserted, wake after a few minutes in standby and confirm that the card is still online (`online()` was not
   wrongly judged as offline); you can also physically remove/insert the SD card during standby and confirm that the hot-plug detection notices it only **after waking**
   (the worker is paused during standby, so it does not detect this insertion or removal during standby). This is
   expected behaviour, not a bug.
6. Further breakdown of the standby current (optional, measurement only, no code change needed): if the standby current measured above is still
   clearly too high, you can try additionally pulling the reset pin of the touch chip CST9217 (GPIO40) low during standby
   (rather than keeping the level from before power-up, as now), and measure the current difference before and after this one operation, to evaluate
   whether "touch chip power during reset" is worth a separate power-saving item later. This is only a one-off measurement
   method, and does not mean the current code needs to change because of it.)
