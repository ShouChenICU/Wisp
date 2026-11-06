# Wisp 子项目 4a：Agent 核心 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 设备上运行的 Agent 能在网页"对话"页和设备对话浮层里对话，写场景并自检（界面树、界面检查、截图、模拟输入），操作已有场景，读写设备设置，不可逆操作由用户在设备上确认。

**Architecture:** `net::LlmClient` 在已有的 `HttpClient::on_body`/`on_status` 上实现 OpenAI Chat Completions 的流式客户端（自己解析 SSE）。新组件 `wisp_agent`（分层在 `wisp_scene` 与 `wisp_app` 之间）包含：`ConversationStore`（`<System>/agent/` 下的 JSONL 对话与归档），`ToolRegistry`（工具登记、参数校验、分派到执行线程），`AgentService`（任务 `wisp.agent`，负责对话循环、上下文、用量、确认），`SceneWorkspace`（草稿、校验、安装、`.prev`），以及各组工具。`scene::UiInspector` 放在场景层，负责界面树、界面检查、截图和模拟输入。入口有三个：`wisp_admin` 的 `/api/v1/agent` 与 WebSocket `agent` 订阅（经 `admin::AgentApi` 接口，由 `wisp_app` 适配）、网页 `chat.js`、`ui::SystemLayer` 的对话浮层。

**Tech Stack:** C++20（`-fno-exceptions -fno-rtti`）、LVGL 9.5.0、Lua 5.5.1、ESP-IDF v6.0.3（`esp_new_jpeg` 1.0.2 的编码器）、ArduinoJson（经 `wisp/base/json.hpp`）、GoogleTest、Preact + htm（网页）、Python 3 标准库（`fake_openai.py`、冒烟测试、文档生成）。

**Status:** 已批准（2026-11-05）；等 3b-1 做完并合并进本分支后开始执行。

**Spec:** `docs/superpowers/specs/2026-11-05-agent-core-design.md`（下文"spec N"均指它；"3a spec"指 `2026-11-04-scene-runtime-core-design.md`，"3b-1 spec"指 `2026-11-05-scene-device-design.md`，"连接 spec"指 `2026-11-03-connectivity-admin-design.md`）

## Global Constraints

逐字摘自 AGENTS.md 的适用规则：

- **可移植层禁止 include 平台头文件**：`esp_*`、`freertos/*`、`driver/*`、IDF 的 `hal/`、`soc/*`、`rom/*`、`lwip/*`、`mbedtls/*`、`curl/*`、`sdkconfig.h`、`nvs.h`/`nvs_flash.h`、`sdmmc_cmd.h`、`SDL*`，以及平台层的 `wisp/port/*`。每一层只能依赖排在它之前的层，由 `tools/check_layering.py` 强制检查。本期分层：`wisp_base` → `wisp_os` → `wisp_hal` → `wisp_core` → `wisp_net` → `wisp_admin` → `wisp_ui` → `wisp_scene` → `wisp_agent` → `wisp_app`。
- **板子无关**：可移植层和平台层里不得出现具体板子的引脚、芯片型号或常量。平台层可以按 SoC 目标选择实现。
- **服务层约定**：Agent 工具、REST 接口、Lua 绑定都只是同一组服务 API 的薄适配层，不允许绕过服务层直接调用驱动。
- **屏幕形状**：任何界面都必须按 Display caps 的 `shape` 和 `hal::safe_rect()`/`row_span()` 布局，不得写死边距或假设整屏可见；滚动内容滚到两端时首尾内容必须完整落在安全区内（3a spec 5.6）。
- **UI 单线程**：只有 UI 线程可以调用 LVGL，其他线程一律通过 `UiRuntime::post()`/`run_blocking()` 交给 UI 线程。
- **存储**：所有持久化写入都走 `StorageManager::write_atomic`；只追加的对话日志走 `StorageManager::append()`（与 `SceneStore` 相同的例外，见细化第 6 条）。
- **语言与错误处理**：C++20，`-fno-exceptions -fno-rtti`，不用 `dynamic_cast`/`typeid`，错误一律经 `wisp::Result<T>` 返回。
- **命名**：组件前缀 `wisp_`，命名空间 `wisp::<层>`（本期新增 `wisp::agent`），头文件 `wisp/<层>/<名>.hpp`，日志宏 `WLOG_D/I/W/E`，组件 CMake 用 `wisp_component(...)`。
- **格式与语言**：以 `.clang-format` 为准；标识符、注释、提交信息用英文；界面文字中/日/英。
- **测试**：GoogleTest，TDD（先写失败的测试），输出干净无告警。
- **配置键**：Settings 的 namespace 和 key 都不超过 15 个字符（`agent`/`day_tokens` 合规）。
- **内存**：对话、上下文 JSON、SSE 缓冲、工具结果、截图、草稿内容都放 PSRAM：`os::alloc(Mem::Large)`、`os::LargeString`、`os::LargeVector`、`os::make_large`。小块 `std::string`/`std::vector`/`Value` 默认落在内部 RAM，只能用于短小的临时值。
- **任务**：每个新任务都要如实声明 `os::TaskConfig::flash_access`。`wisp.agent` 声明 `false`，所以它不能直接调用会访问 flash 的东西：Settings/NVS（包括 `SystemSettings::get/set`）、`StorageManager`、`SceneRepository`、`SceneFiles`、`ModelProfileService` 的修改方法。这些一律作为 `wisp.fs` 上的作业执行（`StorageWorker::submit`），或者交给第 6 条细化里的一次性 flash 任务。消耗内部 RAM 的改动必须上真机重测（`wisp/app/ram_budget.hpp`）。
- **注释**：只解释现在的代码为什么这样写；不写评审轮次，不引用 `.superpowers/`，不用"Task N"之类的编号；`tools/ci.sh` 有 grep 守卫。
- **密钥与隐私**：API Key 只在 `LlmClient` 里经 `ModelProfileService::active_for_request(ModelKind::Llm)` 取得，只放进 `Authorization` 头。Key 不进对话、日志、工具结果、REST/WS 输出或界面。请求与回复的内容不写日志。
- 能在模拟器里验证的就不依赖真机。需要真机的步骤交给用户执行，不得声称已经验证。
- **分支与提交**：在 `feat/agent-core` 上开发；Conventional Commits；每个提交结尾附 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`。**不 push。** `README.md` 不改。

本计划的固定约束（spec 原文数值，集中在 `components/wisp_agent/include/wisp/agent/limits.hpp`，测试直接引用这些常量）：

- 每轮最多 24 次工具调用（`kMaxSteps`），每轮最长 10 分钟（`kTurnMs = 600000`）。排队最多 1 条，再多返回 `busy`。用户消息最长 4000 字符，按 Unicode 码点计（`kMaxUserChars`）。
- 重试：429、5xx 退避后重试 2 次，间隔 1 s、4 s（`kRetryDelaysMs = {1000, 4000}`）；401 不重试。
- 上下文：token 粗估为"字节数 ÷ 3"；超过 `context_window` 的 75% 时压缩；`context_window` 默认 65536；历史中每条工具结果最多 4 KB（`kToolResultKeepBytes = 4096`）；截图只保留最新一张，更早的替换为"[截图已省略]"。
- 工具：`scene.read_file`、`scene.files_read` 单次最多 64 KB（65536）；`scene.data_read` 最多 16 KB；`scene.logs` 的 n ≤ 100；`scene.eval` 最多 200 ms。各工具超时见 spec 4.1–4.4 的表（5/10/30 s）。
- 自检：等待界面稳定，连续 300 ms 没有重绘，最多等 2 s；自动检查的 `W`、`E` 日志各最多 20 条；`ui.tree` 超过 2000 字符时折叠，首尾各保留 5 行；`small_target` 阈值 48×48 px；`low_contrast` 阈值 3:1；截图宽 240 px、JPEG 质量 70。
- 确认：30 秒无操作视为拒绝；同一时间最多一个待确认请求。
- WS 工具摘要：`args`、`result` 各最多 200 字符。
- 对话：最多保留 5 个归档。对话浮层的行高至少 48 px。
- 唤醒锁：持有者 `system.agent`，类型 `cpu` 与 `network`，原因 `"agent_turn"`。
- 验证命令：主机 `cmake -S simulator -B build/sim -G Ninja && cmake --build build/sim && ctest --test-dir build/sim -R '<正则>' --output-on-failure`；提交前 `bash tools/ci.sh`；固件 `bash -c '. ~/esp/esp-idf-v6.0.3/export.sh >/dev/null && cd firmware && idf.py build'`。

## Review Focus

1. **设备在流式输出中途断网**（Wi-Fi 掉线、路由器重启）。期望：`net.wifi.state` 离开 `connected` 后 2 秒内中止请求；收不到事件时，60 秒没有新字节也会中止。已收到的文字保留，对话里记一条"网络中断"说明；唤醒锁释放，状态回到 `idle`；不重试，也不卡在 `thinking`。网络恢复后下一条消息正常进行。测试：Task 4 `AgentLoop.WifiLossMidStreamEndsTurnCleanly`、`AgentLoop.StalledStreamTimesOut`；Task 1 `LlmClient.CancelMidStreamKeepsPartialText`。
2. **模型给出畸形的工具参数**：不是合法 JSON、类型不对，或者 `finish_reason` 为 `length`、参数只生成了一半。期望：工具结果为 `bad_args` 并指明原因，作为一步回给模型，这一轮继续；不崩溃，不把半截参数交给工具执行。测试：Task 3 `ToolRegistry.InvalidJsonArgumentsAreBadArgs`；Task 4 `AgentLoop.MalformedToolArgumentsGoBackAsBadArgs`、`AgentLoop.LengthCutoffWithPartialToolCallIsBadArgs`。
3. **两个网页标签页（加上设备浮层）同时发送**。期望：恰好一条在执行、一条排队，其余得到 409 `busy`；同一时间只有一个模型请求；两个标签页看到同一条流。测试：Task 12 `AgentRoutes.ConcurrentSendsQueueOneRefuseRest`。
4. **场景在自动检查期间崩溃**（启动后几百毫秒内在定时器或 `on_show` 里出错，或卡死超时）。期望：`scene.install` 照常返回，`check.lua_errors` 带上错误原文；之后 `ui.check` 返回 `scene_error` 而不是超时或崩溃；Agent 可以继续修改或 `rollback`。测试：Task 8 `AutoCheck.SceneFaultingDuringSettleIsReported`。
5. **2000 个控件的场景调用 `ui.tree()`**。期望：输出不超过 2000 字符且仍可读，有折叠提示；在 5 s 超时内返回；UI 线程单次被占用不超过 100 ms；可以用 `ref` 参数展开某个子树。测试：Task 7 `UiTree.TwoThousandWidgetsStaysUnderLimit`。

---

## 依赖分支

本分支 `feat/agent-core` 包含 3b-1 的全部内容（到 3b-1 计划的 Task 12）。写计划时已核实下列接口存在，名称以代码为准：

- `net::HttpClientRequest`：`on_body`、`on_status`、`cancelled`、`watch`（`RequestWatch`）、`timeout_ms`、`body_ref`。两个端口的 `HttpClient` 都允许并发调用。`net::TlsGate` 由 `EspHttpClient` 自己在握手期间持有，调用方不用碰它。
- `core::ModelProfileService`：`active_for_request(ModelKind)`、`test(id)`、`set_prober(ModelProber)`、`ModelOptions` 是扁平的字符串表（每项最多 64 字节）。`net::probe_models`/`make_model_prober`。
- `core::SceneRepository::install(zip, SourceChecker, before_replace)`、`load()`、`export_zip()`、`remove()`、`installing()`/`begin_removing()`；`core::install_scene(repo, host, zip)`；`core::ZipWriter`；`SceneHost::check_sources()`（不能在 UI 线程和 `wisp.fs` 上调用）。
- `scene::SceneRuntime`：`open/close/exit/reload/launch/open_background/status/foreground()/background_scenes()/click_ref()/check_sources()/close_async()`；`collect_tree()`、`schema_markdown()`、`nearest_name()`、`kMaxNodes = 2000`；`SceneView::find_ref/object`；`SceneInstance` 可以单独用一个包和 `VmLimits` 构造。
- `ui::UiRuntime::snapshot_rgb888()`、`run_blocking()`、`perf().frames`；`ui::SystemLayer::open_confirm/close_confirm/confirm_open()/show_keyboard/keyboard()/show_banner/set_panel_handlers/update_panel/raise()`；`PanelModel`、`PanelHandlers`；`app::StatusShell`；`ui::tr_fill`、`system_notice_text()`。
- `core::NotificationService::post_system(code, arg, ...)`（`kArgMax = 64`）；`core::WakeLockRegistry`、`core::WakeLock`；`core::SystemSettings`（`get()` 会读 NVS）；`StorageManager::write_atomic/append/read_file_large/list_dir/remove_tree/rename`；`StorageWorker::submit`。
- `admin::WsHub`（`{"sub":[...]}`、`{"t":...}` 消息、每个客户端有消息队列上限、最多 2 个连接）；`detail::Route`、`scene_stream()` 流式上传先例；缓冲请求体上限 8 KiB（`kMaxBodyBytes`）；ESP 的 httpd 是单任务。
- `wisp/base/json.hpp`（`to_json`、`from_json(s, max_depth)`）；`os::LargeString/LargeVector/LargeBuffer/make_large`；`os::Mem::DmaLarge`。
- 固件已依赖 `espressif/esp_new_jpeg ==1.0.2`（目前只用解码器，`firmware/main/esp_new_jpeg.lf` 把它整个放进 flash）。模拟器用 LVGL 自带的 TJPGD 解码 JPEG。
- `tools/fake_openai.py`（目前只有 `GET /v1/models`）；`tests/smoke/admin_smoke.py`；`tests/support/fake_http.hpp` 的 `FakeHttpClient`；`tests/scene/examples_test.cpp` 的 `Examples` fixture（三种屏形）。

## 相对 spec 的细化（写计划时裁定，执行前请用户确认）

1. **工具的线上名称**：OpenAI 的 function name 只允许 `^[a-zA-Z0-9_-]{1,64}$`，不能含点。所以请求里的名称把 `.` 换成 `_`（`scene.install` → `scene_install`，`wire_name()`）。spec、网页、设备、日志和系统说明里仍然显示带点的名称。
2. **`LlmClient` 与 TLS**：`TlsGate` 已由 `EspHttpClient` 在握手期间持有，所以 `LlmClient` 不直接使用它，只保证同一时间只发一个模型请求（`AgentService` 是唯一调用方）。URL 为 `<base_url>/chat/completions`。请求头带 `Accept: text/event-stream`；请求体带 `"stream":true,"stream_options":{"include_usage":true}`。整个请求的期限取"这一轮剩余时间"与 300 s 中较小的一个。
3. **重试**：只有尚未收到任何响应体的 429、5xx 才重试：最多 2 次，间隔 1 s、4 s，忽略 `Retry-After`；等待期间响应"停止"。401 返回 `unauthorized`；其他 4xx 返回 `http`；传输错误返回 `transport`，不重试。
4. **流的中止**：`AgentService` 给每次请求配一个 `RequestWatch`。下列情况调用 `abort()`：用户点停止；`net.wifi.state` 离开 `connected`；60 s 没有新字节（`kStallMs = 60000`，由 `wisp.agent` 的 250 ms 检查定时器判断）。中止后保留已收到的文字，并记一条说明。
5. **模型配置的新选项**：`ModelOptions` 是字符串表，所以 `vision` 存为 `"true"`/`"false"`，`context_window` 存为十进制字符串，范围 4096–1048576，缺省 65536。其他值让 `create`/`update` 返回 `InvalidArgument`。"测试连接"对 `llm` 配置额外做一次视觉探测（`net::probe_vision`）：发一张内嵌的 8×8 JPEG，`max_tokens` 为 1。200 → `true`，400/422 → `false`，其他结果不确定、不改动。结果有确定值时由 `test()` 写回 `options.vision`，用户之后仍可在模型页手动修改。`ModelProber` 增加一个 `ModelKind` 参数。
6. **对话文件**：
   - 位置：`<System>/agent/current.jsonl` 与 `archive-<n>.jsonl`。`n` 单调递增，只保留最大的 5 个，`GET /archive` 按新到旧列出。
   - 每行一条消息：`{"seq","ts","role","turn",...}`。`role` 取 `user`（`text`、`origin: web|device`）、`assistant`（`text`、`calls:[{id,name,args}]`）、`tool`（`call_id`、`name`、`ok`、`result` 全文、`image: bool`）、`note`（系统说明，`text`）、`summary`（`text`、`covers: [first_seq, last_seq]`）、`marker`（`kind: turn_begin|turn_end`、`status: done|stopped|step_limit|time_limit|error|interrupted`）。
   - 追加写用 `StorageManager::append()`；载入时丢弃没有 `\n` 结尾的残行，先用 `write_atomic` 重写文件再继续追加（同 `SceneStore`）。
   - 截图的 JPEG 只放在内存里，不写进文件，所以重启后显示为"[截图已省略]"。
   - `current.jsonl` 超过 1 MiB 时，下一条用户消息先自动归档（相当于"新对话"），并记一条说明。这样 flash 不会被一个对话占满。
   - 恢复出厂时删除 `<System>/agent/`。
7. **线程**：
   - `wisp.agent`：优先级 3，栈 16 KB 起，`flash_access = false`，栈在 PSRAM。
   - 工具的执行位置用 `ToolThread` 声明：`Agent`（本任务）、`Ui`（`UiRuntime::run_blocking` 等价的投递）、`Fs`（`wisp.fs` 作业）、`Io`。
   - `Io` 是每次调用新建的一次性任务 `wisp.agent.io`：`flash_access = true`，栈 8 KB 起，在内部 RAM，做完即退出。原因：`install_scene` 要写 flash，又要等场景任务，所以 `wisp.agent` 和 `wisp.fs` 都不能跑它（`wisp.fs` 上等待会死锁）。这与恢复出厂的 `wisp.reset` 一次性任务是同一个模式。
   - spec 写的"场景任务"执行位置，由 `SceneRuntime` 自己的请求 API 实现（`eval`、`store_dump`、`store_set`）：在 `Agent` 线程上调用，内部投递消息并等待。
8. **安装时的"建树检查"**：新增 `SceneRuntime::check_build(package)`。它在场景任务上用一次性的 `SceneInstance` 运行入口文件并 `collect_tree()`，然后丢弃。这个 VM 用独立的 `VmPool`（2 MB，不计入 3 MB 共享预算），场景数据是一个空的、不落盘的临时 `SceneStore`，`http`、`files`、`notify`、`power`、`sensor` 都未接通（调用时返回各自的 `unavailable`/抛错，与没有服务时一样），预算 200 ms。它失败时安装失败，已安装版本不动。安装后启动阶段的其他错误（`on_show`、定时器里的错误）由自动检查报告（spec 5.1）。
9. **草稿与 `.prev`**：草稿在 `<data_root>/.drafts/<id>/`，`.prev` 在 `<code_root>/.prev/<id>/`（以 `.` 开头，`list()` 本来就跳过）。
   - 安装：把草稿打包成内存 zip（`ZipWriter`），在 `Io` 任务上调用 `install_scene(repo, host, zip, InstallOptions{.keep_prev = true})`。`SceneRepository::install` 在替换的第 4 步改为"删除旧 `.prev/<id>`，把 `<id>` 改名为 `.prev/<id>`"。安装成功后在前台打开（`host.open(id)`），并删除草稿。
   - `rollback`：把 `.prev` 打包成 zip，用同一条路径安装，于是当前版本成为新的 `.prev`（spec "install 和 rollback 都会替换它"）。
   - 删除场景时一并删除 `.prev/<id>`。
   - 开发模式（`--scenes-dir`）下 `install`、`rollback` 返回 `unsupported`。
10. **安装错误的形状**：`{"ok":false,"errors":[{"file","line","message","suggestion"}]}`。`file`、`line` 从 `"<file>:<line>: <msg>"` 解析，解析不出时为 `"scene.json"`/0。`suggestion` 的规则：未知控件或属性时，用 `scene::nearest_name()` 给出最接近的名称（"did you mean `label`?"）；其他错误给出对应的 `docs.read` 主题。
11. **`ui.tree`**：
    - 每行 `<缩进><类型>#<ref> "<文字>" [x,y w×h] <状态>`，缩进 2 空格一层。没有 `ref` 的控件用合成名 `n<NodeId>`，`ui.tap({ref})` 也接受合成名。文字截到 40 字符。
    - 状态取值：`hidden`、`disabled`、`checked`、`focused`、`scroll=<当前>/<最大>`。
    - 系统层的每个部件一行 `system <名称> [x,y w×h]`，名称为 `capsule`/`banner`/`panel`/`error`/`keyboard`/`confirm`/`chat`。
    - 场景部分整体用 `scene_data()` 包裹（6.3）。
    - 新增可选参数 `ref`：只输出该控件的子树（spec 只有无参形式，这是为大界面增加的，属于新增能力）。
    - 折叠算法见 Task 7。
12. **`ui.check` 的判定**见 Task 7 Step 3。示例场景在三种屏形上必须零问题：发现真问题就修示例，发现误报就修判定，不放宽阈值。
13. **模拟输入**：spec 说"真机和模拟器各实现一个"注入点。改为在可移植的 `ui::UiRuntime` 里实现一个：`inject_pointer()` 排入合成触点，指针输入设备的读回调优先消费它们。这一层在 `SystemLayer` 包装的读回调之下，所以注入的输入照样经过返回、下拉手势判断，真机和模拟器走同一条路径。
    - `tap`：按下 60 ms 后松开。
    - `swipe`：每 10 ms 一个插值点。
    - 没有 `touch.main` 时（UiRuntime 没有指针设备）返回 `unavailable`。
    - `ui.type(text)`：对键盘正在输入的文本框（新增 `SystemLayer::keyboard_target()`）调用 `lv_textarea_add_text`，由它触发 `VALUE_CHANGED`，场景收到 Change；没有就返回 `no_focus`。
14. **截图与 JPEG**：
    - 新增 `Platform::jpeg_encode`（类型 `ui::JpegEncodeFn`）。真机用 `esp_new_jpeg` 的编码器（同一个 1.0.2 依赖；输入、输出缓冲用 `Mem::DmaLarge` 满足 16 字节对齐）；模拟器在 `wisp_port_sim_ui` 里重新实现一个基线 JPEG 编码器，参考 jo_jpeg（public domain），按 AGENTS.md 第 6 节登记来源。
    - 输出宽 240，高按 `safe_rect` 的比例向下取整到 16 的倍数（两个编码器都要求 4:2:0 的 MCU 对齐），用盒式滤波缩放。
    - 截图只在内存里保留最新一张，网页经 `GET /agent/screenshot` 取。
15. **界面稳定**：轮询 `UiRuntime::perf().frames`，每 20 ms 一次，连续 300 ms 不变即视为稳定，最多等 2 s。结果附 `stable: bool`。
16. **确认**：`agent::ConfirmService` 经 `SystemLayer::open_confirm` 弹框（标题 `AgentConfirmTitle`，"允许"按钮用 `ButtonRole::Danger`）。确认框已被别的对话框占用时（恢复出厂、网页发起的删除场景），在同一个 30 s 窗口里等它空出来，超时视为拒绝。用户点停止时关闭确认框，结果为 `cancelled`。确认文字用新的 `ui::tr_fill_n()` 填多个值。
17. **系统说明行**：由系统按设备当前语言生成文字（`ui::tr`），存为 `note` 消息，网页显示为灰色。进入上下文的方式：给该次工具结果加一个字段 `"system_note":"<文字>"`，表示已向用户显示。这样不在历史中间插入额外的 system 或 user 消息，前缀缓存不受影响。
18. **动态信息**放在请求的最后一条 `{"role":"system"}` 消息里，每次请求重新生成，不存储。固定部分（第一条 system 消息）对同一台设备和同一种语言逐字节不变。
19. **上下文压缩**：估计值超过窗口的 75% 时，从最早的整轮开始选，直到剩余部分不超过窗口的 40%；最近 2 轮永不压缩。用同一个模型额外请求一次（不带工具，固定的摘要提示），结果存为 `summary` 消息。这次请求失败时，改为直接丢弃这些轮次（原文仍在文件里），并记一条说明。
20. **用量**：`<System>/agent/usage.json` 记 `{"day":"YYYY-MM-DD","day_tokens","month":"YYYY-MM","month_tokens"}`，按本地日期（`utc_offset_min`）换日；时钟未设置时不换日。每轮结束后在 `wisp.fs` 上 `write_atomic`。`day_tokens` 上限是 `SystemSettings` 的新键 `day_tokens`（`agent`/`day_tokens`，整数 0–100000000，0 表示不限，默认 0），随 `GET/PUT /settings` 与 `system.settings` 事件一起出现。
21. **REST 的两处偏差**：
    - `POST /agent/messages` 改为流式请求：4000 个汉字的 UTF-8 约 12000 字节，超过缓冲请求体 8 KiB 的上限。照 `scene_stream()` 的先例收集，上限 16 KiB，超出返回 413。
    - `DELETE /agent/conversations` 返回 202 `{"pending":true}`，结果经 WS `{"t":"agent","k":"cleared","ok":bool}` 送达，不同步返回 403。原因：ESP 的 httpd 是单任务，同步等 30 秒会冻结整个管理后台。这与 `DELETE /scenes/{id}` 的做法一致。
22. **WS**：客户端订阅 `"agent"`。`WsHub::send_agent(json)` 只发给订阅了 `agent` 的客户端。`delta` 每 100 ms 合并一帧。网页收到 `{"t":"dropped"}` 时重新 `GET /conversation`。
23. **`admin::AgentApi`**：`wisp_admin` 在 `wisp_agent` 之下，所以路由经纯虚接口 `admin::AgentApi` 访问 Agent，由 `wisp_app` 的 `AgentWiring` 适配。`AdminDeps::agent` 为空时，所有 `/agent` 路由返回 503 `unavailable`。
24. **网页导航**："对话"是第一个标签，手机宽度的标签栏变成 6 项。
25. **设备浮层**：对话浮层是 `ui::SystemLayer` 的新部件（`open_chat`/`update_chat`/`close_chat`），放在 `raise()` 顺序中面板之上、错误面板之下。浮层打开时，返回手势只关闭浮层。熄屏完成时发系统通知，`code = "agent_done"`，`arg` 为回复的第一行（截到 64 字节）；点击通知打开浮层。
26. **冒烟可观察的日志**（只有元信息）：
    - `agent: turn <n> start`；
    - `agent: tool <name> <ok|error:<code>> <ms> ms`；
    - `agent: turn <n> end <status> steps=<k> tokens=<t>`；
    - `llm: model=<m> status=<s> ms=<t> tokens=<u>`。
27. **`docs.read`**：
    - `widgets`、`widget/<name>` 在运行时由 schema 生成（`schema_markdown()`，新增 `widget_markdown(name)`），永不过期。
    - `lua`、`lua/<module>`、`screen`、`examples`、`example/<id>` 由 `tools/gen_agent_docs.py` 从 `docs/scene-api.md` 和 `scenes/examples/*` 生成 `components/wisp_agent/src/docs_data.cpp`，并提交进仓库（同字体数据的先例）。`tools/ci.sh` 用 `--check` 检查它是否过期。
    - spec 要求按 Lua 模块分主题，但 `docs/scene-api.md` 目前只写了 UI 和 `wisp.store`，所以本期补上 `wisp.timer`、`wisp.http`、`wisp.device`、`wisp.scene`、`wisp.log`、`wisp.notify`、`wisp.power`、`wisp.sensor`、`wisp.files` 的章节（内容以 `bind_*.cpp` 和 3a、3b-1 spec 为准）。
28. **工具结果**：
    - 成功为 `{"ok":true,...}`，失败为 `{"ok":false,"error":"<code>","message":"..."}`。
    - 错误码：`bad_args`（附 `field`）、`unknown_tool`、`timeout`、`cancelled`、`busy`、`not_found`、`not_unique`（附 `count`）、`bad_path`、`too_large`、`quota_exceeded`、`denied`、`unavailable`、`unsupported`、`no_scene`、`scene_error`、`not_visible`、`no_focus`、`invalid_value`、`failed`。
29. **场景内容包裹**：`scene_data(id, text)` 生成 `<data source="scene:<id>">…</data>`，内容里的 `</data>` 转义为 `&lt;/data>`。用于 `scene.data_read`、`files_read`、`logs`、`eval` 的结果，`ui.tree`、`ui.check` 里场景的文字，`notify.list` 里场景通知的标题和正文，以及 `scene.read_file`（场景源码也来自场景，可能被下载内容污染）。
30. **事件**：总线 `agent.state`，`source` 为 `"agent"`，payload `{"state", "tool"}`，其中 `tool` 只在 `state` 为 `tool` 时出现、为带点的名称。这个事件写进 `docs/architecture.md` 的事件清单，不加进 `WsHub` 的转发前缀（网页经 `agent` 订阅拿状态）。
31. **测试替身**：spec 8.1 的 `FakeLlmServer` 实现为进程内的 `net::HttpClient` 替身（`tests/support/fake_llm.hpp`），按脚本回放 SSE 分块、状态码、断流和挂起，结果确定，单元测试和集成测试都用它。冒烟测试需要真正的 HTTP 服务，由扩展后的 `tools/fake_openai.py`（`POST /v1/chat/completions`）承担。

---

## 文件结构总览

```
components/
  wisp_base/  include/wisp/base/json.hpp                       # append_json_string<S>()
  wisp_core/  include/wisp/core/{model_profiles,system_settings,scene_repository,scene_install}.hpp  (改)
  wisp_net/   include/wisp/net/llm_client.hpp  src/{llm_client,sse_parser}.cpp
              include/wisp/net/model_probe.hpp  src/model_probe.cpp (改: probe_vision)
  wisp_admin/ include/wisp/admin/agent_api.hpp  src/routes_agent.cpp  (admin_internal, admin_server, ws_hub: 改)
  wisp_ui/    include/wisp/ui/{jpeg,chat_overlay}.hpp  src/chat_overlay.cpp
              (ui_runtime: inject_pointer; system_layer: keyboard_target, visible_pieces, chat; strings: 改)
  wisp_scene/ include/wisp/scene/ui_inspector.hpp  src/{ui_inspector,ui_tree,ui_check,ui_input,ui_shot,check_build}.cpp
              (scene_runtime: check_build, eval, store_dump, store_set, ui_current_view; scene_view: for_each_node;
               schema: widget_markdown: 改)
  wisp_agent/ CMakeLists.txt
              include/wisp/agent/{limits,conversation,tool_registry,agent_service,context,usage,
                                  scene_workspace,confirm,tools,docs}.hpp
              src/{conversation,tool_registry,args_schema,agent_service,context,prompt,usage,
                   scene_workspace,tools_scene_authoring,tools_scene_ops,tools_ui,tools_device,
                   docs,docs_data,confirm}.cpp
  wisp_app/   src/{agent_wiring,agent_shell}.{hpp,cpp}  (app.cpp, app_impl.hpp, app.hpp Platform::jpeg_encode,
              scene_wiring: delete_scene, factory_reset, notification_router, status_shell: 改)
  wisp_port_esp/ ui/esp_jpeg_encoder.cpp  include/wisp/port/esp/esp_jpeg_encoder.hpp
  wisp_port_sim/ ui/jpeg_encoder.cpp      include/wisp/port/sim/jpeg_encoder.hpp
firmware/main/main.cpp  simulator/main.cpp  simulator/CMakeLists.txt  firmware/main/CMakeLists.txt
web/app/pages/{chat,models,system}.js  web/app/{main,icons,md}.js  web/app/i18n/{zh,ja,en}.js  web/styles/*
tools/{fake_openai.py,gen_agent_docs.py,gen_agent_docs_test.py,check_layering.py,check_layering_test.py,ci.sh}
tests/agent/*  tests/support/{fake_llm,agent_fixture}.hpp  tests/{net,core,scene,ui,admin,app}/...  tests/smoke/admin_smoke.py
docs/{architecture,porting-guide,scene-api,references}.md  THIRD_PARTY_NOTICES.md  AGENTS.md
docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md
```

---

### Task 1: `net::LlmClient` 与模型配置的 `vision`/`context_window`

**Files:**
- Create: `components/wisp_net/include/wisp/net/llm_client.hpp`、`components/wisp_net/src/llm_client.cpp`、`components/wisp_net/src/sse_parser.cpp`（加入 `wisp_net` 的 SRCS）
- Create: `tests/support/fake_llm.hpp`（`FakeLlmServer`，细化第 31 条）
- Modify: `components/wisp_base/include/wisp/base/json.hpp`（`append_json_string`）
- Modify: `components/wisp_core/include/wisp/core/model_profiles.hpp`、`src/model_profiles.cpp`（选项校验、`ModelProber` 签名、`test()` 写回 `vision`）
- Modify: `components/wisp_net/include/wisp/net/model_probe.hpp`、`src/model_probe.cpp`、`components/wisp_admin/src/routes_ai.cpp`（`test()` 不再是 const）
- Test: `tests/net/llm_client_test.cpp`、`tests/net/sse_parser_test.cpp`（新）、`tests/core/model_profiles_test.cpp`、`tests/net/model_probe_test.cpp`、`tests/base/value_json_test.cpp`

**Interfaces:**
- Produces（`wisp::net`，`llm_client.hpp`）：

```cpp
enum class LlmRole : uint8_t { System, User, Assistant, Tool };
struct LlmToolCall { std::string id; std::string name; os::LargeString arguments; };
struct LlmMessage {
    LlmRole role = LlmRole::User;
    std::string_view text;
    std::span<const uint8_t> jpeg;            // User only: adds an image_url part
    std::span<const LlmToolCall> tool_calls;  // Assistant only
    std::string_view tool_call_id;            // Tool only
};
enum class LlmFinish : uint8_t { Stop, ToolCalls, Length, Other };
struct LlmUsage { uint32_t prompt = 0, completion = 0, total = 0; };
struct LlmResult {
    // "" ok; "no_model" "unauthorized" "rate_limited" "server" "http" "transport"
    // "cancelled" "bad_stream"
    std::string error;
    TransportError transport = TransportError::None;
    int status = 0;                       // last attempt; 0 without a response
    os::LargeString text;
    std::vector<LlmToolCall> tool_calls;  // by stream index
    LlmFinish finish = LlmFinish::Other;
    std::optional<LlmUsage> usage;
    uint32_t attempts = 0;
    bool any_body = false;                // a delta or tool fragment arrived
    std::string model;                    // the profile's model name
};
struct LlmCall {
    std::span<const LlmMessage> messages;
    std::string_view tools_json;          // a JSON array, "" for none
    uint32_t timeout_ms = 300000;
    RequestWatch* watch = nullptr;        // armed by complete(); aborted by the caller
    std::function<void(std::string_view)> on_text;
    std::function<void()> on_bytes;       // any body bytes (stall detection)
    std::function<bool()> cancelled;      // also stops the retry waits
};
os::LargeString build_chat_body(std::string_view model, std::span<const LlmMessage> msgs,
                                std::string_view tools_json);
class SseParser {
public:
    static constexpr size_t kMaxEventBytes = 256 * 1024;
    // Calls on_data with each event's data (multi-line data joined by '\n').
    void feed(std::span<const uint8_t> bytes, const std::function<void(std::string_view)>& on_data);
    bool overflowed() const;
};
class LlmClient {
public:
    static constexpr uint32_t kRetryDelaysMs[2] = {1000, 4000};
    LlmClient(HttpClient& http, core::ModelProfileService& models,
              std::function<void(uint32_t)> sleep_ms = os::sleep_ms);
    LlmResult complete(const LlmCall& call);
};
```

- Produces（`wisp_base`）：`template <class S> void append_json_string(S& out, std::string_view s);`，输出与 `to_json(Value(s))` 逐字节相同，可以追加到 `std::string` 或 `os::LargeString`。
- Produces（`wisp::core`）：`inline constexpr const char* kOptVision = "vision"`、`kOptContextWindow = "context_window"`；`bool profile_vision(const ModelOptions&)`；`uint32_t profile_context_window(const ModelOptions&)`（缺省 65536）；`struct ModelProbe` 增加 `std::optional<bool> vision`；`using ModelProber = std::function<ModelProbe(const ResolvedProfile&, ModelKind)>`；`Result<ModelProbe> test(std::string_view id)`（非 const）。
- Produces（`wisp::net`）：`std::optional<bool> probe_vision(const core::ResolvedProfile&, HttpClient&)`。
- Produces（`tests/support/fake_llm.hpp`，`wisp::test`）：`class FakeLlmServer : public net::HttpClient`，提供：
  - `push(FakeReply)`，其中 `FakeReply{int status; std::vector<std::string> chunks; net::TransportError fail = None; int hold_after = -1;}`；
  - 记录请求：`requests()`、`body_json(i)`；
  - `release()`、`max_concurrent()`；
  - SSE 构造函数 `sse_text(s)`、`sse_tool(index, id, name, args_part)`、`sse_finish(reason)`、`sse_usage(p, c)`、`sse_done()`。
  - 脚本用完后返回 500。`hold_after >= 0` 时送完这么多块就挂起，直到 `release()` 或 `req.watch` 中止。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(Json, AppendStringMatchesToJson)          // control bytes, quotes, invalid UTF-8 -> same bytes as to_json(Value(s))
TEST(SseParser, EventSplitAcrossChunks)         // "da" + "ta: {\"a\":1}\n" + "\n" -> one event "{\"a\":1}"
TEST(SseParser, IgnoresCommentsAndCrLf)         // ": ping\r\n\r\n" nothing; "data: x\r\n\r\n" -> "x"
TEST(SseParser, MultipleDataLinesJoinWithNewline)
TEST(SseParser, OversizedEventOverflows)         // kMaxEventBytes + 1 without a blank line -> overflowed()
TEST(LlmClient, StreamsTextDeltasInOrder)        // on_text "Hel","lo"; text == "Hello"; finish Stop
TEST(LlmClient, DoneEndsTheStream)               // chunks after "data: [DONE]" ignored
TEST(LlmClient, AssemblesToolCallFragmentsByIndex)
    // index 1 and 0 interleaved, args split "{\"id\":" + "\"counter\"}" -> calls[0].arguments == "{\"id\":\"counter\"}"; finish ToolCalls
TEST(LlmClient, ReadsUsageFromFinalChunk)        // usage {12,34,46}; body has stream_options.include_usage == true
TEST(LlmClient, RequestCarriesModelToolsAndBearer)
    // url == base_url + "/chat/completions"; header "authorization" == "Bearer sk-test"; body.model; body.tools == given; stream true
TEST(LlmClient, ImagePartFormat)
    // jpeg {FF D8 FF D9}: content[1] == {"type":"image_url","image_url":{"url":"data:image/jpeg;base64,/9j/2Q=="}}
TEST(LlmClient, AssistantToolCallsAndToolResultShape) // assistant.tool_calls[0].function.arguments string; tool.tool_call_id
TEST(LlmClient, Retries429And5xxWithBackoff)     // 429, 503, 200 -> ok, attempts 3, sleeps {1000, 4000}
TEST(LlmClient, GivesUpAfterTwoRetries)          // 500 x3 -> error "server", status 500, attempts 3
TEST(LlmClient, NoRetryOnUnauthorized)           // 401 -> "unauthorized", attempts 1
TEST(LlmClient, CancelledDuringBackoffStopsWaiting)
TEST(LlmClient, NoActiveProfileMakesNoRequest)   // "no_model", requests().size() == 0
TEST(LlmClient, CancelMidStreamKeepsPartialText) // hold_after 1, watch abort -> "cancelled", text "Hel", any_body
TEST(LlmClient, TransportErrorNotRetried)        // Connect -> "transport", attempts 1
TEST(LlmClient, BrokenEventIsBadStream)          // "data: {not json\n\n" -> "bad_stream"
TEST(LlmClient, KeyNeverInLogsOrResult)          // ring_entries() and every LlmResult string field lack "sk-test", also when the 401 body quotes it
TEST(LlmClient, LogsOnlyMetadata)                // one line "llm: model=m status=200 ms=" ... "tokens=46"; no "Hello" in the ring
TEST(ModelProfiles, VisionAndContextWindowValidated) // vision "yes" InvalidArgument; context_window "1000"/"x" InvalidArgument; "65536" ok
TEST(ModelProfiles, ContextWindowDefaultsTo65536)
TEST(ModelProfiles, TestRecordsVisionProbe)      // prober vision true -> options["vision"] == "true"; nullopt leaves it
TEST(ModelProbe, VisionProbeSendsTinyImage)      // POST .../chat/completions with an image_url part, max_tokens 1; 200 true, 400 false, 401/timeout nullopt
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'Json\.|SseParser|LlmClient|ModelProfiles|ModelProbe' --output-on-failure` 编译失败。
- [ ] **Step 3: 实现**
  - `complete()` 先从 `active_for_request(ModelKind::Llm)` 取得配置（没有就返回 `no_model`），再用 `build_chat_body()` 把请求体建在 `os::LargeString` 里，作为 `body_ref` 发送。
  - `on_status` 记录状态码。2xx 的响应体经 `SseParser` 处理，每个事件用 `from_json(data, 16)` 解析 `choices[0].delta`、`finish_reason`、`usage`。非 2xx 的响应体丢弃，不进结果。
  - `tool_calls` 分片按 `index` 拼接到 `os::LargeString`。
  - 重试规则见细化第 3 条。
  - 日志只在 `llm` 标签下记一行元信息（细化第 26 条）。
  - `routes_ai.cpp` 的测试接口返回里增加 `vision`。
- [ ] **Step 4: 验证** — 上面的 ctest 通过；全量 ctest；`bash tools/ci.sh`。
- [ ] **Step 5: Commit** — `feat(net): stream OpenAI chat completions and probe model vision`

---

### Task 2: `wisp_agent` 组件与 `ConversationStore`

**Files:**
- Create: `components/wisp_agent/CMakeLists.txt`（`wisp_component(NAME wisp_agent ... REQUIRES wisp_base wisp_os wisp_hal wisp_core wisp_net wisp_admin wisp_ui wisp_scene)`）、`include/wisp/agent/limits.hpp`、`include/wisp/agent/conversation.hpp`、`src/conversation.cpp`
- Modify: `simulator/CMakeLists.txt`、`firmware/main/CMakeLists.txt`、`components/wisp_app/CMakeLists.txt`（`wisp_app` REQUIRES `wisp_agent`）、`tests/CMakeLists.txt`（新目录 `tests/agent/`）
- Modify: `tools/check_layering.py`、`tools/check_layering_test.py`、`AGENTS.md` §3（分层清单，同一提交，提交信息另起一行 `docs(agents): ...`）、`docs/architecture.md`（分层图）
- Modify: `components/wisp_app/src/factory_reset.cpp`（删除 `<System>/agent/`）
- Test: `tests/agent/conversation_test.cpp`、`tests/app/factory_reset_test.cpp`

**Interfaces:**
- Produces（`limits.hpp`）：`kMaxSteps = 24`、`kTurnMs = 600000`、`kStallMs = 60000`、`kMaxUserChars = 4000`、`kMaxQueued = 1`、`kToolResultKeepBytes = 4096`、`kDefaultContextWindow = 65536`、`kCompressPercent = 75`、`kCompressTargetPercent = 40`、`kKeepRecentTurns = 2`、`kConfirmMs = 30000`、`kSettleQuietMs = 300`、`kSettleMaxMs = 2000`、`kTreeMaxChars = 2000`、`kTreeKeepEnds = 5`、`kShotWidth = 240`、`kShotQuality = 70`、`kSummaryChars = 200`、`kArchivesKept = 5`、`kRolloverBytes = 1024 * 1024`、`kReadMaxBytes = 65536`、`kDataReadMaxBytes = 16384`、`kLogsMax = 100`、`kEvalMs = 200`、`kCheckLogsPerLevel = 20`、`kMinTargetPx = 48`、`kMinContrast = 3.0`。
- Produces（`wisp::agent`，`conversation.hpp`）：

```cpp
enum class Role : uint8_t { User, Assistant, Tool, Note, Summary, Marker };
const char* to_string(Role r);   // "user" "assistant" "tool" "note" "summary" "marker"
struct ToolCallRec { std::string id; std::string name; os::LargeString args; };  // name dotted
struct Message {
    uint64_t seq = 0;
    int64_t ts = 0;               // unix s, 0 while the clock is unset
    uint32_t turn = 0;
    Role role = Role::User;
    os::LargeString text;         // user/assistant/note/summary text; tool result JSON
    std::string origin;           // user: "web" | "device"
    std::vector<ToolCallRec> calls;
    std::string call_id, tool;    // tool
    bool ok = true;               // tool
    bool image = false;           // tool: a screenshot was attached
    std::string marker, status;   // marker: "turn_begin"/"turn_end"; status per 细化 6
    uint64_t covers_first = 0, covers_last = 0;  // summary
};
struct ArchiveInfo { uint32_t n; int64_t ts; std::string first; };  // first user text, <= 100 chars
class ConversationStore {
public:
    // dir: <System>/agent. Every method that touches files runs on the wisp.fs worker;
    // messages()/bytes()/interrupted_turn() are thread-safe snapshots.
    ConversationStore(core::StorageManager& storage, std::string dir);
    Result<void> load();                       // marks an unfinished turn (细化 6)
    Result<uint64_t> append(Message m);        // assigns seq, appends one line
    os::LargeVector<Message> messages() const;
    std::optional<Message> find(uint64_t seq) const;
    uint64_t bytes() const;
    bool needs_rollover() const;               // bytes() > kRolloverBytes
    std::optional<uint32_t> interrupted_turn() const;  // set by load()
    uint32_t last_turn() const;
    Result<void> archive();                    // current -> archive-<n>, keeps kArchivesKept
    Result<std::vector<ArchiveInfo>> archives();  // newest first
    Result<os::LargeVector<Message>> read_archive(uint32_t n);
    Result<void> clear_all();
};
Value to_value(const Message& m, size_t max_result_chars);  // the REST/WS shape, Task 12
```

- [ ] **Step 1: 写失败的测试**

```cpp
// tools/check_layering_test.py
test_agent_sits_between_scene_and_app   // wisp_scene including "wisp/agent/x.hpp" -> violation; wisp_agent including "wisp/app/app.hpp" -> violation; wisp_agent including "wisp/scene/..." ok
// tests/agent/conversation_test.cpp
TEST_F(Conversation, AppendsOneJsonLinePerMessage)   // 3 appends -> 3 lines ending '\n', seq 1..3, each from_json ok
TEST_F(Conversation, LoadRestoresEveryField)          // user/assistant(calls)/tool/note/summary/marker round-trip
TEST_F(Conversation, TornTailDroppedAndRewritten)     // file ends with half a line: load drops it, file rewritten, next append starts a new line
TEST_F(Conversation, UnfinishedTurnMarkedInterrupted) // turn_begin 3 without turn_end -> load appends marker turn_end "interrupted"; interrupted_turn() == 3
TEST_F(Conversation, ArchiveRotatesKeepingFive)       // 7 archive() -> archives() n == {7,6,5,4,3}; archive-1/2 files gone; current empty
TEST_F(Conversation, ArchiveListShowsFirstUserMessage)// first cut to 100 chars (code points)
TEST_F(Conversation, ReadArchiveUnknownIsNotFound)
TEST_F(Conversation, ClearAllRemovesEverything)
TEST_F(Conversation, LongResultsKeptInFull)           // 65536-byte tool result round-trips byte-exact
TEST_F(Conversation, RolloverPastOneMiB)              // bytes() > 1048576 -> needs_rollover()
TEST(FactoryReset, ClearsAgentDirectory)
```

- [ ] **Step 2: 运行，确认失败** — `python3 tools/check_layering_test.py`；`ctest --test-dir build/sim -R 'Conversation|FactoryReset' --output-on-failure`。
- [ ] **Step 3: 实现** — 按细化第 6 条的格式和规则。内存中的副本是 `os::LargeVector<Message>`，由一个互斥锁保护。`check_layering.py` 的层顺序加入 `wisp_agent`，并说明它的位置由 spec 2 决定。
- [ ] **Step 4: 验证** — 上面的测试通过；`bash tools/ci.sh`（分层检查覆盖新组件）；固件 `idf.py build`（新组件能被 idf.py 找到）。
- [ ] **Step 5: Commit** — `feat(agent): add the agent component and the persistent conversation store`

---

### Task 3: `ToolRegistry`

**Files:**
- Create: `components/wisp_agent/include/wisp/agent/tool_registry.hpp`、`src/tool_registry.cpp`、`src/args_schema.cpp`
- Test: `tests/agent/tool_registry_test.cpp`

**Interfaces:**
- Produces（`wisp::agent`）：

```cpp
enum class ToolRisk : uint8_t { Normal, Note, Confirm };
enum class ToolThread : uint8_t { Agent, Ui, Fs, Io };
struct ToolCtx {
    std::function<bool()> cancelled;
    uint64_t deadline_ms = 0;                 // os::now_ms() based
    std::string lang;                         // "zh" | "ja" | "en", for note text
};
struct ToolOutput {
    bool ok = true;
    Value result;                             // {"ok":..., ...}; tool_error() builds failures
    std::string note;                         // system note text (细化 17), "" for none
    std::shared_ptr<const os::LargeBuffer> jpeg;  // ui.screenshot only
    uint32_t jpeg_w = 0, jpeg_h = 0;
    std::string shown_scene;                  // set when the tool brought a scene to the front
};
using ToolFn = std::function<ToolOutput(const Value& args, ToolCtx& ctx)>;
struct ToolDef {
    std::string name;                         // dotted, ^[a-z]+\.[a-z_]+$
    std::string description;
    std::string schema_json;                  // JSON Schema subset (below)
    ToolRisk risk = ToolRisk::Normal;
    ToolThread thread = ToolThread::Agent;
    uint32_t timeout_ms = 5000;
    bool vision_only = false;
    ToolFn run;
};
// Post fn to that thread; false if it could not be queued.
struct ToolExecutors {
    std::function<bool(std::function<void()>)> ui, fs, io;
};
class ToolRegistry {
public:
    explicit ToolRegistry(ToolExecutors ex);
    Result<void> add(ToolDef def);            // InvalidArgument: duplicate, bad name, bad schema
    const ToolDef* find(std::string_view dotted) const;
    const ToolDef* find_wire(std::string_view wire) const;
    // {"type":"function","function":{name(wire),description,parameters}} in add() order,
    // vision_only tools last and only with `vision`. Built once per flag.
    const os::LargeString& tools_json(bool vision) const;
    // Parses args_json ("" -> {}), validates, runs on def.thread, waits up to
    // min(def.timeout_ms, ctx.deadline) while polling ctx.cancelled every 20 ms.
    ToolOutput call(std::string_view wire, std::string_view args_json, ToolCtx& ctx);
};
std::string wire_name(std::string_view dotted);
// Schema subset: type object|string|integer|number|boolean|array; properties; required;
// enum; minimum; maximum; maxLength (code points); items. Returns the failing field path.
std::optional<std::string> validate_args(const Value& schema, const Value& args, std::string* why);
ToolOutput tool_error(std::string_view code, std::string_view message, Map extra = {});
std::string scene_data(std::string_view scene_id, std::string_view content);
```

- 超时后结果作废。执行函数与 `call()` 之间只经过一个 `shared_ptr` 状态通信，晚到的结果直接丢弃。执行线程本身不能被打断，等它跑完。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(ToolRegistry, WireNamesReplaceDots)               // wire_name("scene.list_files") == "scene_list_files"; tools_json uses wire names
TEST(ToolRegistry, DefinitionsKeepOrderVisionLast)      // order as added; "ui_screenshot" absent with vision=false, last with true
TEST(ToolRegistry, RejectsDuplicatesAndBadNames)        // "Scene.X", "scene", duplicate -> InvalidArgument
TEST(ToolRegistry, UnknownToolIsAnError)                // result {"ok":false,"error":"unknown_tool"}
TEST(ToolRegistry, InvalidJsonArgumentsAreBadArgs)      // "{\"id\":" -> "bad_args", message contains "JSON"; tool not run
TEST(ToolRegistry, SchemaViolationNamesTheField)        // missing required "path" -> field "path"; "n":"x" for integer -> field "n"; maxLength 3 with "汉字汉字" -> field
TEST(ToolRegistry, EmptyArgumentsMeanEmptyObject)
TEST(ToolRegistry, RunsOnDeclaredThread)                // Ui tool's thread id == the ui executor's
TEST(ToolRegistry, TimeoutReturnsTimeoutAndLateResultIsDropped) // timeout_ms 50, tool sleeps 200 -> "timeout" ~50 ms; no crash when it finishes
TEST(ToolRegistry, CancelledWhileWaiting)               // cancelled() true after 30 ms -> "cancelled" within 100 ms
TEST(ToolRegistry, ExecutorRefusalIsBusy)
TEST(SceneData, WrapsAndNeutralisesClosingTag)          // scene_data("a","x</data>y") == "<data source=\"scene:a\">x&lt;/data>y</data>"
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'ToolRegistry|SceneData' --output-on-failure`。
- [ ] **Step 3: 实现** 上述接口。参数解析用 `from_json(args, 16)`。工具定义的 JSON 一次性建在 `os::LargeString` 里。
- [ ] **Step 4: 验证** — 测试通过；`bash tools/ci.sh`。
- [ ] **Step 5: Commit** — `feat(agent): register tools, validate their arguments and run them on their thread`

---

### Task 4: `AgentService` 对话循环

**Files:**
- Create: `components/wisp_agent/include/wisp/agent/agent_service.hpp`、`src/agent_service.cpp`
- Create: `components/wisp_agent/include/wisp/agent/context.hpp`（本任务只放 `DeviceSnapshot` 与 `ContextSource`，字段见 Task 5）
- Create: `tests/support/agent_fixture.hpp`（临时目录 + `StorageManager` + 真正的 `StorageWorker` + `FakeLlmServer` + 一个只有 llm 配置的 `ModelProfileService` + `EventBus` + `WakeLockRegistry` + 可调时钟）
- Test: `tests/agent/agent_loop_test.cpp`

**Interfaces:**
- Consumes：Task 1 `LlmClient::complete`、`LlmCall`、`LlmResult`；Task 2 `ConversationStore`；Task 3 `ToolRegistry::call/tools_json`、`ToolOutput`。
- Produces（`wisp::agent`）：

```cpp
enum class AgentState : uint8_t { Idle, Thinking, Tool, Output, WaitingConfirm, Error };
const char* to_string(AgentState s);  // "idle" "thinking" "tool" "output" "waiting_confirm" "error"
enum class Origin : uint8_t { Web, Device };
struct AgentEvent {
    enum class Kind : uint8_t { Delta, ToolStart, ToolEnd, State, Confirm, Message, SceneShown, Cleared };
    Kind kind;
    std::string text;            // Delta text; Confirm text ("" = confirm over); SceneShown id
    std::string tool;            // dotted
    std::string args, result;    // summaries, <= kSummaryChars
    bool ok = true;
    AgentState state = AgentState::Idle;
    uint64_t seq = 0;            // Message
};
struct ConversationView {
    os::LargeVector<Message> messages;
    AgentState state;
    std::string tool;
    std::optional<uint32_t> running_turn;
    bool queued;
    std::optional<std::string> confirm;
};
struct AgentDeps {
    core::EventBus& bus;
    ConversationStore& store;
    ToolRegistry& tools;
    net::LlmClient& llm;
    core::ModelProfileService& models;          // get() only (memory)
    std::function<bool(std::function<void()>)> submit_io;
    core::WakeLockRegistry* locks = nullptr;
    core::NotificationService* notes = nullptr; // agent_done notice
    std::function<bool()> screen_off;
    std::function<bool()> network_up;           // last net.wifi.state == connected
    std::function<std::string()> lang;          // device UI language, no flash
    std::function<uint64_t()> now_ms = os::now_ms;
    std::function<int64_t()> wall_s;
    ContextSource context;                      // Task 5; empty: history only
};
struct AgentConfig {
    uint32_t max_steps = kMaxSteps;
    uint32_t turn_ms = kTurnMs;
    uint32_t stall_ms = kStallMs;
    size_t stack_bytes = 16 * 1024;
    int priority = 3;
};
class AgentService {
public:
    AgentService(AgentDeps deps, AgentConfig config = {});
    ~AgentService();                       // stop()
    static os::TaskConfig task_config(const AgentConfig& c);  // "wisp.agent", flash_access false
    Result<void> start();                  // store.load() on the worker (waits), then the task
    void stop();
    // InvalidArgument "empty" / "too_long" (> kMaxUserChars code points);
    // Busy "busy" when a turn runs and one message is queued.
    Result<void> send(std::string text, Origin from);
    void stop_turn();                      // also cancels a queued message
    Result<void> new_conversation();       // Busy while a turn runs
    AgentState state() const;
    ConversationView view() const;
    void set_listener(std::function<void(const AgentEvent&)> cb);
};
```

- [ ] **Step 1: 写失败的测试**（`AgentLoop` fixture；FakeLlm 按脚本回放；替身工具记录调用）

```cpp
TEST_F(AgentLoop, PlainReplyEndsTurn)              // messages: marker begin, user, assistant "hi", marker end "done"; states thinking -> output -> idle
TEST_F(AgentLoop, RunsToolCallsInOrderThenAnswers) // 2 calls in one reply run in order; 2nd request has assistant.tool_calls + 2 tool msgs with matching ids
TEST_F(AgentLoop, StopsAfter24ToolCalls)           // model always calls a tool: 24 run, 25th not; note "步骤太多，已停止"; end status "step_limit"
TEST_F(AgentLoop, StopsAfterTenMinutes)            // clock +600001 ms during a tool -> status "time_limit"
TEST_F(AgentLoop, StopDuringStreamAborts)          // hold stream; stop_turn() -> request ends < 200 ms; partial assistant text kept; note "已中止"; status "stopped"
TEST_F(AgentLoop, StopDuringToolCancelsIt)         // tool sees ctx.cancelled(); turn ends "stopped"
TEST_F(AgentLoop, QueuesOneMessageAndRefusesMore)  // A running, B ok (queued), C -> Busy "busy"; B starts after A ends
TEST_F(AgentLoop, RejectsEmptyAndOverlongMessages) // "" empty; 4001 x "汉" too_long; 4000 x "汉" ok
TEST_F(AgentLoop, NoModelExplainsInConversation)   // no active llm: note names the model page; no request; state error -> idle
TEST_F(AgentLoop, NetworkDownExplainsInConversation)
TEST_F(AgentLoop, UnauthorizedTellsUserToCheckModel)
TEST_F(AgentLoop, MalformedToolArgumentsGoBackAsBadArgs)   // Review Focus 2: args "{\"id\":" -> tool msg error bad_args; next request sent; turn ends done
TEST_F(AgentLoop, LengthCutoffWithPartialToolCallIsBadArgs)// finish "length", args "{\"path\":\"ma" -> bad_args, tool not run
TEST_F(AgentLoop, WifiLossMidStreamEndsTurnCleanly)        // Review Focus 1: publish net.wifi.state "backoff" during a held stream -> aborted <= 2 s;
                                                           // note "网络中断"; locks released; state idle; next send after "connected" succeeds
TEST_F(AgentLoop, StalledStreamTimesOut)                   // config.stall_ms 200, no bytes -> aborted; note
TEST_F(AgentLoop, HoldsCpuAndNetworkLocksDuringTurn)       // holder "system.agent", reason "agent_turn", kinds cpu+network; none after
TEST_F(AgentLoop, PublishesAgentStateEvents)               // agent.state {state:"tool", tool:"scene.list"}, source "agent"
TEST_F(AgentLoop, InterruptedTurnReportedAfterRestart)     // unfinished turn on disk -> after start() the last message is note "已中断"
TEST_F(AgentLoop, ScreenOffTurnPostsDoneNotice)            // screen_off -> post_system("agent_done", first line cut to 64 bytes)
TEST_F(AgentLoop, ScreenOnTurnPostsNoNotice)
TEST_F(AgentLoop, ListenerSeesDeltasToolsAndMessages)      // ToolStart/End args/result <= 200 chars; Message per appended message
TEST_F(AgentLoop, NewConversationArchives)                 // archives() grows by 1; Busy while running
TEST_F(AgentLoop, RolloverArchivesBeforeTheNextTurn)       // needs_rollover -> archived first, note
TEST_F(AgentLoop, LogsMetadataOnly)                        // "agent: turn 1 start", "agent: turn 1 end done steps=0 tokens=..."; no user text in the ring
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R AgentLoop --output-on-failure`。
- [ ] **Step 3: 实现**
  - 任务主循环从命令队列（`os::Queue`，PSRAM）取 `send`、`new`。
  - 一轮的流程：`turn_begin` → 组装上下文 → `complete()` → 依次执行工具调用 → 再请求，直到不再调用工具，或碰到上限、停止、出错。
  - 每条消息都经 `submit_io` 追加，并等它写完。
  - 说明文字用新的 `ui::StrId`：`AgentStepLimit`、`AgentTimeLimit`、`AgentStopped`、`AgentInterrupted`、`AgentErrNoModel`、`AgentErrNetwork`、`AgentErrNetworkLost`、`AgentErrUnauthorized`、`AgentErrServer`、`AgentErrDayLimit`、`AgentRolledOver`，按 `deps.lang()` 选语言。
  - 中止规则见细化第 4 条。
  - 订阅 `net.wifi.state`，记下最近一次的状态。
  - 熄屏时的完成通知用 `notice::kAgentDone = "agent_done"`（加到 `notification_service.hpp`）。
- [ ] **Step 4: 验证** — 测试通过；`bash tools/ci.sh`。
- [ ] **Step 5: Commit** — `feat(agent): run the conversation loop with limits, stop, queueing and wake locks`

---

### Task 5: 上下文组装、压缩与用量

**Files:**
- Modify: `components/wisp_agent/include/wisp/agent/context.hpp`；Create: `src/context.cpp`、`src/prompt.cpp`、`include/wisp/agent/usage.hpp`、`src/usage.cpp`
- Modify: `components/wisp_agent/src/agent_service.cpp`（接入上下文、压缩、用量与上限）
- Modify: `components/wisp_core/include/wisp/core/system_settings.hpp`、`src/system_settings.cpp`（`day_tokens`，细化第 20 条）
- Test: `tests/agent/context_test.cpp`、`tests/agent/usage_test.cpp`、`tests/agent/agent_loop_test.cpp`、`tests/core/system_settings_test.cpp`

**Interfaces:**
- Consumes：Task 2 `Message`；Task 1 `LlmMessage`、`profile_vision`、`profile_context_window`。
- Produces（`wisp::agent`）：

```cpp
struct DeviceSnapshot {      // gathered without flash (cached values)
    std::string lang;        // "zh" | "ja" | "en"
    hal::ScreenShape shape;
    std::string foreground;  // "" none
    std::vector<std::string> background;
    int battery = -1;
    bool charging = false;
    std::string wifi;
    int64_t wall_s = 0;
    int utc_offset_min = 480;
};
using ContextSource = std::function<DeviceSnapshot()>;
// Fixed part: same bytes for the same lang and shape (no time, no battery).
std::string system_prompt(const DeviceSnapshot& d);
std::string dynamic_info(const DeviceSnapshot& d);
struct BuiltRequest {
    os::LargeVector<net::LlmMessage> messages;  // views into `arena` and the history
    os::LargeString arena;
    size_t est_tokens = 0;                      // bytes / 3
};
// system_prompt, history (summaries in place of what they cover, tool results cut to
// kToolResultKeepBytes + "[truncated, N bytes total]", only the newest screenshot as an
// image part when vision, older ones "[截图已省略]"), then dynamic_info last.
BuiltRequest build_request(std::span<const Message> history, const DeviceSnapshot& d,
                           bool vision, std::shared_ptr<const os::LargeBuffer> latest_shot);
bool needs_compression(size_t est_tokens, uint32_t window);   // est * 100 > window * kCompressPercent
// Oldest whole turns to summarize (first_seq, last_seq); never the last kKeepRecentTurns.
std::optional<std::pair<uint64_t, uint64_t>> pick_compression(std::span<const Message> history,
                                                               uint32_t window);
class UsageMeter {
public:
    UsageMeter(core::StorageManager& storage, std::string path);  // worker for load/save
    Result<void> load();
    void add(uint32_t tokens, int64_t wall_s, int utc_offset_min);
    Result<void> save();
    struct View { std::string day; uint64_t day_tokens; std::string month; uint64_t month_tokens; };
    View view() const;
    bool over(uint64_t day_limit) const;  // day_limit 0: never
};
```

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(Context, FixedPrefixIsStableAcrossRequests)  // two builds, different wall_s/battery: messages[0] text identical; last message is the dynamic info
TEST(Context, SystemPromptCarriesScreenRules)     // contains shape name, corner radius, safe area numbers, "row_span", "<data source=", "docs_read"; lang "ja" -> "Japanese"
TEST(Context, DynamicInfoHasTimeScenesBatteryNetwork)
TEST(Context, ToolResultsCutTo4KiBInHistory)      // 10000-byte result -> first 4096 bytes + "[truncated, 10000 bytes total]"
TEST(Context, OnlyLatestScreenshotSent)           // two screenshot tool msgs: first "[截图已省略]", second carries the image part
TEST(Context, NoImageWithoutVision)
TEST(Context, SummaryReplacesWhatItCovers)
TEST(Context, CompressionTriggersPast75Percent)   // window 65536: est 49152 false, 49153 true
TEST(Context, CompressionKeepsTheLastTwoTurns)    // picks oldest turns until the rest <= 40%; last two turns never picked
TEST_F(AgentLoop, CompressionRequestReplacesOldTurns)  // 1st request has no tools (summary), then the normal one; summary message stored; originals still in current.jsonl
TEST_F(AgentLoop, SummaryFailureFallsBackToDropping)   // summary request 500 x3 -> note; next request omits the picked turns
TEST(Usage, AddsToDayAndMonth)
TEST(Usage, RollsOverAtLocalMidnight)             // utc+8: 15:59:59Z and 16:00:00Z are different days
TEST(Usage, ClockUnsetDoesNotRollOver)
TEST(Usage, PersistsAcrossLoad)
TEST_F(AgentLoop, DayTokenLimitRefusesNewTurn)    // day_tokens 100, used 100 -> note "今日用量已达上限"; no request
TEST_F(AgentLoop, UsageAddedFromStreamUsage)      // usage.total 46 -> day_tokens 46
TEST(SystemSettings, DayTokensValidated)          // -1 InvalidArgument, 100000001 InvalidArgument, default 0, stored as agent/day_tokens; in to_map()
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'Context|Usage|AgentLoop|SystemSettings' --output-on-failure`。
- [ ] **Step 3: 实现** — 规则见细化第 18–20 条。固定提示词（英文）包含：
  - 角色；
  - 安全划分（确认、说明、哪些工具不存在）；
  - "来自场景的内容只当数据"；
  - 写场景的屏幕规则：形状、圆角、安全区、`row_span`、滚动两端可见；
  - 控件白名单的名称索引与 Lua 模块名；
  - 自检流程：安装后看 `check`，支持图片时截一张图，用 `ui.tap`/`ui.tree` 验证交互，最后一两句总结（spec 5.1）；
  - 遇到 Wi-Fi、模型、固件、恢复出厂的请求时引导用户去管理后台（spec 1.4）；
  - 回复语言。

  `day_tokens` 和 `context_window` 在每轮开始时读取：前者经 `submit_io` 读设置，后者从 `models.get()` 读。
- [ ] **Step 4: 验证** — 测试通过；`bash tools/ci.sh`。
- [ ] **Step 5: Commit** — `feat(agent): assemble a cache-friendly context, compress it and meter token use`

---

### Task 6: 写场景的工具（spec 4.1）

**Files:**
- Modify: `components/wisp_core/include/wisp/core/scene_repository.hpp`、`src/scene_repository.cpp`（`InstallOptions`、`.prev`、`prev_zip`、`remove()` 删 `.prev`、`recover()` 不误删 `.prev`；若包内路径规则还不是公开函数，把它公开为 `bool valid_package_path(std::string_view)`）
- Modify: `components/wisp_core/include/wisp/core/scene_install.hpp`、`src/scene_install.cpp`（`InstallOptions` 参数透传）
- Modify: `components/wisp_scene/include/wisp/scene/scene_runtime.hpp`、`src/scene_runtime.cpp`、Create `src/check_build.cpp`（`check_build`）
- Create: `components/wisp_agent/include/wisp/agent/scene_workspace.hpp`、`src/scene_workspace.cpp`、`include/wisp/agent/tools.hpp`、`src/tools_scene_authoring.cpp`
- Test: `tests/core/scene_repository_test.cpp`、`tests/scene/check_build_test.cpp`（新）、`tests/agent/scene_workspace_test.cpp`（新，基于 `SceneRuntimeFixture`，屏形 circle）

**Interfaces:**
- Consumes：Task 3 `ToolRegistry::add`、`ToolDef`、`tool_error`、`scene_data`。
- Produces（`wisp::core`）：

```cpp
struct InstallOptions { bool keep_prev = false; };
Result<SceneInfo> SceneRepository::install(std::span<const uint8_t> zip, const SourceChecker& check,
    const std::function<Result<void>(const SceneManifest&)>& before_replace = {}, InstallOptions opt = {});
bool SceneRepository::has_prev(std::string_view id);            // <code_root>/.prev/<id>/scene.json exists
Result<os::LargeBuffer> SceneRepository::prev_zip(std::string_view id);  // NotFound without one
Result<SceneInfo> install_scene(SceneRepository& repo, SceneHost& host, std::span<const uint8_t> zip,
                                InstallOptions opt = {});
```

- Produces（`wisp::scene`）：`Result<void> SceneRuntime::check_build(std::shared_ptr<const core::ScenePackage> package);`（细化第 8 条）。错误消息就是 `ScriptError::text()`。等待和 `Busy`/`Timeout` 与 `check_sources` 相同。
- Produces（`wisp::agent`）：

```cpp
using ConfirmFn = std::function<bool(const std::string& text, ToolCtx& ctx)>;  // Task 11; tests fake it
using TaskSpawner = std::function<std::unique_ptr<os::Task>(const os::TaskConfig&, std::function<void()>)>;
struct SceneWorkspaceDeps {
    core::SceneRepository& repo;
    scene::SceneRuntime& runtime;
    core::StorageManager& storage;
    std::function<bool(std::function<void()>)> submit_io;
    TaskSpawner spawn = os::Task::spawn;         // the one-shot "wisp.agent.io" task
    ConfirmFn confirm;
    std::string drafts_root;                     // <data_root>/.drafts
};
struct InstallError { std::string file; int line = 0; std::string message, suggestion; };
class SceneWorkspace {
public:
    explicit SceneWorkspace(SceneWorkspaceDeps deps);
    Value list();                                                   // [{id,name,version,state,bytes,draft}]
    Result<Value> list_files(const std::string& id);                // draft if any, else installed
    Result<os::LargeString> read_file(const std::string& id, std::string_view path);   // TooLarge > 65536
    Result<void> write_file(const std::string& id, std::string_view path, std::string_view content);
    // NotFound count 0, AlreadyExists ("not_unique") count >= 2; *count set either way.
    Result<void> edit_file(const std::string& id, std::string_view path, std::string_view old_text,
                           std::string_view new_text, size_t* count);
    Result<void> delete_file(const std::string& id, std::string_view path);
    Result<void> discard(const std::string& id);
    ToolOutput install(const std::string& id, ToolCtx& ctx);         // + auto-check in Task 8
    ToolOutput rollback(const std::string& id, ToolCtx& ctx);
};
void add_scene_authoring_tools(ToolRegistry& reg, SceneWorkspace& ws);
```

- 工具的风险类别、执行线程和超时：
  - `scene.list`、`list_files`、`read_file`、`write_file`、`edit_file`、`delete_file`：Normal，Fs，5 s；
  - `scene.install`：Note（新增 http 域名时由工具自己调用 `confirm`），Agent（内部派 `Io` 任务），30 s；
  - `scene.discard_draft`：Note，Fs，5 s；
  - `scene.rollback`：Note，Agent，30 s。
- 说明文字（`note`）：`AgentNoteInstalled` "已安装「{}」{}"，`AgentNoteDiscarded` "已丢弃「{}」的草稿"，`AgentNoteRolledBack` "已把「{}」恢复到上一个版本"。
- 确认文字：`AgentConfirmHosts` "「{}」将可以访问 {}"（多个域名用"、"连接）。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST_F(SceneRepositoryTest, KeepPrevMovesOldPackageAside)  // v1 -> v2 keep_prev: .prev/counter/scene.json is v1; counter/data stays with v2
TEST_F(SceneRepositoryTest, PrevReplacedOnEachInstall)
TEST_F(SceneRepositoryTest, RemoveDeletesPrev)
TEST_F(SceneRepositoryTest, ListSkipsPrevAndDrafts)
TEST_F(SceneRepositoryTest, PrevZipReinstallsTheOldVersion)
TEST_F(CheckBuild, AcceptsAValidScene)
TEST_F(CheckBuild, UnknownWidgetHasFileAndLine)          // "main.lua:3:" prefix
TEST_F(CheckBuild, UnknownPropertyHasFileAndLine)
TEST_F(CheckBuild, RuntimeErrorInEntryFails)
TEST_F(CheckBuild, LeavesTheRealStoreAlone)              // entry calls wisp.store.set: data/kv.log of the installed scene unchanged
TEST_F(CheckBuild, OutsideTheSharedVmBudget)             // passes with a foreground + 3 background scenes running
TEST_F(CheckBuild, EndlessLoopTimesOut)                  // error mentions timeout within ~200 ms
TEST_F(SceneWorkspace, FirstEditCopiesInstalledIntoDraft)// draft has every package file; list() draft true
TEST_F(SceneWorkspace, NewSceneStartsWithAnEmptyDraft)
TEST_F(SceneWorkspace, ReadPrefersDraft)
TEST_F(SceneWorkspace, ReadOver64KiBIsTooLarge)          // 65537 bytes -> too_large; 65536 ok
TEST_F(SceneWorkspace, EditRequiresExactlyOneMatch)      // 0 -> not_found count 0; 2 -> not_unique count 2; 1 -> replaced
TEST_F(SceneWorkspace, PathRulesFollowPackages)          // "../x", "/x", "data/k", "x.txt" -> bad_path; "lib/u.lua", "assets/a.png" ok
TEST_F(SceneWorkspace, DraftCappedAtPackageLimit)        // total > 512 KiB -> quota_exceeded
TEST_F(SceneWorkspace, DeleteFileOnlyTouchesDraft)
TEST_F(SceneWorkspace, InstallOpensInFrontAndDropsDraft) // foreground() == id, state running, draft gone, note "已安装「计数器」1.0.0"
TEST_F(SceneWorkspace, InstallKeepsPrevious)
TEST_F(SceneWorkspace, SyntaxErrorLeavesInstalledAlone)  // ok false, errors[0] {file "main.lua", line 3}; installed version & files unchanged; draft kept
TEST_F(SceneWorkspace, BadManifestLeavesInstalledAlone)  // errors[0].file "scene.json"
TEST_F(SceneWorkspace, TreeErrorSuggestsAName)           // ui.labl -> suggestion contains "label"; installed unchanged
TEST_F(SceneWorkspace, NewHttpHostAsksFirst)             // confirm text contains "api.example.com"; false -> "denied", installed unchanged; true -> installed
TEST_F(SceneWorkspace, SameHostsDoNotAsk)
TEST_F(SceneWorkspace, RollbackSwapsWithPrev)            // after rollback v1 installed, .prev is v2; note
TEST_F(SceneWorkspace, RollbackWithoutPrevIsNotFound)
TEST_F(SceneWorkspace, DraftsSurviveRestart)
TEST_F(SceneWorkspace, DevModeInstallUnsupported)
TEST_F(SceneWorkspace, InstallRunsOnAFlashTask)          // spawner saw name "wisp.agent.io", flash_access true
TEST_F(SceneWorkspace, ReadFileContentIsWrapped)         // read_file tool result content starts with "<data source=\"scene:"
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'SceneRepositoryTest|CheckBuild|SceneWorkspace' --output-on-failure`。
- [ ] **Step 3: 实现**
  - `install` 的顺序：
    1. 在 `wisp.fs` 上读出草稿，得到 `ScenePackage`；
    2. 校验 manifest；
    3. 比较新旧 `permissions.http`，有新增域名时调用 `confirm`；
    4. `runtime.check_sources()`；
    5. `runtime.check_build()`；
    6. 打包 zip，在 `Io` 任务上 `install_scene(..., {.keep_prev = true})`，并 join 这个任务；
    7. `runtime.open(id)`；
    8. 删除草稿。
  - 任何一步失败都返回 `errors`，不进入后面的步骤。
  - `edit_file` 的计数按字节子串、不重叠。
- [ ] **Step 4: 验证** — 测试通过；`scene_runtime_test`、`app_scenes_test`、`routes_scenes_test` 不改断言即通过；`bash tools/ci.sh`。
- [ ] **Step 5: Commit** — `feat(agent): let the agent edit scene drafts, check and install them with one-step rollback`

---

### Task 7: `ui.tree()` 与 `ui.check()`（spec 5.2、5.3）

**Files:**
- Modify: `components/wisp_scene/include/wisp/scene/scene_view.hpp`、`src/scene_view.cpp`（`for_each_node`）、`scene_runtime.hpp/.cpp`（`ui_current_view`）、`components/wisp_ui/include/wisp/ui/system_layer.hpp`、`src/system_layer.cpp`（`visible_pieces`）
- Create: `components/wisp_scene/include/wisp/scene/ui_inspector.hpp`、`src/ui_inspector.cpp`、`src/ui_tree.cpp`、`src/ui_check.cpp`
- Create: `components/wisp_agent/src/tools_ui.cpp`（本任务只登记 `ui.tree`、`ui.check`）
- Test: `tests/scene/ui_tree_test.cpp`、`tests/scene/ui_check_test.cpp`（新）、`tests/scene/examples_test.cpp`（追加）

**Interfaces:**
- Produces：

```cpp
// scene_view.hpp
struct NodeRef { NodeId id; NodeId parent; Widget widget; std::string_view ref; lv_obj_t* obj; };
void SceneView::for_each_node(const std::function<void(const NodeRef&)>& fn) const;  // pre-order
// scene_runtime.hpp -- UI thread only; null while no view is up
const SceneView* SceneRuntime::ui_current_view() const;
// system_layer.hpp -- UI thread; ("capsule"|"banner"|"panel"|"error"|"keyboard"|"confirm"|"chat", obj)
std::vector<std::pair<const char*, lv_obj_t*>> SystemLayer::visible_pieces() const;
// ui_inspector.hpp (wisp::scene)
enum class IssueKind : uint8_t { OutsideVisible, ClippedText, Overlap, SmallTarget, LowContrast, EmptyScreen };
const char* to_string(IssueKind k);  // "outside_visible" "clipped_text" "overlap" "small_target" "low_contrast" "empty_screen"
struct UiIssue { IssueKind kind; std::string ref; std::string widget; std::string text; hal::Area box; std::string other; };
Value to_value(const UiIssue& i);
struct InspectorDeps { ui::UiRuntime& ui; ui::SystemLayer& layer; SceneRuntime& scenes; hal::ScreenShape shape; ui::JpegEncodeFn jpeg = {}; };
class UiInspector {
public:
    explicit UiInspector(InspectorDeps deps);
    // NotFound "no_scene"; FailedPrecondition "scene_error: <text>" while the scene in front is in Error.
    Result<std::string> tree(std::string_view ref = {});
    Result<std::vector<UiIssue>> check();
};
// pure helpers, unit-tested directly
struct TreeLine { uint16_t depth; std::string text; uint32_t parent_index; };
std::string fold_tree(std::span<const TreeLine> lines, size_t max_chars);
double contrast_ratio(uint32_t rgb_a, uint32_t rgb_b);
bool box_visible(const hal::ScreenShape& s, const hal::Area& box);
```

- `fold_tree` 的算法（签名决定不了）：
  1. 不超过 `max_chars` 时原样输出。
  2. 否则从子行最多的容器开始，把它的子行折叠为"首 5 行 + `…还有 N 行` + 尾 5 行"，每折叠一个就重算总长，直到不超长或者没有超过 10 个子行的容器。
  3. 仍然超长时，按深度从深到浅把整棵子树收成一行 `…（N 个子控件）`。
  4. 最后硬截到 `max_chars`，末行为 `…已截断，用 ui.tree(ref) 查看子树`。

- [ ] **Step 1: 写失败的测试**（`TEST_P` 的参数是三种屏形：`rect` 480×480 半径 0、`rounded_rect` 480×480 半径 70、`circle` 480×480）

```cpp
TEST_P(UiTreeShapes, ListsWidgetsWithRefsTextAndBoxes)  // counter: line matches R"(button#inc "\+1" \[\d+,\d+ \d+×\d+\])"; coords absolute
TEST_F(UiTree, UnnamedNodesGetSyntheticRefs)            // "label#n" + id
TEST_F(UiTree, ShowsStateFlags)                         // hidden, disabled, checked, "scroll=0/" for a scroll
TEST_F(UiTree, ListsSystemPiecesMarkedSystem)           // confirm open -> a line starting "system confirm"
TEST_F(UiTree, SceneTextIsWrapped)                      // scene part inside <data source="scene:counter">
TEST(FoldTree, FoldsTheBiggestContainerFirst)           // 300 rows -> <= 2000 chars, "…还有 290 行", rows 1-5 and 296-300 present
TEST(FoldTree, CollapsesDeepSubtreesWhenStillTooLong)
TEST_F(UiTree, TwoThousandWidgetsStaysUnderLimit)       // Review Focus 5: kMaxNodes widgets -> size <= 2000; tree() < 1 s; one run_blocking <= 100 ms
TEST_F(UiTree, RefExpandsASubtree)
TEST(UiCheckGeometry, BoxVisibleChecksCorners)          // on each shape: safe_rect inside; box touching a cut corner outside
TEST(UiCheckGeometry, ContrastRatio)                    // white/black 21.0; #777777 on #FFFFFF in [4.47, 4.49]
TEST_P(UiCheckShapes, FindsOutsideVisible)
TEST_P(UiCheckShapes, FindsClippedText)                 // label w 40 with "a long sentence" (no wrap) -> clipped_text
TEST_P(UiCheckShapes, FindsOverlap)                     // two clickable siblings in a stack overlapping -> overlap, other = sibling ref
TEST_P(UiCheckShapes, FindsSmallTarget)                 // 47×47 button reported, 48×48 not
TEST_P(UiCheckShapes, FindsLowContrast)                 // #999999 text on #AAAAAA reported; text on surface theme colour not
TEST_P(UiCheckShapes, FindsEmptyScreen)
TEST_P(UiCheckShapes, ScrollListJudgedAtItsEnds)        // 30-row list padded by row_span: nothing; unpadded last row -> outside_visible for it only
TEST_F(UiCheck, NoSceneIsNotFound)
TEST_F(UiCheck, SceneInErrorReportsTheError)            // FailedPrecondition, message starts "scene_error"
INSTANTIATE_TEST_SUITE_P(Shapes, UiCheckShapes, ...);   // the three shapes above; same for UiTreeShapes
TEST_F(Examples, UiCheckCleanOnACircle)                 // every scenes/examples/* -> check() empty
TEST_F(Examples, UiCheckCleanOnTheBoardsRoundedRect)    // radius 70
TEST_F(Examples, UiCheckCleanOnARect)
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'UiTree|FoldTree|UiCheck|Examples' --output-on-failure`。
- [ ] **Step 3: 实现**。每次检查都在 UI 线程的一次 `run_blocking` 里遍历一遍。判定规则：
  - `outside_visible`：可见控件的外框（`lv_obj_get_coords`）四个角都可见才算在可见区内，因为三种屏形都是凸的。滚动容器（`scroll`、`list`）的子孙只检查两端：第一个子项按滚到顶时的位置，最后一个子项按滚到底时的位置，中间的不检查。虚拟列表按 `index * row_h` 计算首行和末行。容器自身的外框照常检查。
  - `clipped_text`：label 的文字尺寸（`lv_text_get_size`，宽度按它的换行模式取）超出内容区，或者 label 外框超出不可滚动父容器的内容区。
  - `overlap`：同一父容器下两个可点控件（`LV_OBJ_FLAG_CLICKABLE`，含扩展点击区）的可点区域相交，面积大于 0。
  - `small_target`：可点区域（外框加 `ext_click_area`）的宽或高小于 48。
  - `low_contrast`：文字颜色与最近一个不透明（`bg_opa >= LV_OPA_COVER/2`）祖先的背景色，相对亮度对比度小于 3.0（WCAG 公式）。
  - `empty_screen`：根下没有任何未隐藏且面积大于 0 的可见控件。
  - 示例场景若报出问题，按细化第 12 条处理，并在提交信息里写明。
- [ ] **Step 4: 验证** — 测试通过；`bash tools/ci.sh`。
- [ ] **Step 5: Commit** — `feat(scene): inspect the UI tree and check layouts against the screen shape`

---

### Task 8: 截图、模拟输入与安装后的自动检查（spec 5.1、5.4、5.5）

**Files:**
- Create: `components/wisp_ui/include/wisp/ui/jpeg.hpp`
- Modify: `components/wisp_ui/include/wisp/ui/ui_runtime.hpp`、`src/ui_runtime.cpp`（`inject_pointer`）、`system_layer.hpp/.cpp`（`keyboard_target`）
- Modify: `components/wisp_app/include/wisp/app/app.hpp`（`Platform::jpeg_encode`）
- Create: `components/wisp_port_sim/include/wisp/port/sim/jpeg_encoder.hpp`、`ui/jpeg_encoder.cpp`（加入 `wisp_port_sim_ui`）；`components/wisp_port_esp/include/wisp/port/esp/esp_jpeg_encoder.hpp`、`ui/esp_jpeg_encoder.cpp`（加入 `wisp_port_esp`）
- Modify: `simulator/main.cpp`、`firmware/main/main.cpp`（设置 `jpeg_encode`）
- Create: `components/wisp_scene/src/ui_input.cpp`、`src/ui_shot.cpp`；Modify `ui_inspector.hpp`
- Modify: `components/wisp_agent/src/tools_ui.cpp`（`ui.tap/swipe/type/screenshot`）、`src/scene_workspace.cpp`（自动检查）
- Modify: `docs/references.md`、`THIRD_PARTY_NOTICES.md`（jo_jpeg）
- Test: `tests/sim/jpeg_encoder_test.cpp`、`tests/ui/pointer_injection_test.cpp`、`tests/scene/ui_input_test.cpp`、`tests/scene/ui_shot_test.cpp`、`tests/agent/auto_check_test.cpp`

**Interfaces:**
- Produces：

```cpp
// wisp/ui/jpeg.hpp
using JpegEncodeFn = std::function<Result<os::LargeBuffer>(std::span<const uint8_t> rgb888,
                                                           uint32_t w, uint32_t h, int quality)>;
// UiRuntime
struct PointerStep { int32_t x, y; bool pressed; uint32_t hold_ms; };
// Unsupported without a touch indev; Busy while an injection is still playing. Returns once
// queued; injecting() turns false after the last step was read.
Result<void> UiRuntime::inject_pointer(std::vector<PointerStep> steps);
bool UiRuntime::injecting() const;
// SystemLayer
lv_obj_t* SystemLayer::keyboard_target() const;
// UiInspector additions
struct Screenshot { std::shared_ptr<const os::LargeBuffer> jpeg; uint32_t w, h; };
Result<Screenshot> UiInspector::screenshot();          // Unsupported without deps.jpeg
Result<bool> UiInspector::wait_stable(uint32_t quiet_ms = kSettleQuietMs, uint32_t max_ms = kSettleMaxMs);
struct TapTarget { std::string ref; std::optional<lv_point_t> at; };
struct UiDiff { std::vector<std::string> added, removed, changed; bool stable; };
Result<UiDiff> UiInspector::tap(const TapTarget& t);    // NotFound unknown ref; "not_visible" hidden/covered
Result<UiDiff> UiInspector::swipe(lv_point_t from, lv_point_t to, uint32_t ms);
Result<UiDiff> UiInspector::type(std::string_view text); // "no_focus"
Value to_value(const UiDiff& d);
// pure helper: masks pixels outside visible() black, crops to safe_rect, box-filters to out_w
os::LargeBuffer mask_and_scale(std::span<const uint8_t> rgb888, const hal::ScreenShape& s,
                               uint32_t out_w, uint32_t* out_h);
// ports
Result<os::LargeBuffer> wisp::port::sim::encode_jpeg(std::span<const uint8_t>, uint32_t, uint32_t, int);
Result<os::LargeBuffer> wisp::port::esp::encode_jpeg(std::span<const uint8_t>, uint32_t, uint32_t, int);
```

- 自动检查附在 `scene.install`/`scene.rollback` 成功结果的 `check` 字段里：`{"lua_errors":[text...],"logs":{"W":[...],"E":[...]},"issues":[...],"stable":bool}`。日志取安装开始后新产生的、标签为 `scene.<id>` 的环形缓冲行，`W`、`E` 各最多 20 条，按 `scene_data` 包裹。
- 工具：`ui.tap`、`ui.swipe`、`ui.type` 为 Normal、Agent、5 s；`ui.screenshot` 为 Normal、Agent、10 s、`vision_only`，返回 `{"ok":true,"w","h","bytes"}`，图片放在 `ToolOutput::jpeg`。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(SimJpeg, EncodesBaselineThatTjpgdDecodes)     // 240×240 gradient q70: FF D8 ... FF D9; TJPGD decode mean abs error < 8 per channel
TEST(SimJpeg, TypicalScreenshotSize)                // counter scene 240×240 -> 5 KB..40 KB
TEST_P(ShotShapes, MasksInvisibleAndCrops)          // three shapes: out width 240, height multiple of 16; a pixel at a cut corner is black
TEST_F(Screenshot, UnsupportedWithoutEncoder)
TEST_F(PointerInjection, TapReachesAButton)         // LVGL button gets CLICKED
TEST_F(PointerInjection, SwipeFromLeftEdgeIsABackSwipe)  // injected input passes SystemLayer's observer: back handler fires
TEST_F(PointerInjection, UnsupportedWithoutTouch)
TEST_F(PointerInjection, BusyWhileInjecting)
TEST_F(UiInput, TapByRefChangesTheCount)            // counter: tap {ref "inc"} -> diff.changed has the count label "0 → 1"
TEST_F(UiInput, TapByCoordinates)
TEST_F(UiInput, HiddenOrCoveredTargetIsNotVisible)  // hidden ref; covered by an open confirm dialog -> "not_visible"
TEST_F(UiInput, TypeGoesToFocusedInput)             // focus input, type "abc" -> scene's ui.ref("name"):value() == "abc"
TEST_F(UiInput, TypeWithoutFocusIsNoFocus)
TEST_F(WaitStable, ReturnsAfterQuietPeriod)         // static scene: stable true within 300..600 ms
TEST_F(WaitStable, CapsAtTwoSeconds)                // animating scene: stable false after 2000 ±200 ms
TEST_F(AutoCheck, InstallReportsLuaErrorsLogsAndIssues) // scene logs 25 warnings -> W has 20; a 40×40 button -> issues has small_target
TEST_F(AutoCheck, RollbackAlsoChecks)
TEST_F(AutoCheck, SceneFaultingDuringSettleIsReported)  // Review Focus 4: timer errors 100 ms after start -> install ok true,
                                                        // check.lua_errors[0] has the error text; returns < 30 s; ui.check -> scene_error
TEST_F(UiTools, ScreenshotOnlyWithVision)           // tools_json(false) lacks ui_screenshot; call returns jpeg with w 240
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'SimJpeg|ShotShapes|Screenshot|PointerInjection|UiInput|WaitStable|AutoCheck|UiTools' --output-on-failure`。
- [ ] **Step 3: 实现**
  - 截图：`snapshot_rgb888()` → `mask_and_scale()`（在调用者线程上做，缓冲在 PSRAM）→ `jpeg`。
  - 注入：`UiRuntime` 的指针读回调先取注入队列（按 `hold_ms` 推进），队列空时才读硬件；注入期间触摸活动回调照常触发，所以 Agent 点击会推迟熄屏，与手指一致。
  - `tap(ref)`：按 `ref`（含合成名）找到对象，取可点区域中心；中心点上最上层的对象（`lv_indev_search_obj`）不是它或它的子孙时返回 `not_visible`。
  - diff：操作前后各取一份"ref → 文字"表，比较得出 added/removed/changed。
  - ESP 编码器：`esp_jpeg_enc.h`，RGB888 输入，4:2:0，`quality` 透传；输入、输出缓冲用 `Mem::DmaLarge`。
  - 自动检查：在 `install` 的第 7 步之后执行。
- [ ] **Step 4: 验证** — 测试通过；`bash tools/ci.sh`；固件 `idf.py build`（编码器能链接，`esp_new_jpeg.lf` 照旧把它放在 flash）。
- [ ] **Step 5: Commit** — `feat(scene): screenshot, tap, swipe and type through the real input path, and check scenes after install`

---

### Task 9: 操作场景的工具（spec 4.2）

**Files:**
- Modify: `components/wisp_scene/include/wisp/scene/scene_runtime.hpp`、`src/scene_runtime.cpp`、`src/scene_message.hpp/.cpp`（`eval`、`store_dump`、`store_set`）
- Modify: `components/wisp_app/src/scene_wiring.hpp/.cpp`（`delete_scene`：不弹框的删除，现有确认流程改为调用它）
- Create: `components/wisp_agent/src/tools_scene_ops.cpp`
- Test: `tests/scene/scene_eval_test.cpp`（新）、`tests/agent/scene_ops_tools_test.cpp`（新）、`tests/app/app_scenes_test.cpp`（删除流程不改断言）

**Interfaces:**
- Produces：

```cpp
// SceneRuntime -- wait like open() (10 s); NotFound when `id` is not running
// JSON of the chunk's return values: none -> null, one -> it, several -> an array.
// Runs as a message of its own with `budget_ms`; a Lua error or timeout is returned as
// the error text and does NOT fault the scene. UI changes it makes are flushed as usual.
Result<os::LargeString> SceneRuntime::eval(const std::string& id, std::string_view lua,
                                           uint32_t budget_ms = kEvalMs);
// {"kv":{...},"collections":{"<name>":[records]}}, cut at max_bytes with "truncated":true
Result<os::LargeString> SceneRuntime::store_dump(const std::string& id, size_t max_bytes);
Result<void> SceneRuntime::store_set(const std::string& id, std::string_view key, std::string_view json);
// SceneWiring -- no dialog; closes the scene if up, removes on the worker, publishes "deleted"
void SceneWiring::delete_scene(const std::string& id, bool with_data,
                               std::function<void(Result<void>)> done);
// tools
struct SceneOpsDeps {
    scene::SceneRuntime& runtime;
    core::SceneRepository& repo;
    core::SceneFiles* files;
    core::StorageManager& storage;
    std::function<bool(std::function<void()>)> submit_io;
    std::function<void(const std::string&, bool, std::function<void(Result<void>)>)> delete_scene;
    ConfirmFn confirm;
};
void add_scene_ops_tools(ToolRegistry& reg, SceneOpsDeps deps);
```

- 场景未运行时，`data_read`、`data_write` 在 `wisp.fs` 上临时打开该场景的 `SceneStore`（`load()`；写入后 `flush()`、`close()`）。场景正在安装或删除时返回 `busy`。
- `files_read` 的内容是合法 UTF-8 时作为文本返回，否则用 base64 并附 `"encoding":"base64"`。
- 说明文字：`AgentNoteOpened` "打开了「{}」"、`AgentNoteClosed`、`AgentNoteExited`、`AgentNoteDataWritten` "改写了「{}」的数据 {}"、`AgentNoteEval` "在「{}」中执行了 Lua"、`AgentNoteDeleted`、`AgentNoteFileRemoved`。
- 确认文字：`AgentConfirmDelete` "Agent 要删除场景「{}」"，有数据时追加 `AgentConfirmWithData` "及其数据"；`AgentConfirmFileRemove` "Agent 要删除「{}」的文件 {}"。
- 风险类别、执行线程和超时按 spec 4.2 的表。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST_F(SceneEval, ReturnsJsonOfResults)             // "return 1+1" -> "2"; "return {a=1}" -> {"a":1}; "return 1, 'x'" -> [1,"x"]; no return -> null
TEST_F(SceneEval, ErrorsDoNotFaultTheScene)         // "error('x')" -> error text "...: x"; status still running
TEST_F(SceneEval, BudgetIs200ms)                    // "while true do end" -> timeout error within ~250 ms; scene still running
TEST_F(SceneEval, UiChangesAreApplied)              // eval sets the count state -> label shows it
TEST_F(SceneEval, NotRunningIsNotFound)
TEST_F(SceneStoreAccess, DumpAndSetWhileRunning)    // store_set visible to wisp.store.get in the scene
TEST_F(SceneOps, OpenCloseExit)                     // close on a "background" scene -> background; exit -> stopped; notes
TEST_F(SceneOps, DeleteAsksOnDevice)                // confirm text "Agent 要删除场景「计数器」及其数据"; false -> "denied", still listed; true -> gone, scene.state "deleted"
TEST_F(SceneOps, DataReadSameRunningOrStopped)      // equal content; > 16384 -> truncated true, size <= 16384; wrapped
TEST_F(SceneOps, DataWriteWhileStoppedPersists)     // reopen -> wisp.store.get returns it
TEST_F(SceneOps, FilesListAndReadLimit)             // len 65537 -> too_large; binary -> encoding base64
TEST_F(SceneOps, FilesRemoveAsksOnDevice)           // denied -> file still there
TEST_F(SceneOps, LogsReturnsNewestN)                // n 101 -> bad_args; only tag scene.<id>; newest last; wrapped
TEST_F(SceneOps, SceneContentIsWrapped)             // data_read, files_read, logs, eval results contain <data source="scene:counter">
TEST_F(SceneOps, BusyWhileInstalling)
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'SceneEval|SceneStoreAccess|SceneOps|AppScenes' --output-on-failure`。
- [ ] **Step 3: 实现**
  - `eval` 新增一种 `SceneMessage`：在场景的 VM 里 `load` 这段代码（chunk 名 `"=eval"`）后 `Vm::call`，用新的预算；结果经 `lua_json` 转换。错误由消息自己捕获，不走 `fault()`。
  - `store_dump`、`store_set` 同样作为消息，在场景任务上读写它的 `SceneStore`。
- [ ] **Step 4: 验证** — 测试通过；`bash tools/ci.sh`。
- [ ] **Step 5: Commit** — `feat(agent): open, close, delete and inspect scenes from the agent`

---

### Task 10: 设备工具与 `docs.read`（spec 4.4）

**Files:**
- Create: `components/wisp_agent/src/tools_device.cpp`、`include/wisp/agent/docs.hpp`、`src/docs.cpp`、`src/docs_data.cpp`（生成）
- Create: `tools/gen_agent_docs.py`、`tools/gen_agent_docs_test.py`；Modify `tools/ci.sh`（运行测试并 `--check`）
- Modify: `docs/scene-api.md`（补 9 个模块的章节，细化第 27 条）
- Modify: `components/wisp_scene/include/wisp/scene/schema.hpp`、`src/schema.cpp`（`widget_markdown`）
- Test: `tests/agent/device_tools_test.cpp`、`tests/agent/docs_test.cpp`、`tests/scene/schema_test.cpp`

**Interfaces:**
- Produces：

```cpp
std::string scene::widget_markdown(std::string_view name);  // "" for an unknown widget
// docs.hpp (wisp::agent)
std::vector<std::string> doc_topics();                    // widgets, widget/<w>..., lua, lua/<m>..., screen, examples, example/<id>...
std::optional<std::string> read_doc(std::string_view topic);
struct DeviceToolDeps {
    std::function<Value()> status;   // no flash: battery, charging, wifi, storage, power level, occupancy, shape, safe_area
    core::SystemSettings& settings;  // get/set only inside submit_io jobs
    core::NotificationService& notes;
    std::function<bool(std::function<void()>)> submit_io;
};
void add_device_tools(ToolRegistry& reg, DeviceToolDeps deps);
```

- `settings.get` 返回 `{brightness, volume, notify_vol, screen_off_s, language}`。
- `settings.set(key, value)`：`key` 的 schema 为这五个键的 `enum`；校验交给 `SystemSettings::set`，它返回 `InvalidArgument` 时，工具结果为 `invalid_value` 并附原消息。说明行 `AgentNoteSetting` "{} {} → {}"，第一个 `{}` 用设置项的本地化名称（`AgentSetBrightness` "亮度"、`AgentSetVolume` "音量"、`AgentSetNotifyVol` "提示音量"、`AgentSetScreenOff` "熄屏时间"、`AgentSetLanguage` "语言"）。
- `notify.dismiss(source, id)`：使用 `DismissReason::Dismiss`。
- 生成脚本：`gen_agent_docs.py [--check]` 按 `## ` 标题切分 `docs/scene-api.md`：`## wisp.<m>` 进入 `lua/<m>`，`## Screen shape` 进入 `screen`，其余进入 `lua`；`scenes/examples/<id>/` 的 `scene.json` 与全部 `.lua` 进入 `example/<id>`；`examples` 是一张 id 与名称的列表。输出排序固定，是一个 `constexpr` 的 `{topic, text}` 表。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST_F(DeviceTools, StatusShape)                    // keys battery, charging, wifi, storage, power, occupancy, shape, safe_area
TEST_F(DeviceTools, SettingsGetListsFiveKeys)       // exactly brightness volume notify_vol screen_off_s language
TEST_F(DeviceTools, SettingsSetValidatesLikeTheWebPage) // brightness 5 -> invalid_value; key "ntp_servers" -> bad_args; brightness 120 -> ok, note "亮度 200 → 120"
TEST_F(DeviceTools, SettingsRunOnTheWorker)         // get/set executed on the worker thread
TEST_F(DeviceTools, NotifyListWrapsSceneText)       // a scene notice's title/body inside <data source="scene:...">; system notices plain
TEST_F(DeviceTools, NotifyDismiss)
TEST(Docs, TopicsCoverEveryModuleAndWidget)         // every Widget has widget/<name>; lua/store, lua/http, ..., lua/files; example/counter
TEST(Docs, UnknownTopicListsTopics)                 // tool result not_found with "topics" array
TEST(Schema, WidgetMarkdownForButton)               // contains "on_click"; unknown -> ""
# tools/gen_agent_docs_test.py
test_splits_modules_by_heading ; test_check_mode_detects_stale_output ; test_output_is_deterministic
```

- [ ] **Step 2: 运行，确认失败** — `python3 tools/gen_agent_docs_test.py`；`ctest --test-dir build/sim -R 'DeviceTools|Docs|Schema' --output-on-failure`。
- [ ] **Step 3: 实现** — 先补 `docs/scene-api.md` 的模块章节，再运行生成脚本，然后实现工具。`device.status` 的数据由 App 提供（Task 12 接线），测试里用替身。
- [ ] **Step 4: 验证** — 测试通过；`bash tools/ci.sh`（含 `gen_agent_docs.py --check`）。
- [ ] **Step 5: Commit** — `feat(agent): read and change device settings and serve scene docs on demand`

---

### Task 11: 设备确认、系统说明行与清空对话（spec 6.1–6.3）

**Files:**
- Create: `components/wisp_agent/include/wisp/agent/confirm.hpp`、`src/confirm.cpp`
- Modify: `components/wisp_agent/src/agent_service.cpp`（`WaitingConfirm` 状态、说明行、`request_clear`）
- Modify: `components/wisp_ui/include/wisp/ui/strings.hpp`、`src/strings_{zh,ja,en}.cpp`、`src/strings.cpp`（本期全部新 `StrId` 与 `tr_fill_n`；若字体子集测试失败，就换个说法，不重新生成字体）
- Test: `tests/agent/confirm_test.cpp`、`tests/agent/agent_loop_test.cpp`、`tests/ui/strings_test.cpp`

**Interfaces:**
- Consumes：Task 6/9 的 `ConfirmFn`；Task 3 `ToolOutput::note`。
- Produces：

```cpp
// wisp::ui
std::string tr_fill_n(StrId id, std::span<const std::string_view> values);  // "{}" in order
// wisp::agent
struct ConfirmDeps {
    ui::UiRuntime& ui;
    ui::SystemLayer& layer;
    std::function<void()> wake;            // turn the screen on, UI thread
    std::function<uint64_t()> now_ms = os::now_ms;
    uint32_t timeout_ms = kConfirmMs;      // tests shorten it
};
enum class Answer : uint8_t { Allowed, Denied, TimedOut, Cancelled };
class ConfirmService {
public:
    explicit ConfirmService(ConfirmDeps deps);
    // Blocks the caller (never the UI thread). One request at a time; a second waits.
    // Waits for another system dialog to close within the same window.
    Answer ask(const std::string& text, const std::function<bool()>& cancelled);
    std::optional<std::string> pending() const;
    void set_listener(std::function<void(const std::optional<std::string>&)> cb);
};
// AgentService
void AgentService::set_confirm(ConfirmService* c);       // tools' ConfirmFn goes through it
ConfirmFn AgentService::confirm_fn();                    // sets WaitingConfirm, emits Confirm events
void AgentService::request_clear(std::function<void(bool allowed)> done);  // allowed: store.clear_all()
```

- 说明行的规则：`ToolRisk::Note` 的工具成功、`note` 非空时，在工具消息之后追加 `Role::Note` 消息，并给发给模型的工具结果加上 `"system_note"` 字段（细化第 17 条）。失败时不追加。
- 清空对话的确认文字：`AgentConfirmClear` "清空全部 Agent 对话？"。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST_F(Confirm, AllowedRunsTheTool)
TEST_F(Confirm, DeniedReturnsDenied)                 // tool result error "denied"
TEST_F(Confirm, SilenceIsDenied)                     // timeout_ms 100 -> TimedOut, dialog closed; static_assert(kConfirmMs == 30000)
TEST_F(Confirm, WakesTheScreenFirst)                 // wake() called before open_confirm
TEST_F(Confirm, OneAtATime)                          // second ask() opens only after the first is answered
TEST_F(Confirm, WaitsForAnotherSystemDialog)         // a factory-reset dialog open: ask opens after it closes; still denied at the window's end
TEST_F(Confirm, StopCancelsPendingConfirm)           // cancelled() -> Cancelled, dialog closed without callback
TEST_P(ConfirmShapes, DialogInsideVisibleArea)       // three shapes: box and both buttons pass box_visible()
TEST_F(AgentLoop, WaitingConfirmStateAndWebSeesText) // state waiting_confirm; listener Confirm(text) then Confirm("")
TEST_F(AgentLoop, NoteToolsInsertSystemNote)         // settings.set: Note message after the tool message; next request's tool content has "system_note"
TEST_F(AgentLoop, NoNoteOnFailure)
TEST_F(AgentLoop, ClearAllNeedsConfirm)              // allowed: messages and archives empty, Cleared event ok; denied: unchanged, Cleared ok false
TEST(Strings, AgentStringsInEveryLanguage)           // every new StrId non-empty in zh/ja/en with the same "{}" count
TEST(Strings, FillNInOrder)
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'Confirm|AgentLoop|Strings' --output-on-failure`。
- [ ] **Step 3: 实现** — 规则见细化第 16、17 条。`ask()` 经 `ui.post()` 打开确认框，回调写入一个共享状态，调用方每 20 ms 轮询一次（检查回答、取消和超时）。
- [ ] **Step 4: 验证** — 测试通过；`python3 tools/gen_cjk_font_test.py`；`bash tools/ci.sh`。
- [ ] **Step 5: Commit** — `feat(agent): ask on the device before irreversible steps and note what the system did`

---

### Task 12: REST、WebSocket 与 App 接线（spec 7.1、7.2）

**Files:**
- Create: `components/wisp_admin/include/wisp/admin/agent_api.hpp`、`components/wisp_admin/src/routes_agent.cpp`
- Modify: `components/wisp_admin/include/wisp/admin/admin_server.hpp`（`AdminDeps::agent`）、`src/admin_internal.hpp`（`add_agent_routes`、`agent_stream`、`kAgentMessagesPath`）、`src/admin_server.cpp`（流式处理器加入 `agent_stream`）、`include/wisp/admin/ws_hub.hpp`、`src/ws_hub.cpp`（`agent` 订阅、`send_agent`）
- Create: `components/wisp_app/src/agent_wiring.hpp`、`src/agent_wiring.cpp`
- Modify: `components/wisp_app/src/app.cpp`、`src/app_impl.hpp`、`src/net_wiring.cpp`（构造与启停顺序、`AdminDeps::agent`、`device.status` 数据源）
- Test: `tests/admin/routes_agent_test.cpp`、`tests/admin/ws_hub_test.cpp`、`tests/app/agent_wiring_test.cpp`、`tests/app/app_boot_test.cpp`

**Interfaces:**
- Consumes：Task 4 `AgentService`、`AgentEvent`、`ConversationView`；Task 2 `to_value(Message, size_t)`；Task 5 `UsageMeter::View`；Task 11 `request_clear`；Task 8 最新截图。
- Produces：

```cpp
namespace wisp::admin {
class AgentApi {
public:
    virtual ~AgentApi() = default;
    // {"state","tool","model":{"name","vision"}|null,"messages":[<= max],"turn":n|null,
    //  "queued":bool,"confirm":text|null}; tool results cut to kSummaryChars with "result_len"
    virtual Value conversation(size_t max_messages) = 0;
    virtual Result<Value> message(uint64_t seq) = 0;     // full, NotFound
    virtual Result<void> send(std::string text) = 0;     // Busy "busy"; InvalidArgument "empty"/"too_long"
    virtual void stop() = 0;
    virtual Result<void> new_conversation() = 0;         // Busy while running
    virtual Value archives() = 0;                        // {"archives":[{n,ts,first}]}
    virtual Result<Value> archive(uint32_t n) = 0;       // {"messages":[...]}
    virtual Result<void> request_clear() = 0;            // Busy while a confirm is pending
    virtual Value usage() = 0;                           // {"day":{date,tokens},"month":{month,tokens},"day_limit"}
    virtual std::shared_ptr<const os::LargeBuffer> screenshot() = 0;  // latest, or null
};
}
void WsHub::send_agent(std::string json);  // only to clients whose {"sub":[...]} has "agent"
// wisp::app
class AgentWiring {   // owns registry, workspace, inspector, confirm, store, meter, service, the AgentApi adapter
public:
    AgentWiring(App::Impl& app);               // with a screen and a scene runtime; else not built
    Result<void> start();  void stop();
    admin::AgentApi* api();
    agent::AgentService* service();
    void attach_ws(admin::WsHub* hub);         // listener -> {"t":"agent",...} frames, deltas coalesced 100 ms
};
```

- 路由（前缀 `/api/v1/agent`，鉴权与 CSRF 同其他路由）：
  - `GET /conversation`（最多 100 条消息）；
  - `POST /messages`（流式，最多 16 KiB，成功 202 `{"queued":bool}`；409 `busy`；400 `too_long`/`empty`；413）；
  - `POST /stop`（200 `{}`）；
  - `POST /conversation/new`（200；409 `busy`）；
  - `GET /archive`；`GET /archive/*`（404）；
  - `DELETE /conversations`（202 `{"pending":true}`；409 `busy`）；
  - `GET /usage`；`GET /messages/*`；
  - `GET /screenshot`（`image/jpeg`，没有截图时 404）。
- WS 帧（spec 7.2，细化第 21–22 条）：
  - `{"t":"agent","k":"delta","text"}`；
  - `{"t":"agent","k":"tool","phase":"start"|"end","name","args","result"}`；
  - `{"t":"agent","k":"state","state"}`；
  - `{"t":"agent","k":"confirm","text":string|null}`；
  - `{"t":"agent","k":"msg","msg":{...}}`；
  - `{"t":"agent","k":"cleared","ok":bool}`。
- App 的顺序：
  - `AgentWiring` 在场景运行时构造之后、网络接线之前构造（路由要拿到它）；
  - 在 `start_runtime()` 之后 `start()`；
  - `App::stop()` 时在场景运行时之前 `stop()`。
  - 没有屏幕或没有场景运行时时不构造，`/agent` 返回 503。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST_F(AgentRoutes, ConversationShape)                 // keys above; tool result <= 200 chars with result_len
TEST_F(AgentRoutes, SendQueuesAndRefusesWhenFull)      // 202, 202, 409 {"error":{"code":"busy"}}
TEST_F(AgentRoutes, ConcurrentSendsQueueOneRefuseRest) // Review Focus 3: 4 threads POST together -> exactly two 202 and two 409;
                                                       // FakeLlm max_concurrent() == 1
TEST_F(AgentRoutes, FourThousandCjkCharactersAccepted) // 4000 x "汉" (12000 bytes) -> 202; 4001 -> 400 too_long; 16385-byte body -> 413
TEST_F(AgentRoutes, WritesNeedCsrf)                    // POST /agent/messages without X-Wisp -> 403
TEST_F(AgentRoutes, ClearIsAsyncAndConfirmedOnDevice)  // 202 {"pending":true}; denied -> WS cleared ok false, conversation unchanged
TEST_F(AgentRoutes, ArchiveListAndRead)                // after new: list has n; GET /archive/<n> messages; unknown -> 404
TEST_F(AgentRoutes, FullMessageBySeq)
TEST_F(AgentRoutes, UsageShape)
TEST_F(AgentRoutes, ScreenshotServedAsJpeg)            // content_type image/jpeg; none -> 404
TEST_F(AgentRoutes, NoKeyInAnyResponse)                // every route's body lacks "sk-test"
TEST_F(AgentRoutes, UnavailableWithoutAgent)           // AdminDeps::agent null -> 503 unavailable
TEST(WsHub, AgentFramesOnlyToAgentSubscribers)
TEST(AgentWiring, WsFramesFollowSpecShapes)            // each kind's keys; tool args/result <= 200 chars
TEST(AgentWiring, DeltasCoalescedPer100ms)             // 50 deltas in 50 ms -> at most 2 delta frames, text concatenated
TEST(AppAgent, BootStartsAgentWithAScreen)             // task "wisp.agent" running; GET /api/v1/agent/conversation 200
TEST(AppAgent, HeadlessHasNoAgent)                     // no display -> 503
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'AgentRoutes|WsHub|AgentWiring|AppAgent' --output-on-failure`。
- [ ] **Step 3: 实现** 上述接口。`AgentWiring` 的数据来源：`DeviceSnapshot` 和 `device.status` 用 App 已有的 `device_status`、`PowerManager::level()`、`OccupancyRegistry::list()`、`SceneRuntime::foreground()/background_scenes()`，以及 `system.settings` 事件里缓存的语言和时区，都不碰 flash。`ToolExecutors` 中：`ui` 为 `UiRuntime::post`，`fs` 为 `StorageWorker::submit`，`io` 为一次性任务。
- [ ] **Step 4: 验证** — 测试通过；`bash tools/ci.sh`；固件 `idf.py build`。
- [ ] **Step 5: Commit** — `feat(admin): expose the agent over REST and the WebSocket and wire it into the app`

---

### Task 13: 网页"对话"页（spec 7.3）

**Files:**
- Create: `web/app/pages/chat.js`、`web/app/md.js`（只支持段落、列表、行内与块代码、粗体、链接；先转义 HTML，链接只允许 `http(s):`）
- Modify: `web/app/main.js`（`NAV` 首项 `/chat`、标签栏 6 项、`DEFAULT_PATH` 不变）、`web/app/icons.js`（`IconChat`）、`web/app/i18n/{zh,ja,en}.js`、`web/app/pages/models.js`（llm 配置的 `vision` 开关与 `context_window` 输入，保存时保留其他选项键）、`web/app/pages/system.js`（`day_tokens` 设置与今日、本月用量）、`web/styles/*`
- Test: `tools/check_i18n.py`；模拟器 + 浏览器检查清单

**Interfaces:**
- Consumes：Task 12 的路由与 WS 帧；`ws.js` 的 `subscribe(['agent'], ...)`。
- 页面状态：进入页面时 `GET /conversation`，之后按 WS 帧增量更新；收到 `dropped` 或重新连上时再次 `GET`。

- [ ] **Step 1: 写检查清单（即本任务的"测试"）**，作为执行记录写进本任务：在模拟器里用 `tools/fake_openai.py --script tests/smoke/agent_script.json`（Task 15 的脚本）跑一遍：
  1. 消息列表：用户消息、Markdown 回复、灰色说明行；工具行显示为"🔧 scene.install counter ✓"，点开显示参数和完整结果（`GET /messages/{seq}`）；截图显示为缩略图（`GET /screenshot`）。
  2. 运行中"发送"变为"停止"，点停止后出现"已中止"。
  3. 顶部显示模型名和"支持图片"；删除激活的模型后，顶部提示并链接到模型页。
  4. 菜单："新对话"、"历史对话"（列表与只读查看）、"清空"（显示"等待设备确认"，拒绝后对话不变）；显示今日用量。
  5. 脚本回复里含 `<img src=x onerror=alert(1)>` 和 `[x](javascript:alert(1))`，两者都显示为文字，不执行。
  6. 两个标签页同时打开：一边发送，两边同步显示；第三条返回"忙碌"提示。
  7. 360 px 宽时标签栏 6 项都能点；从 768 px 起侧栏显示"对话"。
  8. 中、日、英三种语言切换后没有漏翻的键。
- [ ] **Step 2: 运行，确认缺失** — `python3 tools/check_i18n.py`（新键尚未补齐时失败），打开 `#/chat` 得到 404 页。
- [ ] **Step 3: 实现** 页面与文案。`models.js` 的视觉开关默认显示 `options.vision`，"测试连接"之后刷新它。
- [ ] **Step 4: 验证** — `python3 tools/check_i18n.py` 通过；`bash tools/ci.sh`（资源包重新打包）；逐条完成 Step 1 的清单，并把结果写入执行记录。
- [ ] **Step 5: Commit** — `feat(web): add the chat page and the agent's model and usage settings`

---

### Task 14: 设备对话浮层（spec 7.4、3.7）

**Files:**
- Create: `components/wisp_ui/include/wisp/ui/chat_overlay.hpp`、`components/wisp_ui/src/chat_overlay.cpp`（`SystemLayer` 的一个部件）
- Modify: `components/wisp_ui/include/wisp/ui/system_layer.hpp`、`src/system_layer.cpp`（`open_chat/update_chat/close_chat/chat()`、`PanelHandlers::open_chat`、`PanelModel::chat_status`、面板首行"对话"、`raise()` 顺序、浮层打开时返回手势只关浮层）、`src/system_notice.cpp`（`agent_done`）
- Create: `components/wisp_app/src/agent_shell.hpp`、`src/agent_shell.cpp`
- Modify: `components/wisp_app/src/status_shell.cpp`（面板行与状态文字）、`src/notification_router.cpp`（点 `agent_done` 打开浮层）、`src/agent_wiring.cpp`
- Test: `tests/ui/chat_overlay_test.cpp`、`tests/ui/system_layer_test.cpp`、`tests/app/agent_shell_test.cpp`

**Interfaces:**
- Consumes：Task 4 `AgentService::send(Origin::Device)/stop_turn/view/set_listener`，`AgentEvent::SceneShown`（工具结果的 `ToolOutput::shown_scene` 非空时由 `AgentService` 发出；`scene.install`、`scene.rollback`、`scene.open` 会设置它）。
- Produces（`wisp::ui`）：

```cpp
struct ChatLine { enum class Kind : uint8_t { User, Agent, Note, Tool } kind; std::string text; };
struct ChatModel { std::vector<ChatLine> lines; bool running = false; std::string status; };  // status: "思考中…" or a tool line
struct ChatHandlers { std::function<void(std::string text)> send; std::function<void()> stop, on_close; };
void SystemLayer::open_chat(ChatHandlers h);
void SystemLayer::update_chat(ChatModel m);
void SystemLayer::close_chat();
lv_obj_t* SystemLayer::chat() const;           // the overlay's root while open
lv_obj_t* SystemLayer::chat_input() const;
lv_obj_t* SystemLayer::chat_stop() const;
```

- 浮层内容：不透明全屏背景加安全区内的内容框。上方是消息列表，只显示文字，最近 50 条；工具调用压成一行小字，例如 `AgentToolInstall` "正在安装{}…"，其他工具为 `AgentToolGeneric` "正在执行 {}…"。下方一行输入框，点击后 `show_keyboard()`，键盘的 READY 键就是"发送"。运行中标题栏显示状态和"停止"按钮。
- 回复里个别字形不在设备字体中时，由字体的回退方式显示；本期不处理。

- [ ] **Step 1: 写失败的测试**（`TEST_P` 为三种屏形）

```cpp
TEST_P(ChatOverlayShapes, EveryTappableInsideVisibleArea)  // input, send, stop, list ends -> box_visible()
TEST_P(ChatOverlayShapes, RowsAtLeast48px)
TEST_F(ChatOverlay, InputOpensKeyboardAndReadySends)       // tap input -> keyboard(); READY -> send("你好"); input cleared
TEST_F(ChatOverlay, RunningShowsStatusAndStop)             // stop button -> handlers.stop
TEST_F(ChatOverlay, BackGestureClosesOnlyTheOverlay)       // scene in front stays running; on_close called
TEST_F(ChatOverlay, RaiseKeepsConfirmAbove)                // a confirm dialog opened while chat is up stays on top
TEST_F(SystemLayerPanel, ChatRowComesFirst)                // with open_chat handler: first row is "对话", before background scenes
TEST_F(AgentShell, DeviceMessagesReachTheSharedConversation) // send from overlay -> Origin::Device; view() has it with origin "device"
TEST_F(AgentShell, SceneShownCollapsesOverlay)             // SceneShown -> overlay closed; at turn end a banner with the reply's first line
TEST_F(AgentShell, ClosedOverlayFinishesWithBanner)        // tap banner -> overlay open
TEST_F(AgentShell, DoneNoticeTapOpensOverlay)              // agent_done notification tap -> overlay open, notice dismissed
TEST_F(AgentShell, ToolLinesAreOneLineAndNoScreenshots)
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'ChatOverlay|SystemLayerPanel|AgentShell' --output-on-failure`。
- [ ] **Step 3: 实现** — `AgentShell` 订阅 `AgentService` 的事件，经 `UiRuntime::post` 合并刷新浮层（最多每 100 ms 一次）。面板的"对话"行在有 Agent 时总是显示，状态文字取 `agent.state`。
- [ ] **Step 4: 验证** — 测试通过；`python3 tools/gen_cjk_font_test.py`；`bash tools/ci.sh`；改动了渲染路径，所以在模拟器里打开浮层滚动，对比 `WISP_PERF` 的帧率，不得低于基准。
- [ ] **Step 5: Commit** — `feat(ui): add the on-device chat overlay and its pull-down entry`

---

### Task 15: 冒烟测试与文档（spec 8.2）

**Files:**
- Modify: `tools/fake_openai.py`（`POST /v1/chat/completions`：按 `--script <json>` 回放 SSE。第几步由请求里 `role == "tool"` 的消息数决定，所以与时序无关；鉴权同 `/v1/models`；不回显请求）
- Create: `tests/smoke/agent_script.json`（写 `scene.json` 和 `main.lua`，内容是一个计数器场景 → `scene_install` → `ui_tap {"ref":"inc"}` → `ui_tree` → 最终文字）
- Modify: `tests/smoke/admin_smoke.py`（`check_agent()`）
- Modify: `docs/architecture.md`（`wisp_agent` 一节：线程、文件、事件 `agent.state`、日志行）、`docs/porting-guide.md`（`Platform::jpeg_encode`）、`docs/references.md`

**Interfaces:**
- Consumes：Task 12 的路由；细化第 26 条的日志行。

- [ ] **Step 1: 写失败的测试** — 在 `admin_smoke.py` 里加 `check_agent(api, sim, llm_port)`：
  1. 创建 llm 配置，指向 fake 服务，并激活它；
  2. `POST /agent/messages {"text":"做一个计数器场景"}` → 202；
  3. 轮询 `GET /agent/conversation`，直到 `state == "idle"`（最多 60 s）；
  4. 断言：工具名依次为 `scene.write_file`、`scene.write_file`、`scene.install`、`ui.tap`、`ui.tree`；`scene.install` 的结果 `ok` 为 true，`check.issues == []`；`ui.tree` 的结果里计数标签显示 `1`；`GET /scenes` 里有这个新场景；
  5. 有一条说明行，内容以"已安装"开头；
  6. 模拟器输出里有 `agent: turn 1 end done steps=5`；
  7. 整个过程的 WS 帧和日志里都没有 `test-key`。
- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R admin_smoke --output-on-failure`。
- [ ] **Step 3: 实现** fake 服务的脚本回放，并写文档。
- [ ] **Step 4: 验证** — `bash tools/ci.sh` 全绿（含 `sim_smoke`、`admin_smoke`）。
- [ ] **Step 5: Commit** — `test(smoke): drive a scene-building turn end to end against a scripted model`

---

### Task 16: 真机验收（spec 8.3，用户执行）

**Files:**
- Modify: `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`（新增"Agent 核心（4a）验收"一节）、本计划（本任务下追加"执行记录"）
- 可能 Modify：`components/wisp_agent/src/agent_service.cpp` 的 `AgentConfig::stack_bytes` 与 `Io` 任务的栈大小（按实测调整）

**Interfaces:**
- Consumes：Task 1–15 的全部内容。固件已烧录，管理后台已配对，激活 DeepSeek `deepseek-flash` 配置。先"测试连接"：`vision` 应自动变为 `true`。

- [ ] **Step 1: 控制者构建并烧录** — `bash -c '. ~/esp/esp-idf-v6.0.3/export.sh >/dev/null && cd firmware && idf.py build && idf.py -p /dev/ttyACM0 flash'`。串口监视器保持打开。
- [ ] **Step 2: 用户按表操作，控制者记录**

| # | 用户 | 通过线 |
|---|---|---|
| 1 | 网页上说"做一个番茄钟" | 设备上出现能用的场景；`scene.install` 的 `check.issues` 为空；对话里有 `ui.screenshot` 调用和缩略图 |
| 2 | 接着说"字放大、按钮挪到下面" | 场景改对了；工具记录里用的是 `scene.edit_file`，不是整文件重写 |
| 3 | 说"屏幕调暗一点" | 亮度变化；对话里有灰色说明行"亮度 … → …" |
| 4 | 说"删掉番茄钟"，在设备上点"拒绝" | 设备亮屏并弹出确认框，文字为"Agent 要删除场景「番茄钟」…"；拒绝后场景仍在，网页显示"等待设备确认"后恢复 |
| 5 | 在设备下拉面板 → 对话浮层里用键盘打字提问 | 网页与设备同步显示提问和回复 |
| 6 | 一轮进行中（TLS 与流式输出同时存在），控制者在串口发 `mem` | 内部 RAM 空闲量与最大块满足 `ram_budget.hpp` 的最坏预算；记录 `wisp.agent` 和 `wisp.agent.io` 栈的 high-water mark |
| 7 | 让设备熄屏，在网页上让 Agent 跑一轮 | Wi-Fi 不断；完成后收到"Agent 已完成"通知，点开进入浮层 |

- [ ] **Step 3: 定栈** — 按第 6 项实测的 high-water mark 加 25% 余量，设定 `wisp.agent` 与 `wisp.agent.io` 的栈。有改动时重新构建并重测第 6 项。
- [ ] **Step 4: 记录并提交** — 硬件文档写入日期、固件提交号、7 项结果、`mem` 原文和栈的结论；本任务追加执行记录。

```bash
git add docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md docs/superpowers/plans/2026-11-05-agent-core.md components/wisp_agent
git commit -m "docs(hardware): record the agent core acceptance run"
```
