<div align="center">

# Wisp

**一个完全由 AI 驱动的个人掌上 Agent 系统，运行在 ESP32-S3 设备上。**

![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)
![Platform](https://img.shields.io/badge/platform-ESP32--S3-E7352C.svg)
![Status](https://img.shields.io/badge/status-早期开发中-orange.svg)

</div>

> AI 是 Wisp 的灵魂，是属于你的专属 Agent，掌控设备的一切。

你可以和 Wisp Agent 说，“我想要一个待办app”，“我想要个水平仪”，或者“帮我做个2048小游戏”，Wisp Agent 会根据你的需求现场帮你制作独一无二的应用，你可以稍后重新打开它们，也可以随时让 Agent 修改它们，即时生效，不用重新编译或外部计算机参与。

## ✨ 亮点

- **界面由 Agent 生成。** 每个功能都是一个"场景"，包含界面描述、Lua 脚本和自己的数据。场景由 LLM 编写、保存、修改，运行在沙箱里。
- **Agent 能控制整台设备。** 屏幕、音频、传感器、存储，以及场景本身，都以工具调用的形式交给 LLM。前台场景还能把自己的工具临时注入给 Agent。
- **模型由你选。** 支持任何 OpenAI 兼容接口，云端服务、自建网关、本地模型都可以。
- **长期记忆和定时任务。** 它记得你的偏好和交代过的事，到时间会主动执行。
- **你的设备你做主。** 管理后台只在局域网内，用配对码登录。API Key 写入后无法读出。危险操作必须在设备上确认。
- **不绑定某一块板子。** 硬件通过分层抽象和能力声明（caps）接入，换板子只需写板级适配。大部分开发在 PC 模拟器里完成。

## 🧩 核心模块

### 🔥 小鬼火与系统外壳

主界面是黑色背景上的一团蓝色小鬼火。它会燃烧、飘动，还有一双白色眼睛，在待机、收听、思考、执行工具、输出、充电等状态下，表情和动作都不一样。小鬼火完全用代码实时绘制，不依赖图片素材，在 AMOLED 上只重绘火焰所在的小块区域，能适配不同分辨率和圆形屏。

操作靠手势和一个对话键：

| 操作 | 作用 |
| --- | --- |
| 按住 BOOT 键 | 和 Agent 对话，松开发送；在场景里按住时，底部会浮出收听指示 |
| 主界面右划 / 左划 | 设置页 / 场景列表 |
| 主界面上划 / 下划 | 小鬼火颜色预设 / 对话记录 |
| 场景内从左边缘右划 | 返回主界面 |

外壳用原生 C++ 实现，不是 Lua 场景。这样即使某个场景写坏了，你也总能退回主界面修复它。

### 🎭 场景

Wisp 上的每个功能都是一个场景，由 Agent 编写，以目录形式保存：

```
scenes/<scene_id>/
  scene.json   名称、版本、权限、需要的外设
  ui.json      声明式界面
  main.lua     逻辑
  assets/      图标、图片
  data/        场景自己的结构化数据
  files/       大文件（录音、导出等）
  .history/    代码的历史版本，可回滚
```

场景运行在 Lua 沙箱里，看不到文件系统，只能通过 `wisp.store`（键值和集合）和 `wisp.files` 读写属于自己的数据。Agent 修改场景时只替换代码，不会动你的数据。改坏了可以回滚到旧版本。场景可以导出为模板分享给别人，也可以连同数据一起完整备份。

### 🤖 Agent

Agent 通过 OpenAI 兼容接口接入你选的模型，对设备拥有完全的控制权，控制分两层：

- **平台工具**：始终可用，包括创建、修改、打开、删除场景，读写场景代码和数据，截图、读取界面树、模拟点击，调整屏幕、音量等外设。哪怕某个场景卡死，Agent 也能接管和修复。
- **场景工具**：场景可以用 `wisp.tools.register()` 导出自己的工具，比如记账场景导出"记一笔"。场景在前台或后台运行时，它的工具会自动提供给 Agent，所以你在任何地方都能说"记一笔 32 元午饭"。

反过来，场景也能主动找 Agent，比如界面上的"问 AI"按钮或者闹钟到点提醒。场景工具的返回值一律当作数据处理，不会被当成指令执行。删除、外发这类危险操作，无论由谁发起，都必须在设备上确认。

### 🎙️ 语音

按住对话键说话，语音经过断句和识别变成文字交给 Agent，回复以文字流式显示在屏幕上，TTS 朗读可选。麦克风由一条共享的采集管线统一管理；扬声器按优先级分配，比如闹钟优先于 Agent 回复，Agent 回复又优先于背景音乐。

### 🧠 记忆与调度

Agent 会把你的偏好、习惯和待办记在设备上，并且能设置定时任务和事件规则：到点提醒你，或者在某个事件发生时自动执行。

### 🌐 管理后台

设备连上 Wi-Fi 后，在同一局域网的浏览器里打开管理后台，用设备屏幕上显示的配对码登录，就能配置模型和 API Key、查看设备和外设状态、管理场景和数据。后台只在局域网内可用，API Key 写入后无法再读出。

### 🔋 省电

熄屏后，Wisp 会暂停渲染、关闭不用的传感器、让 CPU 降频并进入浅睡。后台场景（比如播放音乐）只保留自己需要的外设。哪些场景在耗电、从什么时候开始，Agent 和管理后台都能看到。

> 这些模块大多还在设计阶段，进度见下方的[当前状态](#-当前状态)。

## 📦 硬件

目前支持 **Waveshare ESP32-S3-Touch-AMOLED-2.16**：480×480 AMOLED 触摸屏，双麦克风，扬声器，IMU，RTC，电池管理，SD 卡槽，不需要焊接。其他 ESP32-S3 板子可以按[移植指南](docs/porting-guide.md)适配。

## 🚀 动手试试

### 在电脑上运行模拟器

不需要任何硬件，在 Linux 电脑上就能跑一个和真机一样的 Wisp 窗口。

```bash
# Arch Linux
sudo pacman -S cmake ninja sdl2 python
# Ubuntu / Debian
sudo apt-get install cmake ninja-build libsdl2-dev python3

git clone https://github.com/ShouChenICU/Wisp.git wisp && cd wisp
cmake -S simulator -B build/sim -G Ninja
cmake --build build/sim
./build/sim/wisp_sim
```

在窗口里用鼠标代替手指；`P` 是电源键（按住 1 秒以上是长按）；`+` 和 `-` 是音量键；方向键模拟倾斜设备；`S` 插拔 SD 卡。

### 刷进真机

需要先安装乐鑫的 ESP-IDF 开发环境，版本固定为 **v6.0.3**。装好后在仓库目录执行：

```bash
. ~/esp/esp-idf-v6.0.3/export.sh
cd firmware
idf.py set-target esp32s3
idf.py bmgr -c ../boards -b waveshare_amoled_216
idf.py build
idf.py -p <串口> flash monitor
```

上电后几秒内会出现自检界面。前面三条准备命令只在第一次构建时需要，之后改了代码只要 `idf.py build` 再刷一次。如果你用的是 fish 等非 bash 终端，需要用 `bash -c '...'` 把命令包起来，因为 `export.sh` 只支持 bash。

---

## 🧱 系统是怎么搭起来的

Wisp 用 C++20 编写，界面基于 LVGL 9.5，真机运行在 ESP-IDF v6.0.3 和 FreeRTOS 上。同一套代码可以编译成两个目标：ESP32-S3 固件，以及基于 SDL2 的 PC 模拟器。绝大部分逻辑都在模拟器里开发和测试，只有少量驱动代码必须上真机验证。

代码分为可移植层和平台层：

```
  firmware/main（真机入口）          simulator/（模拟器入口）
 ─────────────────────────────────────────────────────────
  wisp_app    启动流程、自检界面、电源管理
  wisp_ui     LVGL 对接、UI 线程
  wisp_core   日志、事件总线、设备注册表、配置、存储管理      可移植层
  wisp_hal    外设接口和能力描述
  wisp_os     任务、队列、锁、定时器、内存
  wisp_base   Result、Value、JSON
 ─────────────────────────────────────────────────────────
  wisp_port_esp                     wisp_port_sim             平台层
  FreeRTOS + ESP-IDF                std::thread + SDL2
  boards/ 板级定义 + drivers/ 驱动    模拟外设
```

可移植层不能引用任何平台头文件，也不能依赖比自己更高的层。和具体板子有关的引脚、芯片型号、常量都只能出现在 `boards/` 和 `drivers/` 中。这些规则由 `tools/check_layering.py` 自动检查。

几个设计上的要点：

- **设备模型。** 每个外设注册在 `DeviceRegistry` 里，id 形如 `display.main`、`storage.sd`。设备的能力通过 caps 声明，启动时以 `WISP_MANIFEST` 输出，将来 Agent 也靠它了解设备。某个设备初始化失败时，系统会跳过它继续启动。
- **屏幕形状。** 屏幕可能是圆角或圆形的。所有界面都按 Display caps 给出的安全区布局，不写死边距。
- **UI 单线程。** 只有 UI 线程调用 LVGL，其他线程通过 `UiRuntime::post()` 投递任务。调试构建会检查这条约束。
- **存储。** 内置 Flash 用 LittleFS，SD 卡用 FAT，统一由 `StorageManager` 管理，支持热插拔和配额。所有持久化写入都用能承受掉电的两阶段原子写。
- **待机。** `PowerManager` 是屏幕状态的唯一管理者。待机时它会暂停渲染，让屏幕进入睡眠，给 IMU 断电，暂停 SD 卡探测，并允许 CPU 进入浅睡。电量和充电状态通过事件总线发布。
- **机器可读输出。** `WISP_BOOT_REPORT`（启动报告）、`WISP_MANIFEST`（设备清单）和 `WISP_PERF`（帧率、CPU、刷屏耗时）都是"前缀 + 单行 JSON"，方便脚本和测试解析。

更多细节见 [架构文档](docs/architecture.md)、[移植指南](docs/porting-guide.md) 和 [硬件资料](docs/hardware/waveshare_amoled_216.md)。

### 目录结构

```
components/   可移植层 wisp_base … wisp_app，平台层 wisp_port_esp / wisp_port_sim
boards/       板级定义（esp_board_manager YAML + board.cpp）
drivers/      芯片驱动（AXP2101、PCF85063、QMI8658）
firmware/     ESP-IDF 工程
simulator/    PC 模拟器
tests/        GoogleTest 单元测试和模拟器冒烟测试
tools/        ci.sh、check_layering.py
docs/         架构、移植、硬件资料、设计 spec 和实施计划
```

### 模拟器参数

| 参数 | 作用 |
| --- | --- |
| `--headless` | 不打开窗口运行 |
| `--screenshot <path>` | 渲染完成后保存截图（PNG） |
| `--exit-after-ms <n>` | 运行 n 毫秒后退出 |
| `--disable <设备 id 或类别>` | 模拟设备初始化失败，可重复使用 |
| `--no-sd` | 启动时不插 SD 卡 |
| `--data-dir <path>` | 数据目录，默认 `./sim-data` |
| `--shape <rect\|rounded_rect\|circle>` | 模拟不同形状的屏幕 |

## 🚧 当前状态

**早期开发中。** 第 1 阶段"系统底座"已经完成：屏幕、触摸、按键、音频、存储、电源管理和待机省电都已就绪，在真机和模拟器上都能跑。LLM 相关功能从第 3、4 阶段开始落地。

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| 1 | 系统底座：设备抽象、存储、电源、自检界面 | ✅ |
| 2 | 联网与局域网管理后台 | 🔜 |
| 3 | 场景运行时（Lua 沙箱、场景包、数据存储） | 📋 |
| 4 | Agent 核心（工具调用、场景控制） | 📋 |
| 5 | 语音链路（录音、识别、TTS） | 📋 |
| 6 | 记忆与调度 | 📋 |
| 7 | 安全与自我演化 | 📋 |
| 8 | 主界面：一团会动的小鬼火，加上手势导航 | 📋 |

<p align="center">
  <img src="docs/images/self-check.png" alt="当前的自检界面" width="300"><br>
  <sub>当前固件的自检界面</sub>
</p>

## 🛠️ 参与开发

开始之前请先读 [`AGENTS.md`](AGENTS.md)。项目的规则、约定和工作流程都写在那里，人类开发者和 AI 编程助手都要遵守。每个阶段都有设计文档（[`docs/superpowers/specs/`](docs/superpowers/specs/)）和实施计划（[`docs/superpowers/plans/`](docs/superpowers/plans/)），设计文档的优先级最高。

提交前运行完整检查：

```bash
pip install clang-format==23.1.3   # 版本必须和 CI 一致
bash tools/ci.sh
```

它会检查代码格式、分层规则和注释规范，然后构建模拟器并运行全部单元测试和冒烟测试。在主机上运行时，还可以加上 ThreadSanitizer 或 AddressSanitizer 构建，检查线程和内存问题。

提交信息采用 [Conventional Commits](https://www.conventionalcommits.org/) 风格。

## 📄 许可

Wisp 以 [Apache License 2.0](LICENSE) 开源。

部分芯片驱动移植自作者的 MyOS 项目（MIT 许可）。用到和参考的第三方项目登记在 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) 和 [`docs/references.md`](docs/references.md)。
