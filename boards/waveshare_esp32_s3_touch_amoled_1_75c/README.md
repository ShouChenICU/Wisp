# waveshare_esp32_s3_touch_amoled_1_75c

Board support for the Waveshare ESP32-S3-Touch-AMOLED-1.75C: a 466×466 round AMOLED with a CO5300 display and CST9217 touch, 32 MB flash and no SD card slot. Hardware notes (pins, chips, errata, the device acceptance checklist) are in `docs/hardware/waveshare_esp32_s3_touch_amoled_1_75c.md`.

## Files

| File | Purpose |
|---|---|
| `board_info.yaml`, `board_peripherals.yaml`, `board_devices.yaml` | The esp_board_manager (bmgr) board definition: the buses (I2C, I2S, QSPI, GPIO) and the devices bmgr initialises (display, touch, audio codecs). Derived from the 2.16's definition (itself adapted from ESP-Claw), with this board's pins from Waveshare's BSP; see `docs/references.md`. |
| `setup_device.c` | Factory functions bmgr calls: the CO5300 init command sequence (copied from Waveshare's BSP) and its 6-column offset, and the CST9217 reset/interrupt pins and axis flips (`TOUCH_*` at the top of the file). Compiled by the `gen_bmgr_codes` component that bmgr generates. |
| `board.cpp` | Wisp's board entry point, `create_board()`. It calls `esp_board_manager_init()`, then wraps bmgr's handles as HAL devices and brings up the PMU, IMU, optional RTC and buttons with Wisp's own drivers. Compiled by `firmware/main`; `firmware/CMakeLists.txt` removes it from the `gen_bmgr_codes` sources. |
| `partitions.csv` | This board's partition table (32 MB flash: everything the cache maps below 16 MB, `storage` in the upper 16 MB). The conventions are in `docs/porting-guide.md`. |
| `sdkconfig.defaults.board` | Board sdkconfig: 32 MB QIO flash, octal PSRAM without XIP, the USB serial console and the partition table path. `idf.py bmgr` merges it into `board_manager.defaults`. It applies only to a new sdkconfig, so delete the sdkconfig after changing it. |

The IMU (QMI8658, at 0x6B or 0x6A, probed), the power management chip (AXP2101, with a PMU PWR-key fallback) and the optional PCF85063 RTC are not handed to bmgr; Wisp's own drivers handle them.

## Button input selection

`button.boot` explicitly requests GPIO EdgeEvent input. With `kUseSysOut=true`,
`button.power` selects GPIO3/SYS_OUT EdgeEvent input by default. Exactly one
power key is registered. ButtonService/PowerManager owns standby scheduling.

The user reported button-function tests passing on this board and the 2.16
on 2026-11-12, and approved merging this default. Active-high polarity is
schematic-derived and functionally exercised, not an electrical measurement.
Held-at-reset/boot strapping, battery-only light-sleep wake, cache-off stress
and worst-case RAM/power acceptance remain pending; enabling the input does
not claim those checks passed. See the hardware document for evidence and
remaining acceptance steps.

When enabled, GPIO configuration/shared edge-service failure selects the PMU
fallback before registration. A later sink/resource failure leaves the one
GPIO device registered and ButtonService polls it (100 ms for power standby);
it never adds a second `button.power`. Such GPIO polling can miss a pulse
shorter than the polling interval. GPIO wake uses the independent RTC IO
high/low level detector plus global GPIO wake enable, preserving the digital
ANYEDGE interrupt type (the generic GPIO wake API overwrites that type).
Non-RTC-capable pins downgrade to polling. A NO_LIGHT_SLEEP lock is held
while pressed; task-context edge maintenance disarms wake until release and
then re-arms it. A USB-connected startup snapshot placed `wisp.button` in
PSRAM with 3196 B of stack high-water-mark remaining; it is not worst-case
RAM or standby-power acceptance.

## Build

A separate build directory and sdkconfig keep this board from clobbering the reference board's `firmware/build` and `firmware/sdkconfig` (see "Building several boards" in `docs/porting-guide.md`):

```bash
cd firmware
idf.py set-target esp32s3          # only on a fresh checkout: makes idf.py load the bmgr extension
idf.py bmgr -c ../boards -b waveshare_esp32_s3_touch_amoled_1_75c
idf.py -B build-waveshare_esp32_s3_touch_amoled_1_75c \
    -D SDKCONFIG=build-waveshare_esp32_s3_touch_amoled_1_75c/sdkconfig \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;components/gen_bmgr_codes/board_manager.defaults" \
    -DWISP_BOARD=waveshare_esp32_s3_touch_amoled_1_75c build
```

bmgr's generated `firmware/components/gen_bmgr_codes/` is shared by every build directory, so rerun `idf.py bmgr` whenever you switch boards or change this directory's YAML or `setup_device.c`. It is ignored by git; do not edit it by hand.
