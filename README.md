# Wisp（小鬼火）

Wisp 是一个运行在 ESP32 系列芯片上、完全由 AI 驱动的掌上个人 Agent 系统：没有传统的桌面、锁屏和 App，一切界面都是由 Agent 动态创建、编辑、保存和复用的"场景"（scene）。

本仓库是 Wisp 整体路线的第 1 个子项目——**基础骨架**：可移植分层架构、双目标构建（PC 模拟器 + 真机）、设备抽象与能力清单、存储管理，以及一个用于验收的自检界面。Wisp 的完整路线共 8 个子项目（基础骨架 → 连接与管理后台 → 场景运行时 → Agent 核心 → 语音链路 → 记忆与调度 → 安全与演化 → 主场景与系统外壳），逐个走"设计 → 计划 → 实施"流程，详见 [`AGENTS.md`](AGENTS.md)。

## 快速开始：PC 模拟器

模拟器是日常开发的主力：可移植层的全部逻辑都先在这里开发和测试，再搬到真机上验证。

**依赖**：CMake ≥ 3.24、Ninja、支持 C++20 的编译器（GCC 或 Clang）、SDL2 开发库、Python 3。提交前检查（`bash tools/ci.sh`）额外需要 `clang-format==23.1.3`（`pip install clang-format==23.1.3`；版本必须与这个一致，否则本机的格式检查结论可能和 CI 不一样，见 `tools/ci.sh` 的说明）。

```bash
# Arch Linux
sudo pacman -S cmake ninja sdl2 python

# Ubuntu / Debian
sudo apt-get install cmake ninja-build libsdl2-dev python3
```

GoogleTest、ArduinoJson、LVGL 由 CMake 的 `FetchContent` 在构建时自动拉取（见 `cmake/wisp_host_deps.cmake`），不需要单独安装。

**构建与运行**：

```bash
cmake -S simulator -B build/sim -G Ninja
cmake --build build/sim
ctest --test-dir build/sim --output-on-failure   # 运行全部单元测试与冒烟测试

./build/sim/wisp_sim                              # 弹出 480×480 窗口
```

**模拟器按键**（对应真机外设，详见 `docs/superpowers/specs/2026-11-01-foundation-design.md` 第 8.1 节）：

| 输入 | 作用 |
|---|---|
| 鼠标点击/拖动 | 模拟触摸 |
| `P`（短按 / 按住 ≥ 1 秒） | 电源键短按 / 长按 |
| `+` / `-` | Vol+ / Vol- |
| 方向键 | 模拟 IMU 倾斜 |
| `S` | 模拟 SD 卡插入 / 拔出 |

**常用命令行参数**：`--headless`（无窗口运行）、`--screenshot <path>`（渲染完成后截图保存为 PNG）、`--exit-after-ms <n>`、`--disable <设备id或类别>`（模拟某设备初始化失败，可重复）、`--no-sd`、`--data-dir <path>`（默认 `./sim-data`）、`--shape <rect|rounded_rect|circle>`（模拟屏幕形状）。

## 快速开始：真机（Waveshare ESP32-S3-Touch-AMOLED-2.16）

目前唯一支持的开发板，硬件细节见 [`docs/hardware/waveshare_amoled_216.md`](docs/hardware/waveshare_amoled_216.md)。

1. 安装 ESP-IDF **v6.0.3**（版本已锁定，见 `docs/architecture.md`），按官方文档装到例如 `~/esp/esp-idf-v6.0.3/`。
2. 激活环境并构建。`export.sh` 是 bash 脚本：如果你的 shell 是 fish（或其他非 bash shell），用 `bash -c '...'` 包一层再执行。**全新检出（第一次构建，或切换过板子/芯片）时顺序是 set-target → bmgr → build**：

   ```bash
   bash -c '. ~/esp/esp-idf-v6.0.3/export.sh && cd firmware \
       && idf.py set-target esp32s3 \
       && idf.py bmgr -c ../boards -b waveshare_amoled_216 \
       && idf.py build'
   ```

   - `set-target` 选定芯片（同时让组件管理器拉取 `esp_board_manager`，`idf.py bmgr` 子命令在这之前不存在）；
   - `idf.py bmgr` 根据 `boards/waveshare_amoled_216/board_*.yaml` 生成设备初始化代码（生成结果已被 git 忽略，改了板子 YAML 要重新跑一次这一步）；
   - 后续只改代码、没改板子 YAML 时，重复执行 `idf.py build` 即可，不必每次都跑 set-target/bmgr。

3. 烧录并查看日志：

   ```bash
   bash -c '. ~/esp/esp-idf-v6.0.3/export.sh && cd firmware && idf.py -p <端口> flash monitor'
   ```

   上电 3 秒内应进入自检界面；串口会输出 `WISP_BOOT_REPORT`、`WISP_MANIFEST` 两行 JSON（启动报告与设备能力清单）。真机验收的完整清单见 `docs/hardware/waveshare_amoled_216.md` 的"真机验收清单"一节。

## 目录结构

```
wisp/
├── components/     # 可移植核心（wisp_base/os/hal/core/ui/app）+ 平台适配层（wisp_port_sim/wisp_port_esp）
├── boards/         # 板级定义（esp_board_manager YAML + board.cpp），当前只有 waveshare_amoled_216
├── drivers/        # 组件库里没有、从 MyOS 移植或新写的芯片驱动（AXP2101/PCF85063/QMI8658）
├── firmware/       # ESP-IDF 工程入口（idf.py build/flash）
├── simulator/      # PC 模拟器入口（CMake + SDL2）
├── tests/          # 可移植层单元测试（GoogleTest，PC 上运行）+ 模拟器冒烟测试
├── tools/          # check_layering.py（分层规则检查）、ci.sh（提交前完整检查）
├── docs/           # architecture.md、porting-guide.md、hardware/、references.md、superpowers/（spec 与计划）
└── .github/workflows/ci.yml
```

## 提交前检查

```bash
bash tools/ci.sh
```

依次执行：clang-format 格式检查（不自动改写，格式有误时先自行 `clang-format -i`）、`firmware/sdkconfig.defaults` 行格式检查、`tools/check_layering.py`（分层规则）、模拟器构建、全部 ctest（含冒烟测试）。与 CI 的 `host` job 内容一致；固件编译由 CI 的 `firmware` job 单独跑，本机需要先装好 ESP-IDF 才能复现（见上面的真机步骤）。

## 许可证

[Apache License 2.0](LICENSE)。本项目依赖或参考的第三方项目及其许可证登记在 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) 和 [`docs/references.md`](docs/references.md)。
