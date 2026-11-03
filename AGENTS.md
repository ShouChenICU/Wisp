# AGENTS.md — Wisp 项目指令

> 面向所有在本仓库工作的编程 agent（Claude Code、Codex 等）和人类贡献者。
> 这里只放全局的、长期稳定的内容：项目目标、规则、约定、工作流程。板子、芯片等硬件资料放在 `docs/hardware/`；进度放在各计划文档里。
> 这是一份活文档，规则或约定发生变化时要同步更新它（见文末"维护本文件"）。

## 1. 项目目标

Wisp（小鬼火）是一个运行在 ESP32 系列芯片上、完全由 AI 驱动的掌上个人 Agent 系统。

- **没有 App。** 没有传统的桌面、锁屏和应用。一切界面都是"场景"（scene），由 Agent 动态创建、编辑、保存和复用。设备初始只有一个用于对话的"主场景"。
- **Agent 掌控设备。** LLM 只使用 OpenAI 兼容协议（可以直连，也可以经网关），通过完整的工具调用来控制整台设备。
- **交互方式。** 以语音输入为主，以屏幕文字输出为主，TTS 为辅。
- **长期记忆。** Agent 会记住偏好、待办等信息。
- **轻量、可定制、通用。** 框架提供基础设施和外设抽象，驱动可以插拔，不绑定某一块开发板。

路线图共 8 个子项目，每个都单独走"设计 → 计划 → 实施"：
1. 基础骨架
2. 连接与管理后台
3. 场景运行时（Lua）
4. Agent 核心
5. 语音链路
6. 记忆与调度
7. 安全与演化
8. 主场景与系统外壳（鬼火角色与导航；渲染器原型可提前在模拟器中开发）

## 2. 文档与权威顺序

| 文档 | 用途 |
|---|---|
| `docs/superpowers/specs/*-design.md` | 各子项目的设计 spec，**最高权威** |
| `docs/superpowers/plans/*.md` | 实施计划，是 spec 的展开；两者冲突时以 spec 为准 |
| `docs/architecture.md` | 分层、启动流程、事件主题清单 |
| `docs/porting-guide.md` | 如何新增一块开发板 |
| `docs/hardware/<板名>.md` | 各开发板的硬件资料（引脚、芯片、已知勘误、性能基准） |
| `docs/references.md`、`THIRD_PARTY_NOTICES.md` | 参考来源与第三方许可 |

改动涉及某个子项目时，先读它的 spec，再读对应的计划；改动涉及某块板子时，先读它的硬件文档。

## 3. 架构规则

- **分层**：
  - 可移植层：`wisp_base` → `wisp_os` → `wisp_hal` → `wisp_core` → `wisp_net` → `wisp_admin` → `wisp_ui` → `wisp_app`。
  - 平台层：`wisp_port_sim`（PC 模拟器）、`wisp_port_esp`（ESP-IDF）。
  - 板级适配放在 `boards/<板名>/`，芯片驱动放在 `drivers/`。
- **可移植层禁止 include 平台头文件**：`esp_*`、`freertos/*`、`driver/*`、IDF 的 `hal/`、`soc/*`、`sdkconfig.h`、`SDL*`。另外 `wisp_os` 和 `wisp_hal` 不得反向依赖 core/ui/app。这条规则由 `tools/check_layering.py` 强制检查。
- **板子无关**：可移植层和平台层里不得出现具体板子的引脚、芯片型号或常量。这些只能写在 `boards/` 和 `drivers/` 中，通过设备的 caps 对外暴露。
- **服务层约定**：Agent 工具、REST 接口、Lua 绑定都只是同一组服务 API 的薄适配层，不允许绕过服务层直接调用驱动。
- **屏幕形状**：屏幕不一定是完整矩形（圆角屏、圆形屏）。任何界面（包括场景、Agent 生成的界面）都必须按 Display caps 的 `shape` 和 `hal::safe_rect()`/`row_span()` 布局，不得写死边距或假设整屏可见。根容器铺满屏幕、用内边距把内容收进安全区；滚动内容可以经过不可见区域，但滚到两端时首尾内容必须完整落在安全区内（spec 5.6）。
- **UI 单线程**：只有 UI 线程可以调用 LVGL，其他线程一律通过 `UiRuntime::post()` 把操作交给 UI 线程；脚本也不直接调用 LVGL。
- **设备模型**：
  - 设备注册在 `DeviceRegistry` 里，id 的格式为 `<类别>.<名字>`。
  - 设备能力通过 caps 声明，由 manifest 对外暴露。
  - 设备缺失或初始化失败时只降级运行，不得阻止启动。
- **存储**：数据分为 System / Library / Backup 三个逻辑区，所有持久化写入都走 `StorageManager::write_atomic`。

## 4. 构建与验证

```bash
# 主机侧：单元测试 + 模拟器（日常开发主力）
cmake -S simulator -B build/sim -G Ninja && cmake --build build/sim
ctest --test-dir build/sim --output-on-failure

# 提交前的完整检查：格式、分层、构建、测试
bash tools/ci.sh

# 固件：先激活计划锁定版本的 ESP-IDF 环境（export.sh 是 bash 脚本）
# 全新检出时先 set-target：bmgr 子命令来自组件管理器拉取的 esp_board_manager
cd firmware && idf.py set-target esp32s3 && idf.py bmgr -c ../boards -b <板名> && idf.py build
```

- 能在模拟器里验证的，就不要依赖真机。需要真机的验证步骤要明确写出来交给用户执行，不得声称已经验证过。
- 机器可读的输出（`WISP_BOOT_REPORT`、`WISP_MANIFEST`、`WISP_PERF`）都是"前缀 + 单行 JSON"。修改这些输出的格式时，要同步更新冒烟测试和文档。
- 改动渲染路径时，要对比 `WISP_PERF` 的数值，不得让帧率低于 spec 和计划中约定的基准。

## 5. 编码约定

- **语言与错误处理**：C++20，编译选项 `-fno-exceptions -fno-rtti`。不使用 `dynamic_cast` 和 `typeid`，错误一律通过 `wisp::Result<T>` 返回。
- **命名**：组件前缀 `wisp_`，命名空间 `wisp::<层>`，头文件路径 `wisp/<层>/<名>.hpp`，日志宏 `WLOG_D/I/W/E`。
- **组件 CMake**：使用 `wisp_component(...)`，让同一份 CMakeLists 同时兼容 idf.py 和普通 CMake。
- **格式与语言**：格式以 `.clang-format` 为准。代码标识符、注释和提交信息用英文；面向用户的界面文字可以用中文。
- **测试**：使用 GoogleTest，坚持 TDD（先写失败的测试，再实现）。测试输出要干净，不能有告警。
- **依赖**：外部依赖锁定精确版本，固件提交 `dependencies.lock`。
- **配置键**：Settings 的 namespace 和 key 都不能超过 15 个字符，以兼容 NVS。
- **内存分配**：大块缓冲用 `os::alloc(Mem::Large)`，DMA 缓冲用 `Mem::Dma`，不要直接调用平台的分配函数。
- **注释**：只解释现在的代码为什么这样写（约束、不变量、踩过的坑），不写"哪一轮评审/哪次修复改了什么"——修改历史属于提交信息，不属于注释。禁止引用 `.superpowers/` 下的任何文件（`task-*-brief/report/review.md`、`global-constraints.md` 等，这些不提交到 git，新检出后是死链），禁止评审编号式引用（"Fix round N"、"Re-review N"、"R2-N"、"Task N"、裸的发现编号如 "M8"/"N3"/"I1"）。需要引用上下文时改为指向已提交的权威来源：spec 某节（"spec 6.7" 可以保留）、AGENTS.md 某节、`docs/hardware/*.md`，或仓库里的另一个函数/文件。`tools/ci.sh` 对此有 grep 守卫。

## 6. 参考与许可

- 项目许可证为 Apache-2.0。
- **不 fork、不强依赖上游**。其他开源项目只作参考，按需有针对性地重新实现。
- 移植或改编外部代码时，在文件头注明来源和许可，并登记到 `docs/references.md` 与 `THIRD_PARTY_NOTICES.md`。
- 不得在代码、请求或日志中泄露用户的个人信息或密钥。API Key 只能写入、不能读出。

## 7. 工作流程

- **流程**：新功能或新子项目先写设计 spec，用户批准后写实施计划，用户审阅计划后才开始实施（brainstorming → writing-plans → subagent-driven-development）。
- **分支**：不在 `main` 上直接开发，使用功能分支。push、merge、发布之前先征得用户同意。
- **提交**：采用 Conventional Commits 风格（`feat(core): ...`、`docs: ...`）。agent 创建的提交，按当前环境的署名要求附上 `Co-Authored-By` 行。
- **沟通**：与用户交流一律使用中文。项目和模块取英文名，并用中文解释含义。
- **偏差处理**：发现 spec 或计划有误时不得静默绕过。小问题在计划或执行记录中写明裁定并继续推进；大问题停下来问用户。

## 8. 通用陷阱

- **FAT 上的原子写**：FAT 文件系统的 `rename` 不能覆盖已存在的文件。原子写采用两阶段命名：写 `.tmp` → fsync → 改名为 `.new`（此时 `.new` 一定完整）→ 删除目标 → `.new` 改名为目标。读取时目标缺失而 `.new` 存在则用 `.new` 恢复；`.tmp` 可能写了一半，一律视为垃圾丢弃。
- **LVGL 配置**：LVGL 必须使用 `components/wisp_ui/config/lv_conf.h`（由 `WISP_LV_CONF` 宏守卫）。
- **生成代码**：`idf.py bmgr` 生成的代码已被 git 忽略，不要手改。
- **bmgr 会编译板目录下的全部源文件**：生成的 `gen_bmgr_codes` 用 `SRC_DIRS` 扫描 `boards/<板名>/`，会把 Wisp 自己的 `board.cpp` 也编进去；`firmware/CMakeLists.txt` 负责把它剔除。板目录里新增的 `.c`/`.cpp` 默认都会进入 `gen_bmgr_codes`。

## 9. 维护本文件

- **何时更新**：
  - 项目目标、架构规则、约定或流程发生变化时（同时更新相关的 spec 或 `docs/architecture.md`）；
  - 发现了跨板子、跨子项目都适用的陷阱时。
- **不要写进来**：硬件资料（放 `docs/hardware/`）、进度和状态（放计划文档）、某次任务的细节（放代码或 spec）。
- **保持简洁**：过时的条目直接删除，不要堆积历史。
- **提交**：更新本文件的改动和触发它的改动放在同一个提交里，提交信息使用 `docs(agents): ...`。
