# waveshare_esp32_s3_touch_amoled_2_16

Board support for the Waveshare ESP32-S3-Touch-AMOLED-2.16: a 480×480 rounded AMOLED, with a CO5300 display and CST9217 touch. Hardware notes (pins, chips, errata) are in `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`.

## Files

| File | Purpose |
|---|---|
| `board_info.yaml`, `board_peripherals.yaml`, `board_devices.yaml` | The esp_board_manager (bmgr) board definition: the buses (I2C, I2S, QSPI, GPIO), the buttons (`btn_power`, `btn_vol_up`, `btn_vol_down` as `gpio` peripherals) and the devices bmgr initialises (display, touch, audio codec, SD card). Every pin of the board is here. Adapted from ESP-Claw; see `docs/references.md`. |
| `setup_device.c` | Factory functions bmgr calls: the CO5300 init command sequence, and a CST9217 factory that passes bmgr's touch config (pins, levels and orientation from `board_devices.yaml`) through. Compiled by the `gen_bmgr_codes` component that bmgr generates. |
| `board.cpp` | Wisp's board entry point, `create_board()`: a `BoardSpec` for the board kit (`boards/common/board_kit.hpp`), with the panel facts bmgr does not carry, and `bring_up_chips()` for the AXP2101, PCF85063 and QMI8658. Compiled by `firmware/main`; `firmware/CMakeLists.txt` removes it from the `gen_bmgr_codes` sources. |
| `board.cmake` | The driver components `board.cpp` uses (`WISP_BOARD_REQUIRES`). |
| `../common/partitions/16mb.csv` | The shared 16 MB partition table (set in `sdkconfig.defaults.board`). The layout is in foundation spec 7.4, and the conventions are in `docs/porting-guide.md`. |
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
