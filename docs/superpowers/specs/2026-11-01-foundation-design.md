# Wisp 子项目 1：基础骨架 设计文档

- 日期：2026-11-01
- 状态：待审阅
- 范围：Wisp 整体路线中的第 1 个子项目（基础骨架）

---

## 1. 背景与目标

### 1.1 Wisp 是什么

Wisp 是一个基于 ESP32-S3 的掌上个人全能 Agent 系统。它没有传统意义上的 App、桌面和锁屏，而是由 Agent 根据主人的需求动态创建、编辑和保存"场景"（场景 = 元信息 + 声明式 UI + Lua 逻辑）。出厂时只有一个用于对话的"主场景"，其他场景随着使用逐渐积累。

已确定的全局约束：

- 不绑定具体开发板，驱动可插拔；
- LLM 只实现 OpenAI 兼容协议，可直连或经网关；
- 交互：语音输入为主，屏幕文字输出为主，TTS 为辅；
- Agent 对设备拥有完整的控制权，同时有安全护栏；
- 构建：原生 ESP-IDF 6.x，C++20；
- 场景脚本语言：Lua。

### 1.2 整体路线（子项目拆分）

| # | 子项目 | 核心产出 |
|---|---|---|
| **1** | **基础骨架（本文档）** | 分层架构、双目标构建（真机 + PC 模拟器）、HAL 与能力清单、存储管理、首块真机点亮 |
| 2 | 连接与管理后台 | Wi-Fi 与配网、HTTP/WebSocket 服务、配对鉴权、后台网页框架（设备详情、外设管理、系统设置） |
| 3 | 场景运行时 | 声明式 UI → LVGL（根容器默认贴合屏幕安全区）、常驻 Lua VM、外设 Lua API（含屏幕形状与安全区）、场景后台页面、外设仲裁框架与前台场景模型（见 1.5） |
| 4 | Agent 核心 | 流式 OpenAI 客户端、工具注册表、场景工具（向模型提供屏幕形状与安全区）、截图/日志自检闭环（检查内容是否落在不可见区域）、外设占用查询与抢占请求工具（见 1.5） |
| 5 | 语音链路 | 唤醒词、VAD 断句、ASR、流式文字输出、可选 TTS、共享麦克风采集管线与喇叭音频焦点（见 1.5） |
| 6 | 记忆与调度 | 标签式长期记忆、结构化待办、事件规则与定时器、记忆后台页面 |
| 7 | 安全与演化 | 沙箱配额、危险操作确认、场景版本与回滚、主题令牌 |

每个子项目各自走"设计 → 计划 → 实现"流程。本文档只详细设计子项目 1，但目录结构和接口边界为后续子项目预留了位置。

### 1.3 子项目 1 的目标

1. 建立一套**可移植核心 + 平台适配层**的分层架构，让后续所有逻辑都能先在 PC 上开发和测试；
2. 在 Waveshare ESP32-S3-Touch-AMOLED-2.16 上点亮全部外设；
3. 提供统一的设备抽象和**能力清单**，作为以后 Agent、Lua、后台网页认识设备的唯一数据源；
4. 提供存储管理（内置 Flash + SD 卡分层）、事件总线、日志、配置等基础服务；
5. 提供一个自检界面作为验收依据。

### 1.4 不在本子项目范围内

Wi-Fi 与网络、HTTP 服务与后台网页、场景运行时与 Lua、Agent 与 LLM、完整语音链路（AEC/VAD/唤醒）、记忆、OTA 功能本身（只预留分区）、硬件占用登记表、外接模块热插拔、按需字体加载。

### 1.5 后续子项目的设计输入：外设仲裁

子项目 1 只保证并发访问时不崩溃、不损坏数据：设备内部线程安全，Display 只在 UI 线程调用，音频流独占（重复 `open()` 返回 `AlreadyExists`），按键事件广播给所有订阅者。至于"谁该使用某个外设"，子项目 1 不做仲裁。以下方向作为子项目 3、4、5 brainstorming 的起点，最终方案以各自的 spec 为准：

- **原则**：仲裁在服务层实现，HAL 接口保持不变；Agent 工具、Lua、REST 都通过同一套占用服务访问外设。不同资源的冲突性质不同，不使用同一把锁。
- **麦克风（共享采集、仲裁用途）**：只有一条采集管线，由音频服务持有（AEC/VAD 也在这条管线上），录音场景、Agent 等使用者都订阅它，因此"打开麦克风"本身不冲突。需要仲裁的是语音交给谁：前台场景声明自己正在采集语音时（如录音场景），Agent 的唤醒词暂停，或改为仅按住按键时生效；声明撤销后恢复。（子项目 5）
- **喇叭（音频焦点 + 混音）**：每路声音带优先级，例如闹钟 > Agent 回复 > 场景音效 > 背景音乐。高优先级的声音出现时，低优先级的声音压低音量或暂停，结束后恢复。（子项目 5）
- **屏幕与输入（前台场景 + 系统层）**：只有前台场景渲染并接收触摸和按键。系统层（关机确认、低电量提示、Agent 对话浮层）的优先级高于场景；电源键等系统键由系统先拦截。（子项目 3）
- **传感器（引用计数共享）**：IMU 等传感器允许多个使用者同时读取，采样率取各方请求的最大值；最后一个使用者退出时降频或关闭。（子项目 3）
- **占用登记表对 Agent 可见**：Agent 可以查询外设当前被谁占用，再决定等待、提示用户还是请求抢占。抢占他人正在使用的外设需要用户确认，与危险操作确认的规则一致。（框架在子项目 3，Agent 工具在子项目 4）

---

## 2. 关键决策

| 决策 | 结论 | 理由 |
|---|---|---|
| 构建框架 | 原生 ESP-IDF（`idf.py` + CMake），不用 PlatformIO | esp_board_manager 依赖 `idf.py bmgr`；新版 IDF 支持及时；组件管理器是一等公民；与模拟器共用 CMake 组织 |
| ESP-IDF 版本 | v6.0.x，锁定到具体补丁版本，写入 `docs/` 与 CI | 长期支持至 2028-09 |
| 双目标方案 | 方案 A：可移植核心 + 平台适配层（port） | 后续子项目的逻辑都能在 PC 上开发、测试和截图自检 |
| 语言 | C++20；不使用异常和 RTTI | 与 MyOS 和 xiaozhi 风格一致；主流 PC 编译器支持稳定 |
| 首块真机 | Waveshare ESP32-S3-Touch-AMOLED-2.16（目前唯一目标板） | 外设齐全，双麦适合语音 |
| 许可证 | Apache-2.0 | 与 MIT、Apache-2.0 来源兼容 |
| JSON 库 | ArduinoJson 7.x | 头文件库、无异常、可自定义分配器（PSRAM），PC 与 MCU 通用 |
| 单元测试 | GoogleTest（FetchContent 锁定版本，关闭异常） | 生态成熟 |

### 2.1 外部代码复用策略

不 fork、不以 submodule 依赖 xiaozhi-esp32 或 ESP-Claw。分三层处理：

1. **通用基础库**：LVGL、ESP-SR、esp_codec_dev、esp_audio_codec、esp_board_manager、Lua、ArduinoJson 等，通过 IDF 组件管理器（固件侧）或 FetchContent（模拟器侧）引入，锁定版本并提交 `dependencies.lock`；
2. **参考项目（xiaozhi、ESP-Claw）**：只参考设计，按 Wisp 的接口重新实现，在文件头和 `docs/references.md` 注明参考来源；
3. **细节价值高的小片段**（如 AFE 配置、Opus 参数、字体映射加载）：以函数或片段为单位移植并改写为 Wisp 接口，保留原版权声明，记入 `THIRD_PARTY_NOTICES.md`；Apache-2.0 来源需注明修改。

**MyOS**（用户本人的项目，MIT 许可）中的 CO5300、CST9220、AXP2101 驱动和电源键、功耗相关逻辑，可直接移植并包装为 Wisp HAL。移植时同步迁移其中涉及的第三方声明（Noto 字体 OFL、Fluent 图标 MIT）。

---

## 3. 分层架构

### 3.1 层次

```
┌─────────────────────────────────────────────────────┐
│  入口层   firmware/main (真机)   │  simulator/ (PC)   │
├─────────────────────────────────────────────────────┤
│  wisp_ui      LVGL 驱动对接、UI 任务、自检界面          │
├─────────────────────────────────────────────────────┤
│  wisp_core    日志 · 事件总线 · 设备注册表 · 配置 · 存储 │  ← 可移植
├─────────────────────────────────────────────────────┤
│  wisp_hal     外设接口 + 能力描述（纯接口）            │  ← 可移植
│  wisp_os      任务/队列/锁/定时器/时钟/内存（纯接口）    │  ← 可移植
├──────────────────────────┬──────────────────────────┤
│  wisp_port_esp           │  wisp_port_sim           │
│  FreeRTOS + IDF 驱动实现   │  std::thread + SDL2 实现   │
│  + boards/<板>            │  + 模拟外设               │
└──────────────────────────┴──────────────────────────┘
```

### 3.2 依赖规则

1. 可移植组件（`wisp_os`、`wisp_hal`、`wisp_core`、`wisp_ui`）**禁止 include 任何平台头文件**（`esp_*`、`freertos/*`、`driver/*`、`hal/*` 中的 IDF 头、`sdkconfig.h`、`SDL*`），只能依赖 C++20 标准库、LVGL、ArduinoJson，以及按层次自上而下依赖其他可移植组件；
2. 只有 `wisp_port_esp`、`wisp_port_sim`、`boards/*`、`drivers/*` 和入口层可以接触平台；
3. 入口层在启动时选择 port 并注入可移植层；
4. 规则由 `tools/check_layering.py` 在 CI 中强制检查；
5. 后续子项目（scene、agent、voice、memory、web 等）作为新的可移植组件加入，遵守同样的规则。

### 3.3 服务层的组织方式（为后续子项目预留）

后续子项目中，"删除场景"、"修改记忆"、"设置亮度"这类操作都**只在服务层实现一次**，Agent 工具、后台 REST/WebSocket 接口、Lua API 都是服务层之上的薄适配层，权限检查集中在服务层。子项目 1 不实现具体服务，只确立这一组织约定：服务放在各自的可移植组件中，以普通 C++ 接口暴露，不依赖任何传输方式。

---

## 4. 目录结构

```
wisp/
├── firmware/                  # ESP-IDF 工程：idf.py build/flash
│   ├── main/                  # app_main：选择板子 → 注入 port → 启动
│   ├── CMakeLists.txt
│   ├── idf_component.yml      # 依赖锁定版本
│   ├── sdkconfig.defaults
│   └── partitions.csv
├── simulator/                 # PC 工程：cmake + SDL2
│   ├── CMakeLists.txt         # LVGL 等依赖用 FetchContent 拉取，版本与固件一致
│   └── main.cpp
├── components/
│   ├── wisp_os/
│   ├── wisp_hal/
│   ├── wisp_core/
│   ├── wisp_ui/
│   ├── wisp_port_esp/
│   └── wisp_port_sim/
├── boards/
│   └── waveshare_amoled_216/  # esp_board_manager YAML + setup_device.c + board.cpp
├── drivers/                   # 组件库中没有的芯片驱动（从 MyOS 移植）
├── tests/                     # 可移植层单元测试（PC 运行）
├── tools/                     # check_layering.py、ci.sh、字体脚本等
├── docs/
│   ├── architecture.md
│   ├── porting-guide.md
│   ├── references.md
│   ├── hardware/waveshare_amoled_216.md
│   └── superpowers/specs/
├── .github/workflows/         # CI（推送到 GitHub 后生效）
├── .clang-format
├── .editorconfig
├── LICENSE                    # Apache-2.0
├── THIRD_PARTY_NOTICES.md
└── README.md
```

**双构建系统适配**：每个可移植组件的 `CMakeLists.txt` 用 `if(ESP_PLATFORM)` 区分。IDF 下走 `idf_component_register`，否则走普通 `add_library`。同一份源码被 `idf.py` 和普通 CMake 共用。

**LVGL 版本一致**：固件通过组件管理器锁定 LVGL 9.5.x，模拟器用 FetchContent 拉取同一版本标签；两侧共用同一份 `lv_conf` 配置来源。

**芯片驱动选择规则**：组件库中已有的驱动直接依赖；没有的从 MyOS 移植到 `drivers/`。实施计划阶段逐个核实并列出结果。

**目录按需创建**：子项目 1 只创建本子项目需要的目录，不为后续子项目建空目录。

---

## 5. HAL 与能力清单

### 5.1 原则

1. 按设备类别定义精简的 C++ 抽象接口，驱动负责把芯片包装成接口；
2. **能力自描述**：每个设备实例在代码中填写自己的能力描述，这是能力信息的唯一来源。板级 YAML 只描述引脚和总线；
3. **缺失即不存在**：板上没有、或初始化失败的外设不注册。

### 5.2 设备类别（子项目 1）

| 类别 | 接口要点 | Waveshare 2.16 实现 | 模拟器实现 |
|---|---|---|---|
| `Display` | 分辨率、形状、区域刷新、亮度、开关屏 | CO5300 AMOLED（QSPI） | 480×480 SDL 窗口 + 按形状绘制的遮罩 |
| `Touch` | 读取触点（多点） | CST9220 | 鼠标 |
| `Imu` | 读取加速度、角速度，设置采样率 | QMI8658 | 方向键模拟倾斜 |
| `Power` | 电量、充电状态、电压、关机 | AXP2101 | 模拟电池 |
| `Clock` | 读写 RTC 时间 | PCF85063 | 系统时钟 |
| `Button` | 按下/松开/短按/长按事件 | 电源键（AXP2101 PWRON + GPIO16）、Vol+（GPIO18）、Vol-（GPIO0） | 键盘 |
| `AudioOut` / `AudioIn` | 打开流、读写 PCM、音量、增益 | ES8311 / ES7210 + 功放（GPIO46） | SDL 音频 |
| `Storage` | 介质类型、挂载点、容量和剩余空间、在线状态 | 内置 Flash（LittleFS）、SD 卡（SPI，FAT） | 本机目录 |

子项目 1 的音频只要求跑通放音和录音回环，完整语音链路属于子项目 5。

### 5.3 接口风格

```cpp
namespace wisp::hal {

struct DeviceInfo {
    std::string id;        // "imu.main"
    DeviceClass cls;       // DeviceClass::Imu
    std::string model;     // "QMI8658"
    Properties caps;       // 结构化能力
};

class Device {
public:
    virtual ~Device() = default;
    virtual const DeviceInfo& info() const = 0;
};

class Imu : public Device {
public:
    virtual Result<ImuSample> read() = 0;            // accel(g), gyro(dps), ts_ms
    virtual Result<void> set_rate(uint32_t hz) = 0;
};

}  // namespace wisp::hal
```

- **`Result<T>`**：Wisp 自带的轻量实现，错误为统一的 `ErrorCode` 枚举加说明文字，便于以后原样交给 Agent；
- **`Properties`**：键值结构，值可以是 null、bool、int64、double、string、列表或嵌套映射，可与 JSON 互转。能力清单、事件负载、配置共用这一类型；
- **线程约定**：设备内部保证线程安全；`Display` 例外，只允许 UI 任务调用，调试构建中用断言检查；
- **Display 能力字段**包含 `area_align`（刷新区域坐标对齐要求）。UI 层据此自动对齐刷新区域，适配 CO5300 一类 QSPI AMOLED 的对齐限制。
- **Display 形状字段**：`shape` 取 `rect`、`rounded_rect`、`circle` 之一（缺省为 `rect`），`rounded_rect` 另带 `corner_radius`，`circle` 以短边为直径、居中。详见 5.6 节。

### 5.4 设备注册表（`wisp_core`）

- 启动时板级代码把设备注册进 `DeviceRegistry`，可按 id 查找，也可按类别列出；
- 同一类别允许多个实例（如以后的 `display.external`）；
- 注册表支持运行时注册和注销（为以后的外接模块预留；子项目 1 只在启动时注册）；
- 可导出**能力清单 JSON**。

### 5.5 能力清单示例（Waveshare 2.16，节选）

```json
{
  "board": "waveshare_amoled_216",
  "devices": [
    {"id": "display.main", "class": "display", "model": "CO5300",
     "caps": {"width": 480, "height": 480, "shape": "rounded_rect",
              "corner_radius": 40, "panel": "amoled", "brightness": true,
              "area_align": 2},
     "safe_area": {"x1": 12, "y1": 12, "x2": 467, "y2": 467}},
    {"id": "imu.main", "class": "imu", "model": "QMI8658",
     "caps": {"accel": true, "gyro": true, "magnetometer": false,
              "max_rate_hz": 1000}},
    {"id": "power.main", "class": "power", "model": "AXP2101",
     "caps": {"battery": true, "charging_status": true}},
    {"id": "storage.sd", "class": "storage", "model": "sdcard-spi",
     "caps": {"removable": true, "fs": "fat"}}
  ]
}
```

`area_align` 等驱动相关的具体数值，以移植驱动时核实的芯片要求为准。

---

### 5.6 屏幕形状与安全区

屏幕不一定是完整的矩形：圆角屏的四角、圆形屏的外圈都无法显示内容。Wisp 不允许任何界面假设屏幕是完整矩形。

- `wisp_hal` 提供与平台无关的几何函数，输入是 Display 的 caps：
  - `visible(x, y)`：像素中心是否落在可显示区域；
  - `safe_rect()`：可显示区域内最大的居中矩形。圆角屏四边各缩进 ⌈r·(1−1/√2)⌉（r=40 时为 12 px）；圆形屏四边各缩进 ⌈w/2 − R/√2⌉（直径 480 时为 71 px）；
  - `row_span(y)`：第 y 行可显示的横向范围，供圆形屏上逐行缩进的布局使用。
- 能力清单中，每个 Display 条目附带由几何函数算出的 `safe_area`，与 `caps` 平级。它是派生信息，设备不必自己填写。
- `shape` 值无法识别时按 `circle` 处理（最保守）并打告警。
- 子项目 1 的自检界面和模拟器遮罩都基于这组函数。后续子项目里，场景运行时的根容器默认贴合安全区，Lua 能读取屏幕形状和安全区；Agent 设计场景时，屏幕形状和安全区会写进提示词，截图自检时还会检查是否有内容落在不可见区域。

## 6. 运行时骨架

### 6.1 启动流程

```
入口 (app_main / sim main)
 ① 平台初始化     os 后端、日志输出、内存分配器
 ② 板级初始化     esp_board_manager 初始化总线与设备（模拟器侧创建模拟设备）
                  → 构造驱动 → 注册进 DeviceRegistry
 ③ 核心服务       事件总线、存储管理（挂载介质）、配置存储
 ④ UI 启动        LVGL 任务、绑定 Display 刷新与 Touch 输入
 ⑤ 自检界面       展示能力清单与各外设实时状态
```

**失败处理**：

- 某个设备初始化失败：记录日志，不注册该设备，写入启动报告，系统降级继续运行；
- `Display` 失败：不启动 UI，进入无屏模式，自检结果通过串口日志输出；
- 内置 Flash 存储挂载失败：自动格式化一次再重试，仍失败则进入只读降级（配置不持久化），并在启动报告中标明；
- **启动报告**为 JSON（每个设备的状态：ok / degraded / missing / failed，失败原因），启动完成后输出到串口或 stdout。

### 6.2 OS 抽象（`wisp_os`）

| 抽象 | ESP 实现 | 模拟器实现 |
|---|---|---|
| `Task` | FreeRTOS 任务（栈、优先级、核心） | `std::thread`（忽略核心亲和性） |
| `Queue<T>` | 有界队列 | 互斥锁 + 条件变量 |
| `Mutex` | FreeRTOS 互斥锁 | `std::mutex` |
| `Timer` | esp_timer（单次/周期） | 定时线程 |
| 时钟 | `now_ms()`、`sleep_ms()` | `std::chrono` |
| 内存 | `alloc(size, Mem::Large)` → PSRAM；`Mem::Fast` → 内部 RAM；`Mem::Dma` → DMA 可用内存 | `malloc` |

只封装上表内容，不做成通用 RTOS 抽象。

### 6.3 事件总线（`wisp_core`）

```cpp
struct Event {
    std::string topic;     // "button.power.short"、"storage.sd.removed"
    std::string source;    // 设备 id
    uint64_t    ts_ms;
    Properties  payload;
};
bus.subscribe("button.", handler);   // 前缀订阅，返回订阅句柄，可取消
bus.publish(event);                  // 异步：入队后由分发任务派发
```

- 单个分发任务，按发布顺序派发；处理函数必须快速返回，需要更新界面时用 `ui::post()`；
- 队列有界（默认 64）；满时丢弃最旧的事件，计数并记录警告日志；
- 子项目 1 的主题：`button.<id>.{down,up,short,long}`、`power.charging.changed`、`power.battery.low`、`storage.sd.{inserted,removed}`、`system.boot.completed`。

### 6.4 UI 线程模型（`wisp_ui`）

- **只有 UI 任务可以调用 LVGL**；其他任务通过 `ui::post(fn)` 投递；
- `wisp_ui` 基于 `hal::Display` 和 `hal::Touch` 实现 LVGL 的显示驱动和输入驱动，因此不依赖 esp_lvgl_port，两个目标共用；
- 像素格式 RGB565；显示缓冲区按 `Mem::Dma`/`Mem::Large` 分配，采用局部刷新和双缓冲，缓冲区大小在真机上根据帧率调优；
- LVGL tick 由 `wisp_os` 时钟提供。

### 6.5 日志

- 宏 `WLOG_D/I/W/E(tag, fmt, ...)`；
- 输出：ESP 日志或 stdout，以及 PSRAM 中 32KB 的内存环形缓冲区（模拟器侧为普通内存）；
- 环形缓冲区提供读取接口，供后续子项目的后台网页与 Agent 读取。

### 6.6 配置存储

- 接口 `Settings(namespace).get<T>(key, default)` / `set(key, value)`；
- 真机基于 NVS，模拟器基于数据目录下的 JSON 文件；
- 子项目 1 存储的项：屏幕亮度、音量。

---

## 7. 存储管理

### 7.1 介质

| 介质 | 文件系统 | 挂载点（真机） | 模拟器 |
|---|---|---|---|
| 内置 Flash `storage` 分区 | LittleFS | `/int` | `<data-dir>/int/` |
| SD 卡（SPI） | FAT | `/sd` | `<data-dir>/sd/` |

### 7.2 逻辑区

| 逻辑区 | 内容（含后续子项目） | 位置规则 |
|---|---|---|
| `System` | 主场景、系统配置、身份与密钥、记忆核心索引 | 固定在内置 Flash：`/int/system` |
| `Library` | 场景库、记忆完整内容与归档、日志、录音 | 有序根目录列表：SD 卡在线时为 `[/sd/wisp/library, /int/library]`，否则为 `[/int/library]`；**写入**总是进入列表第一个根；**读取**按顺序枚举所有根 |
| `Backup` | 内置数据备份、导出包 | 仅 SD 卡：`/sd/wisp/backup`；SD 卡不在时不可用 |

- `/int/library` 设置容量上限（默认 3MB），超过后写入返回 `ErrorCode::QuotaExceeded`；
- 上层代码只通过 `StorageManager` 获取逻辑区路径，不写死路径。

### 7.3 接口

```cpp
std::vector<std::string> roots(Area area);    // Library 为有序列表
Result<std::string>      write_root(Area area);
Result<void> write_atomic(const std::string& path, std::span<const uint8_t> data);
StorageStatus status();                        // 各介质在线状态、容量、剩余空间
```

- **原子写**：写入同目录下的临时文件，flush 后改名覆盖目标文件；
- **SD 卡热插拔**：通过 SD 卡检测或周期性探测发现插拔，发布 `storage.sd.inserted` / `storage.sd.removed`。拔出后 SD 上的 `Library` 根从列表中移除，相关写入返回 `ErrorCode::StorageUnavailable`；插回后重新挂载并恢复列表，数据不删除；
- 模拟器中按 `S` 键切换"SD 卡插入/拔出"，或用 `--no-sd` 启动。

### 7.4 分区表（16MB）

| 分区 | 类型 | 偏移 | 大小 | 用途 |
|---|---|---|---|---|
| nvs | data/nvs | 0x9000 | 24KB | 配置 |
| otadata | data/ota | 0xF000 | 8KB | OTA 状态 |
| phy_init | data/phy | 0x11000 | 4KB | 射频校准 |
| ota_0 | app/ota_0 | 0x20000 | 4MB | 固件 A |
| ota_1 | app/ota_1 | 0x420000 | 4MB | 固件 B |
| assets | data/spiffs（原始映射） | 0x820000 | 2MB | 字体、图标、开机资源、后台网页（子项目 2） |
| storage | data/littlefs | 0xA20000 | 约 5.9MB（0x5E0000） | LittleFS（`/int`） |

OTA 功能本身不在子项目 1 中实现，只预留双分区。

---

## 8. 自检界面

单页界面。所有内容放在显示设备的安全区（5.6 节）内，不假设屏幕是完整矩形；在圆角屏和圆形屏上都不能有内容被裁掉。

- 顶部：板名、固件版本、启动耗时；
- 设备列表：读取注册表和启动报告，按状态（正常 / 降级 / 缺失 / 失败）着色；
- IMU：实时数值，加一个随倾斜移动的小球；
- 电源：电量、充电状态；RTC：当前时间；
- 存储：内置 Flash 与 SD 卡的在线状态、已用和总容量，SD 卡插拔实时更新；
- 触摸测试区：显示当前触点；
- 按键事件提示；
- "录音回放"按钮：录制 3 秒到 PSRAM 后立即播放；
- 电源键：短按开关屏；长按弹出关机确认，确认后通过 AXP2101 关机（逻辑移植自 MyOS）。模拟器中关机即退出进程。

中文字体沿用 MyOS 已生成的字库子集。

### 8.1 模拟器操作与参数

| 输入 | 模拟 |
|---|---|
| 鼠标 | 触摸 |
| `P` 键（短按 / 按住 ≥ 1 秒） | 电源键短按 / 长按 |
| `+` / `-` 键 | Vol+ / Vol- |
| 方向键 | IMU 倾斜 |
| `S` 键 | SD 卡插入 / 拔出 |

| 参数 | 作用 |
|---|---|
| `--headless` | 使用 SDL dummy 驱动，无窗口运行 |
| `--screenshot <path>` | 自检界面渲染完成后保存 PNG |
| `--exit-after-ms <n>` | 运行指定时间后退出 |
| `--disable <id或类别>` | 模拟该设备初始化失败（可重复） |
| `--no-sd` | 启动时无 SD 卡 |
| `--data-dir <path>` | 数据目录（默认 `./sim-data`） |
| `--shape <rect\|rounded_rect\|circle>` | 模拟屏幕形状（默认 `rounded_rect`，半径 40），用于检查界面在不同形状屏幕上的表现 |

---

## 9. 测试与工具链

### 9.1 工具链

- ESP-IDF v6.0.x（当前本机未安装，实施计划第一步安装并锁定补丁版本）；
- PC：GCC 16 / Clang、CMake 4.x、Ninja、SDL2（本机已具备）；
- 编辑器：VS Code + ESP-IDF 插件 + clangd，两个工程都生成 `compile_commands.json`。

### 9.2 测试分层

1. **PC 单元测试**（GoogleTest）：`Result`、`Properties` 与 JSON 互转、事件总线（前缀订阅、顺序、溢出策略、取消订阅）、`DeviceRegistry`（注册、查找、注销、能力清单导出）、`StorageManager`（逻辑区解析、原子写、SD 插拔后根目录变化、容量上限）、`Settings`（模拟器实现）、`wisp_os` 的队列与定时器；
2. **模拟器冒烟测试**：`--headless --screenshot out.png --exit-after-ms 3000` 启动，检查进程正常退出、启动报告和能力清单 JSON 与预期一致、截图为 480×480 且非空；降级用例 `--disable imu` 时启动报告标记 IMU 为失败且进程不崩溃；`--no-sd` 时 `Library` 只有内置根；
3. **真机验收**：固件编译由 CI 使用乐鑫官方 `espressif/idf` 镜像（与本机同一版本）完成；烧录和硬件功能按第 10 节清单人工验证。

### 9.3 工具与规范

- `tools/check_layering.py`：依赖规则检查（见 3.2 节）；
- `tools/ci.sh`：依次执行格式检查、依赖规则检查、单元测试、模拟器冒烟测试，与 CI 内容一致；
- `.clang-format`、`.editorconfig`；
- GitHub Actions：PC 测试任务和固件编译任务。

---

## 10. 验收标准

**真机（Waveshare 2.16）**

1. `idf.py build flash` 成功，上电后 3 秒内进入自检界面；
2. 自检界面中 8 类设备状态正确：IMU 小球随倾斜移动；触摸显示触点；3 个按键均有事件提示；显示电量和充电状态；断电重启后 RTC 时间正确；录音回放能听到录音；内置 Flash 与 SD 卡容量正确，SD 卡插拔实时反映；
3. 电源键短按开关屏；长按弹出关机确认，确认后关机，关机状态长按可开机；
4. 串口输出能力清单 JSON 和启动报告，其中 `imu.main` 的 `magnetometer` 为 `false`。

**模拟器**

5. 运行后弹出 480×480 窗口，界面与真机一致，8.1 节的输入映射全部可用；
6. 单元测试、冒烟测试（含降级与无 SD 卡用例）、依赖规则检查全部通过。

**文档**

7. README（模拟器与真机两条快速开始路径）、`docs/architecture.md`、`docs/porting-guide.md`（如何新增一块板）、`docs/hardware/waveshare_amoled_216.md`、`docs/references.md`、`THIRD_PARTY_NOTICES.md` 齐备。

---

## 11. 风险与对策

| 风险 | 对策 |
|---|---|
| esp_board_manager 或其他依赖组件尚未适配 IDF 6 | 实施计划第一阶段先做依赖兼容性验证；不兼容时固定在可用版本，或只在 `boards/` 内用手写初始化代码替代，不影响可移植层 |
| 自研 LVGL 显示驱动在 QSPI AMOLED 上帧率不足 | 优先参照 MyOS 已调通的刷新方式；缓冲区大小与 DMA 策略在真机调优 |
| FAT 在掉电时损坏 | 所有写入走原子写；关键数据放内置 LittleFS |
| 模拟器与真机行为不一致 | LVGL 版本与配置同源；可移植层零平台依赖并由 CI 检查 |
| MyOS 驱动含有不明来源的代码 | 移植前核对 MyOS 的 `THIRD_PARTY_NOTICES.md`，同步迁移声明 |
