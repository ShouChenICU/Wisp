# waveshare_esp32_s3_touch_amoled_2_16

Board support for the Waveshare ESP32-S3-Touch-AMOLED-2.16: a 480×480 rounded AMOLED, with a CO5300 display and CST9217 touch. Hardware notes (pins, chips, errata) are in `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`.

## Files

| File | Purpose |
|---|---|
| `board_info.yaml`, `board_peripherals.yaml`, `board_devices.yaml` | The esp_board_manager (bmgr) board definition: the buses (I2C, I2S, QSPI, GPIO) and the devices bmgr initialises (display, touch, audio codec, SD card). Adapted from ESP-Claw; see `docs/references.md`. |
| `setup_device.c` | Factory functions bmgr calls: the CO5300 init command sequence, and the CST9217 reset/interrupt pins and axis orientation. Compiled by the `gen_bmgr_codes` component that bmgr generates. |
| `board.cpp` | Wisp's board entry point, `create_board()`. It calls `esp_board_manager_init()`, then wraps bmgr's handles as HAL devices and registers them (`display.main`, `touch.main`). Compiled by `firmware/main`; `firmware/CMakeLists.txt` removes it from the `gen_bmgr_codes` sources. |
| `partitions.csv` | This board's partition table (16 MB flash). The layout is in foundation spec 7.4, and the conventions are in `docs/porting-guide.md`. |
| `sdkconfig.defaults.board` | Board sdkconfig: flash, octal PSRAM, the USB serial console and the partition table path. `idf.py bmgr` merges it into `board_manager.defaults`. It applies only while `firmware/sdkconfig` does not exist, so delete `firmware/sdkconfig` after changing it. |

The IMU (QMI8658) and the power management chip (AXP2101) are not handed to bmgr; Wisp's own drivers handle them.

## Build

```bash
cd firmware
idf.py set-target esp32s3          # only on a fresh checkout: makes idf.py load the bmgr extension
idf.py bmgr -c ../boards -b waveshare_esp32_s3_touch_amoled_2_16
idf.py build
```

After changing this directory's YAML or `setup_device.c`, run `idf.py bmgr` again. The generated `firmware/components/gen_bmgr_codes/` is ignored by git; do not edit it by hand.
