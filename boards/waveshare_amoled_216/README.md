# waveshare_amoled_216

Waveshare ESP32-S3-Touch-AMOLED-2.16（480×480 圆角 AMOLED，CO5300 屏幕 + CST9217 触摸）的板级适配。硬件资料（引脚、芯片、勘误）见 `docs/hardware/waveshare_amoled_216.md`。

## 文件

| 文件 | 作用 |
|---|---|
| `board_info.yaml`、`board_peripherals.yaml`、`board_devices.yaml` | esp_board_manager（bmgr）板级定义：总线（I2C、I2S、QSPI、GPIO）和 bmgr 负责初始化的设备（屏幕、触摸、音频编解码、SD 卡）。改编自 ESP-Claw，见 `docs/references.md`。 |
| `setup_device.c` | bmgr 调用的工厂函数：CO5300 的初始化命令序列、CST9217 的复位/中断引脚和坐标方向。由 bmgr 生成的 `gen_bmgr_codes` 组件编译。 |
| `board.cpp` | Wisp 侧的板级入口 `create_board()`：调用 `esp_board_manager_init()`，再把 bmgr 的句柄包装成 HAL 设备注册（`display.main`、`touch.main`）。由 `firmware/main` 编译（`firmware/CMakeLists.txt` 会把它从 `gen_bmgr_codes` 的源文件中剔除）。 |
| `partitions.csv` | 本板分区表（16MB Flash），布局见基础 spec 7.4，约定见 `docs/porting-guide.md`。 |
| `sdkconfig.defaults.board` | 板级 sdkconfig：Flash、八线 PSRAM、USB 串口控制台、分区表路径。`idf.py bmgr` 把它合并进 `board_manager.defaults`；只在 `firmware/sdkconfig` 不存在时生效，改动后要先删掉 `firmware/sdkconfig`。 |

IMU（QMI8658）和电源管理（AXP2101）不交给 bmgr，由 Wisp 自己的驱动处理。

## 构建

```bash
cd firmware
idf.py set-target esp32s3          # 仅全新检出时需要：让 idf.py 加载 bmgr 扩展
idf.py bmgr -c ../boards -b waveshare_amoled_216
idf.py build
```

改动本目录的 YAML 或 `setup_device.c` 后，要重新执行 `idf.py bmgr`。生成的 `firmware/components/gen_bmgr_codes/` 已被 git 忽略，不要手改。
