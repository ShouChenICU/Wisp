# 移植指南：新增一块开发板

本文档描述在 Wisp 基础骨架（子项目 1）之上新增一块 ESP32 开发板需要做哪些事。参考实现是 `boards/waveshare_amoled_216/`；硬件资料写法参考 `docs/hardware/waveshare_amoled_216.md`。

## 总览

一块板子由以下几部分组成：

1. **esp_board_manager YAML**（`boards/<板名>/board_info.yaml`、`board_peripherals.yaml`、`board_devices.yaml`）：描述板子型号、总线（I2C/I2S/SPI/GPIO）和 esp_board_manager 已支持的设备（显示、触摸、音频编解码器、SD 卡等）。只负责把芯片接上总线、分配引脚，**不**填写 Wisp 的能力清单。
2. **`setup_device.c`**（可选，仅当板上有 esp_board_manager 还没有内置工厂函数的设备类型时才需要）：给 bmgr 提供自定义设备的构造入口。
3. **`board.cpp`**：实现 `wisp::board::create_board()`，返回一个 `wisp::app::Board`。它的 `init()` 把 bmgr 已经建好的设备句柄（屏幕、触摸、音频、SD 卡）包一层 `wisp::hal::Device` 接口，并用 Wisp 自己的驱动（通过 bmgr 暴露的总线）点亮 bmgr 不管的设备（IMU、PMU、RTC、按键等），统一注册进 `DeviceRegistry`。
4. **`partitions.csv` 与 `sdkconfig.defaults.board`**：本板的分区表和板级 sdkconfig（见下文"分区与板级配置"和"固件与分区"）。

## 步骤 1：写 bmgr YAML

1. 新建 `boards/<板名>/`，三个 YAML 参照 esp_board_manager 文档和已有板子（`boards/waveshare_amoled_216/`）的字段写。能直接复用其他公开板级定义（如 ESP-Claw）时，按 AGENTS.md 第 6 节在文件头注明来源、登记到 `docs/references.md` 和 `THIRD_PARTY_NOTICES.md`。
2. 只在 YAML 里声明 esp_board_manager 原生支持的设备类型（`display_lcd`、`lcd_touch`、`audio_codec`、`fs_fat`/SDMMC、裸 GPIO/I2C/I2S/SPI 总线等）。IMU、PMU、RTC 等自定义芯片**不要**塞进 YAML 当 `custom` 设备——Wisp 用自己的驱动在 `board.cpp` 里初始化它们，挂在 bmgr 暴露的同一条总线上即可。
3. 用 `idf.py bmgr -c ../boards -b <板名>` 生成代码（生成结果在 `firmware/components/gen_bmgr_codes/`，已被 git 忽略，改了 YAML 要重新跑这一步；不要手改生成代码）。

## 步骤 2：补驱动（`drivers/`）

esp_board_manager 组件库里没有的芯片，在 `drivers/<芯片名>/` 下新建一个纯 C 的 ESP-IDF 组件（CMakeLists 直接用 `idf_component_register(...)`，不是 `cmake/wisp_component.cmake` 的 `wisp_component()`——那个宏是为了让可移植层（`wisp_base`/`os`/`hal`/`core`/`ui`/`app`）同时在 `idf.py` 和宿主机模拟器两种构建下都能用，驱动只会在 `idf.py` 固件构建下编译，直接用 ESP-IDF 的 `driver/*` 头文件，不需要这个双构建切换）。如果 MyOS 项目（MIT，作者 ShouChen）里已经有对应驱动，优先移植：保留寄存器序列和换算公式，去掉 MyOS 特有的睡眠/电源轨控制等与本板无关的逻辑；文件头写明 "Ported from MyOS (MIT, Copyright (c) 2026 ShouChen)"，并登记到 `docs/references.md`。全新寄存器配置（MyOS 没驱动过的功能，例如陀螺仪）要在文件头写明"新写，未经验证"，并在真机验收清单里加一条重点核实。

## 步骤 3：写 `board.cpp`

实现 `wisp::app::Board`（`components/wisp_app/include/wisp/app/board.hpp`）：

```cpp
class Board : public wisp::app::Board {
public:
    std::string name() const override { return "<板名>"; }
    void init(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) override {
        // 1. 从 bmgr 取已经建好的句柄（显示/触摸/音频/SD），包一层 hal::Device 子类，add() 进 registry。
        // 2. 用 drivers/ 里的驱动，在 bmgr 暴露的 I2C 总线上初始化 IMU/PMU/RTC/按键，add() 进 registry。
        // 3. 芯片自带 Wi-Fi 时：wisp::port::esp::EspWifi::create() 注册 net.wifi。放在最后（在显示之后，
        //    免得 Wi-Fi 驱动先占走屏幕 DMA 缓冲要用的内部 RAM），并且 bmgr 失败的提前返回路径上也要调用：
        //    射频开着是 Platform::random（esp_fill_random）给出真随机数的前提。
        // 4. 任何一步失败：report.record({id, class, DeviceState::Failed, "原因"})，不要 add() 这个设备，
        //    不要让 init() 本身失败或中止——缺失的设备只是降级，不阻止启动（AGENTS.md 第 3 节"设备模型"）。
    }
};

namespace wisp::board {
std::unique_ptr<wisp::app::Board> create_board() { return std::make_unique</* 上面这个类 */>(); }
}
```

`App::start()` 会在 `init()` 之后自动为注册表里没有报告过的设备补一条 `DeviceState::Ok`，`init()` 只需要主动记录失败/缺失的情况。

**设备 id 命名约定**：`<类别>.<名字>`，类别用 `wisp::hal::DeviceClass`（`display`/`touch`/`imu`/`power`/`clock`/`button`/`audio_out`/`audio_in`/`storage`/`network`）对应的小写名；例外是 Wi-Fi 设备按子项目 2 的 spec 固定注册为 `net.wifi`（类别 `network`）。本板用的 id：`display.main`、`touch.main`、`imu.main`、`power.main`、`clock.main`、`button.power`/`button.vol_up`/`button.vol_down`、`audio_out.main`、`audio_in.main`、`storage.int`、`storage.sd`。新板子优先沿用同一套 id，除非确有多实例需要区分（如以后的 `display.external`）。

**能力字段（caps）约定**：每个设备在构造时自己填写 `DeviceInfo::caps`，这是能力信息的唯一来源（板级 YAML 只管引脚和总线）。各类别的必填字段：

| 类别 | 必填 caps |
|---|---|
| `Display` | `width`、`height`、`shape`（`rect`/`rounded_rect`/`circle`）、`corner_radius`（`rounded_rect` 时）、`panel`、`brightness`、`area_align`；可选 `rgb565_be`（面板要 RGB565 高字节在前时为 true，UI 直接按这个字节序渲染）；可选绘制缓冲策略，不写就是两块 24 行、内部 DMA RAM、局部刷新：`draw_buf_lines`（每块行数，默认 24，超过屏高按屏高）、`draw_buf_mem`（`"dma"` 默认，内部 DMA RAM；`"large"` 为 DMA 可读的 PSRAM，即 `Mem::DmaLarge`，只在芯片 DMA 能读 PSRAM、屏幕驱动也能从 PSRAM 发送时声明，分配不到时退回 `"dma"`）、`render_mode`（只实现了 `"partial"`；`"full"`/`"direct"` 是保留名，目前告警后按 `"partial"` 处理）。非法值告警后用默认值，不阻止启动（`wisp/hal/draw_buf_policy.hpp`）。ESP 端口通过 `EspDisplayConfig` 的同名字段发布 |
| `Touch` | `max_points` |
| `Imu` | `accel`、`gyro`、`magnetometer`、`max_rate_hz` |
| `Power` | `battery`、`charging_status` |
| `Storage` | `removable`、`fs` |
| `Network` | `ap`（bool，是否支持热点 `start_ap()`；为 false 时 `WifiService` 不开配网热点）、`bands`（频段列表，如 `["2.4GHz"]`） |

`Display` 的 `safe_area` 不用自己算——`DeviceRegistry::manifest()` 用 `wisp::hal::safe_rect()` 从 `shape`/`corner_radius`/`width`/`height` 派生，设备只管填 `shape`。缺失或无法识别的 `shape` 按 `circle`（最保守）处理并告警。

## 分区与板级配置

`firmware/sdkconfig.defaults` 只放与板子无关的公共配置。凡是属于某块板子的配置都放在板目录：

- `boards/<板名>/partitions.csv`：本板分区表，也是唯一的一份。
- `boards/<板名>/sdkconfig.defaults.board`：Flash 大小、模式与频率，PSRAM，控制台，以及
  `CONFIG_PARTITION_TABLE_CUSTOM=y` 和 `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="../boards/<板名>/partitions.csv"`（ESP-IDF 按工程目录 `firmware/` 解析相对路径）。`idf.py bmgr -b <板名>` 会把它合并进生成的 `board_manager.defaults`，优先级高于 `firmware/sdkconfig.defaults`。

sdkconfig 的默认值只在 `firmware/sdkconfig` 不存在时生效。切换板子或修改 `sdkconfig.defaults.board` 后，先删掉 `firmware/sdkconfig`，再执行 `idf.py bmgr` 和 `idf.py build`。

### 分区约定（新板子必须遵守）

代码只按**分区名**查找分区，不依赖偏移和大小。

| 分区名 | 是否必需 | 说明 |
|---|---|---|
| `nvs` | 必需 | 配置 |
| `phy_init` | 必需 | 射频校准 |
| `storage` | 必需 | LittleFS（`/int`） |
| `ota_0`、`ota_1`、`otadata` | 可选（网页固件上传需要） | 双 OTA 应用分区，两个一样大，要装得下整个应用镜像（网页资源包也链接在镜像里）；没有时用单个 `factory` 应用分区，`EspFirmwareUpdater::create()` 找不到第二个槽就返回空，`GET /system/firmware` 报告 `supported: false`，只降级运行 |

没有单独的 `assets` 分区：网页资源包（WispPack，≤ 256 KiB）链接在应用镜像里（见下文"固件与分区"）。分区大小由各板自行决定，例如 32MB 的板子可以把多出的空间分给 `storage`。参考板的分区表见 `boards/waveshare_amoled_216/partitions.csv` 和 `docs/hardware/waveshare_amoled_216.md`。

**改分区表的规则**：已经出货的板子改分区表时，`nvs` 和 `storage` 保持原来的偏移和大小，设置和数据才能保留。分区表和引导程序不在 OTA 范围内，网页上传只换应用镜像；所以改了分区表（或首次打开回滚功能、换了引导程序）之后，必须用 USB 完整烧录一次（`idf.py flash`，写入引导程序、分区表、`otadata` 和 `ota_0`），之后才能用网页上传。

## 固件与分区

### 板名描述符

每个 Wisp 应用镜像都在 `esp_app_desc_t` 之后带一个 48 字节的板名描述符（`firmware/main/board_desc.c`，段 `.rodata_custom_desc`，所以总在镜像偏移 288 处）：

| 偏移（相对描述符） | 类型 | 内容 |
|---|---|---|
| 0 | `u32` | 魔数 `0x42505357`（小端 `"WSPB"`） |
| 4 | `u16` | 版本，当前为 1 |
| 6 | `u16` | 保留，0 |
| 8 | `char[40]` | 板名（`WISP_BOARD`，以 NUL 结尾，最多 39 个字符） |

网页上传收到镜像的前 336 字节时（`core::parse_firmware_head()`）就核对项目名（`wisp`）、芯片 id（`esp_image_header_t` 偏移 12，对比 `CONFIG_IDF_FIRMWARE_CHIP_ID`）和这个板名，任何一项不符都以 400 `wrong_board` 拒绝，一个字节都不写进 Flash；没有描述符的镜像（不是 Wisp 构建的）同样拒绝。整份镜像写完后再按引导程序的规则校验段长、校验和与 SHA-256（`invalid_image`）。网页上传**从不要求签名**：自己构建的镜像必须能装。新板子不需要做任何事，`WISP_BOARD` 会自动写进描述符；`firmware/main/CMakeLists.txt` 还会核对 `idf.py bmgr` 选中的板子（`CONFIG_ESP_BOARD_NAME`）与 `WISP_BOARD` 一致。

### 版本号

`cmake/wisp_version.cmake` 在配置时从 git 得出版本号并作为 `PROJECT_VER` 写进 `esp_app_desc_t`（模拟器用同一函数）：在 `vX.Y.Z` 标签上或之后是 `X.Y.Z[-N-g<hash>]`，没有标签时是 `0.1.0-<提交数>-g<hash>`，有未提交改动时加 `-dirty`。设备用它区分升级和降级（降级时确认框多一行提示）。配置只在提交或切换分支后自动重跑，只改文件不会刷新 `-dirty`。

### 确认与回滚

`sdkconfig.defaults` 打开了 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`：网页上传、设备上确认后，新镜像以"待验证"状态启动；`App::start()` 跑完、输出启动报告并启动控制台之后才调用 `FirmwareUpdater::mark_valid()`（启动流程第 7 步，见 `docs/architecture.md`）。在这之前崩溃、看门狗复位或断电，引导程序都会回到旧镜像。

### 构建配置：发布版与调试版

- 默认（`sdkconfig.defaults`）是发布版：全局 `-Os`（`CONFIG_COMPILER_OPTIMIZATION_SIZE`）、日志最高 INFO、断言静默。
- **渲染路径总是 `-O2`**，不论全局优化级别：`lvgl` 组件、`wisp_ui`、`esp_lcd*` 组件（面板与触摸驱动，由 `firmware/CMakeLists.txt` 按组件名匹配）、以及 `wisp_port_esp` 的 `dev/esp_display.cpp`、`dev/esp_touch.cpp`（LCD 刷新路径）。新板子的显示或触摸驱动如果不叫 `esp_lcd*`，要在 `firmware/CMakeLists.txt` 的匹配规则里补上，否则帧率会下降；构建时 CMake 会打印 `Rendering path built with -O2: ...` 供核对。
- **渲染加速按芯片选择**，不放进可移植层：`firmware/CMakeLists.txt` 按 `IDF_TARGET` 定义 `WISP_LV_DRAW_SW_ASM_INCLUDE`（指向 `wisp_port_esp` 里的钩子头文件），`lv_conf.h` 据此把 LVGL 软件渲染器切到 `LV_DRAW_SW_ASM_CUSTOM`。目前只有 ESP32-S3 有一组 SIMD 填充/拷贝例程（`components/wisp_port_esp/lv_blend/esp32s3/`）；其他目标不定义这个宏，走 LVGL 的 C 路径。新芯片要用自己的 SIMD 或 2D 硬件时，在 `lv_blend/<芯片>/` 下加钩子头文件和源文件，并在两处 CMake 里按目标挑选。
- 调试版叠加 `firmware/sdkconfig.debug`（`-Og`、日志到 DEBUG、断言开启），用单独的构建目录，不碰发布版的 `sdkconfig`：

```bash
cd firmware && idf.py -B build-debug -D SDKCONFIG=build-debug/sdkconfig \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;components/gen_bmgr_codes/board_manager.defaults;sdkconfig.debug" build
```

`SDKCONFIG_DEFAULTS` 一旦给出就替换默认列表，所以 bmgr 生成的 `board_manager.defaults` 要显式写进去，并放在 `sdkconfig.debug` 之前。

## 芯片差异

可移植层不出现芯片名。随芯片变化的东西集中在下面几处，换芯片时逐条核对：

| 差异 | 在哪里决定 |
|---|---|
| SIMD / 2D 渲染加速 | `wisp_port_esp/lv_blend/<芯片>/`，由 `firmware/CMakeLists.txt` 和 `components/wisp_port_esp/CMakeLists.txt` 按 `IDF_TARGET` 挑选（见上文"渲染加速按芯片选择"）；没有对应目录的芯片走 LVGL 的 C 路径 |
| 任务栈放内部 RAM 还是 PSRAM | 任务只声明 `flash_access`（AGENTS.md 第 5 节），由 ESP 端口的 `wisp/port/esp/stack_placement.hpp` 按目标决定；目前所有目标都是"不访问 flash 的任务才可能放 PSRAM" |
| DMA 能否直接读 PSRAM | 板子通过 Display caps 的 `draw_buf_mem` 声明（见上文能力字段表）；`Mem::DmaLarge` 在 `SOC_PSRAM_DMA_CAPABLE` 为假的芯片上总是分配失败，UI 退回内部 DMA RAM |
| 从 PSRAM 执行代码（XIP） | 板级 sdkconfig：`boards/<板名>/sdkconfig.defaults.board` 的 `CONFIG_SPIRAM_XIP_FROM_PSRAM`；它不放宽栈的规则 |
| 最低 ESP-IDF 版本 | 全项目一个范围，写在 `firmware/main/idf_component.yml` 的 `idf:`（目前 `>=6.0.3,<6.1`）；需要更新 IDF 的芯片要先单独升级这个范围和 `dependencies.lock` |
| 内部 SRAM 预算 | 每块板子用 `mem` 命令实测，记在 `docs/hardware/<板名>.md`；预算下限在 `wisp/app/ram_budget.hpp`（AGENTS.md 第 5 节） |

**计划中的目标：ESP32-S31**（未经验证，等有硬件后再核实）。已知情况：RISC-V 内核（`lv_blend/esp32s3/` 的 SIMD 例程不能用，需要另写或先走 C 路径）；flash 和 PSRAM 走各自独立的总线，PSRAM 为 250 MHz 八线 DDR；有 PPA、2D-DMA 和 JPEG 编解码硬件；需要 ESP-IDF ≥ 6.1，超出当前锁定范围。用 XIP 从 PSRAM 执行时 flash 操作不再停 cache，但 ESP-IDF 文档仍说 cache 关闭期间不能访问外部 RAM、任务栈默认在内部 RAM，所以栈的放置策略在实测前保持不变。

## 步骤 4：构建

```bash
bash -c '. ~/esp/esp-idf-v6.0.3/export.sh && cd firmware \
    && idf.py set-target <芯片，如 esp32s3> \
    && idf.py bmgr -c ../boards -b <板名> \
    && idf.py build -DWISP_BOARD=<板名>'
```

`firmware/CMakeLists.txt` 的 `WISP_BOARD` 缓存变量决定编译哪个 `boards/<名>/board.cpp`（默认 `waveshare_amoled_216`），用 `-DWISP_BOARD=<板名>` 覆盖。只要没改板子 YAML，后续重新 `idf.py build` 不需要重复 set-target/bmgr。调试版的构建命令见上文"构建配置：发布版与调试版"。

第一次烧录用 `idf.py -p <端口> flash`（引导程序、分区表和应用一起写）；之后可以在管理后台"系统"页上传 `build/wisp.bin` 更新，设备上确认后重启进入新版本。

## 步骤 5：验收

对照串口输出和自检界面，不需要额外工具：

1. **启动报告**（`WISP_BOOT_REPORT`）：每个设备状态应为 `ok`（或预期中的 `degraded`/`missing`，并带 `reason`），不应有意料之外的 `failed`。
2. **能力清单**（`WISP_MANIFEST`）：核对 `board` 字段是板名，每个设备的 `id`/`class`/`model`/`caps` 符合上面的约定；`Display` 的 `safe_area` 没有裁掉圆角/圆形屏的内容。
3. **自检界面**（spec 第 8 节）：8 类设备在安全区内都有对应的实时状态——设备列表按状态着色、IMU 小球随倾斜移动、触摸测试区跟手、按键事件有提示、电量和 RTC 时间正确、存储容量正确、录音回放能听到声音。
4. 新板子特有的真机验收项，仿照 `docs/hardware/<板名>.md`（新建）的"真机验收清单"一节，参照 `docs/hardware/waveshare_amoled_216.md` 的写法逐条列出怎么测、预期结果，留一栏空的"结果"栏。
