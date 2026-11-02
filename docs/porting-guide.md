# 移植指南：新增一块开发板

本文档描述在 Wisp 基础骨架（子项目 1）之上新增一块 ESP32 开发板需要做哪些事。参考实现是 `boards/waveshare_amoled_216/`；硬件资料写法参考 `docs/hardware/waveshare_amoled_216.md`。

## 总览

一块板子由三部分组成：

1. **esp_board_manager YAML**（`boards/<板名>/board_info.yaml`、`board_peripherals.yaml`、`board_devices.yaml`）：描述板子型号、总线（I2C/I2S/SPI/GPIO）和 esp_board_manager 已支持的设备（显示、触摸、音频编解码器、SD 卡等）。只负责把芯片接上总线、分配引脚，**不**填写 Wisp 的能力清单。
2. **`setup_device.c`**（可选，仅当板上有 esp_board_manager 还没有内置工厂函数的设备类型时才需要）：给 bmgr 提供自定义设备的构造入口。
3. **`board.cpp`**：实现 `wisp::board::create_board()`，返回一个 `wisp::app::Board`。它的 `init()` 把 bmgr 已经建好的设备句柄（屏幕、触摸、音频、SD 卡）包一层 `wisp::hal::Device` 接口，并用 Wisp 自己的驱动（通过 bmgr 暴露的总线）点亮 bmgr 不管的设备（IMU、PMU、RTC、按键等），统一注册进 `DeviceRegistry`。

## 步骤 1：写 bmgr YAML

1. 新建 `boards/<板名>/`，三个 YAML 参照 esp_board_manager 文档和已有板子（`boards/waveshare_amoled_216/`）的字段写。能直接复用其他公开板级定义（如 ESP-Claw）时，按 AGENTS.md 第 6 节在文件头注明来源、登记到 `docs/references.md` 和 `THIRD_PARTY_NOTICES.md`。
2. 只在 YAML 里声明 esp_board_manager 原生支持的设备类型（`display_lcd`、`lcd_touch`、`audio_codec`、`fs_fat`/SDMMC、裸 GPIO/I2C/I2S/SPI 总线等）。IMU、PMU、RTC 等自定义芯片**不要**塞进 YAML 当 `custom` 设备——Wisp 用自己的驱动在 `board.cpp` 里初始化它们，挂在 bmgr 暴露的同一条总线上即可。
3. 用 `idf.py bmgr -c ../boards -b <板名>` 生成代码（生成结果在 `firmware/components/gen_bmgr_codes/`，已被 git 忽略，改了 YAML 要重新跑这一步；不要手改生成代码）。

## 步骤 2：补驱动（`drivers/`）

esp_board_manager 组件库里没有的芯片，在 `drivers/<芯片名>/` 下新建一个纯 C 的 ESP-IDF 组件（CMakeLists 用 `wisp_component(...)`）。如果 MyOS 项目（`~/Projects/MyOS`，MIT）里已经有对应驱动，优先移植：保留寄存器序列和换算公式，去掉 MyOS 特有的睡眠/电源轨控制等与本板无关的逻辑；文件头写明 "Ported from MyOS (MIT, Copyright (c) 2026 ShouChen)"，并登记到 `docs/references.md`。全新寄存器配置（MyOS 没驱动过的功能，例如陀螺仪）要在文件头写明"新写，未经验证"，并在真机验收清单里加一条重点核实。

## 步骤 3：写 `board.cpp`

实现 `wisp::app::Board`（`components/wisp_app/include/wisp/app/board.hpp`）：

```cpp
class Board : public wisp::app::Board {
public:
    std::string name() const override { return "<板名>"; }
    void init(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) override {
        // 1. 从 bmgr 取已经建好的句柄（显示/触摸/音频/SD），包一层 hal::Device 子类，add() 进 registry。
        // 2. 用 drivers/ 里的驱动，在 bmgr 暴露的 I2C 总线上初始化 IMU/PMU/RTC/按键，add() 进 registry。
        // 3. 任何一步失败：report.record({id, class, DeviceState::Failed, "原因"})，不要 add() 这个设备，
        //    不要让 init() 本身失败或中止——缺失的设备只是降级，不阻止启动（AGENTS.md 第 3 节"设备模型"）。
    }
};

namespace wisp::board {
std::unique_ptr<wisp::app::Board> create_board() { return std::make_unique</* 上面这个类 */>(); }
}
```

`App::start()` 会在 `init()` 之后自动为注册表里没有报告过的设备补一条 `DeviceState::Ok`，`init()` 只需要主动记录失败/缺失的情况。

**设备 id 命名约定**：`<类别>.<名字>`，类别用 `wisp::hal::DeviceClass`（`display`/`touch`/`imu`/`power`/`clock`/`button`/`audio_out`/`audio_in`/`storage`）对应的小写名。本板用的 id：`display.main`、`touch.main`、`imu.main`、`power.main`、`clock.main`、`button.power`/`button.vol_up`/`button.vol_down`、`audio_out.main`、`audio_in.main`、`storage.int`、`storage.sd`。新板子优先沿用同一套 id，除非确有多实例需要区分（如以后的 `display.external`）。

**能力字段（caps）约定**：每个设备在构造时自己填写 `DeviceInfo::caps`，这是能力信息的唯一来源（板级 YAML 只管引脚和总线）。各类别的必填字段：

| 类别 | 必填 caps |
|---|---|
| `Display` | `width`、`height`、`shape`（`rect`/`rounded_rect`/`circle`）、`corner_radius`（`rounded_rect` 时）、`panel`、`brightness`、`area_align` |
| `Touch` | `max_points` |
| `Imu` | `accel`、`gyro`、`magnetometer`、`max_rate_hz` |
| `Power` | `battery`、`charging_status` |
| `Storage` | `removable`、`fs` |

`Display` 的 `safe_area` 不用自己算——`DeviceRegistry::manifest()` 用 `wisp::hal::safe_rect()` 从 `shape`/`corner_radius`/`width`/`height` 派生，设备只管填 `shape`。缺失或无法识别的 `shape` 按 `circle`（最保守）处理并告警。

## 步骤 4：构建

```bash
bash -c '. ~/esp/esp-idf-v6.0.3/export.sh && cd firmware \
    && idf.py set-target <芯片，如 esp32s3> \
    && idf.py bmgr -c ../boards -b <板名> \
    && idf.py build -DWISP_BOARD=<板名>'
```

`firmware/CMakeLists.txt` 的 `WISP_BOARD` 缓存变量决定编译哪个 `boards/<名>/board.cpp`（默认 `waveshare_amoled_216`），用 `-DWISP_BOARD=<板名>` 覆盖。只要没改板子 YAML，后续重新 `idf.py build` 不需要重复 set-target/bmgr。

## 步骤 5：验收

对照串口输出和自检界面，不需要额外工具：

1. **启动报告**（`WISP_BOOT_REPORT`）：每个设备状态应为 `ok`（或预期中的 `degraded`/`missing`，并带 `reason`），不应有意料之外的 `failed`。
2. **能力清单**（`WISP_MANIFEST`）：核对 `board` 字段是板名，每个设备的 `id`/`class`/`model`/`caps` 符合上面的约定；`Display` 的 `safe_area` 没有裁掉圆角/圆形屏的内容。
3. **自检界面**（spec 第 8 节）：8 类设备在安全区内都有对应的实时状态——设备列表按状态着色、IMU 小球随倾斜移动、触摸测试区跟手、按键事件有提示、电量和 RTC 时间正确、存储容量正确、录音回放能听到声音。
4. 新板子特有的真机验收项，仿照 `docs/hardware/<板名>.md`（新建）的"真机验收清单"一节，参照 `docs/hardware/waveshare_amoled_216.md` 的写法逐条列出怎么测、预期结果，留一栏空的"结果"栏。
