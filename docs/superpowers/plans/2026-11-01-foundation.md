# Wisp 子项目 1：基础骨架 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 建立 Wisp 的可移植分层骨架，在 PC 模拟器和 Waveshare ESP32-S3-Touch-AMOLED-2.16 上跑起同一套自检界面。

**Architecture:** 可移植层（`wisp_base`/`wisp_os`/`wisp_hal`/`wisp_core`/`wisp_ui`/`wisp_app`）只依赖 C++20、LVGL、ArduinoJson；`wisp_port_sim`（std::thread + SDL2）与 `wisp_port_esp`（FreeRTOS + ESP-IDF）实现 OS 抽象与设备；板级代码把设备注册进 `DeviceRegistry`，`wisp_app` 负责启动编排和自检界面。

**Tech Stack:** ESP-IDF v6.0.3、LVGL 9.5.0、ArduinoJson 7.4.3、GoogleTest 1.17.0、SDL2、esp_board_manager 0.7.x、esp_codec_dev 2.x、esp_lcd_co5300、joltwallet/littlefs、CMake ≥ 3.24、Ninja、Python 3。

**Spec:** `docs/superpowers/specs/2026-11-01-foundation-design.md`

## 相对 spec 的细化（实施前已核实，需用户确认）

1. 新增两个可移植组件：`wisp_base`（`Result`、`ErrorCode`、`Value`/`Properties`、JSON 互转，位于 os/hal 之下）和 `wisp_app`（启动编排 + 自检界面，位于 ui 之上）。spec 的 `wisp_ui` 只保留 LVGL 运行时。
2. 触摸芯片实际为 **CST9217**（MyOS 代码与 Waveshare 官方 BSP 均如此，MyOS 硬件文档写的 CST9220 有误），驱动用 `waveshare/esp_lcd_touch_cst9217`。
3. SD 卡按 Waveshare 官方 BSP 走 **1-bit SDMMC**（CLK=GPIO2、CMD=GPIO1、D0=GPIO3），不是 spec 写的 SPI。
4. 屏幕、触摸、音频编解码器、SD 卡由 esp_board_manager 的设备初始化（YAML 参照 ESP-Claw 中已验证的同款板定义，Apache-2.0，需标注来源）；IMU、PMU、RTC、按键、内置 LittleFS 由 Wisp 自己的驱动初始化，挂在 bmgr 提供的 I2C 总线上。
5. `hal::Button` 只提供电平 `pressed()`；短按/长按由可移植的 `ButtonService` 统一判定（长按阈值 1000 ms）。
6. 版本锁定：ESP-IDF **v6.0.3**，LVGL **9.5.0**。
7. 字体：`lv_font_montserrat_16` 为主字体，回退到从 MyOS 移植的 `lv_font_cjk_16`（GB2312 全集，4bpp）。
8. 模拟器截图用自带的极简 PNG 编码器（未压缩 deflate），不引入第三方图像库。
9. 新增性能监视（spec 未要求）：`UiRuntime` 自行统计 FPS、UI 线程占用率、平均刷新耗时；自检界面显示，每 5 s 输出一行 `WISP_PERF <json>`；可选开启 LVGL 自带的 sysmon 浮层。作为后续场景运行时的性能回归基准。
10. 屏幕形状与安全区（执行期由用户批准加入，spec 5.6 节）：Display caps 的 `shape` 取 `rect`/`rounded_rect`/`circle`（原 `rounded_square` 改名为 `rounded_rect`），`wisp_hal` 提供 `visible`/`safe_rect`/`row_span` 几何函数，能力清单为 Display 条目附带 `safe_area`，自检界面与模拟器遮罩都以此为准。见 Task 12b。
11. 熄屏省电（执行期由用户批准加入，spec 6.7 节、验收第 5 条）：新增 `PowerManager`，熄屏进入待机，此时屏幕和触摸进入睡眠、UI 暂停渲染、按键放慢轮询，真机开启 DFS 和待机自动浅睡眠；同时支持闲置自动熄屏。后台场景和唤醒锁留到子项目 3（spec 1.6）。见 Task 18b。

## Global Constraints

- 可移植组件禁止 include：`esp_*`、`freertos/*`、`driver/*`、`hal/`（IDF 的）、`soc/*`、`sdkconfig.h`、`SDL*`（spec 3.2）。
- 语言 C++20；`wisp_*` 目标编译选项 `-fno-exceptions -fno-rtti`；不使用 `dynamic_cast`/`typeid`。
- 命名：组件前缀 `wisp_`，命名空间 `wisp::<层>`，日志宏 `WLOG_D/I/W/E`，头文件路径 `wisp/<层>/<名>.hpp`。
- 像素格式 RGB565；屏幕 480×480，形状 `rounded_rect`，圆角半径 40。界面内容一律放在 `hal::safe_rect()` 算出的安全区内，不得假设屏幕是完整矩形（spec 5.6）。
- 事件队列默认容量 64，满时丢弃最旧并计数。
- 日志环形缓冲 32 KB（真机放 PSRAM）。
- 内置 `library` 配额默认 3 MB（3·1024·1024 字节）。
- 分区表按 spec 7.4 节，偏移和大小一字不差。
- 启动后 3 秒内进入自检界面（真机验收）。
- UI 性能基准：真机上滚动自检列表时帧率 ≥ 30 fps（由 `UiRuntime::perf()` 和串口 `WISP_PERF` 行度量）。（执行期裁定，用户 2026-11-04：≥ 30 fps 仍是目标；Waveshare 2.16 达不到，验收线改为不低于记下的基准——关热点 22.7 fps、开热点 28.2 fps，见 spec 第 10 节"已记录的偏差"。）
- Settings 的 namespace 与 key 长度均 ≤ 15 字符（NVS 限制，两端一致）。
- 所有外部依赖锁定版本；固件提交 `firmware/dependencies.lock`。
- 许可证 Apache-2.0；移植或参考的代码在文件头注明来源，并登记到 `THIRD_PARTY_NOTICES.md` / `docs/references.md`。
- 提交信息结尾附 `Co-Authored-By` 行，按提交者（控制方或子 agent）当前环境的署名要求填写（AGENTS.md 第 7 节）。

## Review Focus

1. **FAT 上的原子写**：目标文件已存在时 FatFS 的 `rename` 会失败；`write_atomic` 必须"写 `.tmp` → fsync → 改名为 `.new` → 删除目标 → `.new` 改名为目标"，并且 `read_file` 在目标缺失、`.new` 存在时自动恢复（`.tmp` 视为未完成写入，丢弃）。（执行期裁定：原方案会把首次写入中途断电留下的半截 `.tmp` 当成有效数据。）测试见 Task 9。
2. **刷新区域越界**：`align_area` 在区域贴着右/下边缘（x2=479）且对齐为 2 时，不能扩到 480。测试见 Task 10。
3. **事件处理函数里再发布事件**：不能死锁，也不能乱序。测试见 Task 5。
4. **电源键长按重复**：一次按住只触发一次 `long`；确认框已打开时再次长按不能叠加第二个确认框；息屏时长按先亮屏再弹框。测试见 Task 7、Task 12。
5. **配置键超长**：namespace 或 key 超过 15 字符时，模拟器和真机都返回 `InvalidArgument`，不能出现"模拟器正常、真机失败"。测试见 Task 8。

---

## 文件结构总览

```
wisp/
├── .gitignore  .editorconfig  .clang-format  LICENSE  README.md  THIRD_PARTY_NOTICES.md
├── cmake/
│   ├── wisp_component.cmake      # 同一份组件 CMakeLists 兼容 idf.py 与普通 CMake
│   └── wisp_host_deps.cmake      # FetchContent：googletest、ArduinoJson、LVGL
├── components/
│   ├── wisp_base/   include/wisp/base/{error,result,value,json}.hpp  src/{error,value,json}.cpp
│   ├── wisp_os/     include/wisp/os/os.hpp  include/wisp/os/queue.hpp
│   ├── wisp_hal/    include/wisp/hal/{device,display,display_geometry,touch,imu,power,clock,button,audio,storage}.hpp  src/{device,display_geometry}.cpp
│   ├── wisp_core/   include/wisp/core/{log,event_bus,device_registry,boot_report,button_service,settings,storage_manager}.hpp  src/*.cpp
│   ├── wisp_ui/     include/wisp/ui/{ui_runtime,align}.hpp  src/*.cpp  config/lv_conf.h  fonts/lv_font_cjk_16.c
│   ├── wisp_app/    include/wisp/app/{board,app,power_key_controller,format}.hpp  src/{app,self_check_screen,power_key_controller,format,loopback}.cpp
│   ├── wisp_port_sim/  os/*.cpp  settings_json_file.cpp  log_stdout.cpp  dev/*.cpp  sim_board.cpp  png_writer.cpp
│   └── wisp_port_esp/  os/*.cpp  log_esp.cpp  settings_nvs.cpp  dev/*.cpp
├── drivers/
│   ├── axp2101/  pcf85063/  qmi8658/           # 从 MyOS 移植，ESP-IDF 组件
├── boards/
│   └── waveshare_esp32_s3_touch_amoled_2_16/  board_info.yaml  board_peripherals.yaml  board_devices.yaml  setup_device.c  board.cpp  README.md
├── firmware/   CMakeLists.txt  main/{CMakeLists.txt,idf_component.yml,main.cpp}  sdkconfig.defaults  partitions.csv
├── simulator/  CMakeLists.txt  main.cpp  sim_options.{hpp,cpp}
├── tests/      CMakeLists.txt  base/ os/ core/ ui/ app/ sim/ smoke/sim_smoke.py
├── tools/      check_layering.py  ci.sh
├── .github/workflows/ci.yml
└── docs/       architecture.md  porting-guide.md  references.md  hardware/waveshare_esp32_s3_touch_amoled_2_16.md
```

PC 构建统一入口：`cmake -S simulator -B build/sim -G Ninja && cmake --build build/sim && ctest --test-dir build/sim --output-on-failure`。

---

### Task 1: 仓库脚手架、双构建适配与 `Result`

**Files:**
- Create: `.gitignore`、`.editorconfig`、`.clang-format`、`LICENSE`
- Create: `cmake/wisp_component.cmake`、`cmake/wisp_host_deps.cmake`
- Create: `simulator/CMakeLists.txt`、`tests/CMakeLists.txt`
- Create: `components/wisp_base/CMakeLists.txt`、`components/wisp_base/include/wisp/base/error.hpp`、`components/wisp_base/include/wisp/base/result.hpp`、`components/wisp_base/src/error.cpp`
- Test: `tests/base/result_test.cpp`

**Interfaces:**
- Produces:
  - CMake 函数 `wisp_component(NAME <n> SRCS ... INCLUDE_DIRS ... [REQUIRES <wisp组件>...] [IDF_REQUIRES ...] [HOST_LIBS ...])`：`if(ESP_PLATFORM)` 时调用 `idf_component_register(SRCS INCLUDE_DIRS REQUIRES ${REQUIRES} ${IDF_REQUIRES})`；否则 `add_library(<n> STATIC)`（无源文件时用 `INTERFACE`），`target_include_directories(PUBLIC)`，`target_link_libraries(PUBLIC ${REQUIRES} ${HOST_LIBS})`，并加上 `-fno-exceptions -fno-rtti` 和 `cxx_std_20`。
  - `enum class wisp::ErrorCode { Ok, InvalidArgument, NotFound, AlreadyExists, Timeout, IoError, NoMemory, Unsupported, StorageUnavailable, QuotaExceeded, DeviceFailure, Busy }`
  - `const char* wisp::to_string(ErrorCode)`，返回 snake_case：`"ok"`、`"invalid_argument"`、…、`"quota_exceeded"`、`"device_failure"`、`"busy"`
  - `struct wisp::Error { ErrorCode code; std::string message; }`
  - `template<class T> class wisp::Result`：`static Result ok(T)`、`static Result err(ErrorCode, std::string msg = {})`、`bool is_ok() const`、`explicit operator bool() const`、`T& value()`、`const T& value() const`、`T value_or(T) const`、`const Error& error() const`
  - `template<> class wisp::Result<void>`：`static Result ok()`、`err(...)`、`is_ok()`、`operator bool`、`error()`
  - 宏 `WISP_TRY(expr)`：表达式结果为错误时直接 `return` 该错误（用于返回 `Result<U>` 的函数，转换为同错误码与消息）

- [ ] **Step 1: 写仓库基础文件**

`.gitignore` 至少包含：`build/`、`firmware/build/`、`firmware/managed_components/`、`firmware/components/gen_bmgr_codes/`、`firmware/sdkconfig`、`firmware/sdkconfig.old`、`sim-data/`、`*.png`（`docs/` 下除外：`!docs/**/*.png`）、`compile_commands.json`、`.cache/`。`LICENSE` 为 Apache-2.0 全文，版权行 `Copyright 2026 ShouChen`。`.clang-format`：`BasedOnStyle: Google`、`IndentWidth: 4`、`ColumnLimit: 100`。`.editorconfig`：UTF-8、LF、4 空格、末尾换行。

- [ ] **Step 2: 写 `cmake/wisp_host_deps.cmake`**

用 FetchContent 锁定：`googletest` tag `v1.17.0`；`ArduinoJson` tag `v7.4.3`（提供 `ArduinoJson` 目标）；`lvgl` tag `v9.5.0`，在 `FetchContent_MakeAvailable` 之前设置 `LV_CONF_PATH` 为 `${WISP_ROOT}/components/wisp_ui/config/lv_conf.h`（LVGL 在 Task 10 才被用到，此处先声明，`lv_conf.h` 在 Task 10 创建前用 `if(EXISTS)` 跳过 LVGL）。

- [ ] **Step 3: 写 `simulator/CMakeLists.txt` 与 `tests/CMakeLists.txt`**

`simulator/CMakeLists.txt`：`cmake_minimum_required(VERSION 3.24)`，`project(wisp_host CXX C)`，`set(WISP_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)`，`CMAKE_EXPORT_COMPILE_COMMANDS ON`，include 两个 cmake 文件，`add_subdirectory(${WISP_ROOT}/components/wisp_base wisp_base)`，`option(WISP_BUILD_TESTS ON)` 时 `enable_testing()` 并 `add_subdirectory(${WISP_ROOT}/tests tests)`。`tests/CMakeLists.txt` 定义函数 `wisp_test(NAME SRCS LIBS)`：`add_executable` + 链接 `GTest::gtest_main` 与 LIBS + `gtest_discover_tests`。后续任务往这两个文件里追加 `add_subdirectory` 和 `wisp_test` 即可。

- [ ] **Step 4: 写失败的测试 `tests/base/result_test.cpp`**

```cpp
TEST(Result, OkHoldsValue) {
    auto r = wisp::Result<int>::ok(42);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 42);
}
TEST(Result, ErrCarriesCodeAndMessage) {
    auto r = wisp::Result<int>::err(wisp::ErrorCode::QuotaExceeded, "full");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, wisp::ErrorCode::QuotaExceeded);
    EXPECT_EQ(r.error().message, "full");
    EXPECT_EQ(r.value_or(7), 7);
}
TEST(Result, VoidSpecialization) {
    EXPECT_TRUE(wisp::Result<void>::ok());
    EXPECT_FALSE(wisp::Result<void>::err(wisp::ErrorCode::Timeout));
}
TEST(Result, TryPropagates) {
    auto inner = [] { return wisp::Result<int>::err(wisp::ErrorCode::IoError, "disk"); };
    auto outer = [&]() -> wisp::Result<void> { WISP_TRY(inner()); return wisp::Result<void>::ok(); };
    auto r = outer();
    EXPECT_EQ(r.error().code, wisp::ErrorCode::IoError);
    EXPECT_EQ(r.error().message, "disk");
}
TEST(ErrorCode, SnakeCaseNames) {
    EXPECT_STREQ(wisp::to_string(wisp::ErrorCode::StorageUnavailable), "storage_unavailable");
    EXPECT_STREQ(wisp::to_string(wisp::ErrorCode::InvalidArgument), "invalid_argument");
}
```

- [ ] **Step 5: 运行，确认失败**

Run: `cmake -S simulator -B build/sim -G Ninja && cmake --build build/sim`
Expected: 编译失败，提示找不到 `wisp/base/result.hpp`。

- [ ] **Step 6: 实现 `error.hpp`/`error.cpp`/`result.hpp`，写 `components/wisp_base/CMakeLists.txt`（`wisp_component(NAME wisp_base SRCS src/error.cpp INCLUDE_DIRS include)`）**

`Result<T>` 内部用 `std::variant<T, Error>`。

- [ ] **Step 7: 运行，确认通过**

Run: `cmake --build build/sim && ctest --test-dir build/sim --output-on-failure`
Expected: 5 个测试全部 PASS。

- [ ] **Step 8: Commit**

```bash
git add .gitignore .editorconfig .clang-format LICENSE cmake simulator tests components/wisp_base
git commit -m "build: scaffold dual-target build and add Result/ErrorCode"
```

---

### Task 2: `Value`/`Properties` 与 JSON 互转

**Files:**
- Create: `components/wisp_base/include/wisp/base/value.hpp`、`components/wisp_base/include/wisp/base/json.hpp`、`components/wisp_base/src/value.cpp`、`components/wisp_base/src/json.cpp`
- Modify: `components/wisp_base/CMakeLists.txt`（加入新源文件；`IDF_REQUIRES bblanchon__arduinojson`，`HOST_LIBS ArduinoJson`）
- Test: `tests/base/value_json_test.cpp`

**Interfaces:**
- Consumes: Task 1 的 `Result`、`ErrorCode`
- Produces:
  - `class wisp::Value`，类型枚举 `Value::Type { Null, Bool, Int, Double, String, List, Map }`
  - `using wisp::List = std::vector<Value>; using wisp::Map = std::map<std::string, Value>; using wisp::Properties = Map;`
  - 构造：`Value()`、`Value(bool)`、`Value(int)`、`Value(int64_t)`、`Value(double)`、`Value(const char*)`、`Value(std::string)`、`Value(List)`、`Value(Map)`
  - 访问：`Type type() const`、`is_null()`、`as_bool()`、`as_int()`、`as_double()`（Int 也可读为 double）、`as_string()`、`as_list()`、`as_map()`、`const Value* find(std::string_view key) const`（非 Map 或不存在返回 nullptr）、`bool operator==(const Value&) const`
  - `std::string wisp::to_json(const Value&)`：紧凑格式，无空格，Map 键按字典序（`std::map` 天然有序）
  - `wisp::Result<Value> wisp::from_json(std::string_view)`：解析失败返回 `InvalidArgument`，消息包含 ArduinoJson 错误文本

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(Json, RoundTripNested) {
    wisp::Value v = wisp::Map{{"b", wisp::List{1, 2.5, "x", true, wisp::Value()}}, {"a", wisp::Map{{"k", "v"}}}};
    auto s = wisp::to_json(v);
    EXPECT_EQ(s, R"({"a":{"k":"v"},"b":[1,2.5,"x",true,null]})");
    auto back = wisp::from_json(s);
    ASSERT_TRUE(back);
    EXPECT_EQ(back.value(), v);
}
TEST(Json, IntAndDoubleStayDistinct) {
    auto v = wisp::from_json(R"({"i":1,"d":1.5})").value();
    EXPECT_EQ(v.find("i")->type(), wisp::Value::Type::Int);
    EXPECT_EQ(v.find("d")->type(), wisp::Value::Type::Double);
}
TEST(Json, LargeInt64) {
    wisp::Value v = int64_t{9007199254740993};
    EXPECT_EQ(wisp::from_json(wisp::to_json(v)).value().as_int(), 9007199254740993);
}
TEST(Json, EscapesStrings) {
    EXPECT_EQ(wisp::to_json(wisp::Value("a\"b\n中")), "\"a\\\"b\\n中\"");
}
TEST(Json, InvalidInputIsError) {
    auto r = wisp::from_json("{bad");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, wisp::ErrorCode::InvalidArgument);
}
TEST(Value, FindOnNonMapIsNull) { EXPECT_EQ(wisp::Value(3).find("x"), nullptr); }
```

- [ ] **Step 2: 运行，确认失败**

Run: `cmake --build build/sim`
Expected: 编译失败，找不到 `wisp/base/value.hpp`。

- [ ] **Step 3: 实现**

`Value` 内部存储用 `std::variant<std::monostate, bool, int64_t, double, std::string, std::shared_ptr<List>, std::shared_ptr<Map>>`，按值语义拷贝（拷贝时深拷贝 List/Map）。JSON 转换用 ArduinoJson 的 `JsonDocument`：序列化时整数走 `int64_t`、浮点走 `double`；反序列化时 `is<int64_t>()` 优先于 `is<double>()`。

- [ ] **Step 4: 运行，确认通过**

Run: `cmake --build build/sim && ctest --test-dir build/sim --output-on-failure -R "Json|Value"`
Expected: 全部 PASS。

- [ ] **Step 5: Commit**

```bash
git add components/wisp_base tests
git commit -m "feat(base): add Value/Properties with JSON conversion"
```

---

### Task 3: OS 抽象与模拟器实现

**Files:**
- Create: `components/wisp_os/CMakeLists.txt`、`components/wisp_os/include/wisp/os/os.hpp`、`components/wisp_os/include/wisp/os/queue.hpp`
- Create: `components/wisp_port_sim/CMakeLists.txt`、`components/wisp_port_sim/os/os_sim.cpp`
- Modify: `simulator/CMakeLists.txt`（`add_subdirectory` wisp_os、wisp_port_sim）
- Test: `tests/os/os_test.cpp`

**Interfaces:**
- Produces（`namespace wisp::os`，声明在 `os.hpp`，实现由 port 在链接期提供）：
  - `uint64_t now_ms();`、`void sleep_ms(uint32_t ms);`
  - `constexpr uint32_t kForever = UINT32_MAX;`
  - `enum class Mem { Fast, Large, Dma };`、`void* alloc(size_t bytes, Mem kind);`、`void dealloc(void* p);`（不叫 `free`，避免与 `::free` 在 `using namespace` 下二义）
  - `class Mutex`：`lock()`、`unlock()`、`bool try_lock()`（满足 BasicLockable，可用于 `std::lock_guard`）；PIMPL：`struct Impl; std::unique_ptr<Impl> impl_;`
  - `class Semaphore`：`Semaphore(uint32_t max_count, uint32_t initial)`、`bool take(uint32_t timeout_ms)`、`void give()`
  - `struct TaskConfig { const char* name = "wisp"; size_t stack_bytes = 4096; int priority = 5; int core = -1; };`
  - `class Task`：`static std::unique_ptr<Task> spawn(const TaskConfig&, std::function<void()> fn)`、`void join()`（等待 fn 返回；析构前必须已 join 或 fn 已结束）、`bool is_current() const`（调用者是否就在这个任务里运行）
  - `class Timer`：`Timer(const char* name, uint32_t period_ms, bool periodic, std::function<void()> cb)`、`void start()`、`void stop()`、`bool active() const`；析构时自动 stop 并保证回调不再运行
  - `queue.hpp`（纯头文件、可移植，基于 `Mutex` + 两个 `Semaphore` 的环形缓冲）：`template<class T> class Queue { explicit Queue(size_t capacity); bool push(T item, uint32_t timeout_ms); bool push_drop_oldest(T item);  /* 满时丢弃最旧，返回 true 表示发生丢弃 */ bool pop(T& out, uint32_t timeout_ms); size_t size() const; size_t capacity() const; };`
- 库目标：主机侧 `wisp_port_sim` 拆成 `wisp_port_sim_os`（无 SDL，供单元测试链接）和 `wisp_port_sim_dev`（Task 13 加入）。

- [ ] **Step 1: 写失败的测试**

```cpp
using namespace wisp::os;
TEST(Os, ClockIsMonotonic) { auto a = now_ms(); sleep_ms(20); EXPECT_GE(now_ms() - a, 19u); }
TEST(Os, SemaphoreTimeout) {
    Semaphore s(1, 0);
    auto t0 = now_ms(); EXPECT_FALSE(s.take(50)); EXPECT_GE(now_ms() - t0, 45u);
    s.give(); EXPECT_TRUE(s.take(0));
}
TEST(Os, TaskRunsAndJoins) {
    std::atomic<int> x{0};
    auto t = Task::spawn({.name = "t"}, [&] { x = 1; });
    t->join(); EXPECT_EQ(x, 1);
}
TEST(Os, TaskIsCurrent) {
    std::atomic<bool> inside{false};
    std::unique_ptr<Task> t;
    Semaphore ready(1, 0);
    t = Task::spawn({}, [&] { ready.take(kForever); inside = t->is_current(); });
    ready.give(); t->join();
    EXPECT_TRUE(inside); EXPECT_FALSE(t->is_current());
}
TEST(Os, PeriodicTimerFiresRepeatedly) {
    std::atomic<int> n{0};
    Timer tm("tm", 10, true, [&] { ++n; });
    tm.start(); sleep_ms(105); tm.stop();
    int seen = n; EXPECT_GE(seen, 8); sleep_ms(30); EXPECT_EQ(n, seen);
}
TEST(Queue, FifoAndTimeout) {
    Queue<std::string> q(2);
    EXPECT_TRUE(q.push("a", 0)); EXPECT_TRUE(q.push("b", 0)); EXPECT_FALSE(q.push("c", 10));
    std::string s; EXPECT_TRUE(q.pop(s, 0)); EXPECT_EQ(s, "a");
}
TEST(Queue, DropOldestWhenFull) {
    Queue<int> q(2); q.push(1, 0); q.push(2, 0);
    EXPECT_TRUE(q.push_drop_oldest(3));
    int v; q.pop(v, 0); EXPECT_EQ(v, 2); q.pop(v, 0); EXPECT_EQ(v, 3);
}
TEST(Os, AllocKinds) { for (auto m : {Mem::Fast, Mem::Large, Mem::Dma}) { void* p = alloc(64, m); ASSERT_NE(p, nullptr); dealloc(p); } }
```

- [ ] **Step 2: 运行，确认失败**

Run: `cmake --build build/sim`
Expected: 编译失败，找不到 `wisp/os/os.hpp`。

- [ ] **Step 3: 实现 `os.hpp`、`queue.hpp` 与 `os_sim.cpp`**

模拟器实现：`std::mutex`、`std::condition_variable`、`std::thread`、`std::chrono::steady_clock`；`Timer` 用一个专属线程加条件变量等待；`alloc` 直接 `std::malloc`，忽略 kind。

- [ ] **Step 4: 运行，确认通过**

Run: `cmake --build build/sim && ctest --test-dir build/sim --output-on-failure -R "Os|Queue"`
Expected: 全部 PASS。

- [ ] **Step 5: Commit**

```bash
git add components/wisp_os components/wisp_port_sim simulator tests
git commit -m "feat(os): add OS abstraction with simulator backend"
```

---

### Task 4: 日志与环形缓冲

**Files:**
- Create: `components/wisp_core/CMakeLists.txt`、`components/wisp_core/include/wisp/core/log.hpp`、`components/wisp_core/src/log.cpp`
- Create: `components/wisp_port_sim/log_stdout.cpp`
- Modify: `simulator/CMakeLists.txt`
- Test: `tests/core/log_test.cpp`

**Interfaces:**
- Consumes: `wisp::os::Mutex`、`os::alloc`、`os::now_ms`
- Produces（`namespace wisp`）：
  - `enum class LogLevel { Debug, Info, Warn, Error };`
  - `class LogSink { public: virtual ~LogSink() = default; virtual void write(LogLevel, const char* tag, const char* msg) = 0; };`
  - `namespace log`：`void init(size_t ring_bytes = 32 * 1024, os::Mem mem = os::Mem::Large);`、`void add_sink(LogSink*);`、`void set_level(LogLevel);`、`void write(LogLevel, const char* tag, const char* fmt, ...);`、`std::string ring_snapshot();`、`void reset_for_test();`
  - 宏 `WLOG_D/I/W/E(tag, fmt, ...)` → `::wisp::log::write(::wisp::LogLevel::X, tag, fmt, ##__VA_ARGS__)`
  - 环形缓冲每行格式：`"<ts_ms> <D|I|W|E> <tag>: <msg>\n"`；单条消息格式化后截断到 255 字节；空间不足时按整行丢弃最旧的行
  - port_sim 提供 `wisp::port::sim::StdoutLogSink`（写 stdout，每行与环形缓冲格式相同）

- [ ] **Step 1: 写失败的测试**

```cpp
struct CaptureSink : wisp::LogSink { std::vector<std::string> lines;
    void write(wisp::LogLevel, const char* tag, const char* msg) override { lines.push_back(std::string(tag) + "|" + msg); } };
TEST(Log, FiltersByLevelAndFormats) {
    wisp::log::reset_for_test(); wisp::log::init(1024); CaptureSink s; wisp::log::add_sink(&s);
    wisp::log::set_level(wisp::LogLevel::Info);
    WLOG_D("t", "hidden"); WLOG_I("t", "v=%d", 3);
    ASSERT_EQ(s.lines.size(), 1u); EXPECT_EQ(s.lines[0], "t|v=3");
    EXPECT_NE(wisp::log::ring_snapshot().find(" I t: v=3\n"), std::string::npos);
}
TEST(Log, RingDropsOldestWholeLines) {
    wisp::log::reset_for_test(); wisp::log::init(200);
    for (int i = 0; i < 50; ++i) WLOG_I("t", "line-%02d", i);
    auto snap = wisp::log::ring_snapshot();
    EXPECT_LE(snap.size(), 200u);
    EXPECT_NE(snap.find("line-49"), std::string::npos);
    EXPECT_EQ(snap.find("line-00"), std::string::npos);
    EXPECT_EQ(snap.back(), '\n');
    EXPECT_TRUE(isdigit(static_cast<unsigned char>(snap.front())));  // 首行完整
}
TEST(Log, TruncatesLongMessages) {
    wisp::log::reset_for_test(); wisp::log::init(4096); CaptureSink s; wisp::log::add_sink(&s);
    WLOG_W("t", "%s", std::string(1000, 'x').c_str());
    EXPECT_EQ(s.lines[0].size(), 2u + 255u);
}
```

- [ ] **Step 2: 运行，确认失败**

Run: `cmake --build build/sim` → Expected: 找不到 `wisp/core/log.hpp`。

- [ ] **Step 3: 实现 `log.cpp` 与 `log_stdout.cpp`**（全局状态由 `os::Mutex` 保护；sink 调用在锁外进行，避免 sink 内再打日志造成死锁）

- [ ] **Step 4: 运行，确认通过**

Run: `cmake --build build/sim && ctest --test-dir build/sim -R Log --output-on-failure` → Expected: PASS。

- [ ] **Step 5: Commit**

```bash
git add components/wisp_core components/wisp_port_sim simulator tests
git commit -m "feat(core): add logging with in-memory ring buffer"
```

---

### Task 5: 事件总线

**Files:**
- Create: `components/wisp_core/include/wisp/core/event_bus.hpp`、`components/wisp_core/src/event_bus.cpp`
- Test: `tests/core/event_bus_test.cpp`

**Interfaces:**
- Consumes: `os::Queue`、`os::Task`、`os::Mutex`、`os::Semaphore`、`wisp::Properties`、`WLOG_W`
- Produces（`namespace wisp::core`）：
  - `struct Event { std::string topic; std::string source; uint64_t ts_ms = 0; Properties payload; };`（`ts_ms` 为 0 时 `publish` 自动填 `os::now_ms()`）
  - `using EventHandler = std::function<void(const Event&)>; using SubscriptionId = uint32_t;`
  - `class EventBus { explicit EventBus(size_t capacity = 64); ~EventBus(); Result<void> start(); void stop(); SubscriptionId subscribe(std::string prefix, EventHandler); void unsubscribe(SubscriptionId); void publish(Event e); bool wait_idle(uint32_t timeout_ms); uint64_t dropped() const; };`
  - 语义：前缀为空串匹配所有主题；按发布顺序派发；同一事件的多个订阅者按订阅顺序调用；`publish` 不阻塞（满时 `push_drop_oldest`，`dropped()` 加一并打 `WLOG_W`）；处理函数里调用 `publish`、`subscribe`、`unsubscribe` 都安全；`wait_idle` 等到队列为空且没有处理函数在运行。

- [ ] **Step 1: 写失败的测试**

```cpp
using wisp::core::EventBus; using wisp::core::Event;
TEST(EventBus, PrefixMatching) {
    EventBus bus; bus.start(); std::vector<std::string> got; wisp::os::Mutex m;
    bus.subscribe("button.", [&](const Event& e) { std::lock_guard g(m); got.push_back(e.topic); });
    bus.publish({.topic = "button.power.short"}); bus.publish({.topic = "buttons.x"}); bus.publish({.topic = "power.low"});
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(got, std::vector<std::string>{"button.power.short"});
}
TEST(EventBus, EmptyPrefixMatchesAllInOrder) {
    EventBus bus; bus.start(); std::vector<int> got;
    bus.subscribe("", [&](const Event& e) { got.push_back(e.payload.at("i").as_int()); });
    for (int i = 0; i < 20; ++i) bus.publish({.topic = "t", .payload = {{"i", i}}});
    ASSERT_TRUE(bus.wait_idle(1000));
    for (int i = 0; i < 20; ++i) EXPECT_EQ(got[i], i);
}
TEST(EventBus, PublishFromHandlerDoesNotDeadlock) {
    EventBus bus; bus.start(); std::vector<std::string> got;
    bus.subscribe("a", [&](const Event&) { bus.publish({.topic = "b"}); });
    bus.subscribe("", [&](const Event& e) { got.push_back(e.topic); });
    bus.publish({.topic = "a"});
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(got, (std::vector<std::string>{"a", "b"}));
}
TEST(EventBus, Unsubscribe) {
    EventBus bus; bus.start(); int n = 0;
    auto id = bus.subscribe("x", [&](const Event&) { ++n; });
    bus.publish({.topic = "x"}); bus.wait_idle(1000); bus.unsubscribe(id);
    bus.publish({.topic = "x"}); bus.wait_idle(1000); EXPECT_EQ(n, 1);
}
TEST(EventBus, OverflowDropsOldestAndCounts) {
    EventBus bus(4);  // 不 start：事件只入队
    for (int i = 0; i < 10; ++i) bus.publish({.topic = "t", .payload = {{"i", i}}});
    EXPECT_EQ(bus.dropped(), 6u);
    std::vector<int> got; bus.subscribe("", [&](const Event& e) { got.push_back(e.payload.at("i").as_int()); });
    bus.start(); bus.wait_idle(1000);
    EXPECT_EQ(got, (std::vector<int>{6, 7, 8, 9}));
}
TEST(EventBus, FillsTimestamp) {
    EventBus bus; bus.start(); uint64_t ts = 0;
    bus.subscribe("", [&](const Event& e) { ts = e.ts_ms; });
    bus.publish({.topic = "t"}); bus.wait_idle(1000); EXPECT_GT(ts, 0u);
}
```

- [ ] **Step 2: 运行，确认失败** — Run: `cmake --build build/sim` → Expected: 找不到 `event_bus.hpp`。

- [ ] **Step 3: 实现**：派发任务名 `"wisp.bus"`、栈 6144、优先级 5；订阅表用 `os::Mutex` 保护，派发时先复制匹配到的处理函数列表，再在锁外调用。

- [ ] **Step 4: 运行，确认通过** — Run: `cmake --build build/sim && ctest --test-dir build/sim -R EventBus --output-on-failure` → Expected: PASS。

- [ ] **Step 5: Commit**

```bash
git add components/wisp_core tests
git commit -m "feat(core): add async prefix-matching event bus"
```

---

### Task 6: HAL 接口、设备注册表与启动报告

**Files:**
- Create: `components/wisp_hal/CMakeLists.txt`、`components/wisp_hal/include/wisp/hal/{device,display,touch,imu,power,clock,button,audio,storage}.hpp`、`components/wisp_hal/src/device.cpp`
- Create: `components/wisp_core/include/wisp/core/{device_registry,boot_report}.hpp`、`components/wisp_core/src/{device_registry,boot_report}.cpp`
- Create: `tests/support/fake_devices.hpp`（供后续测试复用的假设备）
- Test: `tests/core/device_registry_test.cpp`

**Interfaces:**
- Produces（`namespace wisp::hal`）：
  - `enum class DeviceClass { Display, Touch, Imu, Power, Clock, Button, AudioOut, AudioIn, Storage };`；`const char* to_string(DeviceClass)` → `"display","touch","imu","power","clock","button","audio_out","audio_in","storage"`；`std::optional<DeviceClass> class_from_string(std::string_view)`
  - `struct DeviceInfo { std::string id; DeviceClass cls; std::string model; Properties caps; };`
  - `class Device { public: virtual ~Device() = default; virtual const DeviceInfo& info() const = 0; };`
  - 每个接口都声明 `static constexpr DeviceClass kClass = ...;`
  - `struct Area { int32_t x1, y1, x2, y2; };`（闭区间）
  - `class Display : public Device { virtual Result<void> flush(const Area&, const uint16_t* rgb565) = 0; /* 阻塞直到传输完成 */ virtual Result<void> flush_async(const Area&, const uint16_t*); /* 可在传输中返回，缺省即 flush() */ virtual Result<void> wait_flush(); /* 等 flush_async() 的传输完成，缺省直接返回 */ virtual Result<void> set_brightness(uint8_t level) = 0; virtual Result<void> set_power(bool on) = 0; virtual bool powered() const = 0; };`，caps 键：`width`、`height`、`shape`、`corner_radius`、`panel`、`brightness`、`area_align`、`rgb565_be`（可选，缺省 false：像素高字节在前）
  - `struct TouchPoint { int16_t x, y; uint8_t id; };`，`class Touch : public Device { virtual Result<std::vector<TouchPoint>> read() = 0; };`，caps：`max_points`
  - `struct ImuSample { float ax, ay, az; float gx, gy, gz; uint64_t ts_ms; };`（加速度单位 g，角速度单位 dps），`class Imu : public Device { virtual Result<ImuSample> read() = 0; virtual Result<void> set_rate(uint32_t hz) = 0; };`，caps：`accel`、`gyro`、`magnetometer`、`max_rate_hz`
  - `struct PowerStatus { int battery_percent = -1; uint32_t battery_mv = 0; bool charging = false; bool usb_present = false; };`，`class Power : public Device { virtual Result<PowerStatus> status() = 0; virtual Result<void> power_off() = 0; };`，caps：`battery`、`charging_status`
  - `class Clock : public Device { virtual Result<int64_t> get_unix() = 0; virtual Result<void> set_unix(int64_t seconds) = 0; };`
  - `class Button : public Device { virtual bool pressed() = 0; };`（id 形如 `button.power`、`button.vol_up`、`button.vol_down`）
  - `struct AudioFormat { uint32_t sample_rate = 16000; uint8_t channels = 1; };`
  - `class AudioOut : public Device { virtual Result<void> open(const AudioFormat&) = 0; virtual Result<size_t> write(const int16_t* frames, size_t count) = 0; virtual void close() = 0; virtual Result<void> set_volume(uint8_t percent) = 0; };`
  - `class AudioIn : public Device { virtual Result<void> open(const AudioFormat&) = 0; virtual Result<size_t> read(int16_t* frames, size_t count) = 0; virtual void close() = 0; virtual Result<void> set_gain(float db) = 0; };`
  - `struct Capacity { uint64_t total_bytes = 0; uint64_t free_bytes = 0; };`，`class Storage : public Device { virtual std::string mount_point() const = 0; virtual bool online() = 0; /* 允许探测、重新挂载 */ virtual Result<Capacity> capacity() = 0; };`，caps：`removable`、`fs`
- Produces（`namespace wisp::core`）：
  - `class DeviceRegistry { Result<void> add(std::shared_ptr<hal::Device>); /* 重复 id → AlreadyExists */ Result<void> remove(const std::string& id); /* 不存在 → NotFound */ std::shared_ptr<hal::Device> find(const std::string& id) const; template<class T> std::shared_ptr<T> get(const std::string& id) const; /* 类别不符返回 nullptr，内部 static_pointer_cast */ template<class T> std::vector<std::shared_ptr<T>> list() const; std::vector<std::shared_ptr<hal::Device>> all() const; /* 按 id 排序 */ Value manifest(const std::string& board) const; };`（线程安全）
  - manifest 结构：`{"board": <board>, "devices": [{"id","class","model","caps"}...]}`，devices 按 id 排序
  - `enum class DeviceState { Ok, Degraded, Missing, Failed };`，`const char* to_string(DeviceState)` → `"ok","degraded","missing","failed"`
  - `struct DeviceReport { std::string id; hal::DeviceClass cls; DeviceState state; std::string reason; };`
  - `class BootReport { void record(DeviceReport); void set_boot_ms(uint64_t); void set_note(std::string key, Value v); const std::vector<DeviceReport>& entries() const; std::optional<DeviceReport> find(const std::string& id) const; Value to_value(const std::string& board) const; };`，`to_value` 结构：`{"board", "boot_ms", "devices": [{"id","class","state","reason"}], "notes": {...}}`
- `tests/support/fake_devices.hpp` 提供：`FakeDisplay`（记录 flush 区域、可设 `area_align`、`width/height`）、`FakeImu`（可设下一次 read 的结果或错误）、`FakeButton`（`bool down` 可写）、`FakeStorage(id, dir, removable)`（`online_flag` 可写，`capacity` 根据目录内容计算 used）、`FakePower`、`FakeClock`。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(DeviceRegistry, AddFindGetList) {
    wisp::core::DeviceRegistry reg;
    auto imu = std::make_shared<FakeImu>("imu.main");
    ASSERT_TRUE(reg.add(imu));
    EXPECT_EQ(reg.add(std::make_shared<FakeImu>("imu.main")).error().code, wisp::ErrorCode::AlreadyExists);
    EXPECT_EQ(reg.get<wisp::hal::Imu>("imu.main"), imu);
    EXPECT_EQ(reg.get<wisp::hal::Display>("imu.main"), nullptr);
    EXPECT_EQ(reg.list<wisp::hal::Imu>().size(), 1u);
    EXPECT_EQ(reg.remove("nope").error().code, wisp::ErrorCode::NotFound);
    ASSERT_TRUE(reg.remove("imu.main")); EXPECT_EQ(reg.find("imu.main"), nullptr);
}
TEST(DeviceRegistry, ManifestJson) {
    wisp::core::DeviceRegistry reg;
    reg.add(std::make_shared<FakeImu>("imu.main"));  // caps: accel=true, gyro=true, magnetometer=false, max_rate_hz=1000; model "FAKE"
    EXPECT_EQ(wisp::to_json(reg.manifest("b")),
        R"({"board":"b","devices":[{"caps":{"accel":true,"gyro":true,"magnetometer":false,"max_rate_hz":1000},"class":"imu","id":"imu.main","model":"FAKE"}]})");
}
TEST(BootReport, ToValue) {
    wisp::core::BootReport r; r.set_boot_ms(1234);
    r.record({"imu.main", wisp::hal::DeviceClass::Imu, wisp::core::DeviceState::Failed, "disabled"});
    auto v = r.to_value("b");
    EXPECT_EQ(v.find("boot_ms")->as_int(), 1234);
    auto& d = v.find("devices")->as_list()[0];
    EXPECT_EQ(d.find("state")->as_string(), "failed");
    EXPECT_EQ(d.find("class")->as_string(), "imu");
    EXPECT_EQ(r.find("imu.main")->reason, "disabled");
}
TEST(DeviceClass, StringRoundTrip) {
    EXPECT_EQ(wisp::hal::class_from_string("audio_out"), wisp::hal::DeviceClass::AudioOut);
    EXPECT_FALSE(wisp::hal::class_from_string("gpu").has_value());
}
```

- [ ] **Step 2: 运行，确认失败** — Run: `cmake --build build/sim` → Expected: 找不到 `wisp/hal/device.hpp`。
- [ ] **Step 3: 实现上述接口与 `fake_devices.hpp`**（`wisp_hal` 为 INTERFACE 之外的普通库，仅 `device.cpp` 放 `to_string`/`class_from_string`）
- [ ] **Step 4: 运行，确认通过** — Run: `cmake --build build/sim && ctest --test-dir build/sim -R "DeviceRegistry|BootReport|DeviceClass" --output-on-failure` → Expected: PASS。
- [ ] **Step 5: Commit**

```bash
git add components/wisp_hal components/wisp_core tests
git commit -m "feat(hal,core): add device interfaces, registry and boot report"
```

---

### Task 7: 按键服务（短按/长按判定）

**Files:**
- Create: `components/wisp_core/include/wisp/core/button_service.hpp`、`components/wisp_core/src/button_service.cpp`
- Test: `tests/core/button_service_test.cpp`

**Interfaces:**
- Consumes: `DeviceRegistry::list<hal::Button>()`、`EventBus::publish`、`os::Timer`
- Produces：`class wisp::core::ButtonService { ButtonService(DeviceRegistry&, EventBus&, uint32_t long_press_ms = 1000, uint32_t poll_ms = 20); void start(); /* 内部 Timer 周期调用 poll(os::now_ms()) */ void stop(); void poll(uint64_t now_ms); };`
- 事件：主题 `button.<name>.<down|up|short|long>`，`<name>` 为设备 id 去掉 `button.` 前缀；`source` 为设备 id。规则：按下 → `down`；按住时长达到 `long_press_ms` 时发一次 `long`；松开 → `up`，若未发过 `long` 则再发 `short`。

- [ ] **Step 1: 写失败的测试**（直接调用 `poll` 并传入合成时间，不 start）

```cpp
TEST(ButtonService, ShortPress) {
    Fixture f;  // registry 中有 FakeButton "button.power"；bus 已 start；订阅 "" 收集 topic
    f.btn->down = true;  f.svc.poll(0);
    f.btn->down = false; f.svc.poll(300);
    f.bus.wait_idle(1000);
    EXPECT_EQ(f.topics, (std::vector<std::string>{"button.power.down", "button.power.up", "button.power.short"}));
}
TEST(ButtonService, LongPressFiresOnceWhileHeld) {
    Fixture f;
    f.btn->down = true; f.svc.poll(0); f.svc.poll(999); f.svc.poll(1000); f.svc.poll(1500); f.svc.poll(3000);
    f.btn->down = false; f.svc.poll(3100);
    f.bus.wait_idle(1000);
    EXPECT_EQ(f.topics, (std::vector<std::string>{"button.power.down", "button.power.long", "button.power.up"}));
}
TEST(ButtonService, IndependentButtons) {
    Fixture f; auto up = f.add_button("button.vol_up");
    up->down = true; f.svc.poll(0); up->down = false; f.svc.poll(50);
    f.bus.wait_idle(1000);
    EXPECT_EQ(f.topics.back(), "button.vol_up.short");
}
```

- [ ] **Step 2: 运行，确认失败** — Expected: 找不到 `button_service.hpp`。
- [ ] **Step 3: 实现**（每个按键一份状态：`pressed`、`down_at`、`long_sent`；按键列表在 `start()`/首次 `poll` 时从注册表读取）
- [ ] **Step 4: 运行，确认通过** — Run: `ctest --test-dir build/sim -R ButtonService --output-on-failure` → PASS。
- [ ] **Step 5: Commit**

```bash
git add components/wisp_core tests
git commit -m "feat(core): add button service with short/long press detection"
```

---

### Task 8: 配置存储

**Files:**
- Create: `components/wisp_core/include/wisp/core/settings.hpp`、`components/wisp_core/src/settings.cpp`
- Create: `components/wisp_port_sim/settings_json_file.cpp`、`components/wisp_port_sim/include/wisp/port/sim/settings_json_file.hpp`
- Test: `tests/core/settings_test.cpp`

**Interfaces:**
- Produces（`namespace wisp::core`）：
  - `class SettingsBackend { public: virtual ~SettingsBackend() = default; virtual Result<Value> get(const std::string& ns, const std::string& key) = 0; /* 不存在 → NotFound */ virtual Result<void> set(const std::string& ns, const std::string& key, const Value&) = 0; virtual Result<void> erase(const std::string& ns, const std::string& key) = 0; };`
  - `Result<void> validate_settings_name(std::string_view)`：空串或长度 > 15 → `InvalidArgument`
  - `class Settings { Settings(SettingsBackend&, std::string ns); int64_t get_int(const std::string& key, int64_t def) const; bool get_bool(...); std::string get_string(...); Result<void> set(const std::string& key, const Value&); };`（`get_*` 在不存在、类型不符、名字非法时返回默认值）
- Produces（`namespace wisp::port::sim`）：`class JsonFileSettingsBackend : public core::SettingsBackend { explicit JsonFileSettingsBackend(std::string file_path); };`，文件内容为 `{"<ns>": {"<key>": value}}`，每次 `set`/`erase` 用"写临时文件再改名"整体写回；所有方法先调用 `validate_settings_name` 校验 ns 和 key。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(Settings, DefaultsAndRoundTripPersist) {
    auto path = temp_dir() / "settings.json";
    { wisp::port::sim::JsonFileSettingsBackend be(path.string()); wisp::core::Settings s(be, "display");
      EXPECT_EQ(s.get_int("brightness", 200), 200);
      ASSERT_TRUE(s.set("brightness", 128)); }
    { wisp::port::sim::JsonFileSettingsBackend be(path.string()); wisp::core::Settings s(be, "display");
      EXPECT_EQ(s.get_int("brightness", 200), 128); }
}
TEST(Settings, NameLengthLimit) {
    auto path = temp_dir() / "s2.json";
    wisp::port::sim::JsonFileSettingsBackend be(path.string());
    EXPECT_EQ(be.set("display", "a_key_longer_than_15", 1).error().code, wisp::ErrorCode::InvalidArgument);
    EXPECT_EQ(be.set("namespace_too_long", "k", 1).error().code, wisp::ErrorCode::InvalidArgument);
    EXPECT_TRUE(be.set("display", "exactly15chars_", 1));
}
TEST(Settings, TypeMismatchReturnsDefault) {
    auto path = temp_dir() / "s3.json";
    wisp::port::sim::JsonFileSettingsBackend be(path.string()); wisp::core::Settings s(be, "audio");
    s.set("volume", "loud"); EXPECT_EQ(s.get_int("volume", 60), 60);
}
```

（`temp_dir()` 放进 `tests/support/temp_dir.hpp`：在系统临时目录下创建唯一子目录，测试结束时删除。）

- [ ] **Step 2: 运行，确认失败** — Expected: 找不到 `settings.hpp`。
- [ ] **Step 3: 实现**
- [ ] **Step 4: 运行，确认通过** — Run: `ctest --test-dir build/sim -R Settings --output-on-failure` → PASS。
- [ ] **Step 5: Commit**

```bash
git add components/wisp_core components/wisp_port_sim tests
git commit -m "feat(core): add settings with JSON file backend for simulator"
```

---

### Task 9: 存储管理器

**Files:**
- Create: `components/wisp_core/include/wisp/core/storage_manager.hpp`、`components/wisp_core/src/storage_manager.cpp`
- Test: `tests/core/storage_manager_test.cpp`

**Interfaces:**
- Consumes: `DeviceRegistry::get<hal::Storage>`（固定 id `storage.int`、`storage.sd`）、`EventBus::publish`、`os::Mutex`
- Produces（`namespace wisp::core`）：
  - `enum class Area { System, Library, Backup };`
  - `struct StorageConfig { uint64_t internal_library_quota = 3ull * 1024 * 1024; };`
  - `struct MediaStatus { std::string id; bool online; uint64_t total_bytes; uint64_t free_bytes; };`、`struct StorageStatus { std::vector<MediaStatus> media; };`
  - `class StorageManager { StorageManager(DeviceRegistry&, EventBus&, StorageConfig = {}); Result<void> init(); /* 缺少 storage.int → StorageUnavailable；创建 <int>/system、<int>/library，SD 在线时创建 <sd>/wisp/library、<sd>/wisp/backup */ std::vector<std::string> roots(Area) const; Result<std::string> write_root(Area) const; /* 列表为空 → StorageUnavailable */ Result<void> write_atomic(const std::string& path, std::span<const uint8_t> data); Result<std::vector<uint8_t>> read_file(const std::string& path); void refresh(); /* 探测 SD 在线状态变化 */ StorageStatus status() const; };`
  - 根目录规则（spec 7.2）：`System` = `[<int>/system]`；`Library` = SD 在线时 `[<sd>/wisp/library, <int>/library]`，否则 `[<int>/library]`；`Backup` = SD 在线时 `[<sd>/wisp/backup]`，否则 `[]`。`<int>`/`<sd>` 为设备 `mount_point()`。
  - `write_atomic` 算法：路径以 `<int>/library/` 开头时，先统计该目录现有大小（递归遍历，扣除目标文件旧大小）加上新数据大小，超过配额返回 `QuotaExceeded`；路径位于离线 SD 下返回 `StorageUnavailable`；写入 `<path>.tmp`，`fflush` + `fsync`，`rename(tmp, <path>.new)`，若目标存在则 `remove`，再 `rename(new, path)`；失败返回 `IoError`，消息带 `strerror(errno)`。
  - `read_file`：目标不存在但 `<path>.new` 存在时，先把 `.new` 改名为目标再读取；残留的 `.tmp` 不用于恢复。
  - `refresh`：SD 由离线变在线时创建目录并发布 `storage.sd.inserted`；由在线变离线时发布 `storage.sd.removed`；`source` 为 `storage.sd`。
  - 只使用 `<cstdio>`、`<dirent.h>`、`<sys/stat.h>`、`<unistd.h>`（Linux 与 ESP-IDF newlib 都有），不使用 `std::filesystem`。

- [ ] **Step 1: 写失败的测试**（`FakeStorage` 指向临时目录；SD 的 `online_flag` 可切换）

```cpp
TEST(StorageManager, RootsWithAndWithoutSd) {
    Fixture f(/*sd_online=*/true); ASSERT_TRUE(f.mgr.init());
    EXPECT_EQ(f.mgr.roots(Area::Library), (std::vector<std::string>{f.sd + "/wisp/library", f.in + "/library"}));
    EXPECT_EQ(f.mgr.write_root(Area::Library).value(), f.sd + "/wisp/library");
    f.sd_dev->online_flag = false; f.mgr.refresh();
    EXPECT_EQ(f.mgr.roots(Area::Library), (std::vector<std::string>{f.in + "/library"}));
    EXPECT_TRUE(f.mgr.roots(Area::Backup).empty());
    EXPECT_EQ(f.mgr.write_root(Area::Backup).error().code, wisp::ErrorCode::StorageUnavailable);
}
TEST(StorageManager, AtomicWriteLeavesNoTmpAndOverwrites) {
    Fixture f(false); f.mgr.init();
    auto p = f.in + "/system/a.json";
    ASSERT_TRUE(f.mgr.write_atomic(p, bytes("one")));
    ASSERT_TRUE(f.mgr.write_atomic(p, bytes("two")));   // 目标已存在（FAT 场景）
    EXPECT_EQ(str(f.mgr.read_file(p).value()), "two");
    EXPECT_FALSE(exists(p + ".tmp"));
}
TEST(StorageManager, RecoversPendingTmp) {
    Fixture f(false); f.mgr.init();
    auto p = f.in + "/system/b.json";
    write_raw(p + ".tmp", "pending");  // 模拟"删除目标后、改名前"断电
    EXPECT_EQ(str(f.mgr.read_file(p).value()), "pending");
    EXPECT_TRUE(exists(p)); EXPECT_FALSE(exists(p + ".tmp"));
}
TEST(StorageManager, InternalLibraryQuota) {
    Fixture f(false, StorageConfig{.internal_library_quota = 1000}); f.mgr.init();
    ASSERT_TRUE(f.mgr.write_atomic(f.in + "/library/x.bin", std::vector<uint8_t>(600)));
    ASSERT_TRUE(f.mgr.write_atomic(f.in + "/library/x.bin", std::vector<uint8_t>(900)));  // 覆盖自身不重复计算
    EXPECT_EQ(f.mgr.write_atomic(f.in + "/library/y.bin", std::vector<uint8_t>(200)).error().code,
              wisp::ErrorCode::QuotaExceeded);
}
TEST(StorageManager, OfflineSdWriteFails) {
    Fixture f(true); f.mgr.init(); f.sd_dev->online_flag = false; f.mgr.refresh();
    EXPECT_EQ(f.mgr.write_atomic(f.sd + "/wisp/library/z", bytes("z")).error().code,
              wisp::ErrorCode::StorageUnavailable);
}
TEST(StorageManager, HotPlugEvents) {
    Fixture f(false); f.mgr.init();
    f.sd_dev->online_flag = true;  f.mgr.refresh();
    f.sd_dev->online_flag = false; f.mgr.refresh();
    f.bus.wait_idle(1000);
    EXPECT_EQ(f.topics, (std::vector<std::string>{"storage.sd.inserted", "storage.sd.removed"}));
    EXPECT_TRUE(exists(f.sd + "/wisp/library"));
}
TEST(StorageManager, MissingInternalFailsInit) {
    wisp::core::DeviceRegistry reg; wisp::core::EventBus bus;
    wisp::core::StorageManager m(reg, bus);
    EXPECT_EQ(m.init().error().code, wisp::ErrorCode::StorageUnavailable);
}
```

- [ ] **Step 2: 运行，确认失败** — Expected: 找不到 `storage_manager.hpp`。
- [ ] **Step 3: 实现**
- [ ] **Step 4: 运行，确认通过** — Run: `ctest --test-dir build/sim -R StorageManager --output-on-failure` → PASS。
- [ ] **Step 5: Commit**

```bash
git add components/wisp_core tests
git commit -m "feat(core): add storage manager with tiered areas and atomic writes"
```

---

### Task 10: UI 运行时（LVGL 对接）

**Files:**
- Create: `components/wisp_ui/CMakeLists.txt`、`components/wisp_ui/config/lv_conf.h`、`components/wisp_ui/include/wisp/ui/{ui_runtime,align}.hpp`、`components/wisp_ui/src/{ui_runtime,align}.cpp`
- Create: `components/wisp_ui/fonts/lv_font_cjk_16.c`（从 `/home/shouchen/Projects/MyOS/src/config/lv_font_cjk_16.c` 原样复制，文件头补注释：来源 MyOS，字形来自 Noto Sans CJK SC，SIL OFL 1.1）
- Modify: `cmake/wisp_host_deps.cmake`（启用 LVGL）、`simulator/CMakeLists.txt`
- Test: `tests/ui/align_test.cpp`、`tests/ui/ui_runtime_test.cpp`

**Interfaces:**
- Consumes: `hal::Display`、`hal::Touch`、`os::Task`、`os::Queue`、`os::alloc`、`os::now_ms`、`WLOG_*`
- Produces（`namespace wisp::ui`）：
  - `hal::Area align_area(const hal::Area& in, int align, int width, int height);`：x1/y1 向下取整到 `align` 的倍数，x2/y2 向上扩展到"下一个倍数减一"，结果再夹到 `[0, width-1]`/`[0, height-1]`；`align <= 1` 时只做夹取。
  - `struct UiConfig { uint32_t buffer_lines = 24; uint32_t task_stack = 8192; int task_priority = 4; };`
  - `class UiRuntime { UiRuntime(std::shared_ptr<hal::Display>, std::shared_ptr<hal::Touch> /* 可空 */, UiConfig = {}); ~UiRuntime(); Result<void> start(); void stop(); bool post(std::function<void()>); /* 队列容量 32，满时返回 false */ bool on_ui_thread() const; Result<std::vector<uint8_t>> snapshot_rgb888(uint32_t timeout_ms = 2000); /* 可在任意线程调用，内部 post 到 UI 线程执行 lv_snapshot，返回 width*height*3 字节 */ };`
  - 行为：`start()` 调用 `lv_init`、`lv_tick_set_cb`（返回 `os::now_ms()` 的低 32 位）、`lv_log_register_print_cb`（转到 `WLOG_I("lvgl", ...)`）；创建显示，分辨率取 Display caps 的 `width`/`height`；两块局部缓冲，各 `width*buffer_lines*2` 字节，用 `os::alloc(Mem::Dma)` 分配；注册 `LV_EVENT_INVALIDATE_AREA` 回调，用 `align_area`（对齐取 caps 的 `area_align`，缺省为 1）对齐区域；颜色格式按 caps `rgb565_be` 选 `LV_COLOR_FORMAT_RGB565_SWAPPED` 或 `RGB565`；flush 回调调用 `display->flush_async` 后返回（不调用 `lv_display_flush_ready`），flush-wait 回调调用 `display->wait_flush`；`stop()` 在 `lv_deinit` 前先 `wait_flush`；Touch 非空时注册 pointer 输入设备，取第一个触点。UI 任务循环：取出 post 来的函数逐个执行，然后 `lv_timer_handler()`，再按其返回值睡眠，最长 10 ms。
  - 性能统计：`struct UiPerf { float fps = 0; uint8_t cpu_percent = 0; float avg_flush_ms = 0; uint32_t frames = 0; };`，`UiPerf UiRuntime::perf() const;`（线程安全，统计最近 1 s 的滑动窗口）。帧数在 flush 回调中 `lv_display_flush_is_last()` 为真时加一；`cpu_percent` = UI 任务执行 post 函数与 `lv_timer_handler()` 的耗时 ÷ 窗口时长；`avg_flush_ms` = 每段花在屏幕上的平均时间（`flush_async` 加上随后的 `wait_flush`），按帧聚合存放。`void UiRuntime::set_perf_overlay(bool)`：经 `post` 调用 `lv_sysmon_show_performance`/`lv_sysmon_hide_performance`。
  - 默认字体：`lv_font_montserrat_16`，其 `fallback` 设为 `&lv_font_cjk_16`，并设为默认主题字体。
- `lv_conf.h` 固定项：`#define WISP_LV_CONF 1`、`LV_COLOR_DEPTH 16`、`LV_USE_OS LV_OS_NONE`、`LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB`（字符串和格式化函数同样用 CLIB）、`LV_USE_LOG 1`、`LV_LOG_LEVEL LV_LOG_LEVEL_WARN`、`LV_USE_SNAPSHOT 1`、`LV_USE_MSGBOX 1`、`LV_USE_SYSMON 1`、`LV_USE_PERF_MONITOR 1`（浮层默认隐藏，位置 `LV_ALIGN_BOTTOM_MID`，避开圆角）、`LV_FONT_MONTSERRAT_16 1`、`LV_FONT_MONTSERRAT_24 1`、`LV_FONT_DEFAULT &lv_font_montserrat_16`、`LV_USE_DRAW_SW 1`，不启用任何 LVGL 自带的平台驱动（SDL、Linux framebuffer 等）。`ui_runtime.cpp` 开头写 `#ifndef WISP_LV_CONF` → `#error "wisp lv_conf.h not in use"`，并 `static_assert(LV_COLOR_DEPTH == 16)`。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(Align, EvenAlignment) {
    auto a = wisp::ui::align_area({3, 5, 10, 12}, 2, 480, 480);
    EXPECT_EQ(a.x1, 2); EXPECT_EQ(a.y1, 4); EXPECT_EQ(a.x2, 11); EXPECT_EQ(a.y2, 13);
}
TEST(Align, ClampsAtRightAndBottomEdges) {
    auto a = wisp::ui::align_area({470, 470, 479, 479}, 2, 480, 480);
    EXPECT_EQ(a.x2, 479); EXPECT_EQ(a.y2, 479);
    auto b = wisp::ui::align_area({0, 0, 478, 10}, 4, 479, 480);   // 宽度不是 align 的倍数
    EXPECT_EQ(b.x2, 478);
}
TEST(Align, NoAlignmentOnlyClamps) {
    auto a = wisp::ui::align_area({-5, 3, 500, 9}, 1, 480, 480);
    EXPECT_EQ(a.x1, 0); EXPECT_EQ(a.x2, 479); EXPECT_EQ(a.y1, 3);
}
TEST(UiRuntime, RendersPostedContentWithAlignedFlushes) {
    auto disp = std::make_shared<FakeDisplay>("display.main", 480, 480, /*area_align=*/2);
    wisp::ui::UiRuntime ui(disp, nullptr);
    ASSERT_TRUE(ui.start());
    std::atomic<bool> on_ui{false};
    ui.post([&] { on_ui = ui.on_ui_thread(); lv_obj_t* l = lv_label_create(lv_screen_active()); lv_label_set_text(l, "Wisp 你好"); });
    ASSERT_TRUE(wait_until([&] { return disp->flush_count() > 0; }, 2000));
    EXPECT_TRUE(on_ui); EXPECT_FALSE(ui.on_ui_thread());
    for (auto& a : disp->areas()) { EXPECT_EQ(a.x1 % 2, 0); EXPECT_EQ(a.y1 % 2, 0); EXPECT_EQ((a.x2 + 1) % 2, 0); }
    auto shot = ui.snapshot_rgb888();
    ASSERT_TRUE(shot); EXPECT_EQ(shot.value().size(), 480u * 480u * 3u);
    ui.stop();
}
TEST(UiRuntime, PerfCountsFramesUnderAnimation) {
    auto disp = std::make_shared<FakeDisplay>("display.main", 480, 480, 2);
    wisp::ui::UiRuntime ui(disp, nullptr); ASSERT_TRUE(ui.start());
    ui.post([] { lv_obj_t* o = lv_obj_create(lv_screen_active()); lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, o);
                 lv_anim_set_values(&a, 0, 300); lv_anim_set_duration(&a, 1000); lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
                 lv_anim_set_exec_cb(&a, [](void* v, int32_t x) { lv_obj_set_x(static_cast<lv_obj_t*>(v), x); }); lv_anim_start(&a); });
    wisp::os::sleep_ms(1500);
    auto p = ui.perf();
    EXPECT_GT(p.fps, 5.0f); EXPECT_GT(p.frames, 5u); EXPECT_LE(p.cpu_percent, 100);
    ui.stop();
}
```

（`wait_until(pred, timeout_ms)` 放进 `tests/support/wait.hpp`。）

- [ ] **Step 2: 运行，确认失败** — Expected: 找不到 `wisp/ui/align.hpp`。
- [ ] **Step 3: 实现 `lv_conf.h`、`align.cpp`、`ui_runtime.cpp`，复制字体文件，主机侧启用 LVGL（`wisp_ui` 的 `HOST_LIBS lvgl`）**
- [ ] **Step 4: 运行，确认通过** — Run: `cmake --build build/sim && ctest --test-dir build/sim -R "Align|UiRuntime" --output-on-failure` → PASS。
- [ ] **Step 5: Commit**

```bash
git add components/wisp_ui cmake simulator tests
git commit -m "feat(ui): add LVGL runtime over HAL display/touch"
```

---

### Task 11: 启动编排（`wisp_app`）

**Files:**
- Create: `components/wisp_app/CMakeLists.txt`、`components/wisp_app/include/wisp/app/{board,app}.hpp`、`components/wisp_app/src/app.cpp`
- Test: `tests/app/app_boot_test.cpp`

**Interfaces:**
- Consumes: Task 4–10 的全部核心服务
- Produces（`namespace wisp::app`）：
  - `class Board { public: virtual ~Board() = default; virtual std::string name() const = 0; /* 创建设备，成功的 add 进 registry，失败的写入 report（state Failed，附原因）；SD 卡离线时仍注册设备，report 记 Missing */ virtual void init(core::DeviceRegistry&, core::BootReport&) = 0; };`
  - `struct Platform { std::unique_ptr<Board> board; std::unique_ptr<core::SettingsBackend> settings; std::function<void(const std::string& line)> emit_line; /* 输出一整行到串口或 stdout */ std::function<void()> request_power_off; /* 关机兜底，Power 设备缺失时使用 */ std::function<Value()> heap_info; /* 可空；返回 {"internal_free":…, "large_free":…} */ std::string firmware_version = "0.1.0"; };`（`WISP_PERF` 首行在启动 5 s 后才输出，不影响下面"恰好两行"的断言）
  - `class App { explicit App(Platform); ~App(); Result<void> start(); void stop(); core::DeviceRegistry& registry(); core::BootReport& report(); core::EventBus& bus(); core::StorageManager& storage(); ui::UiRuntime* ui(); /* 无屏模式下为 nullptr */ };`
  - `start()` 顺序（spec 6.1）：①`log::init()`；②`board->init(registry, report)`，对每个已注册设备补记 `Ok`（board 已记录过的不覆盖）；③启动 `EventBus`，然后 `StorageManager::init()`（失败时 `report.set_note("storage", "readonly")`，系统继续），启动周期 2000 ms 的存储刷新 Timer，启动 `ButtonService`；④若有 `display.main`，启动 `UiRuntime` 并构建自检界面（Task 12 提供 `void build_self_check(App&)`，本任务先放一个空实现）；否则 `report.set_note("mode", "headless")`；⑤记录 `boot_ms`，然后依次 `emit_line("WISP_BOOT_REPORT " + to_json(report.to_value(board)))`、`emit_line("WISP_MANIFEST " + to_json(registry.manifest(board)))`，最后发布事件 `system.boot.completed`。
  - 从 Settings `display` 命名空间读取 `brightness`（默认 200）并应用到屏幕。

- [ ] **Step 1: 写失败的测试**（FakeBoard 可配置要注册的假设备和要记录的失败项；emit_line 收集到 vector）

```cpp
TEST(AppBoot, EmitsReportAndManifest) {
    auto h = make_app({.with_display = false});
    ASSERT_TRUE(h.app.start());
    ASSERT_EQ(h.lines.size(), 2u);
    EXPECT_EQ(h.lines[0].rfind("WISP_BOOT_REPORT ", 0), 0u);
    auto rep = wisp::from_json(h.lines[0].substr(17)).value();
    EXPECT_EQ(rep.find("notes")->find("mode")->as_string(), "headless");
    EXPECT_EQ(h.lines[1].rfind("WISP_MANIFEST ", 0), 0u);
    EXPECT_EQ(h.app.ui(), nullptr);
}
TEST(AppBoot, FailedDeviceIsReportedAndBootContinues) {
    auto h = make_app({.with_display = false, .fail = {{"imu.main", "disabled"}}});
    ASSERT_TRUE(h.app.start());
    EXPECT_EQ(h.app.report().find("imu.main")->state, wisp::core::DeviceState::Failed);
    EXPECT_EQ(h.app.registry().find("imu.main"), nullptr);
    EXPECT_EQ(h.app.report().find("storage.int")->state, wisp::core::DeviceState::Ok);
}
TEST(AppBoot, StartsUiWhenDisplayPresent) {
    auto h = make_app({.with_display = true});
    ASSERT_TRUE(h.app.start());
    ASSERT_NE(h.app.ui(), nullptr);
    EXPECT_TRUE(wait_until([&] { return h.display->flush_count() > 0; }, 2000));
    EXPECT_EQ(h.display->brightness(), 200);
}
TEST(AppBoot, MissingInternalStorageIsReadonlyNotFatal) {
    auto h = make_app({.with_display = false, .with_internal_storage = false});
    ASSERT_TRUE(h.app.start());
    auto rep = wisp::from_json(h.lines[0].substr(17)).value();
    EXPECT_EQ(rep.find("notes")->find("storage")->as_string(), "readonly");
}
```

- [ ] **Step 2: 运行，确认失败** — Expected: 找不到 `wisp/app/app.hpp`。
- [ ] **Step 3: 实现**
- [ ] **Step 4: 运行，确认通过** — Run: `ctest --test-dir build/sim -R AppBoot --output-on-failure` → PASS。
- [ ] **Step 5: Commit**

```bash
git add components/wisp_app simulator tests
git commit -m "feat(app): add boot orchestration with boot report and manifest output"
```

---

### Task 12: 自检界面、电源键逻辑与录音回放

**Files:**
- Create: `components/wisp_app/include/wisp/app/{power_key_controller,format}.hpp`、`components/wisp_app/src/{power_key_controller,format,self_check_screen,loopback}.cpp`
- Modify: `components/wisp_app/src/app.cpp`（调用真正的 `build_self_check`）
- Test: `tests/app/power_key_controller_test.cpp`、`tests/app/format_test.cpp`

**Interfaces:**
- Produces（`namespace wisp::app`）：
  - `std::string format_bytes(uint64_t)`：`0`→`"0 B"`、`1536`→`"1.5 KB"`、`6160384`→`"5.9 MB"`、`34359738368`→`"32.0 GB"`（一位小数，1024 进制，小于 1024 时为整数 B）
  - `const char* state_label(core::DeviceState)` → `"正常"`、`"降级"`、`"缺失"`、`"失败"`
  - `std::string format_time(int64_t unix_s)` → `"YYYY-MM-DD HH:MM:SS"`（UTC）
  - `class PowerKeyController { enum class Action { None, ScreenOff, ScreenOn, ShowConfirm, ScreenOnAndConfirm, PowerOff, CancelConfirm }; Action on_short(); Action on_long(); Action on_confirm(bool yes); bool screen_on() const; bool confirming() const; };`，初始状态为亮屏、不在确认中
  - 状态转移：亮屏时短按 → `ScreenOff`；息屏时短按 → `ScreenOn`；确认中短按 → `CancelConfirm`；亮屏时长按 → `ShowConfirm`；息屏时长按 → `ScreenOnAndConfirm`；确认中长按 → `None`；确认"是" → `PowerOff`；确认"否" → `CancelConfirm`
  - `void build_self_check(App&)`：在 UI 线程构建 spec 第 8 节描述的界面：板名/版本/启动耗时、设备列表（状态着色：正常 `#4FC3F7`、降级 `#FFB74D`、缺失 `#9E9E9E`、失败 `#E57373`）、IMU 数值与倾斜小球、电量与充电、RTC 时间、两个存储介质的状态与容量、触摸测试区、按键提示、"录音回放"按钮、性能行（`FPS 58 · CPU 23% · flush 4.1ms`，取自 `UiRuntime::perf()`）；长按性能行切换 LVGL sysmon 浮层，状态存入 Settings（namespace `ui`，key `perf_overlay`），启动时恢复；App 启动后创建周期 5000 ms 的 Timer，`emit_line("WISP_PERF " + to_json({fps, cpu, flush_ms, frames, heap_internal_free, heap_large_free}))`（堆信息由 `Platform` 新增的 `std::function<Value()> heap_info` 提供，可为空）；用 `lv_timer` 每 100 ms 刷新一次，数据取自采样任务（任务名 `"wisp.sample"`，IMU 50 Hz，电源、时钟、存储 1 Hz）用 `os::Mutex` 保护的快照；订阅 `button.` 事件，经 `ui.post` 更新提示，并把电源键事件交给 `PowerKeyController`，执行对应动作（亮/灭屏调用 `Display::set_power`，确认框用 `lv_msgbox`，`PowerOff` 优先调用 `Power::power_off()`，没有 Power 设备时调用 `platform.request_power_off`）
  - `loopback.cpp`：`void run_loopback(App&, std::function<void(const char* status)> on_status)`，在任务 `"wisp.loop"` 中录制 3 s（16 kHz 单声道，`os::alloc(Mem::Large)`，共 96000 字节），然后立即播放；依次回调状态 `"录音中"`、`"播放中"`、`"完成"`，出错时回调 `"失败: <错误码>"`；已在运行时再次触发直接忽略
  - 运行时读取失败（例如 IMU `read()` 返回错误）时，界面对应项显示 `"读取失败"`，不崩溃

- [ ] **Step 1: 写失败的测试**

```cpp
using A = wisp::app::PowerKeyController::Action;
TEST(PowerKey, ShortTogglesScreen) {
    wisp::app::PowerKeyController c;
    EXPECT_EQ(c.on_short(), A::ScreenOff); EXPECT_FALSE(c.screen_on());
    EXPECT_EQ(c.on_short(), A::ScreenOn);  EXPECT_TRUE(c.screen_on());
}
TEST(PowerKey, LongShowsConfirmOnlyOnce) {
    wisp::app::PowerKeyController c;
    EXPECT_EQ(c.on_long(), A::ShowConfirm); EXPECT_EQ(c.on_long(), A::None); EXPECT_TRUE(c.confirming());
    EXPECT_EQ(c.on_confirm(false), A::CancelConfirm); EXPECT_FALSE(c.confirming());
}
TEST(PowerKey, LongWhileScreenOffWakesAndConfirms) {
    wisp::app::PowerKeyController c; c.on_short();
    EXPECT_EQ(c.on_long(), A::ScreenOnAndConfirm); EXPECT_TRUE(c.screen_on());
    EXPECT_EQ(c.on_confirm(true), A::PowerOff);
}
TEST(PowerKey, ShortCancelsConfirm) {
    wisp::app::PowerKeyController c; c.on_long();
    EXPECT_EQ(c.on_short(), A::CancelConfirm); EXPECT_TRUE(c.screen_on());
}
TEST(Format, Bytes) {
    EXPECT_EQ(wisp::app::format_bytes(0), "0 B");
    EXPECT_EQ(wisp::app::format_bytes(1536), "1.5 KB");
    EXPECT_EQ(wisp::app::format_bytes(6160384), "5.9 MB");
    EXPECT_EQ(wisp::app::format_bytes(34359738368ull), "32.0 GB");
}
TEST(Format, TimeAndState) {
    EXPECT_EQ(wisp::app::format_time(0), "1970-01-01 00:00:00");
    EXPECT_STREQ(wisp::app::state_label(wisp::core::DeviceState::Failed), "失败");
}
```

- [ ] **Step 2: 运行，确认失败** — Expected: 找不到 `power_key_controller.hpp`。
- [ ] **Step 3: 实现上述模块与自检界面**（界面布局：竖向滚动的单列容器，左右内边距 40 px；圆角屏的四角不放内容。注：之后由 Task 12b 改为按安全区布局）
- [ ] **Step 4: 运行，确认通过** — Run: `ctest --test-dir build/sim -R "PowerKey|Format|AppBoot" --output-on-failure` → PASS（AppBoot 用例此时会渲染真实界面，必须仍然通过）。
- [ ] **Step 5: Commit**

```bash
git add components/wisp_app tests
git commit -m "feat(app): add self-check screen, power key controller and audio loopback"
```

---

### Task 12b: 屏幕形状与安全区

（执行期由用户批准插入，spec 5.6 节。顺带处理 Task 12 复审遗留的两个问题。）

**Files:**
- Create: `components/wisp_hal/include/wisp/hal/display_geometry.hpp`、`components/wisp_hal/src/display_geometry.cpp`
- Modify: `components/wisp_hal/CMakeLists.txt`、`components/wisp_hal/include/wisp/hal/display.hpp`（caps 注释写明 shape 取值）、`components/wisp_core/src/device_registry.cpp`（manifest 附带 `safe_area`）、`components/wisp_app/src/self_check_screen.cpp`、`components/wisp_app/src/app.cpp`、`tests/support/fake_devices.hpp`（`rounded_square` → `rounded_rect`，构造函数增加可选参数 `std::string shape = "rounded_rect"`）、`tests/CMakeLists.txt`
- Test: `tests/hal/display_geometry_test.cpp`、`tests/core/device_registry_test.cpp`、`tests/app/self_check_screen_test.cpp`

**Interfaces:**
- Produces（`namespace wisp::hal`，纯函数，不依赖 LVGL）：
  - `enum class ScreenShapeKind { Rect, RoundedRect, Circle };`
  - `struct ScreenShape { ScreenShapeKind kind = ScreenShapeKind::Rect; int32_t width = 0; int32_t height = 0; int32_t corner_radius = 0; };`
  - `Result<ScreenShape> screen_shape_from_caps(const Properties& caps);`：`width`/`height` 缺失或 ≤ 0 → `InvalidArgument`；`shape` 缺失 → `Rect`；`"rect"`/`"rounded_rect"`/`"circle"` 对应三种；其他值 → `InvalidArgument`，消息中包含该值；`rounded_rect` 缺少 `corner_radius` 或其值 < 0 → `InvalidArgument`；半径大于 `min(w, h) / 2` 时夹到 `min(w, h) / 2`
  - `bool visible(const ScreenShape&, int32_t x, int32_t y);`：以像素中心 `(x + 0.5, y + 0.5)` 判断；坐标超出 `[0, w) × [0, h)` 返回 false；`RoundedRect` 在四个 r×r 角区内要求到角圆心的距离 ≤ r；`Circle` 以 `(w/2, h/2)` 为圆心、`min(w, h)/2` 为半径
  - `Area safe_rect(const ScreenShape&);`：`Rect` → 整屏；`RoundedRect` 四边各缩进 `ceil(r · (1 − 1/√2))`；`Circle` 横向缩进 `ceil(w/2 − R/√2)`、纵向缩进 `ceil(h/2 − R/√2)`
  - `struct Span { int32_t x1 = 0; int32_t x2 = -1; bool empty() const { return x1 > x2; } };`，`Span row_span(const ScreenShape&, int32_t y);`：第 y 行 `visible` 为真的最左、最右像素；整行不可见或 y 越界时返回空 Span
- 回退规则（manifest 与自检界面共用）：`screen_shape_from_caps` 失败时，若 caps 中 `width`/`height` 有效则按 `Circle` 处理并 `WLOG_W`；否则 manifest 不输出 `safe_area`，自检界面使用整屏
- `DeviceRegistry::manifest`：class 为 display 的条目增加与 `caps` 平级的 `"safe_area": {"x1", "y1", "x2", "y2"}`（int）
- 自检界面：单列容器的位置和尺寸设为 `safe_rect()`（不再是整屏加 40 px 内边距），容器内四边内边距 8 px；容器外不放任何控件。LVGL sysmon 浮层是调试用途，不受此约束
- 复审遗留 N1：`set_boot_ms` 恢复到自检界面构建完成之后调用（spec 6.1 的计时终点）；界面保存表头 label 指针，`set_boot_ms` 之后 post 一次刷新表头文字
- 复审遗留 N2：`ImuReadFailureRendersDifferentlyAndDoesNotCrash` 改为在 UI 线程读取 IMU 那一行 label 的文字，断言为 `"读取失败"`（不再比较整屏快照）

- [ ] **Step 1: 写失败的测试**

```cpp
using namespace wisp::hal;
static ScreenShape shape(const char* s, int w = 480, int h = 480, int r = 40) {
    return screen_shape_from_caps(wisp::Properties{{"width", w}, {"height", h},
                                  {"shape", wisp::Value(s)}, {"corner_radius", r}}).value();
}
TEST(DisplayGeometry, ParsesCaps) {
    EXPECT_EQ(screen_shape_from_caps(wisp::Properties{{"width", 320}, {"height", 240}}).value().kind, ScreenShapeKind::Rect);
    EXPECT_EQ(shape("circle").kind, ScreenShapeKind::Circle);
    auto bad = screen_shape_from_caps(wisp::Properties{{"width", 480}, {"height", 480}, {"shape", wisp::Value("triangle")}});
    EXPECT_NE(bad.error().message.find("triangle"), std::string::npos);
    EXPECT_FALSE(screen_shape_from_caps(wisp::Properties{{"width", 480}, {"height", 480}, {"shape", wisp::Value("rounded_rect")}}));
    EXPECT_FALSE(screen_shape_from_caps(wisp::Properties{{"height", 480}}));
    EXPECT_EQ(shape("rounded_rect", 480, 480, 1000).corner_radius, 240);
}
TEST(DisplayGeometry, RoundedRect) {
    auto s = shape("rounded_rect");
    EXPECT_FALSE(visible(s, 0, 0)); EXPECT_FALSE(visible(s, 11, 11)); EXPECT_TRUE(visible(s, 12, 12));
    EXPECT_TRUE(visible(s, 240, 0)); EXPECT_TRUE(visible(s, 0, 240)); EXPECT_FALSE(visible(s, 480, 240));
    auto a = safe_rect(s);
    EXPECT_EQ(a.x1, 12); EXPECT_EQ(a.y1, 12); EXPECT_EQ(a.x2, 467); EXPECT_EQ(a.y2, 467);
    auto r = row_span(s, 0); EXPECT_EQ(r.x1, 34); EXPECT_EQ(r.x2, 445);
}
TEST(DisplayGeometry, Circle) {
    auto s = shape("circle");
    EXPECT_FALSE(visible(s, 0, 0)); EXPECT_TRUE(visible(s, 240, 0)); EXPECT_TRUE(visible(s, 240, 240));
    auto a = safe_rect(s);
    EXPECT_EQ(a.x1, 71); EXPECT_EQ(a.y1, 71); EXPECT_EQ(a.x2, 408); EXPECT_EQ(a.y2, 408);
    auto r = row_span(s, 0); EXPECT_EQ(r.x1, 225); EXPECT_EQ(r.x2, 254);
    EXPECT_TRUE(row_span(s, -1).empty()); EXPECT_TRUE(row_span(s, 480).empty());
}
TEST(DisplayGeometry, RectIsFullScreen) {
    auto s = shape("rect", 320, 240);
    auto a = safe_rect(s);
    EXPECT_EQ(a.x1, 0); EXPECT_EQ(a.y1, 0); EXPECT_EQ(a.x2, 319); EXPECT_EQ(a.y2, 239);
    EXPECT_TRUE(visible(s, 0, 0)); EXPECT_EQ(row_span(s, 10).x2, 319);
}
TEST(DisplayGeometry, ConsistentWithVisible) {        // 三种形状逐像素核对
    for (const char* k : {"rect", "rounded_rect", "circle"}) {
        auto s = shape(k);
        auto a = safe_rect(s);
        for (int32_t y = 0; y < 480; ++y) {
            auto sp = row_span(s, y);
            for (int32_t x = 0; x < 480; ++x) {
                bool in_span = !sp.empty() && x >= sp.x1 && x <= sp.x2;
                ASSERT_EQ(visible(s, x, y), in_span) << k << " " << x << "," << y;
                if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) ASSERT_TRUE(visible(s, x, y)) << k;
            }
        }
    }
}
```

另外三处测试：
- `device_registry_test`：注册一个 `FakeDisplay("display.main", 480, 480)`，manifest 里该条目的 `safe_area` 为 `{"x1":12,"y1":12,"x2":467,"y2":467}`；`shape` 为 `"circle"` 时为 `{71,71,408,408}`；非 display 设备没有 `safe_area` 键。
- `self_check_screen_test`，`NoContentOutsideSafeRect`：分别用 `rounded_rect` 和 `circle` 的 FakeDisplay 启动 App，等界面渲染完成后 `snapshot_rgb888()`，`safe_rect()` 之外的每个像素都等于屏幕背景色。
- `self_check_screen_test`：表头文字包含 `report().to_value()` 中的 `boot_ms` 值；按上面 N2 的要求修改 IMU 读取失败测试。

- [ ] **Step 2: 运行，确认失败** — Expected: 找不到 `display_geometry.hpp`。
- [ ] **Step 3: 实现几何函数、manifest 的 `safe_area`、自检界面布局与两个遗留问题**
- [ ] **Step 4: 运行，确认通过** — Run: `cmake --build build/sim && ctest --test-dir build/sim --output-on-failure` → 全部 PASS。
- [ ] **Step 5: Commit**

```bash
git add components/wisp_hal components/wisp_core components/wisp_app tests
git commit -m "feat(hal): add display shape geometry and lay out self-check in the safe area"
```

---

### Task 13: 模拟器设备、命令行与主程序

**Files:**
- Create: `components/wisp_port_sim/dev/{sim_display,sim_touch,sim_imu,sim_power,sim_clock,sim_button,sim_audio,sim_storage}.cpp`、`components/wisp_port_sim/include/wisp/port/sim/{sim_board,sim_input,png_writer}.hpp`、`components/wisp_port_sim/sim_board.cpp`、`components/wisp_port_sim/png_writer.cpp`
- Create: `simulator/main.cpp`、`simulator/sim_options.hpp`、`simulator/sim_options.cpp`
- Modify: `simulator/CMakeLists.txt`（`find_package(SDL2 REQUIRED)`，加入库 `wisp_port_sim_dev` 和可执行文件 `wisp_sim`）
- Test: `tests/sim/png_writer_test.cpp`、`tests/sim/sim_options_test.cpp`

**Interfaces:**
- Produces：
  - `struct SimOptions { bool headless = false; std::string screenshot; uint32_t exit_after_ms = 0; std::vector<std::string> disabled; bool no_sd = false; std::string data_dir = "./sim-data"; std::string shape = "rounded_rect"; };`，`wisp::Result<SimOptions> parse_sim_options(int argc, const char* const* argv);`：未知参数或缺少参数值 → `InvalidArgument`，消息中包含该参数名；`--shape` 只接受 `rect`、`rounded_rect`、`circle`，其他值 → `InvalidArgument`，消息中包含该值
  - `wisp::Result<void> wisp::port::sim::write_png_rgb888(const std::string& path, const uint8_t* rgb, uint32_t w, uint32_t h);`：PNG 签名，IHDR（8 位、颜色类型 2），IDAT 为 zlib 存储块（每块 ≤ 65535 字节，每行前加滤波字节 0）、Adler-32，IEND，每个块带 CRC-32
  - `class SimInput`（线程安全的输入状态）：`mouse(x, y, down)`、`key_power(bool)`、`key_vol_up(bool)`、`key_vol_down(bool)`、`tilt(dx, dy)`（每次 ±0.1 g，ax/ay 夹在 [-1, 1]，az = sqrt(max(0, 1 - ax² - ay²))）、`toggle_sd()`
  - `class SimBoard : public app::Board { SimBoard(SimOptions, std::shared_ptr<SimInput>, std::function<void()> on_power_off); }`，`name()` 返回 `"simulator"`。设备 id 与 model：`display.main`/`sdl-display`（caps width 480、height 480、shape 取 `SimOptions::shape`（默认 `rounded_rect`）、corner_radius 40、panel `amoled`、brightness true、area_align 2）、`touch.main`/`sdl-mouse`（max_points 1）、`imu.main`/`sim-imu`（accel true、gyro true、magnetometer false、max_rate_hz 1000）、`power.main`/`sim-battery`（固定 80%、3900 mV、未充电、USB 在位；`power_off` 调用 on_power_off）、`clock.main`/`system-clock`（`set_unix` 保存相对系统时间的偏移）、`button.power`、`button.vol_up`、`button.vol_down`/`sdl-key`、`audio_out.main`、`audio_in.main`/`sdl-audio`、`storage.int`/`host-dir`（`<data>/int`，caps removable false、fs `littlefs`；total 固定为 6160384，free = total - 目录已用）、`storage.sd`/`host-dir`（`<data>/sd`，removable true、fs `fat`；total 32 GiB；`--no-sd` 时初始离线，S 键切换）
  - `--disable <x>` 匹配设备 id 或类别名（`imu` 匹配 `imu.main`）：不创建该设备，report 记 `Failed`、原因 `"disabled by --disable"`；未匹配到任何设备的 `--disable` 值打 `WLOG_W`
  - 主循环（`simulator/main.cpp`，主线程）：非无窗口模式下创建 480×480 的 SDL 窗口；以约 60 Hz 把 SimDisplay 的帧缓冲（RGB565，flush 时在互斥锁下写入）上传到纹理，叠加形状遮罩（启动时按 display caps 用 `hal::visible()` 逐像素预计算一次，不可见像素画成黑色），然后呈现；处理 SDL 事件并更新 SimInput（鼠标 → 触摸；P → 电源键；`+`/`=` 与 `-` → 音量键；方向键 → 倾斜；S → SD 卡切换；关闭窗口 → 退出）。无窗口模式下不初始化 SDL 视频，音频使用 dummy 驱动（启动 SDL 前 `SDL_SetHint(SDL_HINT_AUDIODRIVER, "dummy")`）。`exit_after_ms` 到期后：如设置了 `screenshot`，调用 `app.ui()->snapshot_rgb888()` 并 `write_png_rgb888`；然后 `app.stop()`，退出码 0。电源关机回调使主循环以退出码 0 结束。`emit_line` 写 stdout 并立即 flush。设置后端为 `<data>/settings.json`。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(PngWriter, WritesValidHeader) {
    std::vector<uint8_t> px(4 * 3 * 3, 0x80);
    auto p = temp_dir() / "a.png";
    ASSERT_TRUE(wisp::port::sim::write_png_rgb888(p.string(), px.data(), 4, 3));
    auto b = read_all(p);
    EXPECT_EQ(std::vector<uint8_t>(b.begin(), b.begin() + 8), (std::vector<uint8_t>{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}));
    EXPECT_EQ(be32(&b[16]), 4u); EXPECT_EQ(be32(&b[20]), 3u);       // IHDR 宽高
    EXPECT_EQ(crc32_of(&b[12], 17), be32(&b[29]));                   // IHDR CRC
    EXPECT_EQ(std::string(b.end() - 8, b.end() - 4), "IEND");
}
TEST(SimOptions, ParsesAll) {
    const char* argv[] = {"wisp_sim", "--headless", "--screenshot", "o.png", "--exit-after-ms", "3000",
                          "--disable", "imu", "--disable", "audio_in", "--no-sd", "--data-dir", "/tmp/d"};
    auto o = parse_sim_options(13, argv).value();
    EXPECT_TRUE(o.headless); EXPECT_EQ(o.screenshot, "o.png"); EXPECT_EQ(o.exit_after_ms, 3000u);
    EXPECT_EQ(o.disabled, (std::vector<std::string>{"imu", "audio_in"})); EXPECT_TRUE(o.no_sd); EXPECT_EQ(o.data_dir, "/tmp/d");
}
TEST(SimOptions, RejectsUnknownAndMissingValue) {
    const char* a1[] = {"wisp_sim", "--bogus"};
    EXPECT_NE(parse_sim_options(2, a1).error().message.find("--bogus"), std::string::npos);
    const char* a2[] = {"wisp_sim", "--screenshot"};
    EXPECT_EQ(parse_sim_options(2, a2).error().code, wisp::ErrorCode::InvalidArgument);
}
TEST(SimOptions, Shape) {
    const char* a1[] = {"wisp_sim"};
    EXPECT_EQ(parse_sim_options(1, a1).value().shape, "rounded_rect");
    const char* a2[] = {"wisp_sim", "--shape", "circle"};
    EXPECT_EQ(parse_sim_options(3, a2).value().shape, "circle");
    const char* a3[] = {"wisp_sim", "--shape", "hexagon"};
    EXPECT_NE(parse_sim_options(3, a3).error().message.find("hexagon"), std::string::npos);
}
```

- [ ] **Step 2: 运行，确认失败** — Expected: 找不到 `png_writer.hpp`。
- [ ] **Step 3: 实现模拟设备、SimBoard、PNG 编码器、命令行解析与主程序**
- [ ] **Step 4: 运行，确认通过** — Run: `cmake --build build/sim && ctest --test-dir build/sim -R "PngWriter|SimOptions" --output-on-failure` → PASS。
- [ ] **Step 5: 手动运行窗口模式**

Run: `./build/sim/wisp_sim --data-dir build/sim-data`
Expected: 弹出 480×480 圆角窗口，显示自检界面（再用 `--shape circle` 运行一次：圆形遮罩，内容全部在圆内、没有被裁掉）；鼠标拖动时触摸区显示坐标；方向键让小球移动；P 短按灭屏、再按亮屏；按住 P 超过 1 秒弹出关机确认，选"是"后进程退出；S 键切换 SD 卡状态，界面同步更新；终端打印 `WISP_BOOT_REPORT` 和 `WISP_MANIFEST` 两行。

- [ ] **Step 6: Commit**

```bash
git add components/wisp_port_sim simulator tests
git commit -m "feat(sim): add SDL2 simulator devices, CLI options and main loop"
```

---

### Task 14: 冒烟测试、依赖规则检查与 CI

**Files:**
- Create: `tests/smoke/sim_smoke.py`、`tools/check_layering.py`、`tools/ci.sh`、`.github/workflows/ci.yml`
- Modify: `tests/CMakeLists.txt`（`add_test(NAME sim_smoke COMMAND ${Python3_EXECUTABLE} ${WISP_ROOT}/tests/smoke/sim_smoke.py $<TARGET_FILE:wisp_sim>)`）

**Interfaces:**
- `sim_smoke.py <wisp_sim 路径>`：每个用例在新的临时数据目录里运行 `--headless --exit-after-ms 3000`，超时 20 s；从 stdout 解析 `WISP_BOOT_REPORT ` 和 `WISP_MANIFEST ` 开头的两行 JSON。三个用例：
  1. 默认（附加 `--screenshot <tmp>/s.png`）：退出码 0；manifest 的 board 为 `"simulator"`，devices 的 id 集合恰好是 Task 13 列出的 12 个；`imu.main.caps.magnetometer == false`；report 中除 `storage.sd` 外所有设备都是 `ok`；PNG 文件存在，IHDR 宽高为 480×480，文件大于 1 KB，且解码后的像素不全相同（解析存储块：只需跳过 zlib 头、逐块读取，不需要真正的 inflate）；
  2. `--disable imu`：退出码 0；report 中 `imu.main` 为 `failed`，原因包含 `--disable`；manifest 中没有 `imu.main`；
  3. `--no-sd`：report 中 `storage.sd` 为 `missing`。
- `check_layering.py [--root .]`：扫描 `components/wisp_{base,os,hal,core,ui,app}/` 下的 `.hpp/.cpp/.h/.c`（跳过 `components/wisp_ui/fonts/`），用正则匹配 `#include\s*[<"]` 后面的路径；路径以 `esp_`、`freertos/`、`driver/`、`hal/`、`soc/`、`SDL` 开头，或等于 `sdkconfig.h` 时报错，输出 `path:line: forbidden include <x>`，退出码 1；没有问题时退出码 0。另外扫描 `wisp_os` 和 `wisp_hal`，发现 include `wisp/core/`、`wisp/ui/`、`wisp/app/` 也报错（禁止反向依赖）。
- `tools/ci.sh`：`set -euo pipefail`；依次执行 `clang-format --dry-run --Werror`（对象为除 `fonts/` 外的全部 C/C++ 源文件）、`python3 tools/check_layering.py`、`cmake -S simulator -B build/sim -G Ninja`、`cmake --build build/sim`、`ctest --test-dir build/sim --output-on-failure`。
- `.github/workflows/ci.yml`：任务 `host`（ubuntu-latest，安装 `libsdl2-dev ninja-build clang-format`，运行 `tools/ci.sh`）；任务 `firmware` 在 Task 15 加入。

- [ ] **Step 1: 先写 `check_layering.py` 的自检**：临时在 `components/wisp_core/src/` 放一个 include `freertos/FreeRTOS.h` 的文件，运行 `python3 tools/check_layering.py`，Expected: 退出码 1，输出该文件和行号。删除临时文件后再运行，Expected: 退出码 0。
- [ ] **Step 2: 写 `sim_smoke.py`，注册到 ctest，运行** — Run: `cmake --build build/sim && ctest --test-dir build/sim -R sim_smoke --output-on-failure` → Expected: PASS。
- [ ] **Step 3: 写 `ci.sh`，先对全仓库执行一次 `clang-format -i` 统一格式，再运行** — Run: `bash tools/ci.sh` → Expected: 所有步骤成功，退出码 0。
- [ ] **Step 4: 写 `.github/workflows/ci.yml`**
- [ ] **Step 5: Commit**

```bash
git add tests tools .github
git commit -m "test: add simulator smoke tests, layering check and CI"
```

---

### Task 15: ESP-IDF 环境、固件工程与 ESP 平台层

**Files:**
- Create: `firmware/CMakeLists.txt`、`firmware/main/CMakeLists.txt`、`firmware/main/idf_component.yml`、`firmware/main/main.cpp`、`firmware/sdkconfig.defaults`、`firmware/partitions.csv`
- Create: `components/wisp_port_esp/CMakeLists.txt`、`components/wisp_port_esp/os/os_esp.cpp`、`components/wisp_port_esp/log_esp.cpp`
- Create: `boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp`（本任务只注册 0 个设备，`name()` 返回 `"waveshare_esp32_s3_touch_amoled_2_16"`）
- Modify: `.github/workflows/ci.yml`（加入 `firmware` 任务）

**Interfaces:**
- Consumes: `wisp_os` 头文件；`wisp::app::App`/`Platform`/`Board`
- Produces：
  - ESP 侧 `wisp::os` 实现：`Mutex` 对应 `xSemaphoreCreateMutex`；`Semaphore` 对应 `xSemaphoreCreateCounting`；`Task::spawn` 对应 `xTaskCreatePinnedToCore`（`core == -1` 时用 `tskNO_AFFINITY`，栈大小以字节为单位，按 IDF 的约定传入），任务函数返回后 give 一个完成信号量并 `vTaskDelete(NULL)`，`join` 等待该信号量；`Timer` 对应 `esp_timer`（回调在 esp_timer 任务里运行）；`alloc` 中 `Mem::Large` → `heap_caps_malloc(MALLOC_CAP_SPIRAM)`，`Mem::Fast` → `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`，`Mem::Dma` → `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`；`now_ms` 基于 `esp_timer_get_time() / 1000`
  - `wisp::port::esp::EspLogSink`：调用 `esp_log_write`，日志级别一一对应
  - `std::unique_ptr<wisp::app::Board> wisp::board::create_board();`（声明在 `components/wisp_app/include/wisp/app/board.hpp`，由 `boards/<板>/board.cpp` 定义）
  - `firmware/CMakeLists.txt`：`set(EXTRA_COMPONENT_DIRS ../components ../drivers)`；`set(WISP_BOARD waveshare_esp32_s3_touch_amoled_2_16 CACHE STRING "")`；`include($ENV{IDF_PATH}/tools/cmake/project.cmake)`；`project(wisp)`；在 `project()` 之前用 `idf_build_set_property(COMPILE_DEFINITIONS "LV_CONF_PATH=${CMAKE_CURRENT_LIST_DIR}/../components/wisp_ui/config/lv_conf.h" APPEND)` 指定 LVGL 配置
  - `firmware/main/CMakeLists.txt`：`idf_component_register(SRCS main.cpp ${CMAKE_CURRENT_LIST_DIR}/../../boards/${WISP_BOARD}/board.cpp REQUIRES wisp_app wisp_port_esp)`
  - `firmware/main/idf_component.yml`：`idf: ">=6.0.3,<6.1"`、`lvgl/lvgl: "9.5.0"`、`bblanchon/arduinojson: "7.4.3"`（后续任务追加其他依赖）
  - `main.cpp`：`extern "C" void app_main()`，内容：`log::add_sink(EspLogSink)`；构造 `Platform`（board 来自 `create_board()`；`emit_line` 用 `printf("%s\n")` 后 `fflush`；`request_power_off` 调用 `esp_restart()`；`heap_info` 返回 `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` 与 `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)`）；`App::start()`；然后 `vTaskDelete(NULL)`（App 对象为静态存储期，不随 app_main 返回而析构）
  - `sdkconfig.defaults`：CPU 240 MHz；Flash QIO 80 MHz 16 MB；`CONFIG_PARTITION_TABLE_CUSTOM=y`，文件名 `partitions.csv`；`CONFIG_SPIRAM=y`、`CONFIG_SPIRAM_MODE_OCT=y`、`CONFIG_SPIRAM_SPEED_80M=y`、`CONFIG_SPIRAM_USE_MALLOC=y`；控制台 USB-Serial-JTAG；`CONFIG_LV_CONF_SKIP=n`；`CONFIG_COMPILER_CXX_EXCEPTIONS=n`、`CONFIG_COMPILER_CXX_RTTI=n`；`CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`
  - `partitions.csv` 与 spec 7.4 节完全一致：

```
# Name,    Type, SubType,  Offset,   Size
nvs,       data, nvs,      0x9000,   0x6000
otadata,   data, ota,      0xF000,   0x2000
phy_init,  data, phy,      0x11000,  0x1000
ota_0,     app,  ota_0,    0x20000,  0x400000
ota_1,     app,  ota_1,    0x420000, 0x400000
assets,    data, spiffs,   0x820000, 0x200000
storage,   data, littlefs, 0xA20000, 0x5E0000
```

- [ ] **Step 1: 安装 ESP-IDF v6.0.3**

Run:
```bash
mkdir -p ~/esp && git clone -b v6.0.3 --recursive --depth 1 --shallow-submodules https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6.0.3
~/esp/esp-idf-v6.0.3/install.sh esp32s3
bash -c '. ~/esp/esp-idf-v6.0.3/export.sh && idf.py --version'
```
Expected: 最后一行输出 `ESP-IDF v6.0.3`。

- [ ] **Step 2: 写固件工程文件与 ESP 平台层，构建**

Run: `bash -c '. ~/esp/esp-idf-v6.0.3/export.sh && cd firmware && idf.py set-target esp32s3 && idf.py build'`
Expected: 构建成功，生成 `firmware/build/wisp.bin`，并生成 `firmware/dependencies.lock`。如果 LVGL 没有用上 Wisp 的 `lv_conf.h`，Task 10 的 `#error "wisp lv_conf.h not in use"` 会让构建失败；此时在 `sdkconfig.defaults` 中确认 `CONFIG_LV_CONF_SKIP` 已关闭。

- [ ] **Step 3: 确认依赖规则检查仍然通过** — Run: `python3 tools/check_layering.py` → Expected: 退出码 0。

- [ ] **Step 4: 烧录并查看日志（需要真机）**

Run: `bash -c '. ~/esp/esp-idf-v6.0.3/export.sh && cd firmware && idf.py -p /dev/ttyACM0 flash monitor'`
Expected: 串口依次输出 `WISP_BOOT_REPORT {...}`（其中 `"mode":"headless"`、`"storage":"readonly"`）和 `WISP_MANIFEST {"board":"waveshare_esp32_s3_touch_amoled_2_16","devices":[]}`，没有崩溃或重启循环。

- [ ] **Step 5: 在 CI 中加入固件任务**：`firmware` 任务使用容器 `espressif/idf:v6.0.3`，执行 `cd firmware && idf.py set-target esp32s3 && idf.py bmgr -c ../boards -b waveshare_esp32_s3_touch_amoled_2_16 && idf.py build`（`bmgr` 这一步在 Task 16 生效；本任务先不写这一步，Task 16 再补上）。

- [ ] **Step 6: Commit**

```bash
git add firmware components/wisp_port_esp components/wisp_app boards .github
git commit -m "feat(firmware): add ESP-IDF project, ESP OS backend and headless boot"
```

---

### Task 16: 板级定义、屏幕与触摸

**Files:**
- Create: `boards/waveshare_esp32_s3_touch_amoled_2_16/{board_info.yaml,board_peripherals.yaml,board_devices.yaml,setup_device.c,README.md}`
- Create: `components/wisp_port_esp/dev/{esp_display,esp_touch}.cpp`、`components/wisp_port_esp/include/wisp/port/esp/{esp_display,esp_touch}.hpp`
- Modify: `boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp`、`firmware/main/idf_component.yml`、`.github/workflows/ci.yml`
- Create: `docs/references.md`（首条记录：板级 YAML 与 `setup_device.c` 参照 ESP-Claw 的 `boards/waveshare/waveshare_ESP32_S3_Touch_AMOLED_2_16`，Apache-2.0）

**Interfaces:**
- Consumes: `esp_board_manager_init()`、`esp_board_manager_get_device_handle(name, &handle)`、`esp_board_manager_get_device_config(name, &cfg)`、`esp_board_periph_get_handle("i2c_master", (void**)&bus)`；结构体 `dev_display_lcd_handles_t`（字段 `panel_handle`、`io_handle`）、`dev_display_lcd_config_t`（字段 `lcd_width`、`lcd_height`），以及 lcd_touch 设备句柄中的 `esp_lcd_touch_handle_t`（具体字段名以 `firmware/managed_components/espressif__esp_board_manager` 中的头文件为准）
- Produces：
  - `class wisp::port::esp::EspDisplay : public hal::Display { EspDisplay(esp_lcd_panel_handle_t, esp_lcd_panel_io_handle_t, int w, int h); }`，model `"CO5300"`，caps 与 spec 5.5 节一致（width 480、height 480、shape `rounded_rect`、corner_radius 40、panel `amoled`、brightness true、area_align 2）；`flush` 调用 `esp_lcd_panel_draw_bitmap(x1, y1, x2+1, y2+1)`，用 `esp_lcd_panel_io_register_event_callbacks` 的传输完成回调 give 信号量，等待最多 250 ms，超时返回 `Timeout`；`set_brightness` 用 `esp_lcd_panel_io_tx_param` 写寄存器 `0x51`；`set_power` 调用 `esp_lcd_panel_disp_on_off`
  - `class wisp::port::esp::EspTouch : public hal::Touch { explicit EspTouch(esp_lcd_touch_handle_t); }`，model `"CST9217"`，caps max_points 1；`read` 先调用 `esp_lcd_touch_read_data`，再用 `esp_lcd_touch_get_data` 取数据
  - YAML：三份文件的内容取自 ESP-Claw 同款板定义，修改为：`board: waveshare_esp32_s3_touch_amoled_2_16`；去掉其中的 `imu_sensor`、`axp2101_power_manager` 两个 `custom` 设备（由 Wisp 自己的驱动处理）；其余（i2c_master SDA15/SCL14；I2S MCLK42/BCLK9/WS45/DOUT8/DIN10，16 kHz；spi_display QSPI D0–D3=4/5/6/7、SCLK38；gpio_pa_control 46；audio_dac es8311 地址 0x30；audio_adc es7210 地址 0x80、mask `"0011"`；fs_sdcard sdmmc 1-bit CLK2/CMD1/D0=3、`format_if_mount_failed: false`；display_lcd co5300 CS12、RST39、quad_mode、`bits_per_pixel: 16`、swap_xy true、mirror_y true；lcd_touch cst9217 RST40、INT11、地址 0xB4）保持不变。`setup_device.c` 保留 Espressif 版权头，把 TAG 改为 `"wisp_board"`，在文件头追加说明"Adapted for Wisp"。
  - `board.cpp` 在本任务中：调用 `esp_board_manager_init()`（失败时把所有依赖 bmgr 的设备记为 `Failed` 并附错误名后返回）；从 bmgr 取 display_lcd，注册 `display.main`；取 lcd_touch，注册 `touch.main`；任何一步失败都记入 report，然后继续下一步。
  - `idf_component.yml` 追加：`espressif/esp_board_manager: "~0.7.5"`、`espressif/esp_lcd_co5300: "~2.2.0"`、`waveshare/esp_lcd_touch_cst9217: "*"`（首次构建后，从 `dependencies.lock` 读出实际解析到的版本，改为精确版本号锁定）。

- [ ] **Step 1: 写板级 YAML 与 setup_device.c，生成板级代码**

Run: `bash -c '. ~/esp/esp-idf-v6.0.3/export.sh && cd firmware && idf.py bmgr -c ../boards -b waveshare_esp32_s3_touch_amoled_2_16'`
Expected: 生成 `firmware/components/gen_bmgr_codes/`，没有 YAML 校验错误。如果某个字段被拒绝，对照 `firmware/managed_components/espressif__esp_board_manager/` 下的文档与示例修正，不改动其他设备。

- [ ] **Step 2: 实现 EspDisplay、EspTouch，更新 board.cpp，构建** — Run: `idf.py build`（在 export 环境中，下同） → Expected: 成功。
- [ ] **Step 3: 烧录验证（需要真机）** — Run: `idf.py -p /dev/ttyACM0 flash monitor` → Expected: 屏幕显示自检界面，四角不被裁切；手指触摸时触摸区坐标跟随，且方向正确（左上角接近 (0,0)，右下角接近 (479,479)）；界面中 display、touch 为"正常"，其余设备暂时缺失；启动报告中出现 `display.main` 和 `touch.main`。如果局部刷新出现条纹或错位，先确认 `area_align` 为 2 并检查 Display caps 是否正确传入。
- [ ] **Step 4: 更新 CI 固件任务**，加入 `idf.py bmgr -c ../boards -b waveshare_esp32_s3_touch_amoled_2_16`。
- [ ] **Step 5: Commit**

```bash
git add boards components/wisp_port_esp firmware docs/references.md .github
git commit -m "feat(board): add Waveshare AMOLED 2.16 board with display and touch"
```

---

### Task 17: 电源、RTC、IMU 与按键驱动（移植自 MyOS）

**Files:**
- Create: `drivers/axp2101/{CMakeLists.txt,include/axp2101.h,axp2101.c}`、`drivers/pcf85063/{...}`、`drivers/qmi8658/{...}`（纯 C 的 ESP-IDF 组件，基于 `driver/i2c_master.h`，由 `/home/shouchen/Projects/MyOS/src/hal/{hal_power,hal_rtc,hal_imu}.cpp` 中的寄存器逻辑整理而来）
- Create: `components/wisp_port_esp/dev/{esp_power,esp_clock,esp_imu,esp_button}.cpp` 及对应头文件
- Modify: `boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp`、`docs/references.md`

**Interfaces:**
- Consumes: bmgr 的 `i2c_master` 总线句柄（`i2c_master_bus_handle_t`）
- Produces（驱动 C API，地址为 7 位）：
  - `esp_err_t axp2101_init(i2c_master_bus_handle_t bus, axp2101_handle_t* out);`（地址 0x34；只配置充电与电源键开机时间，**不改动任何电源轨的电压与开关**，沿用 MyOS 的 `configure_charging`/`configure_power_key_press_on_time`）、`esp_err_t axp2101_read_status(axp2101_handle_t, axp2101_status_t* out);`（字段：`battery_percent`、`battery_mv`、`charging`、`vbus_present`）、`esp_err_t axp2101_power_off(axp2101_handle_t);`
  - `esp_err_t pcf85063_init(i2c_master_bus_handle_t, pcf85063_handle_t*);`（地址 0x51）、`pcf85063_get_unix(handle, int64_t*)`、`pcf85063_set_unix(handle, int64_t)`（BCD 编解码，UTC）
  - `esp_err_t qmi8658_init(i2c_master_bus_handle_t, qmi8658_handle_t*);`（地址 0x6B；加速度量程 ±4 g，陀螺仪 ±512 dps，ODR 默认 125 Hz）、`qmi8658_read(handle, qmi8658_sample_t*)`（单位 g、dps）、`qmi8658_set_odr(handle, uint32_t hz)`
- Produces（HAL 适配，id 与 model 按 spec 5.5 节）：`EspPower`（`power.main`/`AXP2101`，caps battery true、charging_status true）、`EspClock`（`clock.main`/`PCF85063`；`App` 启动后用 RTC 时间调用 `settimeofday` 设置系统时间，这一步放在 board.cpp 初始化 Clock 之后）、`EspImu`（`imu.main`/`QMI8658`，caps accel true、gyro true、magnetometer false、max_rate_hz 1000）、`EspGpioButton(gpio_num_t, bool active_low, std::string id, std::string model)`：`button.power`（GPIO16，**高电平有效**——真机实测更正，见硬件文档"已知勘误"；model `axp2101-pwrkey`）、`button.vol_up`（GPIO18，低电平有效）、`button.vol_down`（GPIO0，低电平有效），model 均为 `gpio-button`（电源键除外）；GPIO 配置为输入并启用上拉（GPIO16 改为启用内部下拉，原写的"板上已有外部上拉"有误）

- [ ] **Step 1: 移植三个驱动**：保留 MyOS 中已验证的寄存器序列和换算公式，删除 MyOS 特有的睡眠与 UI 逻辑；每个文件头注明 `Ported from MyOS (MIT, Copyright (c) 2026 ShouChen)`。
- [ ] **Step 2: 实现 HAL 适配并在 board.cpp 中注册**（每个设备初始化失败都记 `Failed` 并附 `esp_err_to_name`，然后继续）；构建 → Expected: 成功。
- [ ] **Step 3: 烧录验证（需要真机）** — Expected：IMU 小球随设备倾斜平稳移动，平放时 az 约为 1.0 g；电量百分比合理，插拔 USB 时充电状态在 1 秒内更新；按任意键都有对应提示；电源键短按灭屏、再按亮屏；长按约 1 秒弹出关机确认，确认后整机断电，关机状态下长按电源键能开机；用 `settimeofday` 测试法验证 RTC：在 monitor 中观察时间，拔掉 USB 和电池重新上电后时间仍连续（若 RTC 从未设置过，先在代码临时调用一次 `set_unix` 写入当前时间，验证后删除这段临时代码）；manifest 中 `imu.main` 的 `magnetometer` 为 `false`。
- [ ] **Step 4: 在 `docs/references.md` 记录三个驱动的 MyOS 来源**
- [ ] **Step 5: Commit**

```bash
git add drivers components/wisp_port_esp boards docs/references.md
git commit -m "feat(drivers): port AXP2101, PCF85063, QMI8658 and add GPIO buttons"
```

---

### Task 18: 音频、存储介质与 NVS 配置

**Files:**
- Create: `components/wisp_port_esp/dev/{esp_audio,esp_storage}.cpp` 及头文件、`components/wisp_port_esp/settings_nvs.cpp`、`components/wisp_port_esp/include/wisp/port/esp/settings_nvs.hpp`
- Modify: `boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp`、`firmware/main/main.cpp`（settings 改为 NVS）、`firmware/main/idf_component.yml`（追加 `joltwallet/littlefs: "1.23.1"`，`esp_codec_dev` 由 bmgr 间接引入，同样从 `dependencies.lock` 读出实际版本后锁定）

**Interfaces:**
- Consumes: bmgr 的 `audio_dac`/`audio_adc` 设备句柄（`dev_audio_codec_handles_t`，字段 `codec_dev`），`fs_sdcard` 设备，`esp_codec_dev_open`/`read`/`write`/`close`/`set_out_vol`/`set_in_gain`
- Produces：
  - `EspAudioOut`（`audio_out.main`/`ES8311`）：`open` 用 `esp_codec_dev_sample_info_t{bits 16, channel 1, sample_rate}`；`set_volume` 对应 `esp_codec_dev_set_out_vol`；功放由 bmgr 的 `gpio_pa_control` 控制
  - `EspAudioIn`（`audio_in.main`/`ES7210`）：以 2 声道打开，读取后两路取平均下混为单声道交给调用方；`set_gain` 对应 `esp_codec_dev_set_in_gain`
  - `EspInternalStorage`（`storage.int`/`littlefs`，caps removable false、fs `littlefs`）：`esp_vfs_littlefs_register`，`base_path "/int"`、`partition_label "storage"`、`format_if_mount_failed true`；容量取自 `esp_littlefs_info`；挂载失败时不注册设备，report 记 `Failed`（App 因此进入只读降级）
  - `EspSdStorage`（`storage.sd`/`sdcard-sdmmc`，caps removable true、fs `fat`）：挂载点使用 bmgr `fs_sdcard` 设备的挂载点（若 YAML 支持配置挂载点则设为 `/sd`）；`online()`：已挂载时用 `sdmmc_get_status` 检查卡是否仍在，失败则卸载（对 `fs_sdcard` 设备执行 bmgr 反初始化）并返回 false；未挂载时距上次尝试已过 2 s 才调用 `esp_board_manager_init_device_by_name("fs_sdcard")` 重试挂载；容量用 `esp_vfs_fat_info`
  - 启动时 SD 卡不在：仍然注册 `storage.sd`，report 记 `Missing`、原因 `"no card"`
  - `NvsSettingsBackend : core::SettingsBackend`：每个 Wisp namespace 对应一个 NVS namespace；值以 `to_json` 后的字符串用 `nvs_set_str` 保存，读取时 `from_json`；先调用 `validate_settings_name`；`nvs_flash_init` 返回 `ESP_ERR_NVS_NO_FREE_PAGES` 或 `ESP_ERR_NVS_NEW_VERSION_FOUND` 时擦除后重试一次

- [ ] **Step 1: 实现并注册音频、存储、NVS，构建** → Expected: 成功。
- [ ] **Step 2: 烧录验证（需要真机）** — Expected：点击"录音回放"后对着麦克风说话，3 秒后能从喇叭听到录音；首次启动内置存储容量约 5.9 MB；不插卡启动时 SD 显示"缺失"，运行中插入后 2 秒内变为"正常"并显示容量，拔出后 2 秒内变回"缺失"，过程中不崩溃；修改亮度（临时在 monitor 中观察日志，或在代码中调用 `Settings::set` 后重启）后，重启设备，设置仍然生效；启动报告中除 SD（视是否插卡而定）外全部为 `ok`，`notes` 中不再有 `"storage":"readonly"`。
- [ ] **Step 3: Commit**

```bash
git add components/wisp_port_esp boards firmware
git commit -m "feat(firmware): add audio codecs, LittleFS/SD storage and NVS settings"
```

---

### Task 18b: 熄屏待机与省电

（执行期由用户批准插入，见 spec 6.7 节和验收第 5 条。范围只有"亮屏 ↔ 待机"两态，不含后台场景和唤醒锁，这两项见 spec 1.6。）

**Files:**
- Create：
  - `components/wisp_app/include/wisp/app/power_manager.hpp`
  - `components/wisp_app/src/power_manager.cpp`
  - `tests/app/power_manager_test.cpp`
- Modify：
  - `components/wisp_hal/include/wisp/hal/touch.hpp`（新增带默认实现的 `set_power`）
  - `components/wisp_ui`（`UiRuntime::set_paused`）
  - `components/wisp_core`（`ButtonService::set_poll_interval_ms`）
  - `components/wisp_app/include/wisp/app/app.hpp`（`Platform::set_standby`）
  - `components/wisp_app/src/{app.cpp,self_check_screen.cpp}`
  - `components/wisp_port_esp/dev/{esp_display,esp_touch}.cpp`
  - `firmware/main/main.cpp`、`firmware/sdkconfig.defaults`
  - `simulator/main.cpp`
  - `tests/support/fake_devices.hpp`
  - `tests/ui/*`、`tests/core/button_service_test.cpp`

**Interfaces:**
- **`hal::Touch`**：新增 `virtual Result<void> set_power(bool on) { return Result<void>::err(ErrorCode::Unsupported, "touch sleep unsupported"); }`。这是非纯虚函数，现有实现不用改动。
- **`hal::Display::set_power(false)`**：语义扩展为"面板进入睡眠模式"。ESP 实现依次调用 `esp_lcd_panel_disp_on_off(false)` 和 `esp_lcd_panel_disp_sleep(true)`；`set_power(true)` 按相反顺序恢复。
- **`EspTouch::set_power`**：使用 `esp_lcd_touch_enter_sleep`/`exit_sleep`。如果 CST9217 驱动不支持，就返回 `Unsupported`。
- **`ui::UiRuntime::set_paused(bool)`**：可以在任意线程调用。
  - 暂停期间，UI 任务不再调用 `lv_timer_handler`，也不刷新屏幕，只阻塞等待并执行 `post()` 进来的任务；
  - 恢复时先 `lv_obj_invalidate(lv_screen_active())`，再继续正常循环；
  - 提供 `bool paused() const`。
- **`core::ButtonService::set_poll_interval_ms(uint32_t)`**：可以在运行中切换。默认 20 ms，待机时用 50 ms。
- **`app::Platform`**：新增 `std::function<void(bool standby)> set_standby;`，可以为空。
  - ESP 端：启动时 `esp_pm_configure({max 240, min 80, light_sleep_enable=false})`；待机时改为 `light_sleep_enable=true`，退出待机后改回。
  - 模拟器端：只输出一条 `WLOG_I`。
- **`app::PowerManager`**：
  - 构造：`PowerManager(core::EventBus&, std::shared_ptr<hal::Display>, std::shared_ptr<hal::Touch>, ui::UiRuntime*, core::ButtonService*, std::function<void(bool)> set_standby, core::Settings settings)`；
  - 成员函数：`enter_standby()`、`exit_standby()`、`bool standby() const`、`note_activity()`、`tick(uint64_t now_ms)`；
  - 进入或退出待机时，在事件总线上发布 `power.state`，载荷为 `{"state":"standby"|"active"}`；
  - 进入和退出都是幂等的，Touch 返回 `Unsupported` 时不算错误；
  - 自动熄屏时长读取 Settings 的 `power`/`screen_off_s`，默认 60，0 表示关闭；
  - 闲置的判定依据是距离上一次 `note_activity()` 超过该时长。触摸输入（UiRuntime 的 indev 读到触点时）和任意按键事件都要调用 `note_activity()`。
- **电源键**：`PowerKeyController` 的 `ScreenOff`/`ScreenOn` 动作改为调用 `PowerManager` 的 `enter_standby`/`exit_standby`，不再直接调用 `Display::set_power`。待机时按任意键都会唤醒。
- **sdkconfig.defaults**：新增 `CONFIG_PM_ENABLE=y`、`CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`。
  - 需要核实 USB-Serial-JTAG 与自动浅睡眠的相互影响，例如 `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION`。原则是：连着 USB 调试时可以不睡，但用电池供电时必须能睡；
  - 结论写进 `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`。

- [ ] **Step 1: 写失败的测试**
  - `PowerManager`：`enter_standby` 之后，FakeDisplay `powered()==false`，UiRuntime `paused()`，按键轮询周期为 50，`set_standby(true)` 被调用一次，并收到 `power.state` 事件；`exit_standby` 全部恢复；连续两次 enter 只生效一次；FakeTouch 返回 Unsupported 时 `enter_standby` 仍然成功；`screen_off_s=1` 时，`tick` 超过 1000 ms 后进入待机，`note_activity` 会重置计时；`screen_off_s=0` 时永不自动熄屏。
  - `UiRuntime`：暂停期间 `post()` 的任务照样执行，但 flush 计数不增加；恢复后会有 flush。
  - `ButtonService`：切到 50 ms 后，短按和长按的判定仍然正确，长按阈值仍是 1000 ms。
- [ ] **Step 2: 运行，确认失败**
- [ ] **Step 3: 实现 PowerManager、UiRuntime 暂停、按键轮询切换和 Touch 睡眠接口，接入 App 和自检界面；然后实现 ESP 端（面板睡眠、触摸睡眠、esp_pm 与 tickless idle），并接上模拟器的钩子**
- [ ] **Step 4: 运行，确认通过**
  - `bash tools/ci.sh` 全部 PASS；
  - 固件从干净状态执行 set-target → bmgr → build，构建成功。
- [ ] **Step 5: 烧录验证（需要真机）**
  - 短按电源键后，屏幕熄灭、触摸无响应；再短按，300 ms 内亮屏，界面完整重绘；
  - 闲置 60 s 后自动熄屏；
  - 断开 USB 数据线，只用电池供电，在电池端串接电流表，分别测亮屏静止和熄屏待机的平均电流。待机电流应不超过亮屏的 1/10。实测值记入硬件文档。
- [ ] **Step 6: Commit**

```bash
git add components boards firmware simulator tests docs/hardware
git commit -m "feat(app): add screen-off standby with panel/touch sleep and light sleep"
```

---

### Task 19: 文档与整体验收

**Files:**
- Create: `README.md`、`docs/architecture.md`、`docs/porting-guide.md`、`docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`、`THIRD_PARTY_NOTICES.md`
- Modify: `docs/references.md`

**Interfaces:**
- `README.md`：项目简介（一句话加小鬼火的定位）、两条快速开始路径（模拟器：依赖安装、构建、运行与按键说明；真机：安装 ESP-IDF v6.0.3、执行 `idf.py bmgr`、build、flash）、目录结构、`tools/ci.sh` 用法、许可证。
- `docs/architecture.md`：分层图、依赖规则、启动流程、事件主题清单（Task 5、7、9、11 中出现的全部主题）、存储逻辑区，以及"相对 spec 的细化"一节（照抄本计划开头的 8 条）。
- `docs/porting-guide.md`：新增一块板需要哪些步骤（写 bmgr YAML，写 `board.cpp` 实现 `create_board()`，为缺少的芯片添加驱动，设备 id 命名约定，能力字段约定，用 `--board` 构建）以及验收办法（对照自检界面和启动报告）。
- `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`：在 MyOS 硬件文档的基础上修正：触摸芯片为 CST9217（地址 0x5A）；SD 卡为 1-bit SDMMC（CLK2/CMD1/D0=3）；ES8311 的 7 位地址为 0x18。
- `THIRD_PARTY_NOTICES.md`：ESP-IDF（Apache-2.0）、LVGL（MIT）、ArduinoJson（MIT）、GoogleTest（BSD-3-Clause，仅测试用）、SDL2（zlib，仅模拟器）、esp_board_manager / esp_codec_dev / esp_lcd_co5300（以组件仓库中声明的许可为准）、waveshare/esp_lcd_touch_cst9217（以组件仓库声明为准）、joltwallet/littlefs（MIT，内含 littlefs 本体为 BSD-3-Clause）、Noto Sans CJK SC 字形（SIL OFL 1.1，经 MyOS 生成）、MyOS 驱动移植（MIT）、ESP-Claw 板级定义参照（Apache-2.0）。每一项写明用途、许可证、分发方式。

- [ ] **Step 1: 写文档**
- [ ] **Step 2: 全量本机检查** — Run: `bash tools/ci.sh` → Expected: 退出码 0。
- [ ] **Step 3: 固件构建** — Run: `bash -c '. ~/esp/esp-idf-v6.0.3/export.sh && cd firmware && idf.py bmgr -c ../boards -b waveshare_esp32_s3_touch_amoled_2_16 && idf.py build'` → Expected: 成功。
- [ ] **Step 4: 真机验收（需要真机，逐条对照 spec 第 10 节）** — 记录结果：①从上电到出现自检界面的时间 < 3 s（在串口日志中查看 `boot_ms`）；②8 类设备的状态与行为全部符合；③电源键的三种行为符合；④串口输出的 manifest 中 `imu.main.caps.magnetometer == false`；⑤手指持续上下滚动自检列表 10 s，串口 `WISP_PERF` 的 fps ≥ 30（本板的验收线见 spec 第 10 节"已记录的偏差"），记录 fps、cpu、flush_ms 三个数值写入 `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md` 的"性能基准"一节。任何一项不通过，回到对应任务修复，不在本任务中绕过。
- [ ] **Step 5: Commit**

```bash
git add README.md docs THIRD_PARTY_NOTICES.md
git commit -m "docs: add README, architecture, porting guide and third-party notices"
```
