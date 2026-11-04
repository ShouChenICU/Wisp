# Wisp 子项目 3a：场景运行时核心 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让开发者手写的 Lua 场景能在模拟器和真机上安装、运行、存数据、调试和导出：每个场景一个带沙箱、200 ms 超时和 2 MB 内存上限的 Lua 5.5 VM，跑在专门的场景任务上，用 Lua 表声明界面、细粒度响应式状态驱动 LVGL 更新。

**Architecture:** 新组件 `wisp_scene`（位于 `wisp_ui` 与 `wisp_app` 之间）持有场景任务、VM、`wisp.*` 绑定、响应式核心（Lua 前导脚本）和控件树到 LVGL 的映射；场景任务只投递请求，文件读写交给 `wisp_core` 新增的存储工作线程，HTTP 交给 `wisp_net` 的 `SceneHttp` 工作线程，界面改动经合并队列由 UI 线程每帧最多应用 64 条。`wisp_core` 新增 `SceneRepository`（场景包）、`SceneStore`（场景数据）与 `SceneHost` 接口，`wisp_admin` 只依赖接口；`wisp_ui` 新增系统层（返回手势、错误面板、屏幕键盘、确认框）。

**Tech Stack:** Lua 5.5.1（原样引入 `third_party/lua`）、LVGL 9.5.0（新开 `lv_ime_pinyin`、LodePNG、Montserrat 32/48）、ESP-IDF v6.0.3、GoogleTest、Preact + htm（网页"场景"页）、Python 3 标准库（`tools/wisp_scene.py`）。

**Spec:** `docs/superpowers/specs/2026-11-04-scene-runtime-core-design.md`（下文"spec N"均指它；"基础 spec"指 `2026-11-01-foundation-design.md`，"连接 spec"指 `2026-11-03-connectivity-admin-design.md`）

## 依赖连接分支

本分支 `feat/scene-runtime-core` 从 `feat/connectivity-admin` 切出，现基于其 `b16bfc3`，该分支尚未合入 `main`。已核实下列依赖在 `b16bfc3` 中存在：

- `net::HttpServer` 的流式请求体：`StreamHandler`/`BodySink`/`StreamStart`（`components/wisp_net/include/wisp/net/http.hpp`），以及 `AdminServer::stream_handler()`（目前只分派 `/api/v1/system/firmware`）；
- 串口命令 `mem`：`firmware/main/main.cpp` 经 `Platform::console_commands` 注册，实现在 `components/wisp_port_esp/mem_report.cpp`；
- 模型配置：`core::ModelProfileService`（`wisp/core/model_profiles.hpp`）。

相对写计划时的 `3d4e33a`，`b16bfc3` 已包含：`9917125` 的 `components/wisp_ui/include/wisp/ui/overlay.hpp`（`mark_opaque_overlay()`：顶层不透明卡片挡住的区域跳过下层重绘；顶层还有别的可见、未标记对象时，碰到它的区域不裁剪）；`52d707b` 的默认字体缓存 8 位字形（`components/wisp_ui/src/a8_font.{hpp,cpp}`，`wisp_ui` 私有；`default_font()` 签名不变，返回的是 A8 包装）；`83598ec`/`9c01a57` 的 ESP32-S3 SIMD 填充与拷贝（`components/wisp_port_esp/lv_blend/`，`lv_conf.h` 与 `firmware/CMakeLists.txt` 的 `WISP_LV_DRAW_SW_ASM_INCLUDE`）；`53183a7` 的二维码按屏幕格式存。自检界面的帧率基准因此是 **22.7 fps（不开热点）/ 28.2 fps（开热点，配网卡片挡住半个列表）**（`docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`"性能基准"）。系统层与 overlay 规则的配合见 Task 10，字号与 A8 字形见 Task 11 与细化第 9 条。

## 相对 spec 的细化（写计划时裁定，执行前请用户确认）

1. **存储工作线程不存在，新增 `core::StorageWorker`**：spec 2.3 说"所有文件读写交给 `StorageManager` 的存储工作线程"，但代码里没有这个线程。新增任务 `wisp.fs`（FIFO 作业队列）。它读写 flash，按 AGENTS.md §5 栈必须在内部 RAM（4096 字节，实测后定）。因此内部 RAM 增量约为 4 KB 栈 + 约 1 KB 作业队列 + 场景消息队列，**超过 spec 2.3 预计的 ≤ 2 KB**；Task 22 用 `mem` 实测并写入硬件文档。
2. **LVGL 的堆改到 PSRAM**：`lv_conf.h` 现在是 `LV_STDLIB_CLIB`，小于 16 KB 的 `malloc` 落在内部 RAM（AGENTS.md §8），2000 个控件会吃掉几百 KB 内部 RAM。改为 `LV_STDLIB_CUSTOM`，分配走 `os::alloc(Mem::Large)` 与新增的 `os::realloc()`；DMA 刷屏缓冲仍是 `Mem::Dma`。这是渲染路径改动，而且 LVGL 对象与 XIP 取指共用 PSRAM 总线（硬件文档的剖析：约 32% 的周期卡在从 PSRAM 取指），回退风险较大。所以 Task 2 之后立即设一道真机关口：用户对比自检界面的 `WISP_PERF`，不开热点不低于 22.7 fps、开热点不低于 28.2 fps，通过后才继续 Task 3；Task 22 第 7 行在最终固件上再测一次。
3. **追加日志与"所有持久化写入都走 `write_atomic`"**：新增 `StorageManager::append()`（fflush + fsync，与 `write_atomic` 同样的路径规则与配额），作为追加日志的唯一入口；压缩照 spec 6.1 用 `write_atomic` 整体重写。
4. **`batch()` 的跨文件原子性**：键值与各集合分属不同日志文件，单靠追加做不到原子。一次刷盘涉及多个文件时，先用 `write_atomic` 写 `data/journal.json`（本次全部操作），再逐个追加，最后删除日志；加载时若日志存在就按 id 幂等重放。`batch` 中途出错时内存索引整体回滚，什么也不落盘。
5. **压缩阈值**："日志超过 64 KB"按"自上次压缩以来追加的字节数超过 64 KB"实现，与"废弃字节 > 有效字节的一半"取或；否则有效数据本身超过 64 KB 的集合每次刷盘都会整体重写。
6. **`collectgarbage`**：spec 3.3 原文"`collectgarbage` 的 `stop`/`step` 以外选项"字面上是只留 stop/step，但 `stop` 会让场景关掉 GC、绕过内存策略。裁定：只允许 `"collect"`、`"count"`、`"step"`，其余（`stop`、`restart`、`isrunning`、`incremental`、`generational`、`param`）报错。
7. **只编译沙箱要的库**：Lua 5.5.1 确有 `void luaL_openselectedlibs(lua_State *L, int load, int preload)`（`linit.c`），但它的表引用全部十个 `luaopen_*`，链接时会带进 io/os/package/debug。裁定：`third_party/lua` 原样收录 `src/` 全部文件，包装组件只编译核心与 base/coroutine/math/string/table/utf8 六个库，`vm.cpp` 用 `luaL_requiref` 逐个打开；`linit.c`、`liolib.c`、`loslib.c`、`loadlib.c`、`ldblib.c`、`lua.c`、`luac.c` 不编译。
8. **栈与递归深度**：包装组件定义 `LUAI_MAXCCALLS=128`（`ldo.h` 用 `#if !defined` 守卫，不改源码）；场景任务栈 32 KB 放 PSRAM。安装时的"编译全部 `.lua`"在场景任务上执行（`SceneHost::check_sources()`），不在 httpd 的 10 KB 内部栈上跑 Lua 解析器。
9. **字号**：只有 Montserrat 16/24 和 CJK 16。`xs`/`sm`/`md` → 16，`lg` → 24，`xl` → 32，`xxl` → 48（新开 `LV_FONT_MONTSERRAT_32`/`48`，只含 ASCII）。`wisp_ui` 在 `default_font()` 旁新增 `ui::font(px)`：24/32/48 返回 A8 包装（与 `default_font()` 同样缓存 8 位字形）且回退到 `lv_font_cjk_16` 的字体，中日文回退到 16 px 的 CJK 字形；`a8_font.hpp` 是 `wisp_ui` 私有的，所以 `wisp_scene` 的 `theme_font` 只转发给 `ui::font`（Task 11）。
10. **图像与图标**：`image` 只支持 PNG（开 `LV_USE_LODEPNG`，解码缓冲走 LVGL 的堆即 PSRAM）；`icon` 的内置图标集是 LVGL 的 `LV_SYMBOL_*`，名称表见 Task 9。
11. **屏幕键盘**：LVGL 的 `lv_keyboard` + `lv_ime_pinyin`，中英切换；日文输入不做（spec 8 只要求中英文）。spec 10 要求复用现有字体子集，而 LVGL 默认词库的候选字多于 CJK 16 子集，所以由 `tools/gen_pinyin_dict.py` 直接读 LVGL 源码里的默认词库、只保留字体中有的字，生成常量词库 `components/wisp_ui/src/pinyin_dict.cpp`（在 flash，不占 RAM），不扩字体；`LV_IME_PINYIN_USE_DEFAULT_DICT` 设为 0，不把 LVGL 的完整默认词库链接进来。
12. **改动队列堆积**（spec 10 第 1 条）：`PatchQueue` 未应用的改动超过 1024 条时丢弃待应用的 `SetProp`、置"整体刷新"标志，场景任务下一次刷新时经 `__wisp.refresh_all()` 重新发出全部绑定的当前值。
13. **GC 停顿的测量**（spec 10 第 2 条）：场景任务对执行超过 50 ms 的消息 `WLOG_W` 一条 `slow message <kind> <ms> ms`（标签 `scene.<id>`），真机验收时据此记录停顿。
14. **定时器最小间隔**：50 ms 同时适用于 `after` 和 `every`。
15. **`list` 虚拟化**：行高统一，由属性 `row_h` 给出（默认 56 px），据此计算可见范围。
16. **上传**：`Content-Length` ≤ 576 KB（512 KB 代码 + zip 头余量），≤ 256 个条目、路径 ≤ 128 字节；支持 stored 与 deflate（`wisp_base` 按 RFC 1951 自写 inflate，不引入第三方代码）；包里出现 `data/` 条目整体拒绝（导入数据不在 3a 范围）。
17. **`scene.state` 的附加时机**：除运行状态变化外，安装后发一次（`stopped`，若正在运行则重载后发 `running`），删除后发 `{"state": "deleted"}`，自检界面和网页据此刷新列表。
18. **出错状态**：出错后在错误面板点"返回"，状态仍是 `error`，直到下次成功打开；最近一次错误只存内存，重启后清空。
19. **网页导航**：窄屏底部仍是 5 个标签（连接 spec 7.2）；"场景"在宽屏侧栏单列，窄屏从"系统"页进入（与"日志"页相同的 `PARENT` 机制）。
20. **无屏幕（headless）**：不构造场景运行时；`POST /scenes/{id}/open` 等返回 503 `unavailable`，上传因无法编译检查同样 503。
21. **HTTP 细节**：请求头用拒绝表（`cookie`、`cookie2`、`proxy-authorization`、`host`、`connection`、`content-length`、`transfer-encoding`、`upgrade`、`te`、`trailer`、`keep-alive`、`expect`、`x-wisp`，以及 `proxy-*`、`sec-*` 前缀），`Authorization` 及其余普通头放行；不跟随重定向（3xx 原样交给场景）；全设备一个 `wisp.shttp` 工作线程串行执行，队列 8 条，满了报 `rate_limited`。
22. **冒烟测试需要的模拟器入口**：新增 `--open-scene <id>` 和模拟器专用调试命令 `debug scene-click <ref>`（与 `debug pair-code` 一样只在 `WISP_SIM_DEBUG_COMMANDS` 下编译）。
23. **`wisp_scene.py logs`** 走现有 WebSocket 日志流（`{"sub":["log"]}`，按标签 `scene.<id>` 过滤），不新增 REST 日志接口。
24. **JSON `null` 转 Lua 时为 `nil`**（数组里会成为空洞，文档说明）；**`wisp.device.info().time`** 在 RTC 未设置（早于 2024-01-01，即 `1704067200`）时为 `nil`。
25. **`--scenes-dir` 模式**：代码只读，`install`/`remove` 返回 `Unsupported`；数据照 spec 6.4 放在 `<data-dir>/scenes/<id>/data/`。

## Global Constraints

逐字摘自 AGENTS.md 的适用规则：

- **可移植层禁止 include 平台头文件**：`esp_*`、`freertos/*`、`driver/*`、IDF 的 `hal/`、`soc/*`、`rom/*`、`lwip/*`、`mbedtls/*`、`curl/*`、`sdkconfig.h`、`nvs.h`/`nvs_flash.h`、`sdmmc_cmd.h`、`SDL*`，以及平台层的 `wisp/port/*`。另外每一层只能依赖上面顺序中排在它之前的层，不得反向依赖（例如 `wisp_net` 不得 include `wisp/admin/`、`wisp/ui/`、`wisp/app/`）。这条规则由 `tools/check_layering.py` 强制检查。
- **服务层约定**：Agent 工具、REST 接口、Lua 绑定都只是同一组服务 API 的薄适配层，不允许绕过服务层直接调用驱动。
- **屏幕形状**：屏幕不一定是完整矩形（圆角屏、圆形屏）。任何界面（包括场景、Agent 生成的界面）都必须按 Display caps 的 `shape` 和 `hal::safe_rect()`/`row_span()` 布局，不得写死边距或假设整屏可见。根容器铺满屏幕、用内边距把内容收进安全区；滚动内容可以经过不可见区域，但滚到两端时首尾内容必须完整落在安全区内（spec 5.6）。
- **UI 单线程**：只有 UI 线程可以调用 LVGL，其他线程一律通过 `UiRuntime::post()` 把操作交给 UI 线程；脚本也不直接调用 LVGL。
- **存储**：数据分为 System / Library / Backup 三个逻辑区，所有持久化写入都走 `StorageManager::write_atomic`。（追加日志的例外见上文细化第 3 条。）
- **语言与错误处理**：C++20，编译选项 `-fno-exceptions -fno-rtti`。不使用 `dynamic_cast` 和 `typeid`，错误一律通过 `wisp::Result<T>` 返回。
- **命名**：组件前缀 `wisp_`，命名空间 `wisp::<层>`，头文件路径 `wisp/<层>/<名>.hpp`，日志宏 `WLOG_D/I/W/E`。
- **组件 CMake**：使用 `wisp_component(...)`，让同一份 CMakeLists 同时兼容 idf.py 和普通 CMake。
- **格式与语言**：格式以 `.clang-format` 为准。代码标识符、注释和提交信息用英文；面向用户的界面文字可以用中文。
- **测试**：使用 GoogleTest，坚持 TDD（先写失败的测试，再实现）。测试输出要干净，不能有告警。
- **依赖**：外部依赖锁定精确版本，固件提交 `dependencies.lock`。
- **配置键**：Settings 的 namespace 和 key 都不能超过 15 个字符，以兼容 NVS。
- **内存分配**：大块缓冲用 `os::alloc(Mem::Large)`，DMA 缓冲用 `Mem::Dma`，不要直接调用平台的分配函数。
- 内部 SRAM 只留给必须放在内部的东西：DMA 缓冲（`Mem::Dma`）；会读、写、擦除或映射内部 flash 的任务的栈；ISR 和 IRAM 代码/数据。其余超过几 KB 的一律用 `Mem::Large`（PSRAM），例如 Lua 堆、LLM 上下文和 JSON、音频环形缓冲、各种缓存。
- 新任务都要声明它会不会访问内部 flash（`os::TaskConfig::flash_access`，httpd 实例是 `EspHttpServerConfig::flash_access`，缺省为 true，即会访问），栈放在哪里由端口根据这个声明按芯片决定，调用方不直接选位置。读、写、擦除或映射内部 flash 都算访问，经由它调用的服务（Settings/NVS、StorageManager/LittleFS、OTA、分区）和它替别人执行的回调都算在内；读已经映射进地址空间的常量和网页资源包不算。只有确实从不访问的任务才能声明 false，目前的 ESP 端口只把这类任务的栈放进 PSRAM（`wisp/port/esp/stack_placement.hpp`）。无论开不开 XIP 都要如实声明，只是违规的表现不同：不开 XIP（`CONFIG_SPIRAM_XIP_FROM_PSRAM`）的板子上，违反时 ESP 端口在 flash 读写里直接 abort；开了 XIP 的板子上普通读写不再停 cache，只有映射 flash（`esp_partition_mmap`、OTA 收尾校验）还会 abort，所以违规代码在这类板子上大多测不出来，模拟器也测不出来。栈的大小按 `mem` 命令实测的 high-water mark 加余量来定。
- 预算：……消耗内部 RAM 的改动必须上真机重新测量。
- **注释**：只解释现在的代码为什么这样写（约束、不变量、踩过的坑），不写"哪一轮评审/哪次修复改了什么"……禁止引用 `.superpowers/` 下的任何文件……禁止评审编号式引用……`tools/ci.sh` 对此有 grep 守卫。
- 移植或改编外部代码时，在文件头注明来源和许可，并登记到 `docs/references.md` 与 `THIRD_PARTY_NOTICES.md`。
- 不得在代码、请求或日志中泄露用户的个人信息或密钥。API Key 只能写入、不能读出。
- 能在模拟器里验证的，就不要依赖真机。需要真机的验证步骤要明确写出来交给用户执行，不得声称已经验证过。
- 改动渲染路径时，要对比 `WISP_PERF` 的数值，不得让帧率低于 spec 和计划中约定的基准。
- **分支**：不在 `main` 上直接开发，使用功能分支。push、merge、发布之前先征得用户同意。
- **提交**：采用 Conventional Commits 风格。每个提交结尾附 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`。**不 push。** `README.md` 不改。

本计划的固定约束：

- 分层变为 `wisp_base` → `wisp_os` → `wisp_hal` → `wisp_core` → `wisp_net` → `wisp_admin` → `wisp_ui` → `wisp_scene` → `wisp_app`。`lua.h`/`lauxlib.h`/`lualib.h`/`lua.hpp`/`luaconf.h` 只有 `wisp_scene` 能 include；`lua` 组件只有 `wisp_scene` 能 REQUIRES。
- **longjmp 规则**：Lua 以 C 编译，错误用 `longjmp` 传播。绑定函数在调用任何可能抛错的 Lua API（`luaL_check*`、`lua_push*`、`lua_new*`、`luaL_error`、`lua_call`）时，栈帧上不得有析构函数非平凡的 C++ 对象（`std::string`、`Value`、容器、`unique_ptr`）。做法：先用 `lua_type`/`lua_to*` 校验参数并在需要时报错，再做 C++ 工作；需要在持有 C++ 对象时调用 Lua API，就经 `scene::protect(L, fn)`（Task 7）在内层 `lua_pcall` 里做，错误消息存在定长 `char[256]` 里再 `lua_error`。
- 数值（spec 原文）：VM 内存 2 MB；每条消息执行 200 ms；每帧最多 64 条界面改动；控件数 2000；场景数据 1 MB；单条记录 16 KB；集合数 16；HTTP 响应 256 KB；HTTP 频率 30 次/分钟；HTTP 超时 15 秒；定时器最小间隔 50 ms、最多 32 个；写入合并 300 ms；压缩阈值 64 KB / 废弃超过有效一半；代码包 512 KB；`id` 为 `[a-z0-9_-]`、1–32 字节；`name` 1–32 字节 UTF-8；`version` 语义化版本；`schema_version` 为 1；`entry` 默认 `main.lua`；返回手势判定区约 16 px、右滑超过 1/4 屏宽；场景放 `/int/scenes/`（`<int 挂载点>/scenes`）。
- 任务配置：`wisp.scene` 优先级 3（低于 UI 的 4）、不绑核（`TaskConfig::core = -1`，UI 任务同样不绑，由 SMP 调度器把两者分到两个核上；spec 1.3/3.1）、栈 32768 字节、`flash_access = false`（场景任务从不碰 flash：文件都经 `wisp.fs`，不读 Settings，语言用 `ui::lang()`）；`wisp.fs` 优先级 3、栈 4096 字节、`flash_access = true`；`wisp.shttp` 优先级 3、栈 8192 字节、`flash_access = false`（只做网络；证书包是已映射的只读常量）。场景消息队列 64 条，元素是指针，消息体经 `os::make_large` 放 PSRAM。
- PSRAM 放置（spec 2.3）：Lua 堆（计数分配器）、场景任务栈、`SceneStore` 索引与写缓冲（`os::LargeString`/`LargeVector`）、HTTP 响应体（`os::LargeBuffer`）、控件树描述与界面改动队列、上传与导出的 zip 缓冲、LVGL 对象。
- 界面文字中/日/英：设备端走 `ui::StrId` 三张表（字形由 `tools/gen_cjk_font_test.py` 检查），网页走 `web/app/i18n/{zh,ja,en}.js`（`tools/check_i18n.py` 检查）。
- API Key、配对令牌、`Authorization` 头的值从不进日志、事件、错误信息或命令行输出。
- 验证命令：主机 `cmake -S simulator -B build/sim -G Ninja && cmake --build build/sim && ctest --test-dir build/sim -R '<正则>' --output-on-failure`；提交前 `bash tools/ci.sh`；固件 `bash -c '. ~/esp/esp-idf-v6.0.3/export.sh >/dev/null && cd firmware && idf.py build'`。

## Review Focus

1. **`bind`/`computed` 的函数在消息末尾刷新界面时出错或超时**：期望这一批改动整体丢弃（不出现半更新的界面），场景进入出错状态并显示错误面板，报错行号指向该函数。测试：Task 8 `Reactive.ErrorInBindFnDiscardsBatch`、Task 13 `SceneRuntime.BindErrorDuringFlushShowsPanelWithoutPartialPatches`。
2. **场景在 HTTP 请求或存储回调未完成时被关闭或重载**：期望回调被丢弃、不在新实例里执行、不访问已销毁的 VM；HTTP 下载被中止；关闭时的刷盘仍然完成。测试：Task 13 `SceneRuntime.StaleMessagesAreDropped`、Task 16 `SceneHttp.CancelSceneSuppressesCallbackAndAborts`、Task 6 `SceneStore.FlushOutlivesOwner`。
3. **`wisp.store.batch()` 里出错、超时，或批处理中途重载**：期望内存与磁盘上都看不到这一批的任何写入，重载后读到的是批处理之前的数据。测试：Task 6 `SceneStore.BatchAbortLeavesNothing`、Task 15 `StoreBinding.BatchErrorAndTimeoutRollBack`。
4. **上传的 zip 含 `../`、绝对路径、反斜杠、符号链接、重复名、加密条目或声明大小远超实际的 zip 炸弹**：期望在写任何文件之前以 400 拒绝，并指出出问题的条目。测试：Task 4 `Zip.RejectsUnsafeEntries`、`Zip.RejectsOversizeBeforeInflating`，Task 17 `AdminScenes.UploadRejectsTraversalWithoutWriting`。
5. **墙上时间跳变（RTC 未设置、之后被校准，或向回调）**：期望 `minute` 事件跳变后只补发一次、不按跳过的分钟数连发、向回跳不会停摆一小时；RTC 未设置时按单调时钟每 60 s 触发。测试：Task 14 `MinuteTicker.*`。

---

## 文件结构总览

```
third_party/lua/
  CMakeLists.txt                       # wisp_component(NAME lua ...)，只编译核心 + 六个库
  lua-5.5.1/{README,src/*}             # 官方 tarball 原样
tools/vendor_lua.sh                    # 下载、校验 SHA-256、解包到 third_party/lua/lua-5.5.1
components/
  wisp_base/  include/wisp/base/{crc32,inflate}.hpp  src/{crc32,inflate}.cpp
  wisp_os/    include/wisp/os/large.hpp  (os.hpp: os::realloc)
  wisp_core/  include/wisp/core/{storage_worker,zip,scene_manifest,scene_repository,scene_store,scene_host}.hpp  src/...
  wisp_net/   include/wisp/net/scene_http.hpp  src/scene_http.cpp  (http.hpp: 流式响应体、响应头、取消)
  wisp_admin/ src/routes_scenes.cpp
  wisp_ui/    include/wisp/ui/{system_layer,lv_heap}.hpp  src/{system_layer,lv_mem_wisp}.cpp
  wisp_scene/ include/wisp/scene/{vm,ui_ops,patch_batch,reactive,patch_queue,schema,scene_view,theme,scene_runtime,
                                  timer_set,minute_ticker,lua_json,dir_watcher}.hpp
              src/{vm,prelude,schema,constructors,scene_view,scene_view_list,scene_view_canvas,
                   scene_runtime,scene_instance,bind_timer,bind_device,bind_store,bind_http,lua_json,...}.cpp
  wisp_app/   src/{scene_wiring,scene_section}.{hpp,cpp}
web/app/pages/scenes.js
tools/{wisp_scene.py,wisp_scene_test.py}
scenes/examples/{counter,pomodoro,todo,ledger,clock,sysinfo,weather}/
docs/scene-api.md                      # 由 schema 生成的控件与属性白名单
tests/scene/  tests/core/scene_*  tests/admin/routes_scenes_test.cpp  tests/app/app_scenes_test.cpp
```

---

### Task 1: 引入 Lua 5.5.1 与 `wisp_scene` 骨架、分层规则

**Files:**
- Create: `third_party/lua/lua-5.5.1/`（`README` 与 `src/` 全部文件，逐字节来自 `lua-5.5.1.tar.gz`）、`third_party/lua/CMakeLists.txt`、`tools/vendor_lua.sh`、`components/wisp_scene/CMakeLists.txt`、`components/wisp_scene/include/wisp/scene/version.hpp`、`components/wisp_scene/src/version.cpp`
- Modify: `tools/check_layering.py`、`tools/check_layering_test.py`、`tools/ci.sh`（clang-format 的 `find` 剪掉 `./third_party`）、`simulator/CMakeLists.txt`（`add_subdirectory` lua 与 wisp_scene）、`firmware/CMakeLists.txt`（`EXTRA_COMPONENT_DIRS` 加 `../third_party`）、`tests/CMakeLists.txt`、`AGENTS.md` §3 分层顺序、`docs/architecture.md`"依赖规则"第 1、2、4 条、`docs/references.md`、`THIRD_PARTY_NOTICES.md`（`## Lua`，MIT 全文取自 `lua.h` 末尾）
- Test: `tests/scene/lua_vendor_test.cpp`

**Interfaces:**
- Produces：CMake 目标/IDF 组件 `lua`（PUBLIC include `lua-5.5.1/src`，PRIVATE 定义 `LUAI_MAXCCALLS=128`）；组件 `wisp_scene`（`REQUIRES wisp_base wisp_os wisp_hal wisp_core wisp_net wisp_ui lua`）；`const char* wisp::scene::lua_release();`
- 版本锁定：`https://www.lua.org/ftp/lua-5.5.1.tar.gz`（2026-07-24，398643 字节），SHA-256 `1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce`（与 lua.org 公布值一致）。`tools/vendor_lua.sh` 把版本与哈希写成常量，下载、校验、解包，只覆盖 `third_party/lua/lua-5.5.1/`。
- 选择原样 vendor 而不是 FetchContent：ESP-IDF 组件需要配置期就在本地的源码，且 spec 2.1 明确"原样引入官方源码"；不改源码、不 fork，升级只换目录和哈希。
- 包装组件 SRCS：`lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring ltable ltm lundump lvm lzio lauxlib lbaselib lcorolib lmathlib lstrlib ltablib lutf8lib`（`.c`）。IDF 若对原版源码报告警，只在包装里关掉那一条告警，不改源码。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(LuaVendor, ReleaseIs551) { EXPECT_STREQ(wisp::scene::lua_release(), "Lua 5.5.1"); }
TEST(LuaVendor, GlobalDeclarationsRejectUndeclaredNames) {
    // luaL_newstate + luaL_loadbufferx(..., "=main.lua", "t")
    // "global<const> print; local a = 1\ny = 2" -> LUA_ERRSYNTAX, message == "main.lua:2: variable 'y' not declared"
}
```

`check_layering_test.py` 新增：`test_scene_sits_between_ui_and_app`（`wisp_ui` include `wisp/scene/x.hpp` 报错；`wisp_scene` include `wisp/ui/x.hpp` 不报错；`wisp_app` include `wisp/scene/x.hpp` 不报错）、`test_lua_header_outside_scene_is_flagged`（`wisp_ui` 里 `#include "lua.h"` 报错，`wisp_scene` 里不报错）、`test_cmake_requires_lua_only_for_scene`（`wisp_ui` 的 `REQUIRES lua` 报错，`wisp_scene` 的不报错）。

- [ ] **Step 2: 运行，确认失败** — `python3 tools/check_layering_test.py` 新用例失败；`ctest --test-dir build/sim -R LuaVendor` 因目标不存在而构建失败。
- [ ] **Step 3: 实现**：运行 `bash tools/vendor_lua.sh`；`COMPONENTS` 在 `wisp_ui` 与 `wisp_app` 之间插入 `wisp_scene`，`LAYER_INCLUDE_PREFIX["wisp_scene"] = "wisp/scene/"`；新增 `LUA_HEADERS = ("lua.h", "lauxlib.h", "lualib.h", "lua.hpp", "luaconf.h")` 只许 `wisp_scene` include；`THIRD_PARTY_REQUIRES = {"wisp_scene": {"lua"}}` 并入 REQUIRES 检查。AGENTS.md §3 的分层行与 `docs/architecture.md` 依赖规则同步改成九层（同一提交，AGENTS.md §9）。
- [ ] **Step 4: 验证** — `bash tools/ci.sh` 全绿；固件 `idf.py build` 成功（`lua` 组件编进来）。
- [ ] **Step 5: Commit** — `build(scene): vendor Lua 5.5.1 and add the wisp_scene layer`

---

### Task 2: PSRAM 原语与 LVGL 堆改到 PSRAM

**Files:**
- Create: `components/wisp_os/include/wisp/os/large.hpp`、`components/wisp_ui/include/wisp/ui/lv_heap.hpp`、`components/wisp_ui/src/lv_mem_wisp.cpp`
- Modify: `components/wisp_os/include/wisp/os/os.hpp`、`components/wisp_port_sim/os/os_sim.cpp`、`components/wisp_port_esp/os/os_esp.cpp`、`components/wisp_ui/config/lv_conf.h`（`LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM`）、`components/wisp_ui/CMakeLists.txt`（IDF 构建时 `lv_mem_wisp.cpp` 在 `wisp_ui` 的 SRCS 里；主机构建时不在，见下）、`simulator/CMakeLists.txt`（主机构建把 `lv_mem_wisp.cpp` 编进 `lvgl` 目标）、`tests/CMakeLists.txt`（注册 `large_test`、`lv_heap_test`；`lv_blend_mask_test` 与 `align_test` 的 LIBS 补 `wisp_port_sim_os`）
- Test: `tests/os/large_test.cpp`、`tests/ui/lv_heap_test.cpp`

**Interfaces:**
- Produces（`wisp::os`）：
  - `void* realloc(void* p, size_t bytes, Mem kind);`（`p == nullptr` 等同 `alloc`；`bytes == 0` 释放并返回 `nullptr`；ESP 用 `heap_caps_realloc`，caps 与 `alloc` 相同）
  - `class LargeBuffer`：`static LargeBuffer allocate(size_t size);`（失败时 `!ok()`）、`bool ok() const`、`uint8_t* data()`、`const uint8_t* data() const`、`size_t size() const`、`void truncate(size_t n)`、`std::span<uint8_t> span()`、`std::span<const uint8_t> span() const`；只能移动
  - `template <class T> struct LargeAllocator`（`allocate` 走 `alloc(n * sizeof(T), Mem::Large)`，得到 `nullptr` 时直接 `abort()`——没有异常可抛；`WLOG_E` 在 `wisp_core`，`wisp_os` 不能用，需要日志的调用方自己在分配前检查并记录）
  - `using LargeString = std::basic_string<char, std::char_traits<char>, LargeAllocator<char>>;`、`template <class T> using LargeVector = std::vector<T, LargeAllocator<T>>;`
  - `template <class T> using LargeUnique = std::unique_ptr<T, LargeDeleter<T>>;`、`template <class T, class... A> LargeUnique<T> make_large(A&&... args);`
- Produces（`wisp::ui`）：`size_t lv_heap_bytes();`（LVGL 当前占用的字节数，诊断与测试用）
- `lv_mem_wisp.cpp` 实现 LVGL 9.5 `LV_STDLIB_CUSTOM` 要求的 `lv_mem_init`、`lv_mem_deinit`、`lv_mem_add_pool`、`lv_mem_remove_pool`、`lv_malloc_core`、`lv_realloc_core`、`lv_free_core`、`lv_mem_monitor_core`、`lv_mem_test_core`；每块前置 16 字节头记录大小（保持 16 字节对齐），总量用原子计数。
- 链接：`LV_STDLIB_CUSTOM` 让 `lvgl` 自己调用 `lv_malloc_core` 等函数。主机上 `lvgl` 是静态库，若这些函数在 `wisp_ui` 里，`lvgl` → `wisp_ui` → `lvgl` 成环，所以主机构建把 `components/wisp_ui/src/lv_mem_wisp.cpp` 直接编进 `lvgl` 目标（`target_sources(lvgl PRIVATE ...)`），并 `target_link_libraries(lvgl PUBLIC wisp_os wisp_port_sim_os)` 给它 `os::alloc` 的实现；`lv_heap_bytes()` 读的计数器随之在 `lvgl` 里，`wisp_ui` 照常链接 `lvgl`。IDF 上组件按链接组解析，`lv_mem_wisp.cpp` 留在 `wisp_ui`。只链接 `wisp_ui` 的现有测试（`lv_blend_mask_test`、`align_test`）同样要能解析 `os::alloc`。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(LargeBuffer, AllocateMoveAndRelease)   // allocate(4096): ok(), size()==4096；移动后源对象 !ok()、size()==0
TEST(LargeAllocator, VectorAndStringGrow)  // LargeVector<int> push 10000 个；LargeString 追加到 100*1024 字节，内容正确
TEST(OsRealloc, PreservesPrefixAndHandlesEdges)
    // alloc 16 字节写 0..15 -> realloc 到 64*1024 前 16 字节不变；realloc(nullptr,32) 非空；realloc(p,0)==nullptr
TEST(LvHeap, ObjectsAreCountedAndReleased)
    // 起 UiRuntime(FakeDisplay)；UI 线程上 before=lv_heap_bytes()；建 100 个 lv_obj -> 增量 > 0；
    // 删除后 lv_heap_bytes() <= before + 1024
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'LargeBuffer|LargeAllocator|OsRealloc|LvHeap'` → 编译失败。
- [ ] **Step 3: 实现** 上述接口。
- [ ] **Step 4: 验证** — 全量 `ctest --test-dir build/sim --output-on-failure`（`ui_runtime_test`、`self_check_screen_test`、`overlay_test`、`a8_font_test`、`lv_blend_mask_test`、`align_test` 等不受影响）；`bash tools/ci.sh`；固件 `idf.py build`。
- [ ] **Step 4a: 真机关口（交给用户执行，细化第 2 条）** — 刷入本任务的固件，不开场景，分别在不开热点与开热点（配网卡片显示）时看自检界面 30 s 的 `WISP_PERF`：fps 不低于 22.7 与 28.2（`docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`"性能基准"）。把结果写进计划的执行记录；低于基准时停下来报告用户，不要自行改回 `LV_STDLIB_CLIB`，也不要开始 Task 3。Task 22 第 7 行在最终固件上再测一次。
- [ ] **Step 5: Commit** — `feat(os): add PSRAM containers and move the LVGL heap to PSRAM`

---

### Task 3: 存储原语与存储工作线程

**Files:**
- Create: `components/wisp_core/include/wisp/core/storage_worker.hpp`、`components/wisp_core/src/storage_worker.cpp`
- Modify: `components/wisp_core/include/wisp/core/storage_manager.hpp`、`components/wisp_core/src/storage_manager.cpp`、`components/wisp_core/CMakeLists.txt`、`tests/CMakeLists.txt`（注册 `storage_worker_test`）
- Test: `tests/core/storage_manager_test.cpp`（追加）、`tests/core/storage_worker_test.cpp`

**Interfaces:**
- Produces（`StorageManager` 新方法；都持有现有 `mutex_`，路径规范化、`..`、`.tmp`/`.new` 保留后缀规则与 `write_atomic` 相同）：
  - `Result<void> append(const std::string& path, std::span<const uint8_t> data);`（不存在就创建；写完 `fflush`+`fsync`；落在 `<int>/library/` 下时计入配额）
  - `enum class EntryKind { File, Dir, Other };  struct DirEntry { std::string name; EntryKind kind; uint64_t size; };`
  - `Result<std::vector<DirEntry>> list_dir(const std::string& path);`（按名字排序，不含 `.`/`..`，用 `lstat`，符号链接为 `Other`；目录不存在 `NotFound`）
  - `Result<void> remove_tree(const std::string& path);`（文件或递归删除目录；不存在算成功；不跟随符号链接）
  - `Result<void> rename(const std::string& from, const std::string& to);`（`to` 已存在时 `AlreadyExists`，FAT 上也成立）
  - `Result<uint64_t> tree_size(const std::string& path);`（普通文件大小之和，不算 `*.tmp`/`*.new`；不存在为 0）
  - `Result<std::string> internal_root() const;`（`<int>` 挂载点；未初始化 `StorageUnavailable`）
  - `Result<os::LargeBuffer> read_file_large(const std::string& path);`（语义同 `read_file`，缓冲在 PSRAM）
- Produces（`wisp::core`）：

```cpp
struct StorageWorkerConfig { size_t queue_capacity = 32; size_t stack_bytes = 4096; int priority = 3; };
class StorageWorker {
public:
    explicit StorageWorker(StorageWorkerConfig config = {});
    ~StorageWorker();                                  // stop()
    static os::TaskConfig task_config(const StorageWorkerConfig& c);  // name "wisp.fs", flash_access=true
    Result<void> start();
    void stop();                                       // 先执行完已排队的作业再 join
    bool submit(std::function<void()> job);            // 队列满或已停止时 false；FIFO
    bool on_worker() const;
    bool drain(uint32_t timeout_ms);                   // 等到队列空且没有作业在跑
};
```

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(StorageManager, AppendCreatesAndExtends)          // append("a\n") 两次 -> read_file == "a\na\n"
TEST(StorageManager, AppendRejectsReservedSuffixAndDotDot)  // "x.tmp"、"a/../b" -> InvalidArgument
TEST(StorageManager, AppendCountsAgainstLibraryQuota)  // quota 1024：追加到 1025 -> QuotaExceeded，文件不变
TEST(StorageManager, ListDirSortedWithKinds)           // {b, a/, link->a} -> [a Dir, b File, link Other]
TEST(StorageManager, RemoveTreeRecursiveAndMissingOk)
TEST(StorageManager, RenameRefusesExistingTarget)      // AlreadyExists；目录改名成功
TEST(StorageManager, TreeSizeExcludesTmpAndNew)
TEST(StorageManager, InternalRootIsIntMount)
TEST(StorageManager, ReadFileLargeMatchesReadFile)     // 70*1024 字节内容一致
TEST(StorageWorker, RunsJobsFifoOnItsOwnTask)          // 100 个作业按 1..100 顺序；作业里 on_worker()==true
TEST(StorageWorker, SubmitFailsWhenFull)               // capacity 2，第一个作业阻塞 -> 第 4 次 submit 返回 false
TEST(StorageWorker, StopRunsQueuedJobs)
TEST(StorageWorker, StackIsInternal)                   // task_config({}): stack_bytes==4096, flash_access==true, name=="wisp.fs"
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'StorageManager|StorageWorker' --output-on-failure`。
- [ ] **Step 3: 实现**；`storage_manager.hpp` 文件头补一段说明 `append` 是追加日志的唯一入口及其掉电语义（末尾可能有半行，由调用方丢弃）。
- [ ] **Step 4: 运行，确认通过**；固件 `idf.py build`。
- [ ] **Step 5: Commit** — `feat(core): add append/list/remove/rename primitives and the storage worker`

---

### Task 4: CRC-32、inflate 与 zip 读写

**Files:**
- Create: `components/wisp_base/include/wisp/base/{crc32,inflate}.hpp`、`components/wisp_base/src/{crc32,inflate}.cpp`、`components/wisp_core/include/wisp/core/zip.hpp`、`components/wisp_core/src/zip.cpp`、`tests/core/fixtures/make_zips.py`（构建时生成测试 zip，同 `fixture.wpak` 的做法）
- Modify: `components/wisp_base/CMakeLists.txt`、`components/wisp_core/CMakeLists.txt`、`tests/CMakeLists.txt`
- Test: `tests/base/crc32_test.cpp`、`tests/base/inflate_test.cpp`（链接 `ZLIB::ZLIB` 只为产生测试数据）、`tests/core/zip_test.cpp`

**Interfaces:**
- Produces（`wisp`）：`uint32_t crc32(std::span<const uint8_t> data, uint32_t crc = 0);`、`Result<size_t> inflate_raw(std::span<const uint8_t> in, std::span<uint8_t> out);`（RFC 1951 原始 deflate，按 RFC 自写，不引入第三方代码；输出写满整块缓冲，不需要 32 KB 滑窗；任何格式错误或输出越界为 `InvalidArgument`）
- Produces（`wisp::core`）：

```cpp
struct ZipLimits { size_t max_entries = 256; size_t max_total = 512 * 1024; size_t max_path = 128; };
struct ZipEntry { os::LargeString path; uint32_t crc32 = 0; uint32_t size = 0; uint32_t comp_size = 0;
                  uint16_t method = 0; uint32_t local_offset = 0; bool is_dir = false; };
class ZipReader {
public:
    static Result<ZipReader> open(std::span<const uint8_t> zip, ZipLimits limits = {});
    const os::LargeVector<ZipEntry>& entries() const;
    Result<os::LargeBuffer> read(const ZipEntry& e) const;   // method 0 或 8；校验 CRC 与大小
};
class ZipWriter {
public:
    void add(std::string_view path, std::span<const uint8_t> data);  // stored
    os::LargeBuffer finish();
};
```

- 条目列表与路径放 PSRAM（`os::LargeVector`/`os::LargeString`）：其大小由不可信的上传包决定，按 `std::vector`/`std::string` 会落进内部 RAM（AGENTS.md 第 8 节），在上传这种最坏预算场景下挤占 httpd/TLS 所需的内部 RAM。原接口写的是 `std::vector`/`std::string`，实施时发现此问题，已裁定改为 Large 容器。

- `open()` 只解析中央目录、校验每个条目，不解压：相对路径；不得以 `/` 开头；不含 `\`、`:`、空段、`.`、`..`；有效 UTF-8；≤ 128 字节；不重复；Unix 外部属性为符号链接（`S_IFLNK`）的拒绝；加密位（通用标志位 0）拒绝；方法只许 0/8；Zip64 拒绝；声明的未压缩大小之和 > `max_total` 拒绝；条目数 > `max_entries` 拒绝。错误为 `InvalidArgument`，消息含条目路径，超大为 `QuotaExceeded`。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(Crc32, KnownVector)                       // crc32("123456789") == 0xCBF43926
TEST(Inflate, RoundTripsZlibRawDeflate)        // 0、1、70000 字节随机加文本，level 0/1/9，windowBits -15
TEST(Inflate, RejectsCorruptAndOverflow)       // 截断 -> InvalidArgument；out 小 1 字节 -> InvalidArgument
TEST(Zip, ReadsStoredAndDeflatedEntries)       // make_zips.py 的 ok.zip：内容与 CRC 正确
TEST(Zip, RejectsUnsafeEntries)                // ../x、/abs、a\\b、a/./b、a//b、C:x、symlink、encrypted、
                                               // method 12、重复路径、129 字节路径、非法 UTF-8：每个都 InvalidArgument 且消息含该路径
TEST(Zip, RejectsOversizeBeforeInflating)      // 声明总大小 512*1024+1 -> QuotaExceeded；257 个条目 -> InvalidArgument
TEST(Zip, CrcMismatchRejected)
TEST(ZipWriter, RoundTripsThroughReader)
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'Crc32|Inflate|Zip' --output-on-failure`。
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 运行，确认通过。**
- [ ] **Step 5: Commit** — `feat(core): add crc32, raw inflate and a validating zip reader/writer`

---

### Task 5: `scene.json` 与 `SceneRepository`

**Files:**
- Create: `components/wisp_core/include/wisp/core/{scene_manifest,scene_repository}.hpp`、`components/wisp_core/src/{scene_manifest,scene_repository}.cpp`
- Modify: `components/wisp_core/CMakeLists.txt`、`tests/CMakeLists.txt`（注册 `scene_manifest_test`、`scene_repository_test`）
- Test: `tests/core/scene_manifest_test.cpp`、`tests/core/scene_repository_test.cpp`

**Interfaces:**
- Consumes：Task 3 的 `StorageManager` 新方法，Task 4 的 `ZipReader`/`ZipWriter`。
- Produces（`wisp::core`）：

```cpp
struct SceneManifest { std::string id, name, version, entry = "main.lua", icon; int schema_version = 1;
                       std::vector<std::string> http_hosts; };
bool valid_scene_id(std::string_view id);
Result<SceneManifest> parse_scene_manifest(std::string_view json);
struct SceneInfo { SceneManifest manifest; uint64_t code_bytes = 0; uint64_t data_bytes = 0; };
struct SourceFile { std::string path; std::string_view text; };
using SourceChecker = std::function<Result<void>(const std::vector<SourceFile>&)>;
struct ScenePackage { SceneManifest manifest; std::vector<std::pair<std::string, os::LargeBuffer>> files; };
struct SceneRepoConfig { std::string code_root; std::string data_root; bool dev = false;
                         size_t max_code_bytes = 512 * 1024; };
class SceneRepository {
public:
    SceneRepository(StorageManager& storage, SceneRepoConfig config);
    Result<void> recover();                                        // 开机在 wisp.fs 上调用
    Result<std::vector<SceneInfo>> list();                         // 跳过以 '.' 开头的目录与无效包
    Result<SceneInfo> info(std::string_view id);
    Result<ScenePackage> load(std::string_view id);                // 除 data/ 外的全部文件读入 PSRAM
    Result<SceneInfo> install(std::span<const uint8_t> zip, const SourceChecker& check);
    Result<os::LargeBuffer> export_zip(std::string_view id, bool with_data);
    Result<void> remove(std::string_view id, bool with_data);
    std::string data_dir(std::string_view id) const;               // <data_root>/<id>/data
};
```

- 校验规则（spec 6.2）：`id` 匹配 `^[a-z0-9_-]{1,32}$`；`name` 1–32 字节有效 UTF-8；`version` 匹配 SemVer 2.0（`MAJOR.MINOR.PATCH`，无前导零，可带 `-pre` 与 `+build`）；`schema_version` 必须为 1；`entry` 是顶层 `*.lua`；`icon` 可空，否则以 `assets/` 开头且包内存在；`permissions.http` 为小写主机名数组（`[a-z0-9.-]`，不含 `*`、`:`、`/`，非空，≤ 253 字节，最多 16 个）；未知顶层字段忽略。
- 包内允许的文件：`scene.json`、`<entry>`、`lib/**/*.lua`、`assets/**`；其余拒绝（含 `data/`）。代码总大小 ≤ 512 KB。
- 安装（spec 6.3）：解到 `<code_root>/.<id>.tmp/`（每个文件 `write_atomic`）→ 调 `check` 编译全部 `.lua`（失败则删暂存目录，错误原样返回，消息形如 `lib/util.lua:3: ...`）→ `rename` 为 `.<id>.new` → 若 `<id>/data` 存在就移进 `.<id>.new/data` → `remove_tree(<id>)` → `rename(.<id>.new, <id>)`。`recover()`：`.<id>.tmp` 一律删除；`.<id>.new` 存在即视为完整，按上面顺序补完剩余步骤。`http_hosts` 与旧版本不同时 `WLOG_I` 标签 `scene.<id>` 记一条 `http hosts changed: [...] -> [...]`。
- `dev` 为真时 `install`/`remove` 返回 `Unsupported`。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(SceneManifest, AcceptsSpecExample)          // spec 6.2 的 JSON 原文：id "todo"、name "待办"、hosts ["api.example.com"]
TEST(SceneManifest, IdRules)                     // "todo" "a" "a-b_9" 32 字节 ok；"" 33 字节 "Todo" "a.b" "a/b" "中" 拒绝
TEST(SceneManifest, NameIsOneTo32BytesUtf8)      // "待办" ok；"" / 33 字节 / "\xff" 拒绝
TEST(SceneManifest, VersionIsSemver)             // "1.0.0" "1.2.3-beta.1+b5" ok；"1.0" "v1.0.0" "01.0.0" 拒绝
TEST(SceneManifest, SchemaEntryIconHosts)        // schema_version 2 拒绝；entry 缺省 main.lua；entry "lib/x.lua" 拒绝；
                                                 // icon "x.png" 拒绝；hosts "*.a.com" "https://a.com" "a.com:443" "A.com" 拒绝
TEST(SceneRepository, InstallsAndLists)          // code_bytes == 包内文件大小之和
TEST(SceneRepository, ReinstallKeepsData)        // 先写 data/kv.log，安装 1.0.1 后 kv.log 原样、version=="1.0.1"
TEST(SceneRepository, RejectsCheckerErrorWithFileAndLine)  // check 返回 "lib/util.lua:3: x" -> 原样返回，code_root 下无 <id>
TEST(SceneRepository, CodeLimitIs512KB)          // 512*1024 ok；512*1024+1 拒绝
TEST(SceneRepository, RejectsUnexpectedFilesAndData)  // "README.md"、"data/kv.log"、"lib/x.txt"
TEST(SceneRepository, RecoverFinishesInterruptedReplace)  // (.todo.new 无 data + todo 有 data) 与 (只有 .todo.new) 两种现场；.todo.tmp 被删
TEST(SceneRepository, ExportRoundTrips)          // data=0 不含 data/；data=1 含 data/kv.log；data=0 的导出可再次安装
TEST(SceneRepository, RemoveKeepsOrDeletesData)
TEST(SceneRepository, DevModeIsReadOnly)         // install/remove -> Unsupported；data_dir == "<data_root>/todo/data"
TEST(SceneRepository, LogsHostChanges)           // 日志环里有 tag "scene.todo"、含 "http hosts changed"
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'SceneManifest|SceneRepository' --output-on-failure`。
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 运行，确认通过。**
- [ ] **Step 5: Commit** — `feat(core): add scene manifests and the scene package repository`

---

### Task 6: `SceneStore`

**Files:**
- Create: `components/wisp_core/include/wisp/core/scene_store.hpp`、`components/wisp_core/src/scene_store.cpp`
- Modify: `components/wisp_core/CMakeLists.txt`、`tests/CMakeLists.txt`（注册 `scene_store_test`）
- Test: `tests/core/scene_store_test.cpp`

**Interfaces:**
- Consumes：`StorageManager::append/write_atomic/read_file_large/remove_tree/tree_size`，`os::LargeString`。
- Produces（`wisp::core`）：

```cpp
struct SceneStoreLimits { uint64_t quota_bytes = 1024 * 1024; size_t record_bytes = 16 * 1024;
                          size_t max_collections = 16; uint32_t coalesce_ms = 300;
                          uint64_t compact_append_bytes = 64 * 1024; size_t key_bytes = 64; };
struct SceneStoreDeps {
    StorageManager& storage;
    std::function<bool(std::function<void()>)> submit_io;                 // 生产：StorageWorker::submit
    std::function<void(uint32_t delay_ms, std::function<void()>)> schedule;  // 生产：一次性 os::Timer
};
struct FindQuery { std::vector<std::pair<std::string, Value>> equals; std::string sort; bool desc = false;
                   size_t limit = SIZE_MAX; size_t offset = 0; };
struct StoreRecord { int64_t id; os::LargeString json; };
class SceneStore : public std::enable_shared_from_this<SceneStore> {
public:
    static std::shared_ptr<SceneStore> create(SceneStoreDeps deps, std::string data_dir, SceneStoreLimits limits = {});
    Result<void> load();                                   // 阻塞；在 wisp.fs 上调用
    std::optional<os::LargeString> kv_get(std::string_view key) const;
    Result<void> kv_set(std::string_view key, std::string_view json);   // json 为编码后的值
    Result<void> kv_del(std::string_view key);
    Result<int64_t> insert(std::string_view coll, std::string_view json);
    Result<void> update(std::string_view coll, int64_t id, std::string_view json);  // 整条替换；不存在 NotFound
    Result<void> remove(std::string_view coll, int64_t id);
    Result<std::vector<StoreRecord>> find(std::string_view coll, const FindQuery& q) const;
    void begin_batch();  Result<void> commit_batch();  void abort_batch();
    void flush(std::function<void(Result<void>)> done = {});   // 立即排一次刷盘（关闭、熄屏）
    uint64_t used_bytes() const;                               // 全部日志文件大小
};
```

- 磁盘格式（spec 6.1）：`<data_dir>/kv.log` 与 `<data_dir>/<集合>.log`，每行 `{"op":"put"|"del","id":…,"v":{…}}`（kv 的 `id` 是键字符串）；集合名 `^[a-z0-9_-]{1,32}$` 且不是 `kv`；集合 id 单调递增，加载时取各集合最大 id。错误码：配额 `QuotaExceeded`（消息 `quota_exceeded`），单条超过 16 KB `InvalidArgument`（`record_too_large`），第 17 个集合 `QuotaExceeded`（`too_many_collections`）。
- 写入：先改索引，再把行追加进该文件的 PSRAM 待写缓冲；第一笔待写时 `schedule(300, ...)`，到点 `submit_io` 一个刷盘作业。刷盘作业涉及多个文件时先 `write_atomic` 写 `journal.json`（行数组），逐个 `append`，再删日志。作业捕获 `shared_from_this()`，所以场景销毁后已排队的刷盘照样完成。
- 压缩：某文件 `dead_bytes > live_bytes / 2` 或自上次压缩追加超过 64 KB 时，刷盘作业改为 `write_atomic` 重写该文件的有效内容。配额按刷盘后全部日志大小预估，超出时先按压缩后的大小再算一次，仍超就拒绝写入。
- 读取：丢弃末尾不完整的行并 `WLOG_W`；`journal.json` 存在时在各日志读完后重放、追加并删除。
- `find`：按顶层字段相等过滤；`sort` 字段数字按数值、字符串按字节序，缺字段的排最后，同值按 id 升序。

- [ ] **Step 1: 写失败的测试**（假 `schedule` 手动触发、`submit_io` 同步执行）

```cpp
TEST(SceneStore, KvSetGetDelete)
TEST(SceneStore, CollectionCrudAndMonotonicIds)   // 1,2,3；删 2；再插 -> 4；重新 load 后再插 -> 5
TEST(SceneStore, FindFilterSortLimitOffset)       // {done=false} + sort "due" desc + limit 2 offset 1
TEST(SceneStore, WritesCoalesceWithin300ms)       // 10 次 kv_set：schedule 只被调用一次且 delay_ms==300；触发前文件不存在；触发后 1 次 append
TEST(SceneStore, FlushWritesPendingNow)
TEST(SceneStore, CompactsWhenDeadExceedsHalfLive) // 重写后文件只含有效行
TEST(SceneStore, CompactsAfter64KBAppendedOnly)   // 100 KB 有效 + 少量追加：不重写；再追加 64*1024+1：重写
TEST(SceneStore, TruncatedTailLineDropped)        // 末尾 `{"op":"put","id":1,"v":{` -> load Ok，之前记录都在，日志环有警告
TEST(SceneStore, QuotaIs1MB)                      // 写到 1024*1024 之后 -> QuotaExceeded，索引不变
TEST(SceneStore, RecordLimitIs16KB)               // 编码后 16384 字节 ok，16385 -> InvalidArgument "record_too_large"
TEST(SceneStore, SixteenCollectionsMax)           // 第 17 个 -> QuotaExceeded "too_many_collections"
TEST(SceneStore, BatchCommitUsesJournal)          // kv + 两个集合：刷盘时先出现 journal.json，结束后不存在；重新 load 一致
TEST(SceneStore, BatchAbortLeavesNothing)         // begin；写 kv 与集合；abort -> kv_get/find 回到之前，flush 后文件不变
TEST(SceneStore, ReadsInsideBatchSeeOverlay)
TEST(SceneStore, JournalReplayedOnLoad)           // 手写 journal.json + 缺这些行的日志 -> load 后数据齐全、journal 删除
TEST(SceneStore, FlushOutlivesOwner)              // 释放最后一个外部 shared_ptr 后执行已排队作业 -> 数据落盘、无崩溃
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R SceneStore --output-on-failure`。
- [ ] **Step 3: 实现**；索引与待写缓冲用 `os::Mutex` 保护（场景任务改、`wisp.fs` 刷）。
- [ ] **Step 4: 运行，确认通过。**
- [ ] **Step 5: Commit** — `feat(core): add the scene data store with coalesced append logs`

---

### Task 7: VM 与沙箱

**Files:**
- Create: `components/wisp_scene/include/wisp/scene/vm.hpp`、`components/wisp_scene/src/vm.cpp`
- Modify: `components/wisp_scene/CMakeLists.txt`、`tests/CMakeLists.txt`（注册 `vm_test`）
- Test: `tests/scene/vm_test.cpp`

**Interfaces:**
- Produces（`wisp::scene`）：

```cpp
enum class ScriptFault { Compile, Runtime, Timeout, Memory };
struct ScriptError { ScriptFault fault; std::string file; int line = 0; std::string message;
                     std::string text() const; };   // "main.lua:12: message"（无行号时 "main.lua: message"）
struct VmLimits { size_t memory_bytes = 2 * 1024 * 1024; uint32_t exec_ms = 200; int hook_instructions = 1000; };
using SourceLookup = std::function<std::optional<std::string_view>(std::string_view path)>;
class Vm {
public:
    static std::unique_ptr<Vm> create(std::string scene_id, VmLimits limits = {},
                                      std::function<uint64_t()> now_ms = os::now_ms);  // 分配失败返回 nullptr
    ~Vm();
    lua_State* L() const;
    const std::string& scene_id() const;
    size_t used_bytes() const;
    void set_sources(SourceLookup lookup);                                   // wisp.import 的来源
    std::optional<ScriptError> load(std::string_view path, std::string_view source);  // 成功时把函数压栈
    std::optional<ScriptError> call(int nargs, int nresults);                // 顶层调用：设截止时间并保护
    bool faulted() const;
};
std::string_view global_preamble();
std::optional<ScriptError> compile_check(std::string_view path, std::string_view source);
int protect(lua_State* L, void (*fn)(lua_State*, void*), void* ctx);         // 内层 lua_pcall；返回 Lua 状态码
// 栈上全部参数经 luaL_tolstring 以 '\t' 连接、截断到 512 字节，写日志，标签 "scene.<id>"（id 取自分配器 ud 指向的 Vm）
void log_lua_args(lua_State* L, core::LogLevel level);
```

- `protect` 只在 `vm.hpp` 声明（不另建私有头），Task 15 等绑定直接 include `vm.hpp`。

- 全局声明机制（已用 5.5.1 实测）：chunk 默认隐含 `global *`；只要出现任何其他 `global` 声明，未声明名字就成为编译错误（`variable 'y' not declared`）。`load()` 把 `global_preamble()` 原样拼在源码**同一行**的最前面，不加换行，所以报错行号不变。前导文本固定为：

```
global<const> ui, state, computed, bind, wisp, scene, assert, error, getmetatable, ipairs, next, pairs, pcall, print, rawequal, rawget, rawlen, rawset, select, setmetatable, tonumber, tostring, type, xpcall, collectgarbage, load, _G, _VERSION, string, table, math, utf8, coroutine;␠
```

  `<const>` 让场景不能给这些名字重新赋值（`attempt to assign to const variable 'ui'`），但 `scene.on_show = ...` 这样的字段赋值不受影响；场景自己的全局要写 `global x` 或 `global function f() end`。chunk 名为 `"=" + path`，所以消息前缀是 `main.lua:3:`。只用 `luaL_loadbufferx(..., "t")`，二进制块被拒绝。`wisp.import` 加载的 `lib/` 文件同样拼前导。
- 状态：`lua_newstate(alloc, this, seed)`（5.5 的签名带 `unsigned seed`，取 `os::now_us()`），用 `luaL_requiref` 打开 `_G`、`coroutine`、`math`、`string`、`table`、`utf8`。之后删除 `dofile`、`loadfile`、`warn`、`string.dump`；`load` 换成强制 `mode = "t"` 的包装；`collectgarbage` 只放行 `collect`/`count`/`step`（其余报 `collectgarbage option '<x>' is not allowed`）；`print` 改为 `log_lua_args(L, core::LogLevel::Info)`（参数以 `\t` 连接，截断到 512 字节，标签 `scene.<id>`）；Task 14 的 `wisp.log.*` 复用同一个函数，不另写连接与截断。
- 分配器：`os::alloc/realloc(Mem::Large)` 加计数；`used + (nsize - osize) > memory_bytes` 时拒绝。Lua 遇到分配失败会先做一次紧急全量 GC 再以同样的 `(ptr, osize, nsize)` 重试，所以**同一请求连续两次被拒绝**才记为内存故障（单次拒绝后重试成功不算）。
- 不可捕获：计数钩子每 `hook_instructions` 条指令检查一次 `now_ms() - start > exec_ms`。超时或内存故障时置 `fault_`，把当前线程与主线程的钩子改为每 1 条指令触发，钩子在 `fault_` 置位期间每次都 `luaL_error`，所以 `pcall`/`xpcall`/协程里的捕获都会在下一条指令再次抛出，直到回到 `call()` 的顶层 `lua_pcall`。`call()` 依据 `fault_` 把结果分类为 `Timeout`（消息 `execution exceeded 200 ms`）或 `Memory`（`memory limit 2 MB exceeded`），之后 `faulted()` 为真，VM 只能销毁。
- `wisp.import(path)`：`path` 必须匹配 `^lib/[a-z0-9_-]+(/[a-z0-9_-]+)*$`（不带 `.lua`），映射到包内 `lib/…​.lua`，按路径缓存返回值，循环导入报 `import cycle: lib/a -> lib/b -> lib/a`。（`wisp` 表本身在 Task 13 建立，这里先由 `Vm` 提供 `wisp.import`。）

- [ ] **Step 1: 写失败的测试**（假时钟）

```cpp
TEST(Vm, OnlySandboxLibraries)                 // io os debug package require dofile loadfile warn 为 nil；string.dump 为 nil；
                                               // utf8 coroutine math table string 存在
TEST(Vm, LoadIsTextOnly)                       // Vm::load("main.lua", "\x1bLua...") -> Compile；脚本内 load("\27Lua") 返回 nil, "attempt to load a binary chunk"
TEST(Vm, CollectgarbageRestricted)             // "count" 返回数字；"collect"/"step" 可用；"stop"/"generational" 报 "is not allowed"
TEST(Vm, UndeclaredGlobalIsCompileErrorWithLine)  // "local a = 1\nb = 2" -> Compile, file "main.lua", line 2, message 含 "variable 'b' not declared"
TEST(Vm, PreambleKeepsLineNumbers)             // 第 3 行 error("x") -> Runtime, line 3
TEST(Vm, DeclaredGlobalsWork)                  // "global counter\ncounter = 1" 与 "global function f() return 1 end" 都 Ok
TEST(Vm, InjectedNamesAreConst)                // "ui = 1" -> Compile, "attempt to assign to const variable 'ui'"
TEST(Vm, TimeoutAt200ms)                       // "while true do end"：时钟推进 199 不超时、201 -> Timeout
TEST(Vm, TimeoutEscapesPcallXpcallAndCoroutines)  // pcall/xpcall/coroutine.wrap 包住死循环，之后 `global flag; flag = true`
                                                  // -> Timeout，且 flag 仍为 nil
TEST(Vm, MemoryLimit2MBIsUncatchable)          // pcall 内保留超过 2 MB 的字符串 -> Memory；任何时刻 used_bytes() <= 2*1024*1024
TEST(Vm, GarbageChurnIsNotAFault)              // 循环创建共 10 MB 短命字符串 -> 无故障
TEST(Vm, FreshVmAfterFault)                    // 故障后新建 Vm 正常运行
TEST(Vm, ImportStaysInLib)                     // import("lib/util") 只执行一次并缓存；"../x" "/lib/x" "lib/../main" "main"
                                               // "lib/x.lua" "lib/missing" 都报错；a<->b 循环报 "import cycle"
TEST(Vm, CompileCheckDoesNotExecute)           // compile_check("main.lua","error('boom')") -> nullopt；语法错误带行号
TEST(Vm, DeeplyNestedSourceFailsCleanly)       // 1000 层括号 -> Compile，无崩溃
TEST(Vm, PrintGoesToSceneLog)                  // print("a", 1) -> 日志环 tag "scene.t" msg "a\t1"；600 字节参数 -> msg 恰好 512 字节
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'Vm\.' --output-on-failure`。
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 运行，确认通过**；固件 `idf.py build`。
- [ ] **Step 5: Commit** — `feat(scene): add the sandboxed Lua VM with time and memory limits`

---

### Task 8: 响应式核心与界面改动批

**Files:**
- Create: `components/wisp_scene/include/wisp/scene/{ui_ops,patch_batch,reactive}.hpp`、`components/wisp_scene/src/{patch_batch,reactive,prelude}.cpp`（`prelude.cpp` 里是 `R"lua(...)lua"` 原始字符串的前导脚本）
- Modify: `components/wisp_scene/CMakeLists.txt`、`tests/CMakeLists.txt`（注册 `patch_batch_test`、`reactive_test`）
- Test: `tests/scene/patch_batch_test.cpp`、`tests/scene/reactive_test.cpp`

**Interfaces:**
- Produces（`wisp::scene`，`ui_ops.hpp`）：

```cpp
using NodeId = uint32_t;  using PropId = uint16_t;  using WidgetId = uint16_t;
using PropValue = std::variant<std::monostate, bool, int64_t, double, os::LargeString>;
struct NodeDesc { NodeId id; NodeId parent; WidgetId widget; uint32_t events;   // 事件位：Click=1, LongPress=2, Change=4
                  os::LargeVector<std::pair<PropId, PropValue>> props; int line; };
struct SetProp { NodeId node; PropId prop; PropValue value; };
struct Build { os::LargeVector<NodeDesc> nodes; };                  // nodes[0] 为根
struct ListReset { NodeId list; uint32_t count; };
struct ListRows { NodeId list; uint32_t first; os::LargeVector<os::LargeVector<NodeDesc>> rows; };
struct CanvasDraw { NodeId canvas; os::LargeVector<uint8_t> commands; };
struct ScrollTo { NodeId node; uint32_t index; };                  // index 从 0 起
struct Focus { NodeId node; };                                     // 聚焦 input（Task 9 的 ui.ref 句柄 :focus()）
using UiOp = std::variant<SetProp, Build, ListReset, ListRows, CanvasDraw, ScrollTo, Focus>;
struct UiBatch { uint32_t instance = 0; os::LargeVector<UiOp> ops; };
```

- Produces（`patch_batch.hpp`）：`class PatchBatch { void set_prop(NodeId, PropId, PropValue); void push(UiOp op); UiBatch take(uint32_t instance); size_t size() const; void clear(); };`——同一 `(node, prop)` 的 `SetProp` 只保留最后一个值，位置取第一次出现的位置；其他操作保持顺序。
- Produces（`reactive.hpp`）：`std::optional<ScriptError> install_prelude(Vm& vm, PatchBatch& out);`、`std::optional<ScriptError> flush_reactive(Vm& vm);`（在消息末尾调用，计入同一次 200 ms）
- 前导脚本定义全局 `state`、`computed`、`bind`，以及内部表 `__wisp`（不在前导声明里，场景代码写出这个名字就是编译错误）：`__wisp.watch(node, prop, source) -> disposer`（source 为 state/computed/bind 对象，变化时经 C 函数 `__wisp.emit(node, prop, value)` 写入 `PatchBatch`）、`__wisp.scope()`/`__wisp.dispose(scope)`（列表行回收用）、`__wisp.flush()`、`__wisp.refresh_all()`（把每个存活 watch 的当前值重新 emit 一次）。
- 语义（spec 4.2）：`state(v)` 的 `:get()` 在追踪上下文里登记依赖；`:set(v)` 与原值相等（基本类型按值，表按引用，`rawequal`）时什么也不做；`computed(fn)` 惰性，首次求值时检测循环依赖（`cyclic dependency: computed@main.lua:3 -> computed@main.lua:4 -> computed@main.lua:3`）；`computed` 里调用 `:set()` 报 `state:set() inside computed`；`flush` 按依赖拓扑重算脏的 computed 与 watch，最多 100 轮，否则报 `reactive updates did not settle`。`flush` 出错时清空 `PatchBatch`（整批丢弃）。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(PatchBatch, CoalescesSamePropKeepsOrder)  // set(1,text,"a"); push(ListReset); set(1,text,"b") -> [SetProp "b", ListReset]
TEST(Reactive, StateGetSet)
TEST(Reactive, EqualWriteDoesNotNotify)        // 同值数字/字符串、同一表引用 -> 0 条；内容相同的新表 -> 1 条
TEST(Reactive, ComputedTracksDependenciesDynamically)  // 分支切换后旧分支的 state 变化不再触发重算
TEST(Reactive, BindAppliesFn)                  // bind(count, function(v) return "已点击 " .. v .. " 次" end)，set(1) -> "已点击 1 次"
TEST(Reactive, MultipleSetsCoalesceToOnePatch) // 一条消息里 set 1、2、3 -> 1 条 SetProp，值 3
TEST(Reactive, CyclicComputedErrorsOnFirstEval) // message 含 "cyclic dependency"
TEST(Reactive, SetInsideComputedErrors)
TEST(Reactive, UnsettledUpdatesError)          // "reactive updates did not settle"
TEST(Reactive, ErrorInBindFnDiscardsBatch)     // 另一个 watch 已产生改动，bind fn 在新值上 error -> Runtime，line 为 fn 所在行，PatchBatch 为空
TEST(Reactive, DisposedScopeStopsPatches)
TEST(Reactive, RefreshAllReemitsLiveWatches)   // 3 个 watch、1 个已 dispose -> refresh_all 后 PatchBatch 恰好 2 条
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'PatchBatch|Reactive' --output-on-failure`。
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 运行，确认通过。**
- [ ] **Step 5: Commit** — `feat(scene): add fine-grained reactive state and coalesced UI patches`

---

### Task 9: 控件白名单与 `ui.*` 构造函数

**Files:**
- Create: `components/wisp_scene/include/wisp/scene/schema.hpp`、`components/wisp_scene/src/{schema,constructors}.cpp`、`docs/scene-api.md`（生成物）
- Modify: `components/wisp_scene/CMakeLists.txt`、`components/wisp_scene/src/prelude.cpp`（`ui.show_if`、`ui.ref` 句柄、`__wisp.set_value`）、`tests/CMakeLists.txt`（注册 `schema_test`，并 `target_compile_definitions(schema_test PRIVATE WISP_ROOT="${WISP_ROOT}")` 供 `DocIsUpToDate` 定位 `docs/scene-api.md`）
- Test: `tests/scene/schema_test.cpp`

**Interfaces:**
- Consumes：Task 7 `Vm`，Task 8 `NodeDesc`、`__wisp`。
- Produces：

```cpp
enum class Widget : WidgetId { Screen, Column, Row, Stack, Grid, Spacer, Scroll, Label, Icon, Image, Button,
                               Checkbox, Switch, Slider, Input, Bar, Arc, Chart, List, Canvas };
enum class Prop : PropId { W, H, Grow, Pad, Gap, Align, Justify, Bg, Fg, Radius, Border, Font, Hidden, Opa, Ref,
                           Text, Placeholder, Password, MaxLen, Name, Src, Value, Min, Max, Checked, Cols,
                           Direction, Type, Series, Items, Row, RowH, Draw, Width, Wrap,
                           OnClick, OnLongPress, OnChange };
enum class PropType { Size, Int, Number, String, Bool, Color, Font, Align, Justify, Pad, Enum, Fn, Items, Series };
struct PropSpec { Prop prop; std::string_view name; PropType type; bool bindable; std::span<const std::string_view> choices; };
struct WidgetSpec { Widget widget; std::string_view name; bool children; std::span<const Prop> props; };
const WidgetSpec* find_widget(std::string_view name);
const PropSpec& prop_spec(Prop p);
std::string nearest_name(std::string_view wrong, std::span<const std::string_view> candidates);  // Levenshtein 最小，并列取字典序第一
std::string schema_markdown();
inline constexpr size_t kMaxNodes = 2000;
std::optional<ScriptError> install_ui(Vm& vm);                       // 全局 ui 表（C 闭包）与 ui.show_if、ui.ref
// 根必须是 screen；超过 2000 报错。错误带 file/line（构造该节点的那一行），Task 13 原样交给错误面板
std::optional<ScriptError> collect_tree(Vm& vm, int index, NodeId& next_id, Build& out);
```

- 白名单（spec 4.3/4.4）：所有控件都有通用属性 `w h grow pad bg fg radius border font hidden opa ref on_click on_long_press`；布局类 `screen column row stack grid scroll` 另有 `gap align justify`（`grid` 加 `cols`，`scroll` 加 `direction`=`v|h`）；`spacer` 只有通用属性；`label`：`text wrap`；`icon`：`name`（`home settings wifi battery bell play pause stop prev next plus minus ok close trash edit refresh left right up down upload download save list image audio warning`，映射到对应 `LV_SYMBOL_*`）；`image`：`src`（`assets/*.png`）；`button`：`text`（或子节点）；`checkbox`：`text checked on_change`；`switch`：`checked on_change`；`slider`：`value min max on_change`；`input`：`text placeholder password max_len on_change`；`bar`：`value min max`；`arc`：`value min max width`；`chart`：`type`=`line|bar`、`series`（≤ 4 条、每条 ≤ 128 个数）、`min max`；`list`：`items row row_h`（`row` 必填）；`canvas`：`draw`。
- 取值：`Size` 为整数像素、`"fill"`、`"content"`；`Pad` 为整数或 `"safe"`；`Color` 为 `#RRGGBB` 或令牌 `accent text muted surface bg danger ok warn`；`Font` 为 `xs sm md lg xl xxl`；`Align` 为 `start center end`；`Justify` 为 `start center end between around evenly`。`bindable` 的属性可以是 state/computed/bind 对象；`Fn` 必须是函数。
- `ui.show_if(s, node)`（spec 4.2）：给 `node` 的 `hidden` 挂一个 `bind(s, function(v) return not v end)`，只切换可见性，从不销毁或重建节点。
- `ui.ref(name)`（spec 4.4）：立即返回句柄，方法调用时才按当前树里 `ref = name` 的节点解析，找不到报 `ui.ref: no widget with ref '<name>'`。`collect_tree` 顺带建 ref → `NodeId` 表，并把 `input` 的 `text`、`slider` 的 `value`、`checkbox`/`switch` 的 `checked` 初值记进 `__wisp.values[node]`；C 函数 `__wisp.set_value(node, v)` 更新它（Task 13 在派发 `Change` 事件、调用 `on_change` 之前调用）。句柄方法：`h:value()` 返回 `__wisp.values[node]`（其他控件报 `ui.ref: '<name>' has no value`）；`h:scroll_to(i)` 只用于 `list`/`scroll`，`i` 从 1 起，向 `PatchBatch` 推 `ScrollTo{node, i - 1}`；`h:focus()` 只用于 `input`，推 `Focus{node}`。用错控件时报 `ui.ref: '<name>' (ui.label) does not support scroll_to` 这样的错误。
- 报错格式（行号取调用构造函数那一行，`lua_getstack(L, 1)` + `lua_getinfo("Sl")`）：`main.lua:12: ui.label: unknown property 'txt' (did you mean 'text'?)`、`main.lua:5: unknown widget 'ui.lable' (did you mean 'ui.label'?)`、`main.lua:7: ui.slider: property 'max' expects number, got string`、`main.lua:20: entry must return ui.screen (got ui.column)`、`main.lua:9: too many widgets (limit 2000)`。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(Schema, UnknownWidgetSuggestsNearest)     // 第 4 行 ui.lable{} -> "main.lua:4: unknown widget 'ui.lable' (did you mean 'ui.label'?)"
TEST(Schema, UnknownPropSuggestsNearest)       // ui.label{txt="a"} -> "... unknown property 'txt' (did you mean 'text'?)"
TEST(Schema, TypeMismatchNamesExpectedType)
TEST(Schema, ColorAcceptsHexAndTokens)         // "#12AB34" "accent" ok；"#12AB3" "blue" 拒绝
TEST(Schema, PadAcceptsSafeAndInts)
TEST(Schema, StateAndBindAcceptedOnlyWhereBindable)  // text=count ok；on_click=count 拒绝
TEST(Schema, RootMustBeScreen)
TEST(Schema, NodeLimit2000)                    // 2000 个 ok；2001 -> "too many widgets (limit 2000)"
TEST(Schema, ListRowFnRequired)
TEST(Schema, CollectTreeAssignsIdsAndLines)    // 根 id 1、parent 关系与 line 正确
TEST(Schema, CollectTreeErrorCarriesFileAndLine)  // 入口第 3 行构造的 ui.column 作根 -> ScriptError{Runtime, file "main.lua", line 3,
                                                  // "entry must return ui.screen (got ui.column)"}；2001 个节点 -> line 为第 2001 个节点的构造行
TEST(Schema, ShowIfHidesWithoutDestroying)     // ui.show_if(flag, ui.label{}) 且 flag=false：Build 里该节点 Hidden==true；flag:set(true) + flush
                                               // -> PatchBatch 恰好 1 条 SetProp(node, Hidden, false)，没有 Build，节点数不变
TEST(Schema, RefValueTracksChanges)            // input{ref="name", text="a"}：value()=="a"；__wisp.set_value(node,"ab") 后 "ab"；
                                               // ui.ref("nope"):value() -> "ui.ref: no widget with ref 'nope'"；label 的 value() 报 "has no value"
TEST(Schema, RefScrollToEmitsScrollOp)         // list{ref="l"}：scroll_to(5) -> PatchBatch 含 ScrollTo{node, 4}；label 上 scroll_to 报 "does not support scroll_to"
TEST(Schema, RefFocusEmitsFocusOp)             // input 的 focus() -> Focus{node}；button 上 focus() 报 "does not support focus"
TEST(Schema, DocIsUpToDate)                    // schema_markdown() == docs/scene-api.md 的内容（WISP_ROOT 编译定义）
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'Schema\.' --output-on-failure`。
- [ ] **Step 3: 实现**；不一致时 `DocIsUpToDate` 把期望内容写到 `build/sim/scene-api.md` 并在失败信息里给出复制命令。
- [ ] **Step 4: 运行，确认通过。**
- [ ] **Step 5: Commit** — `feat(scene): validate widget trees against a compiled-in schema`

---

### Task 10: 系统层（返回手势、错误面板、屏幕键盘、确认框）

**Files:**
- Create: `components/wisp_ui/include/wisp/ui/system_layer.hpp`、`components/wisp_ui/src/system_layer.cpp`、`components/wisp_ui/src/pinyin_dict.cpp`（生成物，提交）、`tools/gen_pinyin_dict.py`
- Modify（追加）: `tools/ci.sh`（`python3 tools/gen_pinyin_dict.py --check`）
- Modify: `components/wisp_ui/config/lv_conf.h`（`LV_USE_IME_PINYIN 1`、`LV_IME_PINYIN_USE_DEFAULT_DICT 0`（词库来自细化第 11 条的生成物）、`LV_IME_PINYIN_USE_K9_MODE 0`）、`components/wisp_ui/include/wisp/ui/strings.hpp` 与 `src/strings_{zh,ja,en}.cpp`、`components/wisp_ui/CMakeLists.txt`、`components/wisp_app/src/{app.cpp,app_impl.hpp,self_check_screen.cpp}`（三个确认框改用 `SystemLayer::open_confirm`，"同一时间只有一个确认框"改用 `SystemLayer::confirm_open()`；`SelfCheck::raise_overlays()` 在 `pair_card_.raise()` 之后调用 `SystemLayer::raise()`，由它提升确认框的背板；`App` 在启动 `UiRuntime` 后、自检界面之前于 UI 线程上构造 `SystemLayer`，停止时在自检界面之后销毁）、`tests/CMakeLists.txt`（注册 `system_layer_test`）
- Test: `tests/ui/system_layer_test.cpp`；`tests/app/{factory_reset_test,firmware_confirm_test,self_check_screen_test,net_ui_test}.cpp` 与 `tests/ui/overlay_test.cpp` 必须不改即通过

**Interfaces:**
- Produces（`wisp::ui`，只能在 UI 线程调用）：

```cpp
struct ErrorPanelInfo { std::string scene_name; std::string file; int line = 0; std::string message; };
class SystemLayer {
public:
    explicit SystemLayer(const hal::ScreenShape& shape);      // 不建常驻容器（见下文"与 overlay 的配合"）
    ~SystemLayer();
    void set_back_handler(std::function<void()> on_back);     // 空函数 = 关闭手势区
    void show_error(ErrorPanelInfo info, std::function<void()> on_reload, std::function<void()> on_back);
    void hide_error();
    bool error_visible() const;
    void show_keyboard(lv_obj_t* textarea);
    void hide_keyboard();
    lv_obj_t* keyboard() const;
    lv_obj_t* open_confirm(StrId title, const std::string& text, std::function<void(bool yes)> on_answer);
                                                              // 已有确认框时返回 nullptr，不回调
    bool confirm_open() const;                                // 同一时间只有一个确认框
    void close_confirm(lv_obj_t* box);
    void raise();                                             // 把当前存在的各块（确认框连同其背板）提到顶层最上面；SelfCheck::raise_overlays() 最后调用
    static bool in_back_zone(const hal::ScreenShape& s, int32_t x, int32_t y);
    static bool is_back_swipe(const hal::ScreenShape& s, lv_point_t start, lv_point_t end);
};
```

- 与 overlay 的配合（`wisp/ui/overlay.hpp`）：顶层只要有一个显示中、未标记的对象碰到某块重绘区，那块就不再裁剪；一个常驻的全屏容器会让配网卡片下的滚动退回整块重画（热点帧率约从 28.2 掉到 14.5，`NetUi.ScrollingUnderTheProvisioningCardLeavesTheCardAlone` 失败）。所以系统层没有常驻容器：手势条、错误面板、键盘各自是 `lv_layer_top()` 的直接子对象，确认框连同它的背板也是，都只在使用期间存在或显示。错误面板与键盘不透明，创建后调用 `mark_opaque_overlay()`；确认框不标记（见下）。系统层建的辅助对象（面板内部容器等）一律清掉 `LV_OBJ_FLAG_CLICKABLE`，只有按钮、键盘和手势条可点。
- 手势区：`y ∈ [safe.y1, safe.y2]` 且 `row_span(y).x1 <= x < row_span(y).x1 + 16`。手势条是一个窄条对象：纵向从 `safe.y1` 到 `safe.y2`，横向从各行 `row_span(y).x1` 的最小值到其最大值 + 16，透明、不标记；创建时隐藏，`set_back_handler` 设了非空函数时才显示，清空时再隐藏。它加 `LV_OBJ_FLAG_ADV_HITTEST`，`LV_EVENT_HIT_TEST` 用 `in_back_zone` 判定，所以圆角/圆形屏上判定区贴着可见边缘，条内其余像素的触摸照常落到场景。`is_back_swipe`：起点在区内、`end.x - start.x > width / 4`、`|dy| < dx`。区内的按下被系统层吃掉，场景收不到。
- 错误面板：不透明（标记为覆盖物），限制在 `safe_rect` 内；标题 `StrId::SceneErrorTitle`，正文为场景名、`file:line` 与错误信息，按钮 `StrId::SceneReload`/`StrId::SceneBack`。
- 键盘：`lv_keyboard` + `lv_ime_pinyin`，`lv_ime_pinyin_set_dict()` 用细化第 11 条生成的词库（`tools/gen_pinyin_dict.py` 读 LVGL 源码里的默认词库与 `tools/gen_cjk_font_test.py` 用的字体字符集，输出排序后的常量表；CI 里用 `--check` 校验已提交的文件是最新的），占安全区下半部，不透明（标记为覆盖物），`LV_EVENT_READY`/`LV_EVENT_CANCEL` 时收起；有一个中/英切换键。
- 确认框：保持模态，用 `lv_msgbox_create(nullptr)`，带 LVGL 默认的全屏背板。确认框守着危险操作（恢复出厂、安装固件、删除场景），框外的触摸绝不能落到下面的界面。背板是未标记的全屏顶层对象，所以确认框打开期间不跳过被遮挡区域的重绘，下面滚动时整块重画；确认框很少出现、开着的时间也短，接受这个代价。框和背板都不调用 `mark_opaque_overlay()`（标记背板会把它当成不透明卡片裁掉下层，而背板是半透明的）。仍是 `lv_msgbox_class`（现有测试按这个类数确认框）；宽度钳制照搬 `SelfCheck::make_confirm_box`。
- 新增 `StrId`：`Scenes`、`ScenesEmpty`、`SceneStateRunning`、`SceneStateError`、`SceneErrorTitle`、`SceneReload`、`SceneBack`、`SceneDeleteTitle`、`SceneDeletePrompt`（含一个 `{}` 填场景名）、`SceneDeleteWithData`、`KeyboardZh`、`KeyboardEn`（zh/ja/en 三份）。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(SystemLayer, BackZoneFollowsRowSpan)      // rect 480：safe 行内 x 0..15 在区内、16 不在；circle 480：y=240 时 x 0..15；
                                               // y=100 时区从 row_span(100).x1 开始；safe 以外的行都不在区内
TEST(SystemLayer, BackSwipeNeedsQuarterWidth)  // 宽 480：dx 120 否、121 是；起点在区外否；dx 130 dy 140 否
TEST(SystemLayer, GestureSwallowedAndCallsBack) // FakeTouch 喂 (4,240)->(200,240)：on_back 恰好一次；区下方的按钮未收到 CLICKED
TEST(SystemLayer, ErrorPanelInsideSafeRect)    // 标签含 "main.lua:12"；两个按钮分别调用 on_reload/on_back；面板区域在 safe_rect 内
TEST(SystemLayer, KeyboardTargetsTextareaInsideSafeRect)
TEST(SystemLayer, PinyinDictGlyphsInFont)      // 生成词库的每个候选字都能由 lv_font_cjk_16 解析；"ni" 的候选含 "你"
TEST(SystemLayer, ConfirmAnswersAndStaysOnTop) // open_confirm 的 box 在全屏背板下（背板是 lv_layer_top() 的子对象）；"是"/"否" 回调 true/false；raise() 后背板在卡片之上
TEST(SystemLayer, ConfirmIsModal)              // 确认框开着时点框外（背板上）的下层按钮 -> 按钮未收到 CLICKED，确认框仍开着
TEST(SystemLayer, OneConfirmAtATime)           // 第一个打开后 confirm_open()==true，第二次 open_confirm 返回 nullptr 且不回调；关闭后 false
TEST(SystemLayer, NoPersistentTopLayerChild)   // 构造后 lv_layer_top() 的子对象数不变；set_back_handler(f) 后多出的手势条宽度 <= 16 + 各行 x1 之差、
                                               // 高度 == safe 高度；set_back_handler({}) 后它隐藏
TEST(SystemLayer, OverlaysKeepOcclusionSkip)   // 照 NetUi.ScrollingUnderTheProvisioningCardLeavesTheCardAlone 的做法：错误面板、键盘分别显示时
                                               // 滚动下层列表，新的刷新区都不进入该块去掉圆角行后的区域
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'SystemLayer' --output-on-failure`。
- [ ] **Step 3: 实现**；把 `SelfCheck::make_confirm_box` 的宽度钳制逻辑搬进 `open_confirm`，`SelfCheck` 的三个确认框改为调用它；"同一时间只有一个确认框"的规则移进 `SystemLayer`（`confirm_open()`、`open_confirm` 忙时返回 `nullptr`），`SelfCheck` 改为查询它；`raise_overlays()` 改为 `pair_card_.raise()` 后 `SystemLayer::raise()`（它提升确认框的背板，原来在 `raise_overlays()` 里的这段逻辑搬过去）。
- [ ] **Step 4: 验证** — 全量 ctest（含 `factory_reset_test`、`firmware_confirm_test`、`self_check_screen_test`、`net_ui_test`、`overlay_test`、`gen_cjk_font_test`）；`bash tools/ci.sh`；固件 `idf.py build`。
- [ ] **Step 5: Commit** — `feat(ui): add the system layer and move the confirm dialogs onto it`

---

### Task 11: 界面改动队列与 `SceneView`（布局、文本、输入、进度）

**Files:**
- Create: `components/wisp_scene/include/wisp/scene/{patch_queue,scene_view,theme}.hpp`、`components/wisp_scene/src/{patch_queue,scene_view,theme}.cpp`
- Modify: `components/wisp_ui/include/wisp/ui/ui_runtime.hpp`、`components/wisp_ui/src/ui_runtime.cpp`（帧钩子；`ui::font(px)`，用 `src/a8_font.hpp` 包装 Montserrat 24/32/48 并回退到 `lv_font_cjk_16`）、`components/wisp_ui/config/lv_conf.h`（`LV_FONT_MONTSERRAT_32 1`、`LV_FONT_MONTSERRAT_48 1`）、`components/wisp_scene/CMakeLists.txt`、`tests/CMakeLists.txt`（注册 `patch_queue_test`、`scene_view_test`）
- Test: `tests/ui/ui_runtime_test.cpp`（追加）、`tests/scene/patch_queue_test.cpp`、`tests/scene/scene_view_test.cpp`

**Interfaces:**
- Produces（`wisp::ui::UiRuntime`）：`void set_frame_hook(std::function<void()> hook);`——在 UI 任务上每轮 UI 循环执行一次（渲染时 ≤ 每帧一次；循环比帧多时也多跑，所以"每帧最多 64 条"实际是"每轮循环最多 64 条"，只会更保守），位于 `drain_post_queue()` 之后、`lv_timer_handler()` 之前；暂停期间不执行；线程安全；空函数表示移除。
- Produces（`wisp::ui`，`ui_runtime.hpp`，与 `default_font()` 并列）：`const lv_font_t* font(int px);`——16 返回 `default_font()`；24/32/48 返回对应 Montserrat 的 A8 包装（首次调用时在 UI 线程上建好并缓存，同 `default_font()`），`fallback` 为 `lv_font_cjk_16`；其他值取不大于它的最大一档，小于 16 取 16。
- Produces（`wisp::scene`）：

```cpp
enum class UiEvent { Click, LongPress, Change, Focus };
struct ViewCallbacks {
    std::function<void(NodeId node, UiEvent ev, Value value)> on_event;          // UI 线程调用，转发给场景任务
    std::function<void(NodeId list, uint32_t first, uint32_t count)> on_list_range;
};
class SceneView {
public:
    SceneView(ui::SystemLayer& system, const hal::ScreenShape& shape, ViewCallbacks cb);  // UI 线程
    ~SceneView();                                      // 删除自己的 screen
    lv_obj_t* screen() const;
    void apply(const UiOp& op);
    lv_obj_t* object(NodeId node) const;
    std::optional<NodeId> find_ref(std::string_view ref) const;
    size_t object_count() const;
};
class PatchQueue {
public:
    explicit PatchQueue(size_t per_frame = 64, size_t max_pending = 1024);
    bool take_refresh_request();                       // 场景任务调用：溢出后为 true 一次，之后发 __wisp.refresh_all()
    void submit(UiBatch batch);                        // 任意线程；未应用的 SetProp 跨批合并（中间没有结构操作时）
    size_t apply_frame(SceneView& view, uint32_t instance);  // UI 线程；最多 64 条；丢弃其他 instance 的批
    size_t pending() const;
    void clear();
};
lv_color_t theme_color(std::string_view token_or_hex);   // accent #4F9DFF, text #FFFFFF, muted #8E8E93, surface #1C1C1E,
                                                         // bg #000000, danger #FF453A, ok #32D74B, warn #FFD60A
const lv_font_t* theme_font(std::string_view size);      // 只转发：xs/sm/md -> ui::font(16)，lg 24，xl 32，xxl 48
```

- 根节点：`screen` 铺满屏幕，内边距强制为 `safe_rect` 到屏幕四边的距离（场景给的 `pad` 只能更大），`pad = "safe"` 的其他节点同样处理。`Build` 一次计 1 条，与它同批的 `SetProp` 随 `Build` 一起应用、不计入上限。`input` 聚焦时调 `SystemLayer::show_keyboard()`，`Change` 事件带当前文本。`ScrollTo` 滚动 `scroll`/`list` 使第 `index` 个子项（`list` 按 `index * row_h`）可见；`Focus` 对 `input` 做 `lv_group_focus_obj` 并调 `show_keyboard()`，节点已不存在时忽略。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(UiRuntime, FrameHookRunsEachLoopIterationNotWhilePaused)  // 运行时计数增长，且在 drain 之后、lv_timer_handler 之前；暂停期间不增长
TEST(UiRuntime, FontSizesWrapA8AndFallBackToCjk)  // font(16)==default_font()；font(24/32/48) 的行高依次 > 前一档；
                                                  // "A" 由该字号自身解析，"你" 经 fallback 由 lv_font_cjk_16 解析；font(20)==font(16)
TEST(PatchQueue, AppliesAtMost64PerFrame)      // 提交 130 条 SetProp -> 三帧分别 64、64、2
TEST(PatchQueue, CoalescesAcrossPendingBatches) // 两批改同一属性 -> 只应用最后值，计 1 条
TEST(PatchQueue, DropsOtherInstances)
TEST(PatchQueue, OverflowCollapsesToFullRefresh) // 未应用 1025 条 -> pending()==0，take_refresh_request() 先 true 后 false
TEST(SceneView, RootForcesSafePadding)         // circle 480 时 safe_rect 为 {71,71,408,408}（含端点），screen 的内边距 == (71,71,71,71)
TEST(SceneView, BuildsCounterTree)             // label 文本、button 文本、column gap 12
TEST(SceneView, SetPropUpdatesLabelAndHidden)
TEST(SceneView, ClickAndChangeEventsReachCallback)  // 按钮 CLICKED -> (id, Click)；slider 改值 -> (id, Change, 30)
TEST(SceneView, InputFocusShowsKeyboard)
TEST(SceneView, ScrollToAndFocusOps)           // list 200 行、row_h 56：ScrollTo{list, 50} 后 scroll_y == 50*56（钳到可滚范围）；Focus{input} 后键盘可见
TEST(SceneView, FontsAndColorsMapToTheme)       // theme_font("xl") == ui::font(32)；令牌颜色与 #RRGGBB
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'UiRuntime|PatchQueue|SceneView' --output-on-failure`。
- [ ] **Step 3: 实现**（`layout` 用 LVGL flex；`grid` 用 LVGL grid，列宽等分）。
- [ ] **Step 4: 验证** — 全量 ctest；固件 `idf.py build`（记录新字体带来的体积增量）。
- [ ] **Step 5: Commit** — `feat(scene): map widget trees to LVGL with a per-frame patch budget`

---

### Task 12: `SceneView` 的列表虚拟化、画布、图表、图像与图标

**Files:**
- Create: `components/wisp_scene/src/{scene_view_list,scene_view_canvas,scene_view_media}.cpp`、`components/wisp_scene/src/list_window.hpp`
- Modify: `components/wisp_scene/include/wisp/scene/scene_view.hpp`（`set_assets`）、`components/wisp_scene/src/scene_view.cpp`、`components/wisp_ui/config/lv_conf.h`（`LV_USE_LODEPNG 1`）、`components/wisp_scene/src/prelude.cpp`（列表行作用域、`g` 绘图对象）、`components/wisp_scene/CMakeLists.txt`、`tests/CMakeLists.txt`（注册下面四个测试）
- Test: `tests/scene/list_window_test.cpp`、`tests/scene/scene_view_list_test.cpp`、`tests/scene/canvas_test.cpp`、`tests/scene/scene_view_media_test.cpp`

**Interfaces:**
- Produces（`SceneView`，UI 线程）：`void set_assets(std::shared_ptr<const core::ScenePackage> package);`——图像从这里按路径取字节；包由 `SceneView` 持有到下一次 `set_assets` 或析构，`lv_image_dsc_t` 直接指向包里的缓冲。
- Produces（纯逻辑，`list_window.hpp`）：`struct ListWindow { uint32_t first; uint32_t count; }; ListWindow visible_rows(int32_t scroll_y, int32_t viewport_h, int32_t row_h, uint32_t total, uint32_t overscan = 2);`
- 列表：容器高度 = `count * row_h`；UI 线程在滚动与 `ListReset` 后计算窗口，经 `on_list_range` 请求；场景任务为缺少的行调用 `row(item, index)`（每行一个 `__wisp.scope`），回 `ListRows`；滑出窗口的行回收（销毁 LVGL 对象、`dispose` 作用域）。`items` 变化时发 `ListReset`，只重建窗口内的行；行内节点 id 失效后，迟到的事件丢弃。
- 画布：`draw = function(g) ... end` 在场景任务上执行并追踪依赖；`g:line(x1,y1,x2,y2,opts)`、`g:circle(cx,cy,r,opts)`、`g:arc(cx,cy,r,start_deg,end_deg,opts)`、`g:rect(x,y,w,h,opts)`、`g:fill(color)`、`g:text(x,y,str,opts)`（`opts` 为 `color width fill radius font`），每次最多 512 条命令，序列化进 `CanvasDraw.commands`；UI 端画进 `os::alloc(Mem::Large)` 的缓冲，颜色格式取 `lv_display_get_color_format()`（真机是 `RGB565_SWAPPED`，与 `flatten_qr` 相同，这样 S3 SIMD 拷贝走同格式快路径），大小为 `lv_draw_buf_width_to_stride(w, cf) * h` 字节。
- 图表：`lv_chart`，`type` 映射 `LV_CHART_TYPE_LINE/BAR`，颜色依次取 `accent ok warn danger`。
- 图像：`src` 指向包内 `assets/*.png`，字节来自打开时载入的 `ScenePackage`（Task 13 的 `Open` 路径在 UI 线程上建好 `SceneView` 后、应用 `Build` 前调用 `set_assets`），以 `lv_image_dsc_t` 内存源交给 LodePNG；包里没有该文件时报 `ui.image: no asset 'assets/x.png'` 并显示空白。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(ListWindow, ComputesVisibleRangeWithOverscan)  // row_h 56、viewport 400、scroll 0、total 200 -> first 0、count 10
TEST(ListWindow, ClampsAtEnd)                        // scroll 超过 total*row_h -> first+count == total
TEST(SceneViewList, OnlyVisibleRowsMaterialized)     // 200 条、row_h 56：已物化的行数 == visible_rows(0, 视口高, 56, 200).count，
                                                     // object_count == 非列表节点数 + 行数 × 每行节点数
TEST(SceneViewList, ItemsReplacedWhileScrolledClampsScroll)  // 滚到底后 items 换成 5 条 -> 滚动位置钳到 0、请求 first 0 count 5、
                                                             // 已回收行的迟到点击被丢弃
TEST(Canvas, CommandsRoundTripAndLimit512)           // 513 条 -> "canvas: more than 512 draw commands"
TEST(Canvas, RedrawsOnDependencyChange)
TEST(Canvas, BufferUsesDisplayColorFormat)          // FakeDisplay 设为 RGB565_SWAPPED 时画布缓冲同格式，大小 == stride * h
TEST(SceneViewMedia, ChartSeriesAndImageFromAssets)  // 4 条 x 128 点 ok；第 5 条报错；set_assets 后 assets/icon.png 解码后尺寸正确；缺失的资源报错
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'ListWindow|SceneViewList|Canvas|SceneViewMedia' --output-on-failure`。
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 验证** — 全量 ctest；固件 `idf.py build`。
- [ ] **Step 5: Commit** — `feat(scene): virtualize lists and add canvas, chart, image and icon widgets`

---

### Task 13: 场景任务、生命周期与 `SceneHost`

**Files:**
- Create: `components/wisp_core/include/wisp/core/scene_host.hpp`、`components/wisp_core/src/scene_host.cpp`、`components/wisp_scene/include/wisp/scene/scene_runtime.hpp`、`components/wisp_scene/src/{scene_runtime,scene_instance,scene_message}.{hpp,cpp}`
- Modify: `components/wisp_core/CMakeLists.txt`、`components/wisp_scene/CMakeLists.txt`、`tests/CMakeLists.txt`（注册 `scene_runtime_test`，`TIMEOUT 60`，并 `target_include_directories(scene_runtime_test PRIVATE ${WISP_ROOT}/components/wisp_scene/src)`，同 `a8_font_test` 的做法）
- Test: `tests/scene/scene_runtime_test.cpp`（`UiRuntime` + `FakeDisplay`/`FakeTouch` + `SystemLayer` + 临时目录的 `StorageManager`/`StorageWorker`/`SceneRepository`）

**Interfaces:**
- Produces（`wisp::core`）：

```cpp
enum class SceneState { Stopped, Running, Error };
const char* to_string(SceneState s);                  // "stopped" "running" "error"
struct SceneStatus { SceneState state = SceneState::Stopped; std::string error; };   // error 为 ScriptError::text()
class SceneHost {
public:
    virtual ~SceneHost() = default;
    virtual Result<void> open(const std::string& id, Value args = {}) = 0;   // 未知 id: NotFound
    virtual Result<void> close(const std::string& id) = 0;
    virtual Result<void> reload(const std::string& id) = 0;
    virtual SceneStatus status(const std::string& id) const = 0;
    virtual Result<void> check_sources(const std::vector<SourceFile>& files) = 0;  // 在场景任务上编译，最多等 10 s
    virtual void on_installed(const std::string& id) = 0;                          // 运行中则重载，否则发 stopped
};
```

- Produces（`wisp::scene`）：

```cpp
struct SceneRuntimeDeps {
    core::EventBus& bus; core::StorageManager& storage; core::StorageWorker& worker;
    core::SceneRepository& repo; ui::UiRuntime& ui; ui::SystemLayer& system; hal::ScreenShape shape;
    net::SceneHttp* http = nullptr;                    // Task 16 接入
    std::function<Value()> device_status;              // Task 14 接入
};
struct SceneRuntimeConfig { VmLimits vm; size_t queue_capacity = 64; size_t stack_bytes = 32768; int priority = 3; };
class SceneRuntime : public core::SceneHost {
public:
    SceneRuntime(SceneRuntimeDeps deps, SceneRuntimeConfig config = {});
    ~SceneRuntime() override;                          // stop()
    static os::TaskConfig task_config(const SceneRuntimeConfig& c);   // "wisp.scene", flash_access = false
    Result<void> start();
    void stop();
    void set_visible(bool visible);                    // power.state：false = 熄屏，true = 亮屏
    std::optional<std::string> foreground() const;
    Result<void> click_ref(std::string_view ref);      // 模拟器调试命令用
    // SceneHost 的全部方法
};
```

- 任务：`wisp.scene` 优先级 3，低于 UI 任务的 4；两者都不绑核（`task_config()` 的 `core` 保持 `-1`），由 SMP 调度器分到两个核上（spec 1.3/3.1；硬件文档实测给 UI 任务绑核没有收益）。
- 消息：`Open{id, instance, args, shared_ptr<const ScenePackage>, shared_ptr<SceneStore>}`、`Close`、`Reload`、`Show`、`Hide`、`Event{node, UiEvent, Value}`、`ListRange`、`HttpDone`、`StoreDone`、`Device{kind}`、`CheckSources`；都带 `(scene_id, instance)`，`instance` 每次打开递增，不匹配的消息直接丢弃并计数（`stale_dropped()` 供测试）。队列 `os::Queue<SceneMessage*>` 容量 64，消息体用 `os::make_large`；UI 线程投递失败时 `WLOG_W` 并丢弃该事件。本任务还没有定时器：主循环 `pop` 一直阻塞到有消息（`stop()` 投一条退出消息唤醒）；Task 14 引入 `TimerSet` 后才把超时改为下一个定时器到期时间。
- 私有头 `src/scene_message.hpp` 定义 `SceneMessage` 各变体，并声明 `bool post_scene_message(SceneRuntime& rt, os::LargeUnique<SceneMessage> msg);`——运行时内部所有投递点（UI 线程事件、`wisp.fs` 回调、HTTP 回调）都走它；队列满时返回 `false` 并释放消息。测试经它注入旧 `instance` 的消息。
- 打开（spec 3.4）：`wisp.fs` 上 `repo.load(id)` + `SceneStore::create(...)->load()` → 投 `Open` → 场景任务建 `Vm`、`install_prelude`、`install_ui`、`wisp.*` → `call` 入口文件 → `collect_tree`（返回的 `ScriptError` 带 file/line，直接进出错流程）→ `Build` 进 `PatchQueue` → UI 线程建 `SceneView`、`set_assets(package)`、`lv_screen_load()`，记下原来的活动屏幕（自检界面，不销毁）、`set_back_handler(close)` → `scene.on_show()`。每条消息执行完调 `flush_reactive`，再 `PatchQueue::submit`。`Event` 为 `Change` 时先 `__wisp.set_value(node, value)` 再调 `on_change`（`ui.ref` 句柄的 `:value()` 读它，Task 9）。
- 关闭/返回：`on_hide` → `on_close`（各自 200 ms，出错或超时也照常继续）→ `SceneStore::flush()` → 取消 HTTP（Task 16 接入）与定时器（Task 14 接入）→ 销毁 VM → UI 线程删 `SceneView`、重新加载原屏幕。重载 = 关闭后打开，数据保留。同时只允许一个前台场景，打开另一个先关当前的。
- 每条消息计时，超过 50 ms 时 `WLOG_W` 标签 `scene.<id>` 输出 `slow message <kind> <ms> ms`（细化第 13 条）；每次刷新前检查 `PatchQueue::take_refresh_request()`。
- 出错（spec 3.3/3.6）：任何未捕获错误 → 销毁 VM、取消定时器（Task 14）与 HTTP（Task 16）→ 状态 `Error`、记住 `ScriptError::text()` → `SystemLayer::show_error`（"重载" = `reload`，"返回" = 回到原屏幕，状态仍为 `error`）→ `WLOG_E` 标签 `scene.<id>`。不自动重试。
- 事件 `scene.state`，`Event::source` 为 `"scene"`，`payload = {"id": string, "state": "stopped"|"running"|"error"|"deleted"}`。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(SceneRuntime, OpensCounterAndAppliesClick)        // spec 4.1 计数器：label "已点击 0 次"；click_ref("plus") 后 1 s 内 "已点击 1 次"
TEST(SceneRuntime, LifecycleHookOrder)                 // 日志顺序：open->show；set_visible(false)->hide；true->show；close->hide,close
TEST(SceneRuntime, HomeScreenHiddenNotRebuilt)         // 关闭后活动屏幕指针与打开前相同
TEST(SceneRuntime, ErrorShowsPanelAndDestroysVm)       // on_click 里 error -> state Error、错误面板含 "main.lua:<行>"、事件 {id,"error"}；reload -> Running
TEST(SceneRuntime, TimeoutFaultsScene)                 // on_click 死循环 -> Error，消息含 "execution exceeded 200 ms"
TEST(SceneRuntime, BindErrorDuringFlushShowsPanelWithoutPartialPatches)
    // 两个 label：A 绑 count，B 绑 bind(count, fn)，fn 在 v == 1 时 error；点击使 count 变 1 ->
    // A 与 B 的文本都仍是点击前的值；PatchQueue::pending()==0；错误面板可见；ScriptError 的 line 为 fn 里 error 那一行；state Error
TEST(SceneRuntime, OnCloseErrorStillCloses)
TEST(SceneRuntime, BackGestureClosesToHome)            // FakeTouch 从 (4,240) 拖到 (200,240) -> foreground() 为空
TEST(SceneRuntime, OpeningAnotherClosesFirst)
TEST(SceneRuntime, StaleMessagesAreDropped)            // 重载后经 post_scene_message 投递旧 instance 的 HttpDone/StoreDone -> stale_dropped()==2，无崩溃
TEST(SceneRuntime, CheckSourcesRunsOnSceneTask)        // 语法错误返回 "main.lua:2: ..."；正确源码 Ok
TEST(SceneRuntime, TaskConfigIsPsramStack)             // task_config({}): stack_bytes 32768, flash_access false, priority 3, core -1, name "wisp.scene"
TEST(SceneRuntime, ImageAssetsReachView)               // 带 assets/icon.png 的包打开后，image 节点的源来自 set_assets 的包
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R SceneRuntime --output-on-failure`。
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 运行，确认通过**。
- [ ] **Step 5: Commit** — `feat(scene): run scenes on a dedicated task with lifecycle and error panel`

---

### Task 14: `wisp.timer`、`wisp.device`、`wisp.log`、`wisp.scene`

**Files:**
- Create: `components/wisp_scene/include/wisp/scene/{timer_set,minute_ticker}.hpp`、`components/wisp_scene/src/{timer_set,minute_ticker,bind_timer,bind_device,bind_log,bind_scene}.cpp`
- Modify: `components/wisp_scene/src/scene_instance.cpp`、`scene_runtime.cpp`（主循环超时、关闭/出错时取消定时器、熄屏/亮屏、设备事件订阅）、`components/wisp_scene/CMakeLists.txt`、`tests/CMakeLists.txt`（注册下面三个测试）
- Test: `tests/scene/timer_set_test.cpp`、`tests/scene/minute_ticker_test.cpp`、`tests/scene/scene_api_test.cpp`

**Interfaces:**
- Produces：

```cpp
class TimerSet {
public:
    static constexpr uint32_t kMinIntervalMs = 50;  static constexpr size_t kMaxTimers = 32;
    Result<uint32_t> after(uint32_t ms, uint64_t now);   // ms < 50: InvalidArgument；第 33 个: QuotaExceeded
    Result<uint32_t> every(uint32_t ms, uint64_t now);
    bool cancel(uint32_t id);
    std::vector<uint32_t> due(uint64_t now);             // after 到期即移除；every 重排
    std::optional<uint64_t> next_due() const;
    void pause(uint64_t now);
    void resume(uint64_t now);                           // 错过的 every 只补触发一次，之后从 now 起按周期
    size_t size() const;
};
class MinuteTicker {
public:
    static constexpr int64_t kValidSince = 1704067200;   // 2024-01-01；更早视为 RTC 未设置
    bool poll(std::optional<int64_t> wall_s, uint64_t mono_ms);   // true = 现在发 minute
    uint64_t next_poll_ms(std::optional<int64_t> wall_s, uint64_t mono_ms) const;
    void force();                                         // 亮屏时补发一次
};
```

- Lua 侧（spec 5.1/5.2/5.5）：`wisp.timer.after(ms, fn)`/`every(ms, fn)` 返回句柄（`h:cancel()`），失败返回 `nil, "interval_too_small"` 或 `nil, "too_many_timers"`；`wisp.device.info()` 返回 `{width, height, shape, safe_area={x1,y1,x2,y2}, lang, time, battery, charging, wifi}`（`time` 来自 `std::time`，未设置时为 `nil`；`battery` 未知为 `nil`；`wifi` 取 `off|connecting|backoff|connected`；电量、充电、Wi-Fi 取自 `SceneRuntimeDeps::device_status()`，语言取 `ui::lang()`）；`wisp.device.on("battery"|"minute"|"lang", fn)`，其他名字 `luaL_argerror`，处理函数收到与 `info()` 相同的表；`battery` 在 `power.charging.changed` 时与每分钟比较到电量/充电变化时触发，`lang` 在 `system.settings` 的 `language` 变化时触发；`wisp.log.info/warn/error(...)` 调 Task 7 的 `log_lua_args(L, Info|Warn|Error)`（`\t` 连接、截断 512 字节、标签 `scene.<id>`，与 `print` 同一实现）；`wisp.scene.close()` 在当前消息结束后关闭；`wisp.scene.args()` 返回打开参数表。
- 接入主循环（Task 13 留下的位置）：`pop` 的超时取 `TimerSet::next_due()` 与 `MinuteTicker::next_poll_ms()` 中较早者，都没有时一直阻塞；关闭与出错时清空 `TimerSet`。
- 熄屏：`on_hide`、`TimerSet::pause`、`SceneStore::flush()`；亮屏：`TimerSet::resume`、`MinuteTicker::force()` 补发 `minute`、`on_show`（spec 3.4）。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(TimerSet, AfterEveryCancel)
TEST(TimerSet, MinIntervalAndCount)          // 49 -> InvalidArgument；50 ok；第 33 个 -> QuotaExceeded；cancel 后可再建
TEST(TimerSet, ResumeFiresMissedEveryOnce)   // every 100，暂停 1000 ms，resume -> due 只含一次；下一次在 now+100
TEST(MinuteTicker, FiresOncePerWallMinute)   // wall_s 为整秒：12:00:59 不触发，12:01:00 触发一次；同一分钟内再 poll 不触发
TEST(MinuteTicker, ForwardJumpFiresOnceNotPerSkippedMinute)   // 向前跳 3 小时 -> 只触发 1 次
TEST(MinuteTicker, BackwardJumpFiresOnceAndDoesNotStall)      // 向回跳 1 小时 -> 立即 1 次，下一次在新时间的下一个整分，不等 1 小时
TEST(MinuteTicker, UnsetClockFallsBackToMonotonic)            // wall < kValidSince -> 每 60000 ms 一次
TEST(MinuteTicker, ClockBecomingValidResyncs)
TEST(SceneApi, DeviceInfoShape)              // 字段齐全；circle 480 的 safe_area == {71,71,408,408}；RTC 未设置时 time == nil
TEST(SceneApi, DeviceOnRejectsUnknownEvent)
TEST(SceneApi, LogTagIsSceneId)              // wisp.log.warn("a", 1) -> Warn、tag "scene.<id>"、msg "a\t1"（与 print 同格式）
TEST(SceneApi, SceneCloseAndArgs)
TEST(SceneApi, HidePausesTimersShowResumes)  // 运行时集成：熄屏期间 every 不触发；亮屏后补 1 次、minute 补 1 次、on_show 被调用
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'TimerSet|MinuteTicker|SceneApi' --output-on-failure`。
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 运行，确认通过。**
- [ ] **Step 5: Commit** — `feat(scene): add timer, device, log and scene control APIs`

---

### Task 15: `wisp.store` 绑定与 Lua↔JSON

**Files:**
- Create: `components/wisp_scene/include/wisp/scene/lua_json.hpp`、`components/wisp_scene/src/{lua_json,bind_store}.cpp`
- Modify: `components/wisp_scene/CMakeLists.txt`、`tests/CMakeLists.txt`（注册 `lua_json_test`、`store_binding_test`）
- Test: `tests/scene/lua_json_test.cpp`、`tests/scene/store_binding_test.cpp`

**Interfaces:**
- Produces：

```cpp
// 把 idx 处的值编码进 out；失败返回错误码字符串（"not_serializable" 或 "too_deep"），不抛 Lua 错误
const char* lua_to_json(lua_State* L, int idx, os::LargeString& out);
// 把 JSON 解码并压栈；失败返回 false 且栈不变。内部经 protect() 执行
bool push_json(lua_State* L, std::string_view json);
```

- 编码规则：`nil`→`null`；布尔、整数、有限浮点照写（NaN/inf → `not_serializable`）；键恰为 `1..n`（n ≥ 1）的表为数组；键全为字符串的表为对象；空表为 `{}`；混合键、函数、userdata、线程、循环引用 → `not_serializable`；嵌套深度 > 32 → `too_deep`。解码：对象→表、数组→序列、`null`→`nil`。
- Lua 侧（spec 5.4）：`wisp.store.get(k)`；`wisp.store.set(k, v)`（`v == nil` 删除）成功返回 `true`，否则 `nil, code`，`code` ∈ `quota_exceeded`、`record_too_large`、`too_many_collections`、`not_serializable`、`too_deep`；`wisp.store.batch(fn)` 用内层 `lua_pcall` 执行 `fn`，出错（包括超时、内存故障）时 `abort_batch()` 后重新抛出，成功时 `commit_batch()`；`wisp.store.collection(name)` 返回对象，`c:insert(t) -> id`、`c:update(id, t)`、`c:delete(id)`、`c:find(filter?, opts?)`（`opts = {sort, desc, limit, offset}`，返回的每条记录带 `id` 字段）。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(LuaJson, RoundTripsNestedValues)        // 整数与浮点区分（3 与 3.5）、UTF-8 字符串、嵌套数组/对象
TEST(LuaJson, RejectsUnserializable)         // 函数、循环、NaN、{1, x=2} -> "not_serializable"
TEST(LuaJson, DepthLimit32)                  // 32 层 ok，33 层 "too_deep"
TEST(LuaJson, NullBecomesNil)
TEST(StoreBinding, KvAndCollectionsFromLua)  // insert 返回 1、2；find({done=false},{sort="due",desc=true,limit=1})
TEST(StoreBinding, ErrorsReturnedAsNilAndCode)  // 16385 字节的值 -> nil, "record_too_large"；第 17 个集合 -> nil, "too_many_collections"
TEST(StoreBinding, BatchErrorAndTimeoutRollBack)  // batch 内 set + insert 后 error("x") -> 场景出错，索引与磁盘都没有这两笔；
                                                  // batch 内死循环 -> Timeout，同样什么都没有；重载后读到批前数据
TEST(StoreBinding, DataSurvivesReload)
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'LuaJson|StoreBinding' --output-on-failure`。
- [ ] **Step 3: 实现**（遵守全局约束里的 longjmp 规则：`std::string`/`Value` 只在 `protect()` 外层持有）。
- [ ] **Step 4: 运行，确认通过。**
- [ ] **Step 5: Commit** — `feat(scene): bind wisp.store to the scene data store`

---

### Task 16: `SceneHttp` 与 `wisp.http`

**Files:**
- Create: `components/wisp_net/include/wisp/net/scene_http.hpp`、`components/wisp_net/src/scene_http.cpp`、`components/wisp_scene/src/bind_http.cpp`
- Modify: `components/wisp_net/include/wisp/net/http.hpp`、`components/wisp_net/src/http.cpp`、`components/wisp_port_sim/net/sim_http_client.cpp`、`components/wisp_port_esp/net/esp_http_client.cpp`、`tests/support/fake_http.hpp`、`components/wisp_net/CMakeLists.txt`、`components/wisp_scene/CMakeLists.txt`、`tests/CMakeLists.txt`（注册 `scene_http_test`、`http_binding_test`）
- Test: `tests/net/scene_http_test.cpp`、`tests/sim/sim_http_client_test.cpp`（追加）、`tests/scene/http_binding_test.cpp`

**Interfaces:**
- Modify（`wisp::net`）：`HttpClientRequest` 增加 `size_t max_response_bytes = 0;`（0 = 端口自己的上限）、`std::function<bool(std::span<const uint8_t>)> on_body;`（设置后响应体分块交给它、不进 `body`，返回 `false` 中止）、`std::function<bool()> cancelled;`（每块之间检查）；`HttpClientResponse` 增加 `std::vector<std::pair<std::string, std::string>> headers;`（小写名，最多 32 个，单值 ≤ 1 KB）；`TransportError` 增加 `TooLarge`、`Cancelled`（`to_string` 为 `"too_large"`、`"cancelled"`）。两个端口都实现这些字段；仍不跟随重定向；`Authorization` 等请求头的值照旧不进日志。
- Produces：

```cpp
struct SceneHttpLimits { size_t max_response = 256 * 1024; uint32_t per_minute = 30; uint32_t timeout_ms = 15000;
                         size_t queue = 8; };
struct SceneHttpRequest { std::string scene_id; uint32_t instance = 0; Method method = Method::Get; std::string url;
                          std::vector<std::pair<std::string, std::string>> headers; std::string body; };
struct SceneHttpResult { std::string error;           // "" 或 permission_denied not_https too_large rate_limited
                                                      // 或传输错误 dns connect tls timeout other
                         int status = 0; std::vector<std::pair<std::string, std::string>> headers; os::LargeBuffer body; };
class SceneHttp {
public:
    SceneHttp(HttpClient& client, SceneHttpLimits limits = {}, std::function<uint64_t()> now_ms = os::now_ms);
    ~SceneHttp();
    static os::TaskConfig task_config();               // "wisp.shttp", 8192, priority 3, flash_access false
    Result<void> start();  void stop();
    void set_hosts(const std::string& scene_id, std::vector<std::string> hosts);
    void submit(SceneHttpRequest req, std::function<void(SceneHttpResult)> done);  // 立即拒绝也经 done 回调
    void cancel_scene(const std::string& scene_id);    // 排队的丢弃、进行中的中止，都不再回调
    static std::string check_url(std::string_view url, const std::vector<std::string>& hosts);
    static bool header_allowed(std::string_view name, std::string_view value);
};
```

- 规则（spec 5.3）：先查 `https://`（否则 `not_https`），再把 URL 主机名小写后与声明列表精确比较（否则 `permission_denied`）；请求头按细化第 21 条的拒绝表，值含 CR/LF 也拒绝（`permission_denied`），最多 16 个头；每场景 60 s 滑动窗口内最多 30 次（第 31 次 `rate_limited`），队列满同样 `rate_limited`；`timeout_ms = 15000`；`max_response_bytes = 256 * 1024`，超过为 `too_large`；响应体写进 PSRAM。Lua：`wisp.http.get(url, opts, cb)`、`wisp.http.post(url, body, opts, cb)`（`opts.headers` 为表；`body` 为字符串或表，表按 JSON 编码并补 `Content-Type: application/json`），`cb(err, {status, headers, body, json})`，`content-type` 含 `json` 时附 `json` 字段（解析失败则无此字段）；回调经 `HttpDone` 消息回到场景任务。表 `body` 的编码用 Task 15 的 `lua_to_json`（失败时 `cb("not_serializable"|"too_deep", nil)`），`json` 字段用 `push_json`，不另写 JSON 编解码。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(SceneHttp, ExactHostMatch)              // 声明 api.example.com：https://api.example.com/x ok；https://API.example.com ok；
                                             // x.api.example.com、api.example.com.evil.com -> permission_denied
TEST(SceneHttp, RejectsNonHttps)             // http://api.example.com -> not_https
TEST(SceneHttp, ResponseLimit256KB)          // 262144 字节 ok；262145 -> too_large；客户端收到 max_response_bytes == 262144
TEST(SceneHttp, ThirtyPerMinute)             // 第 31 次 -> rate_limited；假时钟 +60000 后恢复
TEST(SceneHttp, TimeoutIs15s)                // 客户端收到 timeout_ms == 15000
TEST(SceneHttp, ForbiddenHeaders)            // Cookie、Proxy-Authorization、Host -> permission_denied；Authorization、X-Api-Key 放行；
                                             // 值含 "\r\n" -> permission_denied
TEST(SceneHttp, CancelSceneSuppressesCallbackAndAborts)  // 假客户端阻塞；cancel_scene 后 cancelled() 为 true，done 从未被调用
TEST(SceneHttp, AuthorizationNeverLogged)    // 日志环不含 Authorization 的值
TEST(SimHttpClient, StreamsBodyHeadersAndCancels)  // on_body 分块；headers 小写；max_response_bytes 超出 -> TooLarge；cancelled -> Cancelled
TEST(HttpBinding, GetDeliversJson)           // cb(nil, {status=200, json={a=1}})
TEST(HttpBinding, ErrorsAreStrings)          // 未声明域名 -> cb("permission_denied", nil)
TEST(HttpBinding, TableBodyIsJson)           // post(url, {a=1}) -> 请求体 {"a":1}、Content-Type application/json；body 含函数 -> cb("not_serializable", nil)
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'SceneHttp|SimHttpClient|HttpBinding' --output-on-failure`。
- [ ] **Step 3: 实现**；`SceneRuntime` 打开场景时 `set_hosts(id, manifest.http_hosts)`，关闭与出错时 `cancel_scene(id)`。
- [ ] **Step 4: 验证** — 全量 ctest；固件 `idf.py build`（`EspHttpClient` 用 `HTTP_EVENT_ON_HEADER` 收集响应头，按块读入 `on_body`）。
- [ ] **Step 5: Commit** — `feat(net): add the per-scene HTTP proxy and wisp.http`

---

### Task 17: 管理接口 `/scenes` 与网页"场景"页

**Files:**
- Create: `components/wisp_admin/src/routes_scenes.cpp`、`web/app/pages/scenes.js`
- Modify: `components/wisp_admin/include/wisp/admin/admin_server.hpp`（`AdminDeps` 增加 `core::SceneRepository* scenes = nullptr; core::SceneHost* scene_host = nullptr; std::function<Result<void>(const std::string& id, bool with_data)> request_scene_delete;`）、`components/wisp_admin/src/{admin_internal.hpp,admin_server.cpp}`（`stream_handler()` 按路径分派固件与场景上传）、`components/wisp_net/include/wisp/net/http.hpp`（`HttpResponse` 增加 `std::shared_ptr<const void> body_owner;`，让 `static_body` 指向 PSRAM 里的导出 zip）、`components/wisp_admin/CMakeLists.txt`、`web/app/main.js`（路由、侧栏条目、`PARENT['/scenes'] = '/system'`）、`web/app/pages/system.js`（"场景"入口）、`web/app/pages/logs.js`（读 `#/logs?tag=` 预填过滤）、`web/app/api.js`（`exportUrl`）、`web/app/i18n/{zh,ja,en}.js`、`tests/CMakeLists.txt`（注册 `routes_scenes_test`）
- Test: `tests/admin/routes_scenes_test.cpp`

**Interfaces:**
- Consumes：`SceneRepository`、`SceneHost`（含 `check_sources`、`on_installed`）。
- REST（spec 7.1，鉴权、Host、`X-Wisp` 与现有 API 相同；下列路径都在 `kApiPrefix` 即 `/api/v1` 之下，外部客户端实际请求 `/api/v1/scenes…`）：
  - `GET /scenes` → `{"scenes": [{"id","name","version","code_bytes","data_bytes","state","hosts":[...],"error"}]}`
  - `POST /scenes`（流式）：无 `Content-Length` 411；> 576 KB 413；请求体收进 `os::LargeBuffer`；`on_end` 调 `install(zip, [&](files){ return scene_host->check_sources(files); })`，成功 `on_installed(id)` 并 201 返回该场景的列表项；校验失败 400 `{"error":"invalid_scene","message":"lib/util.lua:3: ...","file":"lib/util.lua","line":3}`；无 `scene_host` 503 `unavailable`；`dev` 模式 409 `dev_mode`
  - `GET /scenes/{id}/export?data=0|1` → `application/zip`，`Content-Disposition: attachment; filename="<id>-<version>.zip"`
  - `POST /scenes/{id}/open|close|reload` → 200 `{"state": ...}`；未知 id 404；无 `scene_host` 503
  - `DELETE /scenes/{id}?data=0|1` → 202 `{"pending": true}`；`request_scene_delete` 返回 `Busy` 时 409 `busy`
- 网页（spec 7.2）：列表（名称、版本、状态徽标、代码大小、数据占用）、上传 zip（进度条，复用 `upload()`）、每行的打开/关闭/重载/导出（含"连同数据"选项）/删除（提示需在设备上确认），详情显示声明的域名与最近错误，"日志"按钮跳 `#/logs?tag=scene.<id>`；订阅 WebSocket 的 `scene.state` 事件刷新列表。只经 `t()` 取文字，新增键：`nav.scenes`、`scenes.title`、`scenes.empty`、`scenes.upload`、`scenes.uploading`、`scenes.installed`、`scenes.invalid`、`scenes.open`、`scenes.close`、`scenes.reload`、`scenes.export`、`scenes.export_data`、`scenes.delete`、`scenes.delete_data`、`scenes.delete_pending`、`scenes.hosts`、`scenes.no_hosts`、`scenes.last_error`、`scenes.logs`、`scenes.state.stopped`、`scenes.state.running`、`scenes.state.error`、`scenes.code_size`、`scenes.data_size`、`scenes.unavailable`、`scenes.dev_mode`（zh/ja/en 三份）。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(AdminScenes, RequiresAuthAndCsrf)            // 无 cookie 401；POST 无 X-Wisp 403
TEST(AdminScenes, ListShape)
TEST(AdminScenes, UploadStreamsAndInstalls)       // 4 KB 分块；201 含 id/name/version/hosts；on_installed 被调用
TEST(AdminScenes, UploadErrors)                   // 411；576*1024+1 -> 413；语法错误 -> 400 file/line
TEST(AdminScenes, UploadRejectsTraversalWithoutWriting)  // "../evil.lua" 条目 -> 400，code_root 下没有任何新文件
TEST(AdminScenes, ReuploadKeepsData)
TEST(AdminScenes, ExportZip)                      // content-type application/zip；data=1 含 data/
TEST(AdminScenes, OpenCloseReload)                // 假 SceneHost 记录调用；未知 404；无 host 503
TEST(AdminScenes, DeleteNeedsDeviceConfirm)       // 202；request_scene_delete("todo", true)；返回 Busy 时 409
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R AdminScenes --output-on-failure`。
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 验证** — 全量 ctest；`bash tools/ci.sh`（含 `check_i18n.py` 与 150 KB 网页预算）；模拟器 + 浏览器实际走一遍上传、打开、导出、删除（Playwright），截图 zh/ja/en。
- [ ] **Step 5: Commit** — `feat(admin): manage scenes over REST and from the admin page`

---

### Task 18: App 接线与自检界面"场景"区块

**Files:**
- Create: `components/wisp_app/src/{scene_wiring,scene_section}.{hpp,cpp}`
- Modify: `components/wisp_app/include/wisp/app/app.hpp`（`Platform` 增加 `std::string scenes_dir;`；`App` 增加 `scene::SceneRuntime* scenes();`、`core::SceneRepository* scene_repository();`）、`components/wisp_app/src/{app.cpp,app_impl.hpp,net_wiring.cpp,self_check_screen.cpp}`、`components/wisp_app/CMakeLists.txt`（`REQUIRES ... wisp_scene`）、`simulator/CMakeLists.txt`、`firmware/main/main.cpp`（无新字段需要填，只确认编译）、`tests/CMakeLists.txt`（注册 `app_scenes_test`）
- Test: `tests/app/app_scenes_test.cpp`

**Interfaces:**
- Consumes：前面各任务的全部接口。
- 启动（插在现有步骤里）：存储初始化后启动 `StorageWorker` 并在其上 `SceneRepository::recover()`（代码根 `<internal_root>/scenes`，`scenes_dir` 非空时为 dev 模式：代码根 `scenes_dir`、数据根 `<data-dir>/scenes`，由模拟器传入）；有 `Platform::http_client` 时构造并启动 `SceneHttp`；UI 存在时在 `SystemLayer` 之后构造并启动 `SceneRuntime`（`device_status` 汇总 `PowerMonitor::last_status()` 与 `WifiService::status()`）；`AdminDeps` 填 `scenes`、`scene_host`、`request_scene_delete`；订阅 `power.state` 调 `set_visible()`。停止：`SceneRuntime` 在自检界面之前停，`SceneHttp` 随后，`StorageWorker` 在 `StorageManager` 刷新定时器之前停（先 `drain`）。
- `request_scene_delete`：UI 线程上 `SystemLayer::open_confirm(SceneDeleteTitle, tr_fill(SceneDeletePrompt, name) + 是否含数据, ...)`；`SystemLayer::confirm_open()` 为真（或 `open_confirm` 返回 `nullptr`）时返回 `Busy`，规则只有 `SystemLayer` 一处（Task 10）；"是"→ 运行中先 `close` → `wisp.fs` 上 `repo.remove(id, with_data)` → 发 `scene.state {id, "deleted"}`；"否"→ 什么也不做。
- 自检界面"场景"区块（spec 8）：标题 `StrId::Scenes`；列表来自 `wisp.fs` 上的 `repo.list()`，经 `post` 回 UI；每行名称 + 状态（出错带 `StrId::SceneStateError` 徽标）；空时 `ScenesEmpty`；点击行 → `SceneRuntime::open(id)`；收到 `scene.state` 时重新列出。布局遵守安全区。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(AppScenes, SelfCheckListsInstalledScenes)   // <int>/scenes/counter 预置 -> 自检界面出现 "场景" 区块与 "计数器"
TEST(AppScenes, TapOpensSceneAndBackReturns)     // 点行 -> foreground()=="counter"；返回手势后自检屏幕指针不变
TEST(AppScenes, StandbyHidesAndWakeShows)        // PowerManager 进入待机 -> 日志 "hide"；退出 -> "show"
TEST(AppScenes, DeleteConfirmFlow)               // request_scene_delete -> 确认框；"是" -> 目录删除、事件 deleted；"否" -> 保留
TEST(AppScenes, DeleteBusyWhenDialogOpen)        // 恢复出厂确认框开着（confirm_open()==true）-> Busy，不新开确认框
TEST(AppScenes, ErrorBadgeInList)
TEST(AppScenes, HeadlessHasNoRuntime)            // 无 display.main -> scenes()==nullptr；admin open -> 503
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R AppScenes --output-on-failure`。
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 验证** — 全量 ctest；`bash tools/ci.sh`；固件 `idf.py build`。
- [ ] **Step 5: Commit** — `feat(app): wire the scene runtime into boot and the self-check screen`

---

### Task 19: 模拟器目录热重载与 `tools/wisp_scene.py`

**Files:**
- Create: `components/wisp_scene/include/wisp/scene/dir_watcher.hpp`、`components/wisp_scene/src/dir_watcher.cpp`、`tools/wisp_scene.py`、`tools/wisp_scene_test.py`
- Modify: `simulator/{sim_options.hpp,sim_options.cpp,main.cpp}`、`components/wisp_app/src/scene_wiring.cpp`（dev 模式下每 500 ms 在 `wisp.fs` 上轮询）、`tools/ci.sh`（跑 `wisp_scene_test.py`）、`components/wisp_scene/CMakeLists.txt`、`tests/CMakeLists.txt`（注册 `dir_watcher_test`）
- Test: `tests/sim/sim_options_test.cpp`（追加）、`tests/scene/dir_watcher_test.cpp`、`tools/wisp_scene_test.py`

**Interfaces:**
- Produces：`class DirWatcher { public: DirWatcher(core::StorageManager& storage, std::string root); std::vector<std::string> poll(); };`——快照为每个场景目录下（不含 `data/`）各文件的 `(路径, 大小, crc32)`，返回内容有变化的场景 id；连续两次轮询都变化时等下一次（300 ms 去抖，避免读到写了一半的文件）。
- 模拟器选项：`--scenes-dir <目录>`（填 `Platform::scenes_dir`），`--open-scene <id>`（启动完成后 `app.scenes()->open(id)`）；调试命令 `debug scene-click <ref>` → `SceneRuntime::click_ref`，输出 `scene-click=ok` 或 `scene-click=<错误>`。前台场景的文件变化时自动 `reload`；错误同时出现在错误面板和控制台（`WLOG_E` 标签 `scene.<id>`）。
- `tools/wisp_scene.py`（只用标准库）：`push <目录> [--open] [--watch]`、`ls`、`logs <id>`；全局选项 `--host`（默认 `wisp.local:8900`）与 `--config-dir`（默认 `$XDG_CONFIG_HOME/wisp`，否则 `~/.config/wisp`）。令牌存 `<config-dir>/scene_tool.json`（按主机分键，先写临时文件再 `os.replace`，权限 0600）；所有接口都在 `/api/v1` 之下：首次使用时 `POST /api/v1/pair/start`，从标准输入读屏幕上的配对码，`POST /api/v1/pair/confirm` 取 `wisp_token`；场景接口为 `/api/v1/scenes…`；401 时重新配对。所有请求带 `Cookie: wisp_token=…` 与 `X-Wisp: 1`。`push` 只打包 `scene.json`、入口、`lib/**/*.lua`、`assets/**`（其余文件列为"已跳过"），`ZIP_DEFLATED`；输出 id、版本、声明的域名、状态，绝不打印令牌。`--watch` 每 0.5 s 比较文件快照，变化后去抖 300 ms 再推送（设备端安装后自动重载）。`logs` 用最小的 WebSocket 客户端订阅 `{"sub":["log"]}`，只打印标签为 `scene.<id>` 的行。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(SimOptions, ScenesDirAndOpenScene)
TEST(DirWatcher, DetectsChangeEvenWithSameSize)    // 改一个字节、大小不变 -> 返回该 id
TEST(DirWatcher, IgnoresDataDirectory)
```

```python
def test_packs_only_allowed_files()      # README.md、.git/ 被跳过并列出；lib/a.lua、assets/x.png 在包内
def test_token_file_is_0600_and_atomic()
def test_token_never_printed()           # 捕获 push/ls 的 stdout/stderr，不含令牌
def test_requests_use_api_prefix()       # 假服务器记录的路径都以 /api/v1/ 开头（pair/start、pair/confirm、scenes）
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'SimOptions|DirWatcher' --output-on-failure`；`python3 tools/wisp_scene_test.py`。
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 运行，确认通过**；`bash tools/ci.sh`。
- [ ] **Step 5: Commit** — `feat(sim): hot-reload scenes from a directory and add the push tool`

---

### Task 20: 示例场景与冒烟测试

**Files:**
- Create: `scenes/examples/{counter,pomodoro,todo,ledger,clock,sysinfo,weather}/{scene.json,main.lua}`（需要时加 `lib/`、`assets/`）、`tests/scene/examples_test.cpp`、`tests/scene/fixtures/broken/{scene.json,main.lua}`
- Modify: `tests/smoke/sim_smoke.py`、`tests/smoke/admin_smoke.py`、`tests/CMakeLists.txt`
- Test: 同上

**Interfaces:**
- 示例（spec 9.3）：`counter`（spec 4.1 原文加 `wisp.store` 记住次数，点击时 `wisp.log.info("count=" .. n)`，按钮 `ref = "plus"`）；`pomodoro`（`arc`、`wisp.timer.every(1000)`，熄屏恢复后按墙上时间校正剩余时间）；`todo`（`list` + `input` + 集合 `todos`，`row_h = 56`）；`ledger`（集合 `entries`、`find` 按日期降序、`chart` 柱状最近 7 天）；`clock`（`canvas` 表盘、`wisp.device.on("minute")`、根节点之外的绘制按 `safe_area` 缩放）；`sysinfo`（`wisp.device.info()` 与 `battery`/`lang` 事件）；`weather`（`wisp.http.get` 无需 Key 的公开接口 `https://api.open-meteo.com/v1/forecast?...`，`permissions.http = ["api.open-meteo.com"]`）。所有示例只用白名单里的控件与属性，文字中文为主，变量都用 `local` 或显式 `global`。
- `examples_test.cpp`：对每个示例目录 `parse_scene_manifest` 成功、全部 `.lua` 过 `compile_check`；在 `SceneRuntime`（`FakeDisplay`、假 `HttpClient` 对 `api.open-meteo.com` 返回固定 JSON）里打开，2 s 内状态为 `Running`、没有错误面板。
- `sim_smoke.py` 新用例（spec 9.2）：
  - `scene_counter`：把 `scenes/examples` 复制到临时目录，`--scenes-dir` + `--open-scene counter`，经 stdin 发 `debug scene-click plus` 两次，日志出现 `scene.counter` 的 `count=1`、`count=2`；
  - `scene_hot_reload`：同上点击一次后改写 `main.lua`（加一行 `wisp.log.info("v2")`），日志出现 `v2`，再点击得到 `count=2`（数据保留）；
  - `scene_error`：`--scenes-dir tests/scene/fixtures` + `--open-scene broken`，日志出现 `scene.broken` 的 `main.lua:<行>`，`--swipe 4,240,240,240@<ms>` 后日志出现关闭与 `scene.state` 回到自检界面，截图非空；
  - `scene_push`：不带 `--scenes-dir`，配对（`debug pair-code`）后运行 `tools/wisp_scene.py --host 127.0.0.1:<port> --config-dir <tmp> push scenes/examples/counter --open`（配对码经其 stdin 传入），输出含 `running`；再 `DELETE /api/v1/scenes/counter?data=1` 得 202，等 WebSocket 日志里的确认框日志行，经 stdin `tap` 点"是"，`GET /api/v1/scenes` 为空。
- `admin_smoke.py`（路径都带 `/api/v1` 前缀）：上传 `counter` 的 zip（201）、列表、导出（`data=0`/`1`）、打开/关闭/重载，所有响应与 WS 帧不含令牌。

- [ ] **Step 1: 写失败的测试**（`examples_test.cpp` 与冒烟用例先于示例与修正落地）。
- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'Examples|sim_smoke|admin_smoke' --output-on-failure`。
- [ ] **Step 3: 写示例并修正暴露出的问题**（每个修正先补单元测试）。
- [ ] **Step 4: 验证** — `bash tools/ci.sh` 全绿；模拟器窗口里逐个手动打开示例截图（rounded_rect 与 `--shape circle` 各一张），确认圆形屏上内容都在安全区内。
- [ ] **Step 5: Commit** — `feat(scenes): add example scenes and scene smoke tests`

---

### Task 21: 文档

**Files:**
- Modify: `docs/architecture.md`、`docs/porting-guide.md`、`docs/references.md`、`THIRD_PARTY_NOTICES.md`、`AGENTS.md`（只在发现新的通用陷阱时）、本计划（记录执行期裁定）

- [ ] **Step 1: `docs/architecture.md`**：标题范围改为涵盖子项目 3a；分层图加入 `wisp_scene`；依赖规则核对九层与 Lua 头文件限制；启动流程加入 `StorageWorker`/`recover()`、`SceneHttp`、`SystemLayer`、`SceneRuntime`，停止顺序同步；事件表加 `scene.state`（发布方 `scene::SceneRuntime` 与 `app` 的删除流程，payload 与细化第 17 条一致）；改正第 9 条的覆盖物说明：确认框是模态的（`SystemLayer::open_confirm` 建的 `lv_msgbox` 带全屏背板），不做跳过标记，开着时下层整块重画；被标记的只有配网/配对卡片、错误面板与屏幕键盘；返回手势条是只在场景前台时显示的未标记窄条（碰到它的重绘区不裁剪），`SelfCheck::raise_overlays()` 最后调用 `SystemLayer::raise()`；新增"场景运行时"一节（任务表：`wisp.scene` PSRAM 32 KB、`wisp.fs` 内部 4 KB、`wisp.shttp` PSRAM 8 KB；消息流；每帧 64 条；`/int/scenes/` 布局与两阶段替换；`SceneStore` 文件格式与日志）；模拟器一节加 `--scenes-dir`、`--open-scene`、`debug scene-click`；存储一节加 `<int>/scenes`。
- [ ] **Step 2: `docs/porting-guide.md`**：新板子运行场景的前提（PSRAM：Lua 堆与 LVGL 堆都在 PSRAM；`CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` 已在公共配置里）；场景层没有板级代码，屏幕形状只经 caps 生效；验收时补跑 Task 22 的场景行。
- [ ] **Step 3: references 与 notices 核对**：Lua 5.5.1（版本、SHA-256、`tools/vendor_lua.sh`、只编译的文件列表）已在 Task 1 登记；LVGL 自带的 LodePNG（zlib 许可，随 LVGL 分发）与 `lv_ime_pinyin` 默认词库补一行说明；inflate 为自写实现，不登记。
- [ ] **Step 4: 验证** — `bash tools/ci.sh`（注释守卫覆盖 `docs/`）。
- [ ] **Step 5: Commit** — `docs: describe the scene runtime in the architecture and porting guides`

---

### Task 22: 真机验收清单与内部 RAM 实测

**Files:**
- Modify: `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`（"真机验收清单"表追加行，"内部 RAM"一节追加"场景运行时"小节）、本计划

**Interfaces:**
- 依赖：串口命令 `mem` 来自连接分支（`firmware/main/main.cpp` 的 `console_commands`，`b16bfc3` 中已有）；若合并后改了名字或输出格式，这里同步改。

- [ ] **Step 1: 在验收表末尾追加以下行**（编号接现有最后一行，"结果"栏留空；来源写 "scene spec 9.4.n"）：

| 项目 | 怎么测 | 预期结果 |
|---|---|---|
| 两种安装方式（9.4.1） | 网页"场景"页上传 `counter` 的 zip；`python3 tools/wisp_scene.py --host <IP>:8900 push scenes/examples/todo --open`；从自检界面"场景"区块打开两者 | 都安装成功，推送输出显示声明的域名；列表点击即打开 |
| 示例功能与断电保留（9.4.2） | 逐个打开 7 个示例；在待办加 3 条、记账加 2 条后拔电池和 USB 断电重启 | 功能正常；重启后数据仍在 |
| 返回手势与错误面板（9.4.3） | 圆角屏左边缘向右滑；推送 `tests/scene/fixtures/broken` 并打开，点"返回"，再打开点"重载" | 手势可用；错误面板显示文件与行号，返回与重载正常 |
| 长列表帧率与内存（9.4.4） | 待办放 200 条，手指持续滚动 10 s，看 `WISP_PERF`；同时执行 `mem`；记下日志里 `slow message` 的最大耗时（GC 停顿） | fps 不低于自检界面基准（"性能基准"一节）；`internal free` ≥ 60 KB、`largest` ≥ 20 KB（`ram_budget.hpp` 的空闲下限） |
| 熄屏恢复与待机耗电（9.4.5） | 番茄钟和时钟运行时熄屏 3 分钟再亮屏；按第 28 行（"验收第 5 条"）的方法测待机电流，对比无场景时 | 剩余时间与表盘正确，`minute` 只补一次；待机电流与无场景时相当 |
| 未声明域名被拒（9.4.6） | 临时把天气场景的 URL 改成未声明的主机后推送打开 | 回调收到 `permission_denied`，没有出站连接 |
| LVGL 堆在 PSRAM 后的帧率（细化第 2 条） | 不开场景，在最终固件上重复"UI 帧率基准"一行，不开热点与开热点各测一次（Task 2 之后已在关口测过一次） | `WISP_PERF` 的 fps 不开热点 ≥ 22.7、开热点 ≥ 28.2（"性能基准"一节）；低于时停下来报告，不要自行改回 |

- [ ] **Step 2: 内部 RAM 实测步骤**（写进"内部 RAM"一节，交给用户执行并填表）：分别在 (a) 开机空闲无场景、(b) 打开待办并滚动 200 条、(c) 天气场景发出 HTTPS 请求期间、(d) STA + 热点 + 网页日志页 + 上传一个场景 zip 时执行 `mem`，记录 `internal free`/`largest`/`min` 与 `wisp.scene`（应在 `psram`）、`wisp.fs`（应在 `internal`）、`wisp.shttp`（应在 `psram`）的栈大小与 high-water mark；与 (a) 相比的内部 RAM 增量对照 spec 2.3 的 ≤ 2 KB 预计与细化第 1 条的说明；三个任务的栈按 high-water 加余量调整后改代码与 `docs/architecture.md`。
- [ ] **Step 3: 交给用户执行** Step 1 与 Step 2，逐项记录结果；不通过的项回到对应任务，先在模拟器里写出能复现的失败测试再修。
- [ ] **Step 4: 验证** — `bash tools/ci.sh`；固件 `idf.py build`。
- [ ] **Step 5: Commit** — `docs(hardware): add the scene runtime acceptance checklist and RAM checks`
