# Wisp 子项目 2：连接与管理后台 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 Wisp 能按需配网、连上局域网，并通过浏览器管理后台完成配对、查看设备、修改设置和配置 LLM；真机与模拟器跑同一套服务与路由。

**Architecture:** `hal::Wifi` 设备 + 可移植的 `wisp_net`（Wi-Fi 状态机、配对、资源包、HTTP 接口类型）与 `wisp_admin`（路由与 WebSocket），两者只依赖 core/hal；`wisp_port_esp` 封装 `esp_wifi`/`esp_http_server`/`esp_http_client`/mDNS，`wisp_port_sim` 用 POSIX socket 手写 HTTP/WS 服务、libcurl 客户端和假 Wi-Fi；前端是 `web/` 下免构建的 Preact + htm，由 `tools/pack_assets.py` 打成 WispPack 写进 `assets` 分区。

**Tech Stack:** ESP-IDF v6.0.3（`esp_wifi`、`esp_http_server`、`esp_http_client` + `esp_crt_bundle`）、`espressif/mdns`（锁定精确版本，写入 `dependencies.lock`）、LVGL 9.5.0（`lv_qrcode`）、htm 3.1.1 的 `preact/standalone.module.js`（内含 Preact 与 hooks）、libcurl（仅模拟器）、GoogleTest、Python 3 标准库。

**Spec:** `docs/superpowers/specs/2026-11-03-connectivity-admin-design.md`（下文"spec N"均指它；"基础 spec"指 `2026-11-01-foundation-design.md`）

## 相对 spec 的细化（写计划时裁定，执行前请用户确认）

1. **`LlmConfigService` 与连通测试拆开**：spec 2.1 把 `LlmConfigService` 放在 `wisp_core`，但 `test()` 要用 `wisp_net` 的 `HttpClient`，core 不能反向依赖 net。裁定：配置的存取留在 core；连通测试改为 `wisp_net` 里的自由函数 `net::probe_llm(const core::LlmConfigService&, HttpClient&)`。
2. **HTTP 服务端只接一个总处理函数**：`HttpServer::start()` 接收一个 `HttpHandler` 和一组 WebSocket 回调，路由全部在 `wisp_admin` 里完成。ESP 实现用通配 URI `/*` 加一个 `/api/v1/ws` 的 WebSocket URI。
3. **Wi-Fi 设备用轮询，不用回调**：`hal::Wifi::link()` 返回当前链路状态，`WifiService::tick(now_ms)` 由 `App` 的定时器每 250 ms 调用一次。这与 `PowerManager::tick()` 的模式一致，测试可以直接注入时间。
4. **`Platform` 新增注入点**：`random`（随机字节）、`http_server`、`http_client`、`assets`（资源包字节区间）、`reboot`、`read_line`（串口行输入）。
5. **新增 `ErrorCode::PermissionDenied`（→403）与 `ErrorCode::RateLimited`（→429）**。spec 6.1 的状态码表补上 429，用于配对锁定。
6. **SHA-256 与 base64url 放在 `wisp_base`**：可移植层不能用 mbedTLS，这里自己写一份精简实现，用标准测试向量验证。
7. **恢复出厂设置需要 `SettingsBackend::erase_all()`**：JSON 文件后端清空文件，NVS 后端调用 `nvs_erase_all` 并 commit。
8. **亮度在待机时修改**：只记下新值，唤醒时再应用，不点亮屏幕（新增 `PowerManager::set_brightness()`）。
9. **模拟器重启**：`Platform::reboot` 在模拟器中输出一行 `WISP_REBOOT` 并以退出码 0 结束进程；真机调用 `esp_restart()`。
10. **配网热点的 DNS 劫持**放在 `wisp_port_esp`（随 `start_ap` 启停，UDP 53，所有 A 查询都回答热点地址）；模拟器不实现。mDNS（`wisp.local`）也由 `EspWifi` 在 STA 拿到 IP 时自己启动。

## Global Constraints

- 可移植组件（现有六个加上 `wisp_net`、`wisp_admin`）禁止 include 平台头文件（AGENTS.md §3、`tools/check_layering.py` 的 `FORBIDDEN_*`），另外禁止 `curl/`；外部依赖只允许 ArduinoJson 和 LVGL。
- C++20，`-fno-exceptions -fno-rtti`，错误一律用 `wisp::Result<T>`；命名空间 `wisp::net`、`wisp::admin`；头文件 `wisp/net/*.hpp`、`wisp/admin/*.hpp`。
- 服务层约定：路由只调服务，不碰驱动（基础 spec 3.3）。
- **API Key 只能写入、不能读出**：任何 HTTP 响应、事件、日志、错误信息里都不得出现 Key 的任何部分（spec 5.3）。Wi-Fi 密码、配对码、令牌明文同样不进日志和事件（spec 4.4、5.2）。
- Settings 的 namespace 与 key 都不超过 15 个字符：本计划新增 `net`/`idle_off_min`。
- 持久化一律走 `StorageManager::write_atomic`：`<System 根>/net/wifi.json`、`<System 根>/net/clients.json`、`<System 根>/secrets/llm.json`。
- 固定数值（来自 spec）：退避 5 s 起、每次翻倍、上限 5 min；最多保存 5 个网络；热点 10 min 无人连接自动关闭，配网成功后保留 30 s；待机超过 `net`/`idle_off_min`（默认 10）分钟断开 Wi-Fi；配对码 6 位、有效 2 min、连续错 5 次锁 5 min；令牌 32 字节随机、只存 SHA-256、最多 8 个、Cookie `wisp_token; HttpOnly; SameSite=Strict; Path=/`、有效期 1 年；请求体 ≤ 8 KB；同时 ≤ 7 个连接；WebSocket ≤ 2 个；`/llm/test` 超时 10 s；网页 gzip 后 ≤ 150 KB；`assets` 分区 ≥ 1 MB。
- 热点 SSID 为 `Wisp-` 加 MAC 末 4 位十六进制（大写）；密码为 8 位 `[A-Za-z0-9]`，每次开热点重新生成。
- 后台端口 8900（真机与模拟器相同，`http://wisp.local:8900`）；模拟器 HTTP 服务只监听 `127.0.0.1`，默认端口 8900。真机热点运行期间另有一个 80 端口的强制门户重定向服务（spec 4.3）。
- 机器可读输出改动（`WISP_BOOT_REPORT` 新增 `notes.net`）必须同步更新 `tests/smoke/sim_smoke.py` 和 `docs/architecture.md`。
- 设备端新增的卡片按基础 spec 5.6 的安全区规则布局。
- 第三方文件（htm/Preact、mDNS 组件）登记到 `docs/references.md` 与 `THIRD_PARTY_NOTICES.md`。
- 注释规则见 AGENTS.md §5（不写修改历史、不引用 `.superpowers/`、不写评审编号）。提交信息用 Conventional Commits，结尾附 `Co-Authored-By` 行。

## Review Focus

1. **令牌的"最近访问时间"写放大**：每个请求都写 `clients.json` 会磨损 Flash，也会拖慢请求。期望：内存里每次都更新，落盘最多每小时一次（或撤销、新增时立即落盘）。测试见 Task 6。
2. **待机时改亮度**：网页在设备熄屏时修改亮度，不应点亮屏幕，唤醒后应显示新亮度。测试见 Task 7。
3. **配网过程中已保存的网络恰好恢复**：热点开着时 STA 连上了旧网络，热点不能立刻关掉，否则手机上的配网页会断开；只有"配网会话里新添加的网络连接成功"才触发 30 s 后关闭。测试见 Task 5。
4. **Cookie 或请求头大小写与重复**：`Cookie` 里有多个键值对、请求头大小写混用（`x-wisp`、`X-WISP`）时都要正确识别；`Host` 带端口号（`192.168.1.5:80`、`localhost:8080`）也要通过校验。测试见 Task 8。
5. **WebSocket 客户端处理很慢或已断开**：日志推送不能阻塞写日志的线程，也不能无限占内存；积压超过上限就丢弃最旧的，并发送 `dropped` 通知。测试见 Task 10。

---

## 文件结构总览

```
boards/waveshare_amoled_216/
  partitions.csv                 # 从 firmware/ 迁来
  sdkconfig.defaults.board       # Flash/PSRAM/控制台/分区表路径
components/
  wisp_base/  include/wisp/base/{sha256,base64}.hpp  src/{sha256,base64}.cpp
  wisp_hal/   include/wisp/hal/wifi.hpp
  wisp_core/  include/wisp/core/{llm_config,system_settings}.hpp  src/...
  wisp_net/   include/wisp/net/{http,asset_pack,wifi_service,pairing_service,llm_probe}.hpp  src/...
  wisp_admin/ include/wisp/admin/{admin_server,ws_hub}.hpp  src/{admin_server,routes_*.cpp,ws_hub,filters}.cpp
  wisp_app/   src/{net_section,net_cards,console,factory_reset}.cpp  include/wisp/app/console.hpp
  wisp_port_sim/ net/{sim_http_server,sim_http_client,sim_wifi,sha1}.cpp
  wisp_port_esp/ net/{esp_wifi,esp_http_server,esp_http_client,captive_dns}.cpp  console_esp.cpp
web/
  index.html  setup.html  app/*.js  styles/{tokens,components}.css  vendor/htm-preact-3.1.1.mjs
tools/
  pack_assets.py  pack_assets_test.py  fake_openai.py
tests/
  base/{sha256,base64}_test.cpp  net/*_test.cpp  admin/*_test.cpp  core/{llm_config,system_settings}_test.cpp
  sim/sim_http_server_test.cpp  app/console_test.cpp  smoke/admin_smoke.py
  support/fake_wifi.hpp  support/fake_http.hpp
```

---

### Task 1: 板级分区表与 sdkconfig 迁移

**Files:**
- Move: `firmware/partitions.csv` → `boards/waveshare_amoled_216/partitions.csv`
- Create: `boards/waveshare_amoled_216/sdkconfig.defaults.board`
- Modify: `firmware/sdkconfig.defaults`（删掉板级配置项）、`docs/porting-guide.md`（新增"分区约定"一节，内容即 spec 3.2 的表）、基础 spec 7.4 标题改为"参考板分区表（Waveshare 2.16，16MB）"并在节末引用子项目 2 spec 第 3 节、`docs/hardware/waveshare_amoled_216.md`（分区表的位置）

**Interfaces:**
- Produces：`boards/<板名>/partitions.csv` 是本板唯一的分区表；Task 4 的 `pack_assets.py --partitions` 从这里读取 `assets` 分区的大小。

- [ ] **Step 1:** 把下列配置项从 `firmware/sdkconfig.defaults` 移到 `sdkconfig.defaults.board`：`CONFIG_ESPTOOLPY_FLASHMODE_QIO`、`CONFIG_ESPTOOLPY_FLASHFREQ_80M`、`CONFIG_ESPTOOLPY_FLASHSIZE_16MB`、`CONFIG_SPIRAM*`、`CONFIG_ESP_CONSOLE_*`；并在其中加入 `CONFIG_PARTITION_TABLE_CUSTOM=y` 和 `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="../boards/waveshare_amoled_216/partitions.csv"`（路径相对 `firmware/`）。`firmware/sdkconfig.defaults` 的文件头注释改为"与板子无关的公共配置"。
- [ ] **Step 2: 从干净状态验证**

Run: `bash -c '. ~/esp/esp-idf-v6.0.3/export.sh >/dev/null && cd firmware && rm -f sdkconfig && idf.py bmgr -c ../boards -b waveshare_amoled_216 && idf.py build'`
Expected: 构建成功；`grep -E 'FLASHSIZE_16MB=y|SPIRAM_MODE_OCT=y|CUSTOM_FILENAME' firmware/sdkconfig` 三项都存在；`build/partition_table/partition-table.bin` 的内容与迁移前一致（用 `gen_esp32part.py` 解析后对比）。
- [ ] **Step 3: 跑 `bash tools/ci.sh`**，确认主机侧不受影响。
- [ ] **Step 4: Commit** — `build(boards): move the partition table and board sdkconfig into the board directory`

---

### Task 2: SHA-256、base64url 与新错误码

**Files:**
- Create: `components/wisp_base/include/wisp/base/{sha256,base64}.hpp`、`src/{sha256,base64}.cpp`
- Modify: `components/wisp_base/include/wisp/base/error.hpp` 及 `to_string`、`components/wisp_base/CMakeLists.txt`
- Test: `tests/base/sha256_test.cpp`、`tests/base/base64_test.cpp`

**Interfaces:**
- Produces（`namespace wisp`）：
  - `std::array<uint8_t, 32> sha256(std::span<const uint8_t> data);`
  - `std::string to_hex(std::span<const uint8_t> data);`（小写）
  - `std::string base64url_encode(std::span<const uint8_t> data);`（不带 `=` 填充）
  - `std::string base64_encode(std::span<const uint8_t> data);`（标准字母表，带填充；模拟器 WebSocket 握手要用）
  - `ErrorCode::PermissionDenied`、`ErrorCode::RateLimited`（追加到枚举末尾，`to_string` 返回 `"PermissionDenied"`/`"RateLimited"`）

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(Sha256, KnownVectors) {
    EXPECT_EQ(wisp::to_hex(wisp::sha256(bytes(""))),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(wisp::to_hex(wisp::sha256(bytes("abc"))),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 跨越一个 64 字节块边界的输入
    EXPECT_EQ(wisp::to_hex(wisp::sha256(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}
TEST(Base64, UrlSafeNoPadding) {
    EXPECT_EQ(wisp::base64url_encode(bytes("\xfb\xff")), "-_8");
    EXPECT_EQ(wisp::base64_encode(bytes("\xfb\xff")), "+/8=");
    EXPECT_EQ(wisp::base64_encode(bytes("")), "");
}
TEST(ErrorCode, NewCodesHaveNames) {
    EXPECT_STREQ(wisp::to_string(wisp::ErrorCode::RateLimited), "RateLimited");
}
```

- [ ] **Step 2: 运行，确认失败** — Run: `cmake --build build/sim && ctest --test-dir build/sim -R 'Sha256|Base64|ErrorCode' --output-on-failure` → 编译失败（头文件不存在）。
- [ ] **Step 3: 实现**：按 FIPS 180-4 写 SHA-256，不引入第三方代码。
- [ ] **Step 4: 运行，确认通过**（同上命令）→ PASS。
- [ ] **Step 5: Commit** — `feat(base): add sha256, base64 and permission/rate-limit error codes`

---

### Task 3: `hal::Wifi` 接口与 `FakeWifi`

**Files:**
- Create: `components/wisp_hal/include/wisp/hal/wifi.hpp`、`tests/support/fake_wifi.hpp`
- Modify: `components/wisp_hal/include/wisp/hal/device.hpp` 与 `src/device.cpp`（新增 `DeviceClass::Network` ↔ `"network"`）
- Test: `tests/hal/wifi_types_test.cpp`、`tests/core/device_registry_test.cpp`（增加一个用例）

**Interfaces:**
- Produces（`namespace wisp::hal`）：

```cpp
struct WifiScanEntry { std::string ssid; int rssi = 0; bool secured = true; };
enum class WifiLinkState { Idle, Connecting, Connected, Failed };
enum class WifiFailReason { None, NotFound, AuthFailed, Other };
struct WifiLink {
    WifiLinkState state = WifiLinkState::Idle;
    std::string ssid; std::string ip; int rssi = 0;
    WifiFailReason reason = WifiFailReason::None;
};
class Wifi : public Device {
public:
    static constexpr DeviceClass kClass = DeviceClass::Network;
    virtual Result<std::vector<WifiScanEntry>> scan() = 0;              // 阻塞，最长约 3 s
    virtual Result<void> connect(const std::string& ssid, const std::string& password) = 0;  // 异步，结果看 link()
    virtual Result<void> disconnect() = 0;
    virtual WifiLink link() = 0;
    virtual Result<std::string> start_ap(const std::string& ssid, const std::string& password) = 0;  // 返回热点 IP
    virtual Result<void> stop_ap() = 0;
    virtual int ap_clients() = 0;
    virtual std::array<uint8_t, 6> mac() = 0;
    virtual Result<void> set_power_save(bool on) = 0;
};
```
  - caps 约定：`{"ap": true, "bands": ["2.4GHz"]}`。
  - `tests/support/fake_wifi.hpp`：`class FakeWifi : public hal::Wifi`，公开字段 `std::vector<WifiScanEntry> networks`、`std::map<std::string, std::string> passwords`（SSID→正确密码）、`bool reachable = true`、`int ap_client_count = 0`；`connect()` 记录调用次数 `connect_calls`，下一次 `link()` 返回结果：SSID 存在、密码正确且 `reachable` 为真时为 `Connected`（IP 为 `192.168.1.50`），否则为 `Failed` 并附原因。`start_ap()` 返回 `"192.168.4.1"`，`ap_active` 字段记录热点状态。

- [ ] **Step 1: 写失败的测试**：`WifiTypes.NetworkClassRoundTrips`（`class_from_string("network") == DeviceClass::Network`）；`DeviceRegistry.ManifestListsNetworkDevice`（注册 `FakeWifi("net.wifi")` 后，manifest 里该条目的 `class` 为 `"network"`，`caps.ap` 为 `true`）。
- [ ] **Step 2: 运行，确认失败。**
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 运行，确认通过。**
- [ ] **Step 5: Commit** — `feat(hal): add the Wifi device interface`

---

### Task 4: `wisp_net` 组件、HTTP 类型与 WispPack

**Files:**
- Create: `components/wisp_net/CMakeLists.txt`、`include/wisp/net/{http,asset_pack}.hpp`、`src/{http,asset_pack}.cpp`、`tools/pack_assets.py`、`tools/pack_assets_test.py`、`tests/net/fixtures/web/{index.html,app/main.js,app/util.js,logo.png}`
- Modify: `simulator/CMakeLists.txt`（`add_subdirectory` wisp_net）、`tests/CMakeLists.txt`、`tools/check_layering.py`（`COMPONENTS` 与 `LAYER_INCLUDE_PREFIX` 加入 `wisp_net` → `wisp/net/`，位于 `wisp_core` 之后、`wisp_ui` 之前，并在 `FORBIDDEN_PREFIXES` 加入 `"curl/"`）、`tools/ci.sh`（运行 `pack_assets_test.py`）
- Test: `tests/net/asset_pack_test.cpp`、`tests/net/http_test.cpp`

**Interfaces:**
- Produces（`namespace wisp::net`，`http.hpp`）：

```cpp
enum class Method { Get, Post, Put, Delete, Other };
enum class Iface { Sta, Ap, Loopback };
struct HttpRequest {
    Method method = Method::Get;
    std::string path;                               // 不含查询串
    std::string query;
    std::map<std::string, std::string> headers;     // 键一律小写
    std::string body;
    std::string remote_ip;
    Iface iface = Iface::Sta;
    std::string header(std::string_view lower_name) const;  // 缺失返回空串
    std::string cookie(std::string_view name) const;
};
struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::span<const uint8_t> static_body;            // 非空时优先于 body（零拷贝）
    static HttpResponse json(int status, const Value& v);
    static HttpResponse error(int status, std::string_view code, std::string_view message);
};
using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;
class WsConnection {
public:
    virtual ~WsConnection() = default;
    virtual int id() const = 0;
    virtual bool send_text(std::string_view text) = 0;   // 不阻塞：发送队列满时返回 false
    virtual void close() = 0;
};
struct WsHandlers {
    std::function<bool(std::shared_ptr<WsConnection>, const HttpRequest&)> on_open;  // 返回 false 拒绝
    std::function<void(int id, std::string_view text)> on_message;
    std::function<void(int id)> on_close;
};
class HttpServer {
public:
    virtual ~HttpServer() = default;
    virtual Result<void> start(uint16_t port, HttpHandler handler, WsHandlers ws) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;
    virtual uint16_t port() const = 0;   // 实际监听的端口（start(0, ...) 时由系统分配）
};
enum class TransportError { None, Dns, Connect, Tls, Timeout, Other };
struct HttpClientRequest {
    Method method = Method::Get; std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body; uint32_t timeout_ms = 10000;
};
struct HttpClientResponse { TransportError transport = TransportError::None; int status = 0; std::string body; };
class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual HttpClientResponse request(const HttpClientRequest& req) = 0;
};
int http_status_for(ErrorCode code);  // InvalidArgument→400, PermissionDenied→403, NotFound→404,
                                      // AlreadyExists/Busy→409, QuotaExceeded→409, RateLimited→429,
                                      // Unsupported→501, Timeout→504, StorageUnavailable/NoMemory→503, 其余→500
```
- Produces（`asset_pack.hpp`）：

```cpp
struct AssetEntry { std::string path; std::string mime; bool gzip; std::string etag; std::span<const uint8_t> data; };
class AssetPack {
public:
    static Result<AssetPack> parse(std::span<const uint8_t> bytes);
    const AssetEntry* find(std::string_view path) const;  // "/" 映射到 "/index.html"
    size_t size() const;
};
```
- WispPack 二进制格式（`pack_assets.py` 与 `AssetPack::parse` 必须一致，全部小端）：头部 16 字节 = 魔数 `WPAK`、`u16 version=1`、`u16 count`、`u32 table_bytes`、`u32 total_bytes`；之后 `count` 条记录，每条为 `u16 path_len, path, u8 mime_len, mime, u8 flags(bit0=gzip), u32 offset, u32 length, 32 字节 sha256`；文件内容从头部起算的 `offset` 开始，按 4 字节对齐。`etag` = `"` + sha256 前 8 字节的十六进制 + `"`。
- `pack_assets.py <src_dir> -o <out.wpak> [--partitions <csv>] [--budget-kb N]`：
  - 后缀为 html/js/mjs/css/svg/json/txt 的文件用 gzip 压缩（`mtime=0`，保证输出可复现），其他原样存放；
  - 检查每个 `.js`/`.mjs` 里 `import ... from './x.js'` 和 `import('./x.js')` 形式的相对路径都在包内；
  - 给了 `--partitions` 时，包大小超过该表中 `assets` 分区的大小就失败；给了 `--budget-kb` 时，所有 gzip 文件的总大小超过预算就失败；
  - 失败时以非零码退出，并打印原因。

- [ ] **Step 1: 写失败的 Python 测试**（`pack_assets_test.py`，`unittest`）：`test_roundtrip_header`（打包 fixtures 后，魔数、count、对齐都正确）、`test_reproducible`（两次打包输出逐字节相同）、`test_missing_import_fails`（临时目录里放一个 import 了 `./nope.js` 的文件，退出码非零，输出包含 `nope.js`）、`test_partition_overflow_fails`（`assets` 大小为 0x100 的临时 csv）。
- [ ] **Step 2: 写失败的 C++ 测试**：CMake 在构建时用 `pack_assets.py` 把 `tests/net/fixtures/web` 打成 `${CMAKE_BINARY_DIR}/fixture.wpak`，通过编译定义 `WISP_FIXTURE_WPAK` 传入路径。

```cpp
TEST(AssetPack, ParsesFixtureAndMapsRoot) {
    auto pack = wisp::net::AssetPack::parse(read_all(WISP_FIXTURE_WPAK)).value();
    const auto* index = pack.find("/");
    ASSERT_NE(index, nullptr);
    EXPECT_EQ(index->path, "/index.html");
    EXPECT_TRUE(index->gzip);
    EXPECT_EQ(index->mime, "text/html; charset=utf-8");
    EXPECT_FALSE(pack.find("/logo.png")->gzip);
    EXPECT_EQ(pack.find("/missing"), nullptr);
}
TEST(AssetPack, RejectsTruncatedAndCorrupt) {
    auto bytes = read_all(WISP_FIXTURE_WPAK);
    for (size_t n : {0u, 8u, 20u, static_cast<unsigned>(bytes.size() - 1)}) {
        EXPECT_FALSE(wisp::net::AssetPack::parse({bytes.data(), n})) << n;
    }
    bytes[0] = 'X';
    EXPECT_FALSE(wisp::net::AssetPack::parse(bytes));
}
TEST(Http, HeaderAndCookieLookupIsCaseInsensitiveAndMultiValued) {
    wisp::net::HttpRequest r;
    r.headers["cookie"] = "a=1; wisp_token=abc ; b=2";
    EXPECT_EQ(r.cookie("wisp_token"), "abc");
    EXPECT_EQ(r.cookie("c"), "");
}
TEST(Http, StatusMapping) {
    EXPECT_EQ(wisp::net::http_status_for(wisp::ErrorCode::RateLimited), 429);
    EXPECT_EQ(wisp::net::http_status_for(wisp::ErrorCode::PermissionDenied), 403);
}
```

- [ ] **Step 3: 运行，确认失败。**
- [ ] **Step 4: 实现**：`parse` 校验所有偏移和长度都落在 `bytes` 之内（防止越界读）。
- [ ] **Step 5: 运行 `python3 tools/pack_assets_test.py` 与 `ctest -R 'AssetPack|Http\.'`，确认通过；跑 `python3 tools/check_layering.py`。**
- [ ] **Step 6: Commit** — `feat(net): add HTTP types and the WispPack asset format`

---

### Task 5: `WifiService`

**Files:**
- Create: `components/wisp_net/include/wisp/net/wifi_service.hpp`、`src/wifi_service.cpp`
- Test: `tests/net/wifi_service_test.cpp`

**Interfaces:**
- Consumes：Task 3 的 `hal::Wifi`、`FakeWifi`；`core::StorageManager`、`core::EventBus`、`core::Settings`
- Produces（`namespace wisp::net`）：

```cpp
enum class WifiState { Off, Connecting, Backoff, Connected };
const char* to_string(WifiState s);   // "off"/"connecting"/"backoff"/"connected"
struct WifiStatus { WifiState state; std::string ssid; std::string ip; int rssi; };
struct ApSession { std::string ssid; std::string password; std::string ip; };
struct ScanResult { std::string ssid; int rssi; bool secured; bool saved; };
struct WifiServiceConfig {
    uint32_t backoff_min_ms = 5000, backoff_max_ms = 300000;
    uint32_t ap_idle_timeout_ms = 600000, ap_linger_ms = 30000;
    size_t max_networks = 5;
};
using RandomFn = std::function<void(uint8_t* out, size_t len)>;
class WifiService {
public:
    WifiService(std::shared_ptr<hal::Wifi> wifi, core::StorageManager& storage, core::EventBus& bus,
                core::Settings settings /* ns "net" */, RandomFn random, WifiServiceConfig config = {});
    Result<void> init(uint64_t now_ms);       // 读取 net/wifi.json；有已保存的网络就开始连接
    void tick(uint64_t now_ms);
    void set_standby(bool standby, uint64_t now_ms);
    WifiStatus status() const;
    Result<std::vector<ScanResult>> scan();
    Result<void> add_network(std::string_view ssid, std::string_view password, uint64_t now_ms);
    Result<void> forget_network(std::string_view ssid);
    std::vector<std::string> saved_ssids() const;
    Result<ApSession> start_provisioning(uint64_t now_ms);
    Result<void> stop_provisioning();
    std::optional<ApSession> ap_session() const;   // 只给设备屏幕用
    void forget_all();                             // 恢复出厂设置
};
```
- 行为（全部为 spec 4.2–4.4 的固定值）：
  - `init`/重连时先 `scan()`，在已保存的网络里选 RSSI 最大的一个；扫描不到任何已保存的网络也按失败计入退避。
  - 失败后进入 `Backoff`，等待 5 s、10 s、20 s……最多 300 s；`Connected` 后重置为 5 s。`Connected` 状态下 `link()` 变为 `Idle` 或 `Failed` 视为断开，立即进入 `Connecting`。
  - `add_network`：SSID 1–32 字节，密码为空（开放网络）或 8–63 字节，否则返回 `InvalidArgument`；同名网络覆盖；已有 5 个且是新 SSID 时返回 `QuotaExceeded`。添加后立即尝试连接这个网络。
  - 每次状态变化发布 `net.wifi.state`；热点启停和客户端数量变化发布 `net.ap.state`（payload 不含密码）。
  - 热点：`start_provisioning` 生成 SSID 与密码（密码字母表 `A–Z a–z 0–9`，用 `random` 取字节后对 62 取模），调用 `start_ap`。`ap_clients()` 连续 10 分钟为 0 时关闭热点；**只有在配网会话期间通过 `add_network` 添加的网络连接成功**，才在 30 s 后关闭热点。
  - `set_standby(true)` 后满 `idle_off_min` 分钟就 `disconnect()`，状态变为 `Off`（不退避）；`set_standby(false)` 时如果有已保存的网络就重新连接。Wi-Fi 连上后调用 `set_power_save(true)`。
  - `net/wifi.json` 格式：`{"networks": [{"ssid": "...", "password": "..."}]}`；不打印任何密码。

- [ ] **Step 1: 写失败的测试**（每个用例用 `FakeWifi` + 临时目录里的 `StorageManager` + 手动推进的 `now`；`tick` 时间步长由用例自己控制）

```cpp
TEST(WifiService, PicksStrongestSavedNetwork)        // 保存 A(-70)、B(-40) → connect 到 B
TEST(WifiService, BackoffDoublesAndCaps) {
    // reachable=false：两次 connect 的间隔依次为 5000、10000、20000…，第 8 次之后固定为 300000
}
TEST(WifiService, ResetsBackoffAfterConnect)
TEST(WifiService, DropFromConnectedReconnectsImmediately)
TEST(WifiService, AddNetworkValidatesAndCapsAtFive) {
    // 密码 7 字节 → InvalidArgument；第 6 个新 SSID → QuotaExceeded；覆盖已有 SSID 仍然成功
}
TEST(WifiService, PersistsNetworksAcrossInstancesWithoutLoggingPasswords) {
    // 第二个实例 init 后 saved_ssids() 相同；log::ring_snapshot() 中不包含密码字符串
}
TEST(WifiService, ProvisioningSsidAndPasswordFormat) {
    // mac 末两字节 0x3F 0x2A → "Wisp-3F2A"；密码匹配 ^[A-Za-z0-9]{8}$；两次开启得到的密码不同
}
TEST(WifiService, ApClosesAfterTenIdleMinutes)       // 599999 ms 时仍开着，600000 ms 时关闭，并发布 net.ap.state active=false
TEST(WifiService, ApLingersThirtySecondsAfterProvisionedConnect)
TEST(WifiService, ApStaysOpenWhenOldNetworkReconnectsDuringProvisioning)   // Review Focus 3
TEST(WifiService, StandbyDisconnectsAfterIdleMinutesAndWakeReconnects)     // idle_off_min=10
TEST(WifiService, StateEventsCarryNoPassword)
```

- [ ] **Step 2: 运行，确认失败。**
- [ ] **Step 3: 实现。** 所有公共方法用一把 `os::Mutex` 保护（HTTP 线程、控制台线程和定时器都会调用）；`scan()` 调用设备期间不持锁。
- [ ] **Step 4: 运行 `ctest -R WifiService`，确认通过。**
- [ ] **Step 5: Commit** — `feat(net): add WifiService with backoff, provisioning and standby handling`

---

### Task 6: `PairingService`

**Files:**
- Create: `components/wisp_net/include/wisp/net/pairing_service.hpp`、`src/pairing_service.cpp`
- Test: `tests/net/pairing_service_test.cpp`

**Interfaces:**
- Produces（`namespace wisp::net`）：

```cpp
struct ClientInfo { std::string user_agent; std::string remote_ip; Iface iface; };
struct PairedClient { std::string id; std::string label; int64_t created_unix; int64_t last_seen_unix; };
struct PairingConfig { uint32_t code_ttl_ms = 120000; int max_attempts = 5; uint32_t lock_ms = 300000;
                       size_t max_clients = 8; uint32_t last_seen_flush_ms = 3600000; };
class PairingService {
public:
    PairingService(core::StorageManager& storage, core::EventBus& bus, RandomFn random,
                   std::function<int64_t()> unix_now /* RTC 未设置时返回 0 */, PairingConfig config = {});
    Result<void> init();
    Result<void> request_code(uint64_t now_ms);          // 锁定期内返回 RateLimited
    std::optional<std::string> active_code(uint64_t now_ms) const;   // 只给设备屏幕用
    Result<std::string> confirm(std::string_view code, const ClientInfo& c, uint64_t now_ms);  // 返回令牌
    Result<std::string> pair_from_ap(const ClientInfo& c, uint64_t now_ms);  // iface != Ap → PermissionDenied
    Result<std::string> authenticate(std::string_view token, uint64_t now_ms);  // 返回 client id
    std::vector<PairedClient> clients() const;
    Result<void> revoke(std::string_view id);
    void revoke_all();
};
```
- 行为：
  - 配对码是 000000–999999 的 6 位数字（保留前导零）；`request_code` 每调用一次就生成新码，使旧码失效；码过期或配对成功后清空，并发布 `net.pairing.code {active:false}`。
  - 输错时计数加 1，发布 `net.pairing.failed {attempts, locked}`；第 5 次错误后锁定 5 min，锁定期内 `confirm` 和 `request_code` 都返回 `RateLimited`。比较配对码用恒定时间比较。
  - 令牌 = `base64url_encode(32 个随机字节)`；`id` = 令牌 SHA-256 十六进制的前 12 位；`label` 由 User-Agent 归纳而来（例如 `"Chrome · Android"`，归纳不出来时用 `"浏览器"`）。
  - `net/clients.json` 只保存 `{id, hash, label, created, last_seen}`，`hash` 是令牌 SHA-256 的十六进制。
  - 第 9 个客户端配对时，淘汰 `last_seen_unix` 最小的那个；`last_seen` 相同时淘汰 `created` 最早的。
  - `authenticate` 每次都更新内存里的 `last_seen`，但距上次落盘不足 `last_seen_flush_ms` 时不写文件（新增和撤销时立即写）。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(Pairing, CodeIsSixDigitsAndExpires)          // 119999 ms 仍有效，120000 ms 失效
TEST(Pairing, NewRequestInvalidatesOldCode)
TEST(Pairing, LocksAfterFiveWrongCodes)          // 第 5 次错误后 confirm 返回 RateLimited；300000 ms 后恢复
TEST(Pairing, TokenStoredOnlyAsHash) {
    // clients.json 的原始内容里找不到令牌明文，但能找到 to_hex(sha256(token))
}
TEST(Pairing, AuthenticateSurvivesRestart)
TEST(Pairing, EvictsLeastRecentlySeenNinthClient)
TEST(Pairing, ApPairingRequiresApInterface)
TEST(Pairing, LastSeenFlushIsThrottled) {           // Review Focus 1
    // 1 小时内 authenticate 100 次只写盘 0 次（用文件 mtime 或包装 StorageManager 计数）；超过 1 小时后的下一次调用写 1 次
}
TEST(Pairing, RevokeAllClearsFile)
TEST(Pairing, CodeNeverAppearsInEventsOrLog)
```

- [ ] **Step 2: 运行，确认失败。**
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 运行 `ctest -R Pairing`，确认通过。**
- [ ] **Step 5: Commit** — `feat(net): add PairingService with screen codes and hashed tokens`

---

### Task 7: `LlmConfigService`、`SystemSettings` 与 `SettingsBackend::erase_all`

**Files:**
- Create: `components/wisp_core/include/wisp/core/{llm_config,system_settings}.hpp`、`src/{llm_config,system_settings}.cpp`
- Modify: `components/wisp_core/include/wisp/core/settings.hpp`（`virtual Result<void> erase_all() = 0;`）、`components/wisp_port_sim/settings_json_file.cpp`、`components/wisp_port_esp/settings_nvs.cpp`、测试里的内存后端；`components/wisp_app/include/wisp/app/power_manager.hpp` 与 `src/power_manager.cpp`（新增 `void set_brightness(uint8_t level);`）
- Test: `tests/core/llm_config_test.cpp`、`tests/core/system_settings_test.cpp`、`tests/core/settings_test.cpp`（加 `erase_all` 用例）、`tests/app/power_manager_test.cpp`

**Interfaces:**
- Produces（`namespace wisp::core`）：

```cpp
struct LlmConfig { std::string base_url; std::string model; bool has_key = false; };
struct LlmConfigUpdate { std::optional<std::string> base_url, model, api_key; };  // api_key: nullopt=保留, ""=清除
class LlmConfigService {
public:
    explicit LlmConfigService(StorageManager& storage);
    Result<void> init();
    LlmConfig get() const;
    Result<void> set(const LlmConfigUpdate& u);   // base_url 必须以 http:// 或 https:// 开头，结尾的 '/' 去掉；model ≤ 128 字节
    // 内部接口：只供发起 LLM 请求的代码使用（net::probe_llm、子项目 4 的客户端），任何适配层都不得调用。
    std::string authorization_for_request() const;   // "Bearer <key>"，没有 Key 时为空串
    void clear();
};
struct SystemSettingsValues { int brightness; int volume; int screen_off_s; };
class SystemSettings {
public:
    SystemSettings(SettingsBackend& backend, std::function<void(uint8_t)> apply_brightness);
    SystemSettingsValues get() const;    // 默认 200 / 60 / 60
    Result<void> set(const std::map<std::string, int64_t>& changes);
};
```
- 取值范围：`brightness` 10–255（`display`/`brightness`），`volume` 0–100（`audio`/`volume`），`screen_off_s` 为 0（永不熄屏）或 15–3600（`power`/`screen_off_s`）。任何一项越界或键名未知都返回 `InvalidArgument`，**整批都不写入**。
- `secrets/llm.json` 保存 `{"base_url","model","api_key"}`。
- `PowerManager::set_brightness(level)`：亮屏时立即调用 `display->set_brightness`；待机时只记下，`exit_standby()` 中 `set_power(true)` 之后再应用。`App` 用它作为 `apply_brightness` 回调。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(LlmConfig, KeyIsWriteOnly) {
    svc.set({.base_url = "https://api.example.com/v1/", .model = "gpt-x", .api_key = "sk-SECRET123"});
    auto c = svc.get();
    EXPECT_EQ(c.base_url, "https://api.example.com/v1");
    EXPECT_TRUE(c.has_key);
    EXPECT_EQ(svc.authorization_for_request(), "Bearer sk-SECRET123");
    EXPECT_EQ(wisp::log::ring_snapshot().find("SECRET"), std::string::npos);
}
TEST(LlmConfig, OmittedKeyIsKeptEmptyKeyClears)
TEST(LlmConfig, RejectsNonHttpUrl)
TEST(LlmConfig, SurvivesRestart)
TEST(SystemSettings, ValidatesWholeBatchAtomically)  // {brightness:100, volume:101} → InvalidArgument，brightness 仍为 200
TEST(SystemSettings, AppliesBrightnessImmediately)   // 回调收到 100
TEST(SystemSettings, ScreenOffZeroMeansNever)        // 0 合法、10 非法、15 合法
TEST(Settings, EraseAllRemovesEveryNamespace)
TEST(PowerManager, BrightnessChangedInStandbyAppliesOnWake) {   // Review Focus 2
    // standby 期间 set_brightness(50)：display 的 set_brightness 没有被调用、set_power 仍为 false；
    // exit_standby() 之后 brightness()==50
}
```

- [ ] **Step 2: 运行，确认失败。**
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 运行 `ctest -R 'LlmConfig|SystemSettings|Settings\.|PowerManager'`，确认通过。**
- [ ] **Step 5: Commit** — `feat(core): add LLM config, system settings and settings erase_all`

---

### Task 8: `wisp_admin` 骨架：请求过滤、鉴权与配对接口

**Files:**
- Create: `components/wisp_admin/CMakeLists.txt`、`include/wisp/admin/admin_server.hpp`、`src/{admin_server,filters,routes_pair}.cpp`、`tests/support/fake_http.hpp`
- Modify: `simulator/CMakeLists.txt`、`tests/CMakeLists.txt`、`tools/check_layering.py`（`wisp_admin` → `wisp/admin/`，位于 `wisp_net` 之后、`wisp_ui` 之前）
- Test: `tests/admin/filters_test.cpp`、`tests/admin/pair_routes_test.cpp`

**Interfaces:**
- Consumes：Task 4–7
- Produces（`namespace wisp::admin`）：

```cpp
struct AdminDeps {
    core::DeviceRegistry& registry; core::BootReport& report; core::StorageManager& storage;
    core::EventBus& bus; net::WifiService& wifi; net::PairingService& pairing;
    core::LlmConfigService& llm; core::SystemSettings& settings; net::HttpClient* http_client;
    const net::AssetPack* assets;                     // 可为空：只提供 API
    std::string board; std::string firmware_version; std::string device_name = "Wisp";
    std::function<Value()> device_status;             // uptime_ms、heap、battery 等
    std::function<void()> reboot;
    std::function<void()> request_factory_reset;      // 在设备上弹确认框，结果通过事件返回
    std::function<uint64_t()> now_ms;
};
class AdminServer {
public:
    explicit AdminServer(AdminDeps deps);
    net::HttpResponse handle(const net::HttpRequest& req);
    net::WsHandlers ws_handlers();    // Task 10 实现
};
```
- `tests/support/fake_http.hpp`：`net::HttpRequest make_req(net::Method, std::string path, std::string body = {}, net::Iface = net::Iface::Sta)`，默认带 `host: 192.168.1.50`；`with_token(req, token)`、`with_csrf(req)` 两个辅助函数。
- 处理顺序（spec 6.3）：
  1. **Host 校验**：去掉端口后，必须是 IPv4 点分十进制、`wisp.local`、`localhost`，或者（`iface == Ap` 时）任意值，否则返回 403 `bad_host`。热点模式下 DNS 已被劫持，探测请求的 Host 会是 `connectivitycheck.gstatic.com` 之类的域名，必须放行。
  2. **探测路径**：`/generate_204`、`/gen_204`、`/hotspot-detect.html`、`/library/test/success.html`、`/connecttest.txt`、`/ncsi.txt`、`/canonical.html`、`/success.txt`。来自热点的请求返回 302，跳转到 `http://<热点 IP>:<http_port>/setup.html`（端口为 80 时省略）；其他来源返回 404。
  3. **静态文件**：非 `/api/` 路径从 `assets` 查找，返回 gzip 内容加 `ETag`；请求带的 `If-None-Match` 相同时返回 304；找不到返回 404。
  4. **API**：免鉴权白名单为 `GET /api/v1/info`、`POST /api/v1/pair/start`、`POST /api/v1/pair/confirm`、`POST /api/v1/pair/ap`；其余接口没有有效 `wisp_token` 时返回 401 `unauthorized`。
  5. **CSRF**：POST/PUT/DELETE 请求没有 `x-wisp: 1` 请求头时返回 403 `csrf`（白名单接口同样要求）。
  6. 请求体超过 8192 字节返回 413；JSON 解析失败返回 400 `bad_json`。
  - `OPTIONS` 一律返回 405，不带任何 CORS 头。
  - 配对接口：`/pair/confirm` 成功时返回 `Set-Cookie: wisp_token=<t>; HttpOnly; SameSite=Strict; Path=/; Max-Age=31536000`；`/logout` 返回 `Max-Age=0` 并撤销令牌；`GET /info` 返回 `{"name","fw","paired": <请求是否带有效令牌>}`。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(Filters, RejectsForeignHostButAllowsPortsAndAp) {
    EXPECT_EQ(srv.handle(host(make_req(Get, "/api/v1/info"), "evil.com")).status, 403);
    EXPECT_EQ(srv.handle(host(make_req(Get, "/api/v1/info"), "192.168.1.50:80")).status, 200);
    EXPECT_EQ(srv.handle(host(make_req(Get, "/api/v1/info"), "localhost:8080")).status, 200);
    EXPECT_EQ(srv.handle(host(make_req(Get, "/generate_204", {}, Iface::Ap), "connectivitycheck.gstatic.com")).status, 302);
}
TEST(Filters, CsrfHeaderRequiredAnyCase) {           // Review Focus 4
    auto r = with_token(make_req(Post, "/api/v1/logout"), token);
    EXPECT_EQ(srv.handle(r).status, 403);
    r.headers["x-wisp"] = "1";  // HttpServer 实现负责把请求头名转成小写
    EXPECT_EQ(srv.handle(r).status, 200);
}
TEST(Filters, UnauthenticatedApiIs401AndOptionsIs405)
TEST(Filters, OversizedBodyIs413)
TEST(Filters, StaticFileGzipEtagAnd304)
TEST(PairRoutes, ConfirmSetsHttpOnlyCookie) {
    auto res = srv.handle(with_csrf(make_req(Post, "/api/v1/pair/confirm", R"({"code":")" + code + "\"}")));
    EXPECT_EQ(res.status, 200);
    EXPECT_THAT(set_cookie(res), HasSubstr("HttpOnly; SameSite=Strict; Path=/; Max-Age=31536000"));
}
TEST(PairRoutes, WrongCodeIs403AndLockIs429)
TEST(PairRoutes, ApPairOnlyFromApInterface)
TEST(PairRoutes, ErrorEnvelopeShape)   // {"error":{"code":..., "message":...}}
```

- [ ] **Step 2: 运行，确认失败。**
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 运行 `ctest -R 'Filters|PairRoutes'`，并跑 `check_layering.py`，确认通过。**
- [ ] **Step 5: Commit** — `feat(admin): add request filtering, auth and pairing routes`

---

### Task 9: 业务接口与 LLM 连通测试

**Files:**
- Create: `components/wisp_net/include/wisp/net/llm_probe.hpp`、`src/llm_probe.cpp`、`components/wisp_admin/src/{routes_device,routes_wifi,routes_llm,routes_system}.cpp`
- Test: `tests/net/llm_probe_test.cpp`、`tests/admin/routes_test.cpp`

**Interfaces:**
- Produces（`namespace wisp::net`）：

```cpp
struct LlmProbe { bool ok; TransportError transport; int http_status; uint32_t latency_ms; std::vector<std::string> models; };
LlmProbe probe_llm(const core::LlmConfigService& llm, HttpClient& client, std::function<uint64_t()> now_ms);
```
  - 请求 `GET {base_url}/models`，带 `Authorization: Bearer <key>`，超时 10 s；`ok` 的条件是 HTTP 200 且 body 能解析出 `data[].id`。没有配置 `base_url` 时不发请求，直接返回 `ok=false, transport=Other`。
- 接口（spec 6.1；响应字段固定如下）：
  - `GET /api/v1/device` → `{"manifest": <registry.manifest>, "boot": <report>, "status": <device_status()>}`
  - `GET /api/v1/devices` → `{"devices": [{"id","class","model","state","caps"}]}`
  - `GET /api/v1/settings` → `{"brightness","volume","screen_off_s"}`；`PUT` 接收其中的任意子集
  - `GET /api/v1/wifi` → `{"state","ssid","ip","rssi","saved": [ssid...], "ap": {"active","ssid","clients"}}`（**不含热点密码**）
  - `GET /api/v1/wifi/scan` → `{"networks": [{"ssid","rssi","secured","saved"}]}`
  - `POST /api/v1/wifi/networks {ssid, password}` → 202；`DELETE /api/v1/wifi/networks/{ssid}`（SSID 经过 URL 编码，需要解码）→ 204
  - `GET /api/v1/llm` → `{"base_url","model","has_key"}`；`PUT /api/v1/llm`；`POST /api/v1/llm/test` → `{"ok","error": "dns"|"connect"|"tls"|"timeout"|"http"|"other"|null,"http_status","latency_ms","models"}`
  - `GET /api/v1/clients` → `{"clients": [{"id","label","created","last_seen","current": bool}]}`；`DELETE /api/v1/clients/{id}` → 204
  - `POST /api/v1/system/reboot` → 202，延迟 500 ms 再调用 `reboot()`，让响应先发出去
  - `POST /api/v1/system/factory-reset` → 202，调用 `request_factory_reset()`

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(LlmProbe, ParsesModelsAndSendsBearer)   // FakeHttpClient 记录请求：url 为 ".../v1/models"，带 Authorization 头
TEST(LlmProbe, MapsTransportAndHttpErrors)   // Tls → ok=false；HTTP 401 → http_status=401
TEST(Routes, NoResponseEverContainsApiKey) {
    llm.set({.base_url = "https://x/v1", .api_key = "sk-NEVERLEAK"});
    fake_client.next = {.status = 401, .body = R"({"error":"bad key sk-NEVERLEAK"})"};
    for (auto& req : every_authenticated_get_route() + {post("/api/v1/llm/test")}) {
        auto res = srv.handle(req);
        EXPECT_EQ(res.body.find("NEVERLEAK"), std::string::npos) << req.path;
    }
}
TEST(Routes, WifiStatusNeverContainsApPassword)
TEST(Routes, SettingsPutValidatesAndApplies)
TEST(Routes, WifiNetworkDeleteDecodesSsid)          // "/api/v1/wifi/networks/My%20Home" → forget("My Home")
TEST(Routes, ClientsListMarksCurrent)
TEST(Routes, FactoryResetDelegatesToDevice)         // 返回 202，回调被调用一次；接口本身不清除任何数据
```

  其中 `NoResponseEverContainsApiKey` 要求 `/llm/test` 不把上游返回的 body 透传给前端（只返回状态码和分类）。
- [ ] **Step 2: 运行，确认失败。**
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 运行 `ctest -R 'LlmProbe|Routes'`，确认通过。**
- [ ] **Step 5: Commit** — `feat(admin): add device, settings, Wi-Fi, LLM and system routes`

---

### Task 10: WebSocket 推送（日志与事件）

**Files:**
- Create: `components/wisp_admin/include/wisp/admin/ws_hub.hpp`、`src/ws_hub.cpp`
- Modify: `components/wisp_admin/src/admin_server.cpp`（`ws_handlers()` 做鉴权并转交给 `WsHub`）
- Test: `tests/admin/ws_hub_test.cpp`

**Interfaces:**
- Produces（`namespace wisp::admin`）：

```cpp
class WsHub : public LogSink {
public:
    WsHub(core::EventBus& bus, size_t max_clients = 2, size_t max_queue = 256);
    ~WsHub();
    bool open(std::shared_ptr<net::WsConnection> conn);    // 超过 2 个时返回 false
    void on_message(int id, std::string_view text);        // {"sub":["log","events"]}
    void close(int id);
    void write(LogLevel level, const char* tag, const char* msg) override;   // 只入队，不阻塞
    void pump();      // 由 WsHub 自己的发送任务循环调用；测试里直接调用
};
```
- 行为（spec 6.2）：
  - 订阅 `log` 时，先把 `log::ring_snapshot()` 按行拆开，作为 `{"t":"log","ts":0,"level":"I","tag":"","msg":<行>}` 补发，再推送实时日志 `{"t":"log","ts","level","tag","msg"}`。
  - 订阅 `events` 时，转发前缀为 `net.`、`power.`、`storage.`、`system.` 的事件，格式为 `{"t":"event","topic","payload"}`。
  - 每个连接一个有界队列（256 条）；入队满时丢弃最旧的一条并计数，下一次 `pump` 先发送 `{"t":"dropped","count":N}`。`send_text` 返回 false 时把消息放回队首，留到下次再发。
  - `write()` 只在自己的锁里入队，绝不调用 `send_text`；也不能写日志（避免递归）。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(WsHub, ThirdClientRejected)
TEST(WsHub, BackfillsRingThenStreamsLive)
TEST(WsHub, ForwardsOnlyAllowedTopics)          // "net.wifi.state" 转发，"button.power.short" 不转发
TEST(WsHub, SlowClientDropsOldestAndReportsCount) {   // Review Focus 5
    // FakeWsConnection 的 send_text 一直返回 false；写入 300 条日志后改为返回 true
    // → pump 发出的第一条为 {"t":"dropped","count":44}，之后正好 256 条
}
TEST(WsHub, LogWriteNeverBlocksOnSend)          // send_text 阻塞 1 s 时，write() 仍在 10 ms 内返回
TEST(AdminServer, WsRequiresToken)              // on_open 对没有有效 Cookie 的请求返回 false
```

- [ ] **Step 2: 运行，确认失败。**
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 运行 `ctest -R 'WsHub|AdminServer'`，确认通过。**
- [ ] **Step 5: Commit** — `feat(admin): stream logs and events over WebSocket`

---

### Task 11: 模拟器 HTTP/WebSocket 服务

**Files:**
- Create: `components/wisp_port_sim/include/wisp/port/sim/sim_http_server.hpp`、`net/sim_http_server.cpp`、`net/sha1.cpp`（WebSocket 握手用，自行实现）
- Modify: `components/wisp_port_sim/CMakeLists.txt`（新库 `wisp_port_sim_net`，`REQUIRES wisp_net`，`HOST_LIBS Threads::Threads`）
- Test: `tests/sim/sim_http_server_test.cpp`

**Interfaces:**
- Produces：`class SimHttpServer : public net::HttpServer`，构造参数为 `std::string bind = "127.0.0.1"`；`port()` 在 `start(0, ...)` 后返回系统分配的端口。
- 行为：
  - 一个 accept 线程，每个连接一个线程，最多 7 个连接，超出时直接关闭新连接。
  - 支持 HTTP/1.1 keep-alive；请求头名转为小写；按 `Content-Length` 读取请求体，超过 8192 字节返回 413；不支持 chunked 请求体（返回 411）。
  - `iface` 固定为 `Loopback`。`static_body` 不拷贝，直接发送。
  - `/api/v1/ws` 的 Upgrade 请求按 RFC 6455 完成握手（`Sec-WebSocket-Accept` = base64(SHA-1(key + GUID))），之后收发文本帧：处理掩码、ping/pong、close 帧，以及最长 64 KB 的帧。
  - 每个 WebSocket 连接有自己的发送队列（容量 64）；`send_text` 在队列满时返回 false。
  - `stop()` 关闭监听 socket 和所有连接，并 join 所有线程。

- [ ] **Step 1: 写失败的测试**（真实 socket 连 `127.0.0.1:port()`）

```cpp
TEST(SimHttpServer, ServesHandlerResponseWithKeepAlive)   // 同一连接发两个请求，都收到 200
TEST(SimHttpServer, LowercasesHeaderNames)                // handler 看到 headers["x-wisp"]
TEST(SimHttpServer, RejectsOversizedBody)                 // 413
TEST(SimHttpServer, WebSocketHandshakeAndEcho) {
    // key "dGhlIHNhbXBsZSBub25jZQ==" → accept "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="；发一个带掩码的文本帧，收到 on_message
}
TEST(SimHttpServer, EighthConnectionClosed)
TEST(SimHttpServer, StopJoinsCleanly)                     // 有连接打开时 stop() 在 1 s 内返回
```

- [ ] **Step 2: 运行，确认失败。**
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 运行 `ctest -R SimHttpServer`，确认通过，并且测试输出里没有告警。**
- [ ] **Step 5: Commit** — `feat(port-sim): add a loopback HTTP/1.1 and WebSocket server`

---

### Task 12: 模拟器 Wi-Fi、HTTP 客户端与 App 装配

**Files:**
- Create: `components/wisp_port_sim/net/{sim_wifi,sim_http_client}.cpp`、`include/wisp/port/sim/{sim_wifi,sim_http_client}.hpp`、`components/wisp_app/src/net_wiring.cpp`
- Modify: `components/wisp_app/include/wisp/app/app.hpp`（`Platform` 新增字段）、`components/wisp_app/src/app.cpp`、`app_impl.hpp`、`components/wisp_app/CMakeLists.txt`（`REQUIRES wisp_net wisp_admin`）、`components/wisp_port_sim/sim_board.cpp`（注册 `net.wifi`）、`simulator/{main.cpp,sim_options.{hpp,cpp},CMakeLists.txt}`（构建时打包 `web/` 为 `wisp_sim` 旁边的 `assets.wpak`）、`cmake/wisp_host_deps.cmake`（`find_package(CURL 7.80 REQUIRED)`）、`docs/architecture.md`（启动流程第 4 步之后的网络步骤、新增事件主题、`notes.net`）
- Test: `tests/app/app_net_test.cpp`、`tests/sim/sim_options_test.cpp`、`tests/sim/sim_wifi_test.cpp`

**Interfaces:**
- Produces（`Platform` 新增字段）：

```cpp
std::function<void(uint8_t*, size_t)> random;          // 必须提供（模拟器用 std::random_device，真机用 esp_fill_random）
std::unique_ptr<net::HttpServer> http_server;           // 可为空：不启动后台
std::unique_ptr<net::HttpClient> http_client;           // 可为空：/llm/test 返回 transport=Other
std::span<const uint8_t> assets;                        // 可为空：只提供 API
uint16_t http_port = 8900;
std::unique_ptr<net::HttpServer> captive_server;        // 可为空（模拟器）：热点运行期间在 80 端口运行 AdminServer::captive_handler()
bool http_always_on = false;                            // 模拟器为 true：不等网络接口就绪，直接启动 HTTP 服务
std::function<void()> reboot;
std::function<bool(std::string& line)> read_line;      // 可为空：不启动串口控制台（Task 16 使用）
```
  - `App` 新增访问器：`net::WifiService* wifi()`、`net::PairingService* pairing()`、`core::LlmConfigService& llm()`、`core::SystemSettings& system_settings()`。
- 装配（`net_wiring.cpp`，在 PowerManager 构造之后执行）：
  - 有 `net.wifi` 时构造 `WifiService` 并 `init`，用 250 ms 的周期定时器调用 `tick`；订阅 `power.state` 并调用 `set_standby`。
  - 订阅 `net.wifi.state` 与 `net.ap.state`：任意一个接口就绪时 `http_server->start(http_port, ...)`，两个都断开时 `stop()`。
  - `report.set_note("net", ...)` 取值为 `"missing"`（没有 `net.wifi`）、`"off"`（没有已保存的网络）、`"connecting"`。
  - `unix_now` 读取 `clock.main`，读不到时返回 0。
- `SimWifi`（`net.wifi`，model `sim-wifi`）：内置网络 `WispLab`（-45，密码 `wisp1234`）、`Neighbor`（-70，密码 `neighbor99`）、`OpenCafe`（-80，开放网络）；`--wifi-fail` 时所有连接都失败（原因为 `NotFound`）；`start_ap` 只改变状态；`ap_clients()` 返回 1，用来模拟手机已连上。模拟器的 `main.cpp` 设置 `http_always_on = true`，这样即使 `SimWifi` 处于 `Idle`，后台也能在电脑上调试。
- `SimHttpClient`：基于 libcurl，把 `CURLE_COULDNT_RESOLVE_HOST`、`CURLE_COULDNT_CONNECT`、`CURLE_SSL_*`、`CURLE_OPERATION_TIMEDOUT` 映射为对应的 `TransportError`。
- 模拟器命令行新增 `--http-port <n>`（默认 8900，0 表示不启动）、`--web-dir <path>`（从目录读文件，用 `pack_assets.py` 的同一套 MIME/gzip 规则在内存里打包，启动时打包一次）、`--wifi-fail`。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(SimOptions, ParsesNetFlags)   // --http-port 0、--web-dir web、--wifi-fail；--http-port 70000 → InvalidArgument
TEST(SimWifi, ConnectsWithRightPasswordOnly)
TEST(AppNet, BootReportHasNetNote)               // 没有已保存网络 → "off"；不注册 net.wifi → "missing"
TEST(AppNet, StartsHttpServerWhenStaConnects)    // 用 FakeHttpServer 记录 start/stop；FakeWifi 连接成功后 running()==true
TEST(AppNet, AlwaysOnStartsServerWithoutNetwork) // http_always_on=true 时开机即 start，断网也不 stop
TEST(AppNet, StandbyEventReachesWifiService)
TEST(AppNet, BootDoesNotWaitForWifi)             // FakeWifi::scan 阻塞 3 s 时，start() 仍在 1 s 内返回
```

- [ ] **Step 2: 运行，确认失败。**
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 运行 `ctest --test-dir build/sim --output-on-failure`（全量），并手动运行 `build/sim/wisp_sim --web-dir web`，确认 `curl -s localhost:8900/api/v1/info` 返回 JSON。**
- [ ] **Step 5: Commit** — `feat(app): wire Wi-Fi, pairing and the admin server into boot`

---

### Task 13: 前端外壳、样式与基础页面

**Files:**
- Create: `web/index.html`、`web/vendor/htm-preact-3.1.1.mjs`（来自 `htm@3.1.1/preact/standalone.module.js`，文件头加注来源与 MIT 许可）、`web/styles/{tokens,components}.css`、`web/app/{main,api,ws,router,icons}.js`、`web/app/pages/{pair,overview,devices,logs}.js`、`web/app/components/{card,badge,toast,dialog}.js`
- Modify: `docs/references.md`、`THIRD_PARTY_NOTICES.md`

**Interfaces:**
- Consumes：Task 8–10 的接口（字段名以 Task 9 为准）
- Produces（供 Task 14 使用）：
  - `api.js`：`export async function api(method, path, body)`：自动带上 `X-Wisp: 1` 和 JSON 请求头；401 时跳转到配对页；非 2xx 时抛出 `{status, code, message}`。
  - `ws.js`：`export function subscribe(kinds, onMessage)`：断线后按 1 s、2 s、4 s…（上限 30 s）重连，返回取消订阅的函数。
  - `router.js`：基于 hash 的路由（`#/overview` 等），`export function route(path, component)`。
  - 组件：`Card({title, children})`、`Badge({tone, children})`（tone 取 `ok`/`warn`/`err`/`muted`）、`toast(message, tone)`、`confirmDialog({title, body, okText})` → `Promise<boolean>`。
  - `tokens.css`：`--bg --surface --surface-2 --text --text-dim --accent --accent-glow --ok --warn --err --radius --gap`；默认深色（底色 `#0b0f17`，强调色 `#5ab4ff`），`@media (prefers-color-scheme: light)` 下切换为浅色。
- 布局：宽度 < 768 px 时为顶部标题栏加底部标签栏（概览、外设、网络、模型、系统），"日志"放在系统页里作为入口；宽度 ≥ 768 px 时为左侧侧栏，日志单独成项。

- [ ] **Step 1:** 写 `tokens.css`、`components.css` 和外壳；`pair.js` 完成"获取配对码 → 输入 6 位数字 → 成功后进入概览"的流程（输入框用 `inputmode="numeric"`，限制 6 位）。
- [ ] **Step 2:** 概览页：状态卡片（电量与充电、Wi-Fi、存储、运行时长、固件版本），通过 `events` 订阅实时刷新；外设页：可展开的设备列表；日志页：最多保留 2000 行，支持按级别过滤、暂停和清屏，暂停期间新日志只计数不渲染。
- [ ] **Step 3: 验证**：`python3 tools/pack_assets.py web -o /tmp/x.wpak --budget-kb 150` 通过；运行 `build/sim/wisp_sim --web-dir web`，用浏览器打开 `http://localhost:8900`，在 390×844（手机）和 1280×800 两种窗口尺寸下截图（Playwright），检查配对 → 概览 → 外设 → 日志的流程，控制台没有错误。
- [ ] **Step 4: Commit** — `feat(web): add the admin shell, design tokens and overview/devices/logs pages`

---

### Task 13b: 设备端多语言基础（spec 8.3）

**Files:**
- Create: `components/wisp_ui/include/wisp/ui/strings.hpp`、`components/wisp_ui/src/strings.cpp`、`components/wisp_ui/src/strings_{zh,ja,en}.cpp`、`tools/gen_cjk_font.py`
- Modify: `components/wisp_core/{include/wisp/core/system_settings.hpp,src/system_settings.cpp}`（新增字符串键 `language`）、`components/wisp_admin/src/routes_*.cpp`（`/settings` 读写 `language`）、`components/wisp_app/src/self_check_screen.cpp` 与其他设备端中文文字、`components/wisp_ui/fonts/lv_font_cjk_16.c`（重新生成）、`docs/references.md`、`THIRD_PARTY_NOTICES.md`
- Test: `tests/ui/strings_test.cpp`、`tests/core/system_settings_test.cpp`、`tests/admin/routes_settings_test.cpp`（或现有对应文件）、`tests/app/self_check_screen_test.cpp`

**Interfaces:**
- Produces：
  - `enum class StrId : uint16_t`、`enum class Lang : uint8_t { Zh, Ja, En }`、`const char* ui::tr(StrId)`、`void ui::set_lang(Lang)`、`Lang ui::lang()`、`std::optional<Lang> ui::parse_lang(std::string_view)`（"zh"/"ja"/"en"）。`tr` 线程安全（语言存为原子量），但文字只在 UI 线程上使用。
  - `SystemSettings` 的 `language` 键：取值 `"zh"`/`"ja"`/`"en"`，默认 `"zh"`，存为 `sys`/`lang`；非法值返回 `InvalidArgument`。`GET /settings` 返回它，`PUT /settings` 可以修改。
  - 语言变化时，App 在 UI 线程上调用 `ui::set_lang` 并重建当前界面的文字（自检界面重建即可）。
- 字体：`tools/gen_cjk_font.py` 只用 Python 标准库生成字符清单：ASCII、GB2312（`gb2312` 编解码器枚举）、JIS X 0208 第 1–8 区符号与假名、第 16–47 区第一水准汉字（`euc_jp` 编解码器枚举），去重排序后调用固定版本的 `lv_font_conv`（`npx lv_font_conv@1.5.2`），参数与现字体一致（16 px、4 bpp、Noto Sans CJK SC Regular，固定上游版本并校验 SHA-256）。生成的 `.c` 文件头写明来源、许可与生成命令。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(Strings, EveryIdHasTextInEveryLanguage)          // 遍历全部 StrId × 3 种语言：非空
TEST(Strings, EveryGlyphIsInTheBuiltInFont)           // 遍历全部文字的每个码点：lv_font_get_glyph_dsc 成功
TEST(Strings, ParseLang)                              // "zh"/"ja"/"en" 解析成功，"fr"、"" 失败
TEST(SystemSettings, LanguageDefaultsToZhAndValidates)
TEST(AdminSettings, LanguageRoundTrips)               // PUT {"language":"ja"} 后 GET 返回 "ja"；"fr" → 400
TEST(SelfCheckScreen, RebuildsInNewLanguage)          // 切到 en 后，屏幕上的标题文字变成英文
```

- [ ] **Step 2: 运行，确认失败。**
- [ ] **Step 3: 实现**：先生成字体（确认新增的 ja 字形都在），再把设备端的中文文字全部迁到字符串表。
- [ ] **Step 4: 验证**：全量 ctest、`bash tools/ci.sh`、固件 `idf.py build`，记录固件大小变化；模拟器分别用三种语言截图自检界面（`--screenshot`）。
- [ ] **Step 5: Commit** — `feat(ui): add device UI strings in Chinese, Japanese and English`

---

### Task 14: 网络、模型、系统页面与配网向导

**Files:**
- Create: `web/app/pages/{network,model,system}.js`、`web/setup.html`、`web/app/setup.js`
- Modify: `tools/ci.sh`（对 `web/` 运行 `pack_assets.py --budget-kb 150`）

**Interfaces:**
- Consumes：Task 13 的 `api`/`subscribe`/组件；Task 9 的接口

- [ ] **Step 1: 网络页**：当前连接（SSID、IP、信号格数）；已保存的网络（删除前用 `confirmDialog` 确认）；"扫描"按钮列出网络，点击后输入密码（开放网络不显示密码框），提交后订阅 `net.wifi.state` 显示结果。
- [ ] **Step 2: 模型页**：端点输入框（placeholder 为 `https://api.openai.com/v1`）；API Key 用 `type=password`，已设置时显示"已设置（不可查看）"和"清除"按钮，输入框留空提交表示保留原 Key；"测试连接"显示延迟与错误类别（中文）；测试成功后模型变为下拉框，也可以手动输入。
- [ ] **Step 3: 系统页**：亮度、音量滑块（松手时 `PUT`）、熄屏时间下拉框（永不、15 s、30 s、60 s、2 min、5 min、10 min）、设备语言下拉框（中文 / 日本語 / English，对应 `language`，与网页自身的语言切换分开）；已配对的浏览器（当前浏览器带标记，可撤销）；重启；恢复出厂设置（先在网页上确认，再提示"请在设备上确认"，等待 `system.factory_reset` 事件）。
- [ ] **Step 4: 配网向导 `setup.html`**：先调用 `POST /pair/ap`，然后依次为选择网络 → 输入密码 → 连接中（订阅 `net.wifi.state`，60 s 超时）→ 成功时显示 `http://<新 IP>:8900` 与 `http://wisp.local:8900`，失败时显示原因并允许重试。只引用包内资源。
- [ ] **Step 5: 验证**：同 Task 13 的 Step 3，加上：模拟器里用 `WispLab`/`wisp1234` 走一遍配网向导（模拟器中 `iface` 为 `Loopback`，向导检测到 `/pair/ap` 返回 403 时退回到配对码流程）；用 `--wifi-fail` 验证失败提示；`bash tools/ci.sh` 通过。
- [ ] **Step 6: Commit** — `feat(web): add network, model and system pages and the setup wizard`

---

### Task 15: 设备端网络区块、配网与配对卡片、恢复出厂设置

**Files:**
- Create: `components/wisp_app/src/{net_section,net_cards,factory_reset}.cpp` 与对应的 `.hpp`
- Modify: `components/wisp_app/src/self_check_screen.cpp`（`build_net_section(col)` 插在设备列表之后）、`components/wisp_ui/config/lv_conf.h`（`LV_USE_QRCODE 1`）
- Test: `tests/app/net_ui_test.cpp`、`tests/app/factory_reset_test.cpp`

**Interfaces:**
- Consumes：`App::wifi()`、`App::pairing()`、Task 7 的服务、`SettingsBackend::erase_all()`
- Produces：`void factory_reset(App& app);`：依次 `settings.erase_all()`、`wifi->forget_all()`、`pairing->revoke_all()`、`llm.clear()`，发布 `system.factory_reset {state:"done"}`，然后调用 `platform.reboot()`。用户在设备上取消时发布 `{state:"cancelled"}`。
- 界面规则：
  - 网络区块：一行状态文字（`未配网` / `连接中…` / `重试中（N 秒后）` / `已连接 <SSID> <IP>`）加一个按钮（`开始配网` / `停止配网`）；没有 `net.wifi` 时显示 `无 Wi-Fi 硬件`，不显示按钮。
  - 配网卡片（`lv_layer_top`，宽度限制在安全区内）：标题 `配网`、二维码（边长取安全区宽度的 45% 与 200 px 中的较小值，内容为 `WIFI:T:WPA;S:<ssid>;P:<password>;;`）、`热点：<ssid>`、`密码：<password>`、`连上后打开 http://<ip>`、按钮 `停止配网`。`net.ap.state active=false` 时关闭。
  - 配对码卡片：标题 `配对请求`、大号 6 位数字（字间留空）、`剩余 N 秒` 每秒刷新、按钮 `拒绝`（使配对码立即失效）。码失效或配对成功后关闭。配网卡片打开时，配对码卡片叠在它之上。
  - 恢复出厂确认框复用 `show_confirm_dialog` 的样式：标题 `恢复出厂设置`，正文 `将清除 Wi-Fi、配对和模型配置并重启，场景数据保留。`。
  - 所有 LVGL 调用都经 `UiRuntime::post()`。
  - 上面列出的中文文字是 `zh` 的文案；全部文字经 Task 13b 的 `ui::tr()` 取用，同时补齐 `ja`/`en`，语言切换后卡片随之重建。

- [ ] **Step 1: 写失败的测试**（沿用 `self_check_screen_test.cpp` 的 Harness，加上 `FakeWifi`）

```cpp
TEST(NetUi, SectionShowsStateAndButton)            // 没有已保存网络 → 标签文字包含"未配网"
TEST(NetUi, ProvisioningCardShowsQrAndFitsSafeArea) {
    // 点击"开始配网"后，lv_layer_top 下出现 lv_qrcode 对象；卡片四角都在 safe_rect 之内（rounded_rect 与 circle 两种形状）
    // 卡片里所有标签的字形都能渲染（复用 all_label_glyphs_resolvable）
}
TEST(NetUi, PairingCardCountsDownAndCloses)
TEST(NetUi, NoWifiDeviceShowsNoButton)
TEST(FactoryReset, ClearsEverythingButLibraryAndReboots) {
    // 预先写入：Settings、wifi.json、clients.json、llm.json，以及 Library 区的一个文件
    // factory_reset 后：前四项都被清除，Library 文件仍在；reboot 回调被调用 1 次
}
TEST(FactoryReset, CancelPublishesCancelled)
```

- [ ] **Step 2: 运行，确认失败。**
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 运行 `ctest -R 'NetUi|FactoryReset|SelfCheckScreen'`，确认通过；运行模拟器 `--shape circle --screenshot /tmp/c.png` 并查看截图。**
- [ ] **Step 5: Commit** — `feat(app): add the network section, provisioning and pairing cards and factory reset`

---

### Task 16: 串口控制台

**Files:**
- Create: `components/wisp_app/include/wisp/app/console.hpp`、`src/console.cpp`
- Modify: `components/wisp_app/src/net_wiring.cpp`（有 `read_line` 时启动控制台任务）、`simulator/main.cpp`（`read_line` 读 stdin；`--headless` 且 stdin 不是终端时也照常读取）
- Test: `tests/app/console_test.cpp`

**Interfaces:**
- Produces（`namespace wisp::app`）：

```cpp
Result<std::vector<std::string>> split_command(std::string_view line);   // 支持双引号与 \" 转义；引号未闭合 → InvalidArgument
std::string run_command(App& app, std::string_view line);                // 返回要打印的文本（可能多行）
```
- 命令（spec 8.2）：`wifi status` → `state=<s> ssid=<x> ip=<y> rssi=<n>`；`wifi add <ssid> [password]` → `ok` 或 `error: <message>`；`wifi forget <ssid>`；`wifi ap on` → `ap ssid=<x> ip=<y>`（**不打印热点密码**，密码只显示在屏幕上）；`wifi ap off`；`help`；其他输入 → `error: unknown command`。
- 控制台把输出写到 `emit_line`，不经过 `WLOG`，所以 `wifi add` 的整行输入不会进入日志环形缓冲。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(Console, SplitsQuotedSsid)        // wifi add "My Home" "p@ss word" → ["wifi","add","My Home","p@ss word"]
TEST(Console, UnterminatedQuoteIsError)
TEST(Console, AddNeverEchoesPassword)  // 返回值和 log::ring_snapshot() 里都没有密码
TEST(Console, ApOnDoesNotPrintPassword)
TEST(Console, UnknownCommand)
```

- [ ] **Step 2: 运行，确认失败。**
- [ ] **Step 3: 实现。**
- [ ] **Step 4: 运行 `ctest -R Console`，确认通过。**
- [ ] **Step 5: Commit** — `feat(app): add the serial console with Wi-Fi commands`

---

### Task 17: 端到端冒烟测试与 CI

**Files:**
- Create: `tests/smoke/admin_smoke.py`、`tools/fake_openai.py`
- Modify: `tests/CMakeLists.txt`（注册 `admin_smoke` 用例）、`tests/smoke/sim_smoke.py`（断言 `notes.net == "off"`）、`tools/ci.sh`、`.github/workflows/ci.yml`（host job 安装 `libcurl4-openssl-dev`）

**Interfaces:**
- `fake_openai.py --port <n>`：`GET /v1/models` 在 `Authorization: Bearer test-key` 时返回 `{"data":[{"id":"fake-1"},{"id":"fake-2"}]}`，否则返回 401。只用 Python 标准库。

- [ ] **Step 1: 写冒烟脚本**（Python 标准库，`http.client` + 手写最小 WebSocket 客户端）：
  1. 启动 `fake_openai.py` 和 `wisp_sim --headless --http-port 0 --data-dir <tmp>`，从 stdout 读取 `WISP_HTTP {"port": <n>}`（与其他机器可读输出一样是"前缀 + 单行 JSON"）（App 在 HTTP 服务启动后用 `HttpServer::port()` 输出这一行；`--http-port 0` 由系统分配端口）；
  2. `POST /api/v1/pair/start` → 从模拟器的 stdin 发送调试命令 `debug pair-code`，读回配对码（这个命令只在模拟器里编译进去，真机没有）→ `POST /pair/confirm` 拿到 Cookie；
  3. 依次请求 `GET /device`、`/devices`、`/wifi`，`PUT /settings {"brightness":120}`，然后 `GET /settings` 验证新值；
  4. `PUT /llm {"base_url":"http://127.0.0.1:<p>/v1","api_key":"test-key"}`，`POST /llm/test` → `ok == true` 且 `models == ["fake-1","fake-2"]`；然后在所有响应 body 里查找 `test-key`，一个都不能有；
  5. 连接 WebSocket 并订阅 `log`，30 s 内收到至少一条日志；
  6. 不带 `X-Wisp` 请求头的 `POST` 返回 403；`Host: evil.com` 返回 403。
- [ ] **Step 2:** 在 `wisp_app` 里补上 `WISP_HTTP` 输出行和模拟器专用的 `debug pair-code` 命令（编译宏 `WISP_SIM_DEBUG_COMMANDS`，只在 `wisp_sim` 目标上定义）；更新 `docs/architecture.md` 的机器可读输出清单。
- [ ] **Step 3: 运行 `bash tools/ci.sh`，全部通过。**
- [ ] **Step 4: Commit** — `test(smoke): exercise pairing, routes, LLM probe and WebSocket end to end`

---

### Task 18: ESP 平台实现与固件集成

**Files:**
- Create: `components/wisp_port_esp/net/{esp_wifi,esp_http_server,esp_http_client,captive_dns}.cpp`、`include/wisp/port/esp/{esp_wifi,esp_http_server,esp_http_client}.hpp`、`components/wisp_port_esp/console_esp.cpp`
- Modify: `components/wisp_port_esp/CMakeLists.txt`（`IDF_REQUIRES esp_wifi esp_netif esp_http_server esp_http_client esp-tls mbedtls espressif__mdns esp_driver_usb_serial_jtag`）、`components/wisp_port_esp/idf_component.yml` 或 `firmware/main/idf_component.yml`（`espressif/mdns` 锁定精确版本）、`firmware/dependencies.lock`、`boards/waveshare_amoled_216/board.cpp`（注册 `net.wifi`）、`firmware/main/main.cpp`（填写 `Platform` 的新字段：`random`=`esp_fill_random`、`assets`=`esp_partition_mmap("assets")`、`reboot`=`esp_restart`、`read_line`、`captive_server`）、`firmware/CMakeLists.txt`（构建时运行 `pack_assets.py web -o build/assets.wpak --partitions ../boards/${WISP_BOARD}/partitions.csv --budget-kb 150`，并用 `esptool_py_flash_to_partition(flash assets build/assets.wpak)` 一并烧录）、`firmware/sdkconfig.defaults`（`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`、`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`、`CONFIG_HTTPD_WS_SUPPORT=y`、`CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024`）、`docs/references.md`、`THIRD_PARTY_NOTICES.md`
- Test: `tests/port_esp/`（只编译、不运行的固件构建检查，沿用子项目 1 的做法）

**Interfaces:**
- Consumes：Task 3、4 的接口；Task 12 的 `Platform` 字段
- 实现要点：
  - `EspWifi`：初始化 `esp_netif` 和 APSTA，并在 board init 里就 `esp_wifi_start()`——射频开启后 `esp_fill_random` 才是真随机数，`Platform::random` 的所有调用方都依赖这一点（见 `firmware/main/main.cpp` 的注释）；`link()` 从事件回调更新的原子快照里读取；`start_ap` 设置 WPA2-PSK、信道 1、最多 4 个客户端，启动 `captive_dns`；STA 拿到 IP 时用主机名 `wisp` 启动 mDNS，并广播 `_http._tcp` 端口 8900。初始化失败时由 `board.cpp` 写进启动报告，状态为 `Failed`。
  - `EspHttpServer`：`max_open_sockets = 7`，`lru_purge_enable = true`，`uri_match_fn = httpd_uri_match_wildcard`；注册 `/*` 的 GET/POST/PUT/DELETE/OPTIONS，以及 `/api/v1/ws`（`is_websocket = true`）；请求头逐个读取：`Host`、`Cookie`、`X-Wisp`、`Content-Type`、`If-None-Match`、`User-Agent`；`iface` 按客户端 IP 是否在热点子网 `192.168.4.0/24` 内判断；`send_text` 用 `httpd_ws_send_data_async`，在途发送超过 64 条时返回 false。
  - 强制门户服务：`firmware/main/main.cpp` 再构造一个 `EspHttpServer` 作为 `Platform::captive_server`（`App` 只在热点运行期间在 80 端口启动它）。这是第二个 httpd 实例：`ctrl_port` 必须与后台实例不同（例如 `ESP_HTTPD_DEF_CTRL_PORT + 1`），配置尽量小——`max_open_sockets = 4`、小栈（约 3 KB）、不注册 WebSocket；同时注意 lwIP 套接字总数（`CONFIG_LWIP_MAX_SOCKETS`）要容纳两个实例加 DNS 劫持。
  - `EspHttpClient`：`esp_http_client` 加 `crt_bundle_attach = esp_crt_bundle_attach`，根据 `esp_err_t`/`esp_tls` 错误映射为 `TransportError`。
  - `console_esp.cpp`：安装 `usb_serial_jtag` 驱动并让 VFS 使用它，`read_line` 用 `fgets` 读 stdin。
- [ ] **Step 1: 构建** — Run: `bash -c '. ~/esp/esp-idf-v6.0.3/export.sh >/dev/null && cd firmware && idf.py build'` → 成功；`idf.py size` 的应用大小不超过 `ota_0` 的 80%；`build/assets.wpak` 存在且小于 2 MB。
- [ ] **Step 2: 交给用户刷机并采一次启动日志**：确认没有已保存网络时 `notes.net` 为 `off`，开机时间与子项目 1 的基准相比没有明显增加；从日志里记录 Wi-Fi 初始化之后的内部 RAM 和 PSRAM 剩余量。
- [ ] **Step 3: Commit** — `feat(port-esp): add Wi-Fi, HTTP server/client, captive DNS and mDNS`

---

### Task 19: 真机联调、验收与文档

**Files:**
- Modify: `docs/hardware/waveshare_amoled_216.md`（Wi-Fi 之后的内存剩余量、`WISP_PERF` 对比、配网实测结果）、`docs/architecture.md`（全面核对）、`README.md`（"当前状态"一节）、本计划（勾选完成项、记录执行期裁定）

- [ ] **Step 1: 交给用户执行 spec 9.2 的验收清单第 1–9 项**，逐项记录结果。需要用户提供的信息：手机型号与系统版本（检查强制门户是否自动弹出）、路由器的频段（确认是 2.4 GHz）。
- [ ] **Step 2:** 第 9 项：在 Wi-Fi 已连接、后台日志页打开的状态下，滚动自检界面，采集 `WISP_PERF`，与子项目 1 的基准（≥ 30 fps）对比，写进硬件文档。
- [ ] **Step 3:** 修复联调中发现的问题，每个问题先写失败的测试（能在模拟器里复现的都在模拟器里复现）。
- [ ] **Step 4:** 跑 `bash tools/ci.sh` 和固件构建；更新文档。
- [ ] **Step 5: Commit** — `docs: record sub-project 2 on-device acceptance`
