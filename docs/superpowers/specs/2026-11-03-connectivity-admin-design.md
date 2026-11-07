# Wisp 子项目 2：连接与管理后台 设计文档

- 日期：2026-11-03
- 状态：待审阅
- 范围：Wisp 整体路线中的第 2 个子项目（连接与管理后台）
- 前置：子项目 1 基础骨架（`2026-11-01-foundation-design.md`，下文简称"基础 spec"）

---

## 1. 背景与目标

### 1.1 目标

子项目 2 让 Wisp 能**联网并被主人从浏览器管理**，同时为子项目 4（Agent）和子项目 5（语音）准备好模型配置：

1. Wi-Fi 连接管理与配网（热点 + 强制门户为主，串口命令为补充）；
2. 跨真机与模拟器的 HTTP/WebSocket 服务；
3. 屏幕配对码 + 长期令牌的配对鉴权；
4. 管理后台网页：概览、外设、网络、模型（对话 / 语音识别 / 语音合成的多套配置与连通测试）、系统设置、实时日志；
5. 板级分区表与 sdkconfig 迁移到 `boards/<板名>/`，确立分区约定。

### 1.2 使用场景与原则

- **单一主人、只在局域网**：后台只给设备主人自己用，不支持多用户，不经过公网。
- **离线优先**：Wi-Fi 是可选能力，不是开机前提。没配过网或者连不上网时，设备照常开机、照常使用，已有场景等本地功能不受影响；只有需要联网的功能不可用，并在界面上提示。
- **后台网页是这一阶段的主要配置入口**：设备上的设置页属于子项目 8，在那之前模型端点、Key 等都从后台网页填写。
- **服务层约定**（基础 spec 3.3）：REST/WebSocket 只是服务层之上的薄适配层，不允许绕过服务层直接调驱动。这次新增的服务以后也供 Agent 工具、Lua 绑定和设置页复用。

### 1.3 已定决策

| 议题 | 决策 | 理由 |
|---|---|---|
| 前端技术栈 | Preact + htm，免构建，原生 ES 模块 | 不引入 Node 工具链；改完刷新即可看到效果，迭代快；运行时约 5KB |
| 样式 | 手写小型 CSS，不用 UI 框架 | 体积可控；用设计令牌延续鬼火视觉方向 |
| 配网 | 热点 + 强制门户，**按需开启**；串口命令补充 | 不需要 App；配网页复用后台的前端与 HTTP 服务 |
| 鉴权 | 屏幕配对码 + 长期令牌（HttpOnly Cookie） | 能看到实物屏幕才能配对，相当于"物理在场"即授权；没有密码可忘 |
| 传输 | 普通 HTTP | 自签名证书会让浏览器反复弹警告；风险与缓解措施见 6.4，HTTPS 留给子项目 7 评估 |
| 模型配置 | 多套配置（profile），按类型 `llm`/`asr`/`tts` 分开，各自激活；预设平台表编译进固件 | 换平台、对比模型不用反复填写；加平台只需加一行表项 |
| ASR/TTS 协议 | 不限定为 OpenAI 兼容；本子项目只提供兼容预设，私有流式协议在子项目 5 接入 | 国内主流语音服务多为私有流式协议，限定兼容协议会让可选平台很少、延迟也差 |
| 跨平台 HTTP | 可移植接口 + 真机和模拟器两套薄实现 | 与现有 HAL/port 模式一致；业务逻辑都能在主机上测试；排除 Mongoose（GPLv2，与 Apache-2.0 冲突） |

### 1.4 不在本子项目范围内

HTTPS 与 Flash 加密（子项目 7 评估）、设备自动联网拉取更新与固件签名（子项目 7；本子项目只做局域网网页上传固件，见 6.5）、场景和记忆相关的后台页面（子项目 3、6）、Agent 对话本身（子项目 4；这次只做模型配置与连通测试）、语音识别与合成的调用及私有协议客户端（子项目 5）、蓝牙、远程或公网访问、设备上的设置页（子项目 8）。

---

## 2. 组件与分层

### 2.1 新增与改动的组件

| 层 | 新增内容 | 职责 |
|---|---|---|
| `wisp_hal` | `hal::Wifi` 设备接口 | 扫描、连接、断开、开关热点、查询状态。注册为 `net.wifi`（新设备类别 `Network`），caps 写进能力清单；初始化失败时只降级运行 |
| `wisp_net`（新，可移植） | `HttpServer`、`HttpClient` 接口；`WifiService`、`PairingService`、`AssetPack` | 网络服务层：连接管理、配网会话、配对码与令牌、静态资源包读取 |
| `wisp_core` | `ModelProfileService`、`SystemSettings` | 对话 / 语音识别 / 语音合成的多套模型配置与预设表，API Key 只写不读；亮度、音量、自动熄屏时间的校验与立即生效 |
| `wisp_admin`（新，可移植） | 路由表、REST 处理、WebSocket 推送 | 管理后台适配层：只把 HTTP 请求翻译成服务调用，不含业务逻辑 |
| `wisp_app` | 装配、网络区块与配网/配对卡片、串口命令 | 构造并连接上述服务；设备端界面 |
| `wisp_port_esp` | `EspWifi`、`EspHttpServer`、`EspHttpClient`、mDNS | 分别封装 `esp_wifi`、`esp_http_server`、`esp_http_client`（加 `esp_crt_bundle`，LLM 端点都是 HTTPS）和乐鑫 mDNS 组件（`wisp.local`，广播 `_http._tcp` 端口 8900） |
| `wisp_port_sim` | `SimWifi`、`SimHttpServer`、`SimHttpClient` | 假 Wi-Fi；用 POSIX socket 手写的精简 HTTP/1.1 + WebSocket 服务（只监听 `127.0.0.1`）；基于系统 libcurl 的 HTTPS 客户端 |
| `web/`（新目录） | 前端源码 | 原生 ES 模块；第三方库锁定版本放在 `web/vendor/` |
| `tools/` | `pack_assets.py` | 生成 WispPack 资源包并做检查（见 2.3） |

### 2.2 依赖方向

`wisp_admin` → `wisp_net` → `wisp_core` → `wisp_hal`，由 `wisp_app` 负责装配。`tools/check_layering.py` 把 `wisp_net`、`wisp_admin` 加入可移植层名单：它们和其他可移植组件一样，不得 include 平台头文件（`lwip/*`、`esp_http_*`、`curl/*` 等）。

`HttpServer`/`HttpClient` 接口放在 `wisp_net` 而不是 `wisp_hal`：它们不是硬件，而是平台服务，由 `app::Platform` 注入（与 `Platform::settings` 相同）。`Wifi` 是射频设备，按设备模型放在 HAL，经 `DeviceRegistry` 注册。

新增外部依赖只有两个：乐鑫 mDNS 组件（真机，组件管理器锁定精确版本）和 libcurl（只在模拟器使用，取系统包，记录最低版本）。Preact 与 htm 以锁定版本的文件放入仓库，并登记到 `docs/references.md` 与 `THIRD_PARTY_NOTICES.md`（均为 MIT 许可）。

### 2.3 WispPack 资源包

管理后台网页打包成自定义的极简只读包格式，**链接进应用镜像**（不再占用单独的分区）：网页与固件永远是同一版本，网页上传固件（6.5）时两者一起在 A/B 分区间切换、一起回滚。资源只读，用不上文件系统。

- **格式**：头部（魔数 `WPAK`、版本号、文件数），文件表（路径、偏移、长度、MIME 类型、是否 gzip、内容哈希），然后是连续存放的文件内容。所有多字节整数用小端序，文件内容按 4 字节对齐。
- **生成**：`tools/pack_assets.py` 把 `web/` 等目录打成包。文本类文件（html/js/css/svg/json）逐个 gzip，已压缩的格式原样存放。脚本同时检查：每个 JS 模块的相对 import 都能在包内解析；包的总大小不超过 256 KB，超出即构建失败。
- **读取**：可移植的 `AssetPack` 接收一段只读字节区间，解析并校验头部和文件表；包损坏或被截断时返回错误，不崩溃。真机直接使用链接进固件的只读数据（4 字节对齐），零拷贝发送；模拟器读取构建目录下的同一个包文件，或用 `--web-dir web/` 直接读源码目录，改完刷新就生效。
- **发送**：gzip 文件带 `Content-Encoding: gzip`，由浏览器解压；用内容哈希做 `ETag`，命中时返回 304。
- **构建**：固件构建先运行 `pack_assets.py`，再把生成的包作为二进制数据链接进应用；不需要单独烧录。以后较大的只读资源（提示音、更大的字体）放进固件或 `storage`/SD 卡，不再设专门分区。

---

## 3. 板级分区表与配置

子项目 1 只有一块板子，`firmware/partitions.csv` 和 `firmware/sdkconfig.defaults` 名义上是公共文件，内容却完全属于这块板子（16MB Flash、八线 PSRAM、USB 串口控制台），不符合"板子无关"的规则。从本子项目开始，分区和板级配置迁移到各自的板目录。

### 3.1 文件位置

- `boards/<板名>/partitions.csv`：本板分区表。
- `boards/<板名>/sdkconfig.defaults.board`：Flash 大小与模式、PSRAM、控制台，以及指向本板分区表的 `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME`。esp_board_manager 在 `idf.py bmgr -b <板名>` 时把它合并进生成的 `board_manager.defaults`，优先级高于项目级 `sdkconfig.defaults`。
- `firmware/sdkconfig.defaults`：只保留与板子无关的公共配置（LVGL、日志级别、mbedTLS 证书包、HTTP 服务参数等）。

### 3.2 分区约定（新板子必须遵守）

代码只按**分区名**查找分区，不依赖偏移和大小。

| 分区名 | 是否必需 | 说明 |
|---|---|---|
| `nvs` | 必需 | 配置 |
| `phy_init` | 必需 | 射频校准 |
| `storage` | 必需 | LittleFS（`/int`） |
| `ota_0`、`ota_1`、`otadata` | 可选 | Flash 足够时提供双 OTA 分区；没有时用单个 `factory` 应用分区，固件检测到缺少 OTA 分区就关闭固件更新（`GET /system/firmware` 返回 `supported: false`），只降级运行 |

分区大小由各板自行决定，例如 32MB 的板子可以把多出的空间分给 `storage`。参考板（16MB）的分区表：

| 分区 | 偏移 | 大小 |
|---|---|---|
| `nvs` | 0x9000 | 0x6000 |
| `otadata` | 0xF000 | 0x2000 |
| `phy_init` | 0x11000 | 0x1000 |
| `ota_0` | 0x20000 | 0x500000（5 MB） |
| `ota_1` | 0x520000 | 0x500000（5 MB） |
| `storage` | 0xA20000 | 0x5E0000 |

`nvs` 与 `storage` 的偏移和大小与之前相同，重新整片烧录后设置与数据仍在。配置的存放位置：亮度、音量、自动熄屏、语言等键值设置在 `nvs`；已保存的网络、已配对浏览器、模型配置等结构化文件在 System 区（`storage` 上的 LittleFS，`/int/system`）。

**发布构建**：`firmware/sdkconfig.defaults` 即发布配置——全局按体积优化（`-Os`），日志最高编译到 INFO，断言静默；**渲染路径固定 `-O2`**（LVGL 组件、`wisp_ui`、显示与触摸驱动及刷屏路径，在各自组件里单独指定，覆盖全局选项）。调试时叠加 `firmware/sdkconfig.debug`（`-Og`、DEBUG 日志、断言开启），用单独的构建目录：`idf.py -B build-debug -D SDKCONFIG=build-debug/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;components/gen_bmgr_codes/board_manager.defaults;sdkconfig.debug" build`。`SDKCONFIG_DEFAULTS` 一旦给出就替换 esp_board_manager 默认加入的列表，所以 bmgr 生成的 `board_manager.defaults`（含本板的 Flash、PSRAM 与分区表配置，见 3.1）必须显式列出，否则调试版会丢掉板级配置。预算：应用不超过 `ota_0` 的 80%；`WISP_PERF` 帧率不低于基础 spec 的基准。基础 spec 7.4 的分区表改为"参考板（Waveshare 2.16）的分区表"，并引用本节约定；`docs/porting-guide.md` 同步补充。

---

## 4. 启动与 Wi-Fi

### 4.1 启动流程的变化

- 在基础 spec 6.1 的第 4 步之后：如果 `net.wifi` 已注册，`App` 构造 `WifiService` 并在后台启动，立即返回，开机不等待联网。
- HTTP 服务在任意一个网络接口就绪时启动（STA 拿到 IP，或者热点开启），两者都断开时停止。
- `WISP_BOOT_REPORT` 的 `notes` 增加 `net` 键，记录开机那一刻的网络状态（`"off"` / `"connecting"`，或 `"missing"` 表示没有 `net.wifi`）。修改格式时同步更新冒烟测试与 `docs/architecture.md`。

### 4.2 Wi-Fi 状态机

```
         没有已保存网络 / 用户关闭
   ┌──────────► Off ◄───────────────┐
   │             │ 有已保存网络       │
   │             ▼                   │
   │        Connecting ──失败──► Backoff（5s→10s→…→上限 5min）
   │             │ 拿到 IP      ▲        │
   │             ▼             │        └─到点─► Connecting
   │        Connected ──断开───┘
   │
   └ Provisioning（热点）：与上面的状态并存（APSTA）
```

- **连接**：扫描后，在已保存的网络里选信号最强的连接。最多保存 5 个网络。
- **密码错误**：某个网络**连续 3 次**尝试都因密码错误（`auth_failed`）失败后，才被标记为 `auth_failed`，不再参加自动选择，直到用户重新输入它的密码（重新添加会清除标记并立即直接尝试它）；标记与网络一起存进 `net/wifi.json`，重启后仍然有效。一次握手失败不算数：路由器重启或信道拥挤丢了 EAPOL 帧，看起来和密码错误一样。连续计数只在内存里：连接成功、重新添加（即用户再次选择这个网络）或重启都会让它归零；其他原因的失败（找不到网络、超时等）既不计数也不清零。这样，用户给一个信号更强的网络填错密码，也不会让设备离开原本能用的网络后再也回不来。
- **全部被拒**：所有已保存的网络都被标记为 `auth_failed` 时，自动尝试不可能成功：设备照常按退避节奏扫描，但状态的 `reason` 报 `auth_failed`、`ssid` 报其中一个网络（优先报在范围内信号最强的），设备屏幕显示"密码被拒绝，请重新配网"（见 8.1）而不是"重试中"，后台网页同样提示重新输入密码。
- **退避**：每次失败后等待时间翻倍，从 5 秒开始，上限 5 分钟；连接成功后重置。退避期间不打扰前台。
- **省电**：已连接时默认开启 modem sleep。设备进入待机（`power.state = standby`）超过 Settings `net`/`idle_off_min`（默认 10 分钟）后断开 Wi-Fi，唤醒后自动重连。更细的唤醒锁属于子项目 3。
  - **已被取代**：上面的待机断网规则（含 `net`/`idle_off_min`）已由场景设备 spec（`2026-11-05-scene-device-design.md`）4.1/4.4 取代：Wi-Fi 只在深度待机时关闭，普通待机保持连接；深度待机期间场景的请求排队，由每 15 分钟的联网窗口发出。`idle_off_min` 不再读取，残留在 NVS 里的值无效。

### 4.3 配网会话（热点）

- **按需开启**，不会自动弹出：通过设备上的"开始配网"按钮或串口命令 `wifi ap on`。
- 热点名为 `Wisp-` 加 MAC 地址末 4 位十六进制；密码为随机 8 位字母数字，每次开启热点都重新生成。
- 热点与 STA 并存，配网过程中可以边扫描边尝试连接。
- 配网成功后热点尽快关闭，但先让手机页面能显示"已连接，新地址是 …"：配网页经热点读到成功结果（`GET /wifi` 显示已连上该网络）后 3 秒关闭；热点上连续 3 秒没有设备时关闭（设备连上另一信道的网络时热点会跟着换信道，手机可能被短暂断开，所以不在没有设备的一瞬间就关）；否则最多保留 30 秒（`WifiServiceConfig::ap_seen_linger_ms`/`ap_empty_linger_ms`/`ap_linger_ms`）。配网页从事件流得知成功时会再读一次 `GET /wifi`。
- 10 分钟内一直没有设备连上热点就自动关闭；关闭时 `net.ap.state` 发布 `active: false`。
- 热点运行期间提供 DNS 劫持（所有域名都解析到设备自身），配合 6.1 的探测路径触发手机的强制门户弹窗。
- 手机的强制门户探测走 80 端口，而后台在 8900 端口（见 6.1），所以真机在热点运行期间另开一个只监听 80 端口的极简 HTTP 服务：任何请求都以 `302` 重定向到 `http://<热点地址>:8900/setup.html`（带 `Cache-Control: no-store`），没有 API、静态文件和 WebSocket。它随热点开启而启动、随热点关闭而停止；模拟器没有这个服务。

### 4.4 网络的保存

已保存的网络写在 System 区的 `net/wifi.json`，走 `StorageManager::write_atomic`。Wi-Fi 密码明文存放在内置 Flash 上（本子项目不开启 Flash 加密，留给子项目 7 评估），只在设备内部使用：任何接口都不回传，日志也不打印。

### 4.5 模拟器

`SimWifi` 默认提供几个虚拟网络（信号强度不同，其中一个的密码固定），可以模拟连接成功、密码错误和信号丢失。用命令行参数 `--wifi-fail` 模拟"路由器不可达"，以便验证退避逻辑。模拟器的"热点"不开真实射频，只改变状态并启用配网路由。

---

## 5. 服务接口

### 5.1 `WifiService`（`wisp_net`）

```cpp
Result<WifiStatus> status();                 // state, ssid, ip, rssi
Result<std::vector<ScanResult>> scan();      // ssid, rssi, auth, 是否已保存
Result<void> add_network(std::string_view ssid, std::string_view password);
Result<void> forget_network(std::string_view ssid);
std::vector<std::string> saved_ssids();      // 只有 SSID，没有密码
std::vector<SavedNetwork> saved_networks();  // SSID 与 auth_failed 标记（见 4.2），没有密码
Result<ApSession> start_provisioning();      // ssid、密码（只交给屏幕显示）、地址
Result<void> stop_provisioning();
```

### 5.2 `PairingService`（`wisp_net`）

```cpp
Result<void> request_code();                 // 生成配对码并通知屏幕显示
Result<Token> confirm(std::string_view code, const ClientInfo& client);
Result<Token> pair_from_ap(const ClientInfo& client);  // 只允许来自热点网段的请求
Result<ClientId> authenticate(std::string_view token);
std::vector<PairedClient> clients();
Result<void> revoke(ClientId id);
void revoke_all();                           // 恢复出厂设置时调用
```

- 配对码为 6 位数字，2 分钟内有效，同一时刻只有一个。连续输错 5 次锁定 5 分钟，并发布 `net.pairing.failed`。
- `POST /pair/start` 不需要鉴权，所以只要当前配对码还有效，再次请求一律沿用这个码：不换码、不发事件、不重新点亮或重绘屏幕，同一局域网里的脚本既无法替换主人正在输入的码，也无法让屏幕一直亮着。只有在没有有效配对码时（已过期、被用户在屏幕上拒绝、因锁定作废、或已配对成功）才生成新码，而且两次生成之间至少隔 10 秒；不到 10 秒的请求返回 429 `too_soon`（带 `Retry-After`，见 6.1 节的错误说明）。
- 令牌为 32 字节随机数（base64url 编码），设备上只存它的 SHA-256 哈希，写在 System 区的 `net/clients.json`。最多保存 8 个已配对的浏览器，超出时淘汰最久未使用的那个。
- 配对码本身只交给屏幕显示，不进事件、不进日志。

### 5.3 `ModelProfileService`（`wisp_core`）：模型配置（对话 / 语音识别 / 语音合成）

用户可以保存多套模型配置（profile），每套属于一种类型 `kind`：`llm`（对话）、`asr`（语音识别）、`tts`（语音合成）。新建时从预设平台选起，三种类型**各自**指定一套"当前激活"，互不影响。子项目 4 的 Agent 只用 `llm` 的激活配置，子项目 5 的语音链路只用 `asr`/`tts` 的激活配置。

```cpp
ProfilesState get();                                    // {active:{llm,asr,tts}, profiles[]}；每项 {id, kind, name, preset, base_url, model, options, has_key}，不含 Key
std::span<const ModelPreset> presets();                 // 编译进固件的预设表
Result<ModelProfile> create(const ProfileInput& in);    // 新建；该类型还没有激活配置时自动设为激活
Result<ModelProfile> update(std::string_view id, const ProfileUpdate& u);
                                                        // 部分更新；api_key: 不提供 = 保留，空串 = 清除
Result<void> remove(std::string_view id);
Result<void> activate(std::string_view id);             // 设为其所属类型的激活配置
Result<ModelProbe> test(std::string_view id);           // 请求该配置的 {base_url}/models
```

- **配置项**：`id` 由服务生成（8 位小写十六进制随机数，创建后不变）；`kind` 创建后不可改；`name` 1–32 字节 UTF-8，可重名；`preset` 为预设 id，且该预设必须支持此 `kind`；`base_url`；`model`（可空，使用方在各自子项目要求非空）；`options` 为扩展字段（扁平对象，值为字符串，最多 8 个键，键不超过 15 字节，值不超过 64 字节），本子项目只定义 `tts` 的 `voice`（音色），其余键由子项目 5 按平台定义，未知键原样保存。全部类型合计最多 16 套，超出返回 409 `limit_reached`。
- **协议**：`llm` 只用 OpenAI 兼容协议（1.3 已定）。`asr`/`tts` **不限定协议**：本子项目只提供 OpenAI 兼容语音接口（`/audio/transcriptions`、`/audio/speech`）的预设；通义、火山、讯飞等私有流式协议的客户端、预设与专属字段（经 `options` 承载）在子项目 5 设计时加入，存储格式不变。
- **预设表**：编译进 `wisp_core` 的常量表，每项 `{id, name, kinds, base_url, key_required}`。`name` 是平台品牌名，不翻译。新建时 `base_url` 未给出就取预设值；之后地址、模型都可自由修改，`preset` 只用于显示和默认值。不预填模型名（各平台更新太快），模型从"测试连接"返回的列表里选，也可以手动输入。以后增加平台只需在表中加一行。首批预设：

  | id | 名称 | 类型 | 默认 base_url | 需要 Key |
  |---|---|---|---|---|
  | `custom` | OpenAI 兼容（自定义） | llm asr tts | （空，必须填写） | 否 |
  | `openai` | OpenAI | llm asr tts | `https://api.openai.com/v1` | 是 |
  | `anthropic` | Anthropic | llm | `https://api.anthropic.com/v1` | 是 |
  | `gemini` | Google Gemini | llm | `https://generativelanguage.googleapis.com/v1beta/openai` | 是 |
  | `deepseek` | DeepSeek | llm | `https://api.deepseek.com/v1` | 是 |
  | `qwen` | 通义千问 | llm | `https://dashscope.aliyuncs.com/compatible-mode/v1` | 是 |
  | `moonshot` | Kimi | llm | `https://api.moonshot.cn/v1` | 是 |
  | `zhipu` | 智谱 GLM | llm | `https://open.bigmodel.cn/api/paas/v4` | 是 |
  | `ark` | 火山方舟 | llm | `https://ark.cn-beijing.volces.com/api/v3` | 是 |
  | `siliconflow` | 硅基流动 | llm asr tts | `https://api.siliconflow.cn/v1` | 是 |
  | `groq` | Groq | llm asr | `https://api.groq.com/openai/v1` | 是 |
  | `openrouter` | OpenRouter | llm | `https://openrouter.ai/api/v1` | 是 |
  | `ollama` | Ollama（局域网） | llm | `http://<主机>:11434/v1`（占位） | 否 |

  含 `<` 的占位地址不能保存，返回 400 `invalid_argument`。
- **激活**：`active` 对每个类型给出激活配置的 id，该类型没有配置时为空。删除某类型当前激活的配置时，如果该类型还有别的配置返回 409 `active_profile`（先激活另一套）；如果是该类型最后一套则删除，该类型的激活变为空。
- **存储**：全部配置连同 Key 存放在 System 区的 `secrets/models.json`（`{"version":2, "active":{...}, "profiles":[...]}`），经 `write_atomic` 写入。读到子项目 2 早期的 `secrets/llm.json`（`{base_url, model, api_key}`）时迁移为一套 `kind = llm`、`preset = custom`、名称 "Default" 的激活配置，写入新文件成功后删除旧文件。
- **Key 只写不读**：服务对外**没有任何能读出 Key 的接口**，连部分字符也不给；只有使用方（Agent、语音链路）通过内部接口在发请求时取用激活配置的 Key。
- **Key 绑定源**：每套配置的 Key 只属于保存它时该配置的 `base_url` 源（scheme://host:port，scheme 与 host 不区分大小写，省略的默认端口按 80/443 补齐，路径不算）。一次 `update()` 把 `base_url` 改到另一个源、却没有同时提供 `api_key` 时，服务**清除这套配置已存的 Key**；在还没有 `base_url` 时存下的 Key 第一次设置 `base_url` 时同样清除。后台网页在这种情况下提示重新输入 Key。
- **沿用 Key**：`create`/`update` 可以用 `api_key_from: <另一套配置的 id>` 代替 `api_key`，由服务在设备内部复制 Key（Key 不经过浏览器）。只有两套配置的 `base_url` 源相同且来源配置有 Key 时才允许，否则返回 400 `key_origin_mismatch`；`api_key` 与 `api_key_from` 同时给出返回 400。
- `test(id)` 通过 `HttpClient` 以 `Authorization: Bearer <key>`（没有 Key 时不带）请求该配置的 `/models`，超时 10 秒，返回是否成功、延迟、模型 id 列表，或错误类别（DNS、TLS、超时、HTTP 状态码）。HTTP 404 单独报告为 `no_model_list`，网页提示"该平台不提供模型列表，请手动填写模型名"。错误信息里不得带上 Key。
- 每次成功修改后发布 `model.profiles` 事件（见 5.5）。

### 5.4 `SystemSettings`（`wisp_core`）

封装亮度（`display`/`brightness`）、音量、自动熄屏时间、设备界面语言（`sys`/`lang`，取值 `zh`/`ja`/`en`，默认 `zh`，见 8.3）、时区 `utc_offset_min`（`sys`/`tz_min`，UTC 以东的分钟数，−720…840，默认 480；场景 spec 5.2）、校时服务器 `ntp_servers`（`sys`/`ntp`，逗号分隔的 1–2 个主机名或 IPv4 地址，每个不超过 63 个字符，默认 `pool.ntp.org,ntp.aliyun.com`；见 5.6）这些 Settings 键：校验取值范围，写入后立即生效（例如亮度立即应用到 `display.main`）。后台网页、以后的 Agent 工具和设置页都调用它。

### 5.5 事件主题（同步写入 `docs/architecture.md`）

| 主题 | 发布者 | payload |
|---|---|---|
| `net.wifi.state` | `WifiService` | `{state, ssid, ssid_hex, ip, rssi, reason}`（`ssid_hex` 见 6.1），`state` 取值为 `off`/`connecting`/`backoff`/`connected`；`reason` 是上一次连接失败的原因（`not_found`/`auth_failed`/`other`），只在 `backoff` 时非空 |
| `net.ap.state` | `WifiService` | `{active, ssid, clients}`（不含密码） |
| `net.pairing.code` | `PairingService` | `{active, expires_ms}`（不含配对码） |
| `net.pairing.failed` | `PairingService` | `{attempts, locked}` |
| `system.settings` | `App`（`SystemSettings` 的 `on_changed`） | `{brightness, volume, screen_off_s, language, utc_offset_min, ntp_servers}`：每批设置写入成功后的全部当前值（与 `GET /settings` 同形） |
| `system.time` | `TimeSyncService` | `{state, server, time}`：每次校时尝试结束后发布；`state` 为 `synced`（`time` 为同步到的 UTC 秒）或 `failed`（不含 `time`，`server` 是最后问过的服务器），见 5.6 |
| `model.profiles` | `ModelProfileService` | `{active:{llm,asr,tts}, count}`：任何配置被新建、修改、删除或激活后（不含 Key、地址） |
| `system.firmware` | 固件更新服务 | `{state, version}`：`state` 为 `received`（已写入并校验，等待设备确认）/`confirmed`（即将重启）/`rejected`（设备上点了否或 2 分钟未确认）/`failed`（写入或校验失败，附 `error`） |

### 5.6 网络校时 `TimeSyncService`（`wisp_net`）

联网后用 SNTP 校准系统时钟和硬件 RTC（`clock.main`），让断电重启后时间仍然正确。全部时间都是 UTC，显示时各自加上 `utc_offset_min`。

- **时机**：站点（STA）每次拿到 IP 时校一次；成功后每 6 小时再校一次；失败后从 30 秒开始退避，每次翻倍，最长 30 分钟，直到成功或断网。断网时不安排任何尝试，下次拿到 IP 从头开始（即使上一个链路的尝试还在进行，新 IP 的那次也照样立即进行，退避从 30 秒重新算）。`POST /system/time/sync` 立即触发一次。
- **服务器**：每次尝试按 `ntp_servers`（5.4）的顺序逐个询问，第一个回答的为准；每个服务器等待约 3 秒（不含域名解析）。默认先 `pool.ntp.org`，国内网络下它可能很慢或不通，再退到 `ntp.aliyun.com`。
- **成功后**：设置系统时钟（系统时钟拒绝时这次尝试算失败），写入 `clock.main`（写 RTC 失败只记警告），发布 `system.time`（5.5）。场景运行时收到 `synced` 时在亮屏状态下触发一次 `minute`（场景 spec 5.2），自检页"电源与时钟"卡片显示"已同步 HH:MM"，未同步时显示"未同步"。
- **线程**：`wisp.net` 任务每次 tick 只判断是否到期，到期时在本任务内读取 Settings（会访问 flash）并准备好服务器列表；真正的网络请求（每个不通的服务器要等几秒）在单独的 `wisp.ntp` 任务里执行。`wisp.ntp` 只做 socket、设置系统时间、I2C 写 RTC、记日志和发事件，不访问内部 flash，所以声明 `flash_access=false`（AGENTS.md 第 5 节）。关机时取消服务，进行中的尝试在问下一个服务器之前停止，不发事件。
- **平台端口**：`net::Sntp` 只有一个 `query(host, timeout_ms)`。ESP 端口（`wisp_port_esp/net/`）经 lwIP 发一次 SNTP v4 请求，用随机数作发送时间戳并校验回包（模式、版本、闰秒指示、层级、原始时间戳）；模拟器端口直接返回主机时间，`*.invalid` 主机模拟超时。平台不提供 `Sntp` 时不创建该服务。
- **接口**：`GET /system/time` 返回 `{synced, last_sync, server, time, attempts, last_result}`，`last_sync` 是上次同步得到的 UTC 秒、`time` 是按它推算的当前 UTC 秒，未同步时两者都为 `null`；`attempts` 是本次开机已结束的尝试数，`last_result` 是最近一次的结果（`synced`/`failed`，还没有时为 `null`）。`POST /system/time/sync` 返回 202 `{after}`：回答这次请求的是 `attempts` 超过 `after` 后的第一次尝试（请求时已有尝试在进行，就算那一次），结果同时经 `system.time` 事件推送；没有网络时返回 409 `no_network`。服务不存在时两者都返回 503。后台网页按 `after` 轮询 `GET /system/time` 得出结果，事件只用来提前轮询；60 秒内没有结束的尝试时只提示"暂时还没有结果"，不报失败。

---

## 6. HTTP 接口

### 6.1 REST 接口

后台监听 **8900** 端口（真机与模拟器相同；模拟器只监听 `127.0.0.1`），访问地址为 `http://wisp.local:8900` 或 `http://<设备 IP>:8900`。80 端口只在热点运行期间用于强制门户重定向（见 4.3）。

统一前缀 `/api/v1`，请求和响应都是 JSON。

| 分组 | 接口 | 鉴权 | 说明 |
|---|---|---|---|
| 基础 | `GET /info` | 否 | 设备名、固件版本、是否已配对、本次开机的标识 `boot_id` |
| 配对 | `POST /pair/start` | 否 | 让屏幕显示配对码 |
| | `POST /pair/confirm {code}` | 否 | 校验通过后用 `Set-Cookie` 下发令牌 |
| | `POST /pair/ap` | 否，仅限热点网段 | 配网期间免配对码直接配对 |
| | `POST /logout` | 是 | 撤销当前令牌 |
| | `GET /clients`、`DELETE /clients/{id}` | 是 | 已配对的浏览器列表与撤销 |
| 概览 | `GET /device` | 是 | 能力清单、启动报告、运行时长、内存、电量 |
| 外设 | `GET /devices` | 是 | 设备注册表：id、类别、状态、caps |
| 设置 | `GET /settings`、`PUT /settings` | 是 | 经 `SystemSettings` 校验并立即生效 |
| Wi-Fi | `GET /wifi`、`GET /wifi/scan` | 是 | `GET /wifi`：`{state, ssid, ssid_hex, ip, rssi, reason, saved, ap}`，`saved` 的每项为 `{ssid, ssid_hex, auth_failed}`（`auth_failed` 见 4.2）；扫描结果每项为 `{ssid, ssid_hex, rssi, secured, saved}` |
| | `POST /wifi/networks {ssid \| ssid_hex, password?}`、`DELETE /wifi/networks/{ssid}` | 是 | 添加 / 删除网络；`ssid` 与 `ssid_hex` 必须且只能给一个，`ssid_hex` 不是偶数位十六进制时返回 400；`DELETE` 的路径段按 `%XX` 解码成原始字节 |
| 模型 | `GET /ai/profiles` | 是 | `{active, profiles}`，见 5.3 |
| | `GET /ai/presets` | 是 | 预设表 |
| | `POST /ai/profiles {kind, name, preset, base_url?, model?, options?, api_key? \| api_key_from?}` | 是 | 201，返回新配置（不含 Key） |
| | `PUT /ai/profiles/{id} {name?, preset?, base_url?, model?, options?, api_key? \| api_key_from?}` | 是 | 部分更新，未知字段 400；返回更新后的配置 |
| | `DELETE /ai/profiles/{id}` | 是 | 见 5.3 的删除规则 |
| | `POST /ai/active {id}` | 是 | 设为其类型的激活配置 |
| | `POST /ai/profiles/{id}/test` | 是 | 测试该配置的连通性 |
| 系统 | `POST /system/reboot` | 是 | 直接执行 |
| | `POST /system/factory-reset` | 是 | 先在设备上弹确认框，接口返回 202，结果通过 WebSocket 推送；范围见下文 |
| | `GET /system/firmware`、`POST /system/firmware` | 是 | 固件信息与网页上传固件，见 6.5 |
| | `GET /system/time`、`POST /system/time/sync` | 是 | 网络校时状态与立即校时，见 5.6 |
| 实时 | `WS /ws` | 是 | 见 6.2 |

配网向导（`/setup`）先调用 `POST /pair/ap` 拿到令牌，之后与后台一样走需要鉴权的接口，不另设免鉴权的例外。

**强制门户探测路径**（`/generate_204`、`/hotspot-detect.html`、`/connecttest.txt`、`/ncsi.txt` 等）在热点模式下一律重定向到 `http://<热点地址>:8900/setup.html`；非热点模式下返回 404。

**恢复出厂设置的范围**：清除 Settings、已保存的网络、全部令牌和全部模型配置（含 Key），然后重启；不删除 Library 和 Backup 区的数据（以后的场景与用户数据）。完整擦除留给子项目 7。

**SSID 的字节**：SSID 是最多 32 字节的原始字节，不一定是 UTF-8（GBK 编码的路由器很常见），而 JSON 字符串里的非法 UTF-8 会被替换成 U+FFFD。所以响应和事件里的每个 SSID 都附带 `ssid_hex`（原始字节的小写十六进制），前端遇到含 U+FFFD 的 SSID 时用 `ssid_hex` 回传。

**错误格式**：`{"error": {"code": "...", "message": "..."}}`，配合相应的 HTTP 状态码（400/401/403/404/409/413/500/503），由 `wisp::Result` 的错误码映射得到。

配对接口的错误要能让浏览器给出明确提示：配对码错误返回 403 `permission_denied`；输入的正是刚过期的那个码（之后没有新码、也没有被锁定）时返回 403 `code_expired`；锁定期间 `POST /pair/start` 和 `POST /pair/confirm` 都返回 429 `rate_limited`，带 `Retry-After` 响应头（剩余秒数，向上取整，至少 1），`error` 对象里同时给出 `retry_after_s`；距上一次生成配对码不到 10 秒、又没有有效配对码时，`POST /pair/start` 返回 429 `too_soon`，`Retry-After`/`retry_after_s` 的含义相同（这不是锁定，前端不能说"错误次数过多"）。`message` 只是英文说明，前端按 `code` 显示本地化文字（见 7.4）。

### 6.2 WebSocket

- 每个连接收到的第一条消息是 `{"t":"hello","boot_id":"<id>"}`，先于任何补发和推送。`boot_id` 是**本次开机的标识**：开机时用 `Platform::random` 生成的 64 位随机数，写成 16 个小写十六进制字符，整个开机期间不变，与 `GET /info` 的 `boot_id` 相同（不叫 `boot`，以免和 `GET /device` 的启动报告 `boot` 混淆）。
- 客户端连接后发送 `{"sub": ["log", "events"]}` 订阅。
- `log`：先补发日志环形缓冲区（基础 spec 6.5）里现有的记录，每条为 `{"t":"log","seq","ts","level","tag","msg","replay":true}`；再实时推送新日志，每条为 `{"t":"log","seq","ts","level","tag","msg"}`（没有 `replay`）。
  - `seq` 是日志行在**本次开机内的序号**：`wisp::log` 在同一把锁里给每条被接受的日志分配序号（第一条为 1，之后逐条加 1，被级别过滤掉的不占序号）并写入环形缓冲区，所以缓冲区顺序就是序号顺序，补发和实时推送里同一行的 `seq` 相同。
  - `ts` 是**开机以来的毫秒数**（模拟器为进程启动以来），与 `seq` 一起在锁内取得，单调不减，与 `GET /device` 的 `status.uptime_ms` 同一基准（即 `os::now_ms()`）。补发的记录带原始的 `ts`、`level` 和 `tag`。
  - 一条日志就是一条消息：`msg` 里含换行也不拆分。
  - **去重与重启识别**：`(boot_id, seq)` 唯一确定一行日志。每次订阅 `log`（包括重连、停止后再开始）都会重新补发，订阅与新日志并发时同一行也可能先后到达两次；多个线程同时写日志时，实时推送的顺序也可能与 `seq` 顺序差几行（sink 在日志锁外调用）。所以前端不按"最大 `seq`"过滤，而是为当前 `boot_id` 记住见过的 `seq`：一个下限 `floor` 加一个集合，`seq` 不大于 `floor` 或已在集合里的是重复，丢弃；否则显示并加入集合。集合超过 4096 个时把 `floor` 提高到丢掉较旧的一半（乱序只有几行，不会跨几千行，所以这样安全）。行按到达顺序显示，不重排。`hello` 里的 `boot_id` 与之前不同说明设备重启过：前端插入一条"设备已重启"标记、清空集合和 `floor`，保留已显示的历史。不依赖时间戳或内容比较。
- `events`：转发 `net.*`、`power.*`、`scene.*`、`storage.*`、`system.*` 主题，每条为 `{"t":"event","topic","payload"}`。`scene.*`（目前只有 `scene.state`）由场景运行时子项目加入，见场景 spec 3.6。
- 最多同时 2 个 WebSocket 连接，超出时拒绝新连接。发送队列积压超过上限时丢弃最旧的日志，并推送一条 `{"t":"dropped","count"}`。
- **保活**（场景设备 spec 4.3 加入）：打开的连接持有 `system.admin` 唤醒锁，半开的连接（客户端离开 Wi-Fi、合上笔记本）不能一直占着它。设备每 30 秒给每个连接推送 `{"t":"ping"}`，前端立即回 `{"t":"pong"}`；连续 3 轮没收到客户端的任何消息就关闭这个连接。

### 6.3 鉴权与请求过滤

处理顺序：Host 校验 → 鉴权 → CSRF 检查 → 路由。

- **Host 校验（防 DNS 重绑定）**：只接受 `Host` 为 IPv4 地址、`wisp.local` 或热点地址的请求，其余返回 403。热点模式下的探测路径不受此限制。
- **令牌**：Cookie 名为 `wisp_token`，属性 `HttpOnly; SameSite=Strict; Path=/`，有效期 1 年；每次访问更新"最近访问时间"。
- **CSRF**：所有修改类请求（POST/PUT/DELETE）必须带请求头 `X-Wisp: 1`。设备不响应任何 CORS 预检，所以别的网站无法跨域带上这个头。
- **WebSocket 来源**：WebSocket 升级请求同样做 Host 校验和令牌校验；带 `Origin` 头时，其 `host[:port]` 必须与 `Host` 完全一致（不区分大小写），`null` 或非 http(s) 的来源一律拒绝。浏览器在升级请求里总会带 `Origin`，这样即使 Cookie 被带上，别的网站也打不开 WebSocket；不带 `Origin` 的脚本客户端不受影响。
- **资源限制**：请求体最大 8KB（超出返回 413），同时最多 7 个连接。连接已满时，新连接淘汰最久未用的空闲 keep-alive HTTP 连接（WebSocket 和正在处理请求的连接不会被淘汰）；没有空闲连接时才关闭新连接。真机用 httpd 的 `lru_purge_enable` 实现，模拟器的 `SimHttpServer` 行为相同。

### 6.4 HTTP 的风险与缓解

使用普通 HTTP 时，同一局域网内的抓包者可以看到令牌，以及填写 API Key 那一次请求的内容。缓解措施：

- API Key 只在写入那一次传输，之后任何接口都不再发出；
- 令牌可以随时在后台或恢复出厂设置时撤销；
- 配对必须看到实物屏幕。

HTTPS 是否引入、如何解决证书信任问题，留给子项目 7 评估。

### 6.5 网页上传固件（局域网 OTA）

本子项目的固件更新只有一种方式：从已配对的浏览器上传固件文件。设备不主动联网检查或下载更新（留给子项目 7）。

- `GET /system/firmware` → `{supported, version, board, running, pending}`：`supported` 在板子没有 OTA 分区时为 false；`board` 是编译进固件的板名；`pending` 为已上传、等待设备确认的版本（没有时为空）。
- `POST /system/firmware`：请求体为固件镜像（`application/octet-stream`，必须带 `Content-Length`），以流的方式边收边写，不受 6.1 的 8192 字节请求体上限约束。`HttpServer` 为此增加流式请求体处理（真机与模拟器两套实现）。
  - 收到镜像头时立刻检查：魔数、芯片型号、项目名 `wisp`、编译进固件描述的板名，不符返回 400 `wrong_board`（或 `invalid_image`）；`Content-Length` 超过空闲 OTA 分区返回 413。同一时间只允许一次上传，否则 409 `busy`。
  - 写入当前未运行的 OTA 分区，结束时校验整个镜像（SHA-256）；成功返回 202，并在设备上弹出确认框："当前版本 → 新版本"，新版本更旧时额外提示"降级"。点"是"后把它设为启动分区并重启；点"否"或 2 分钟无人确认则丢弃。结果经 `system.firmware` 事件推送。
- **回滚**：开启 bootloader 的应用回滚。新固件只有在 `App::start()` 完成、启动报告已输出后才标记为有效；在此之前崩溃或复位，下次启动自动回到旧固件。
- **签名**：网页上传**不要求签名**，自己构建的固件同样可以安装；信任来自"已配对的浏览器 + 设备上有人确认"。签名只用于子项目 7 的自动拉取更新（Ed25519 签名的发布清单，私钥放在 GitHub Environment 的 Secret 中并需人工审批，支持自定义更新源与公钥）；默认不开启 Secure Boot。
- **模拟器**：固件更新服务写到 `<data-dir>/firmware.bin`，确认后输出 `WISP_REBOOT` 并退出，供冒烟测试覆盖完整流程；镜像头检查使用同一套可移植解析代码。

---

## 7. 前端

### 7.1 技术栈与目录

- Preact + htm，原生 ES 模块，无构建步骤；第三方库放在 `web/vendor/`，锁定版本，文件头标注来源与许可。
- 不使用 TypeScript；在关键数据结构上用 JSDoc 注释说明类型。接口契约以 C++ 侧的路由测试为准。
- 不引用任何外部资源（没有 CDN、在线字体或在线图标）：热点模式下手机没有外网。
- 体积预算：整个资源包里网页部分 gzip 后不超过 150KB。

```
web/
  index.html        # 后台入口
  setup.html        # 配网向导入口
  app/              # 页面与组件（.js）
  styles/           # tokens.css（设计令牌）、components.css
  vendor/           # preact、htm（锁定版本）
```

### 7.2 视觉

- 用 CSS 变量定义设计令牌（颜色、圆角、间距、字号），延续鬼火的视觉方向：深色底配幽蓝色强调色，跟随系统自动适配浅色和深色模式。
- 自带十来个基础组件：卡片、表单、开关、滑块、状态徽章、列表、对话框、提示条、日志面板等。
- 手机竖屏优先，底部标签栏导航；屏幕宽度 ≥ 768px 时切换为侧栏布局。
- 图标为少量内联 SVG。

### 7.3 页面

| 页面 | 内容 |
|---|---|
| 配对 | 未配对时显示：按下"获取配对码"，再输入屏幕上显示的 6 位数字 |
| 概览 | 电量与充电、Wi-Fi、存储、运行时长、固件版本等状态卡片，通过 WebSocket 事件实时刷新 |
| 外设 | 设备列表（id、类别、状态），展开可查看 caps |
| 网络 | 当前连接、已保存的网络（可删除）、扫描并添加网络 |
| 模型 | 「对话 / 语音识别 / 语音合成」三个标签页，三类配置严格分开、互不混列：每个标签页只列出、新建、编辑、测试、激活和删除本类型的配置，"设为当前"只影响本类型；所选标签记在地址的 hash 中（如 `#/models/asr`）。各标签页显示该类型的配置列表（名称、平台、地址、模型、Key 已设置/未设置，激活的有标记）；"新建配置"：先选该类型可用的预设，自动填入地址，再填名称、Key（或"沿用某套配置的 Key"，只列出同源的配置）；`tts` 多一个音色输入框；每套配置可编辑、测试连接（成功后模型下拉框列出可选模型，也可手动输入）、设为当前、删除 |
| 系统 | 亮度、音量、自动熄屏时间、设备语言；网络时间（上次同步时间、服务器、"立即同步"按钮）；已配对的浏览器列表；重启；恢复出厂设置；固件：当前版本与板名、选择文件上传（进度条）、"请在设备上确认"、结果 |
| 日志 | 实时滚动，可按级别过滤、暂停、清屏 |
| `/setup` 配网向导 | 选择网络 → 输入密码 → 连接中 → 显示新地址并提供"进入后台"按钮 |

### 7.4 多语言

网页支持中文、日文、英文三种界面语言。首次访问按 `navigator.languages` 中第一个 `zh*`/`ja*`/`en*` 选择，都不匹配时用英文；配对页和后台外壳都有语言切换，选择存入 `localStorage`（读写失败时只在本次访问有效），并同步 `<html lang>`。所有面向用户的文字都经 `web/app/i18n/index.js` 的 `t(key, params)` 取自 `web/app/i18n/{zh,ja,en}.js` 三份扁平字典（缺键时退回英文、再退回键名本身），数字、容量与时长用 `Intl` 按当前语言格式化；接口错误按 `error.code` 映射为本地化文字，服务端的英文 `message` 只作为次要细节显示。`tools/check_i18n.py` 在 CI 中检查三份字典的键集合与占位符完全一致。

---

## 8. 设备端界面与串口命令

### 8.1 界面（本子项目临时放在自检界面中，子项目 8 移到"设置 → Wi-Fi"）

- 自检界面新增"网络"区块：状态、SSID、IP、信号强度，以及"开始配网 / 停止配网"按钮。所有已保存网络都因密码错误被搁置时（见 4.2"全部被拒"），状态显示"<SSID> 的密码被拒绝，请重新配网"，不显示重试倒计时。SSID 中不是合法 UTF-8 的字节显示为 `?`。
- **配网卡片**：热点开启时显示在顶层，包含二维码（`WIFI:T:WPA;S:<ssid>;P:<password>;;`，扫码直接连热点）、热点名、密码和配网页地址。配网成功时切换为成功视图："已连接 <ssid>"、管理后台地址 `http://<新 IP>:8900` 和一个"确定（N）"按钮，按钮上倒数 15 秒，到时自动关闭；点"确定"关闭卡片，热点还开着就立即关闭热点。处于二维码视图时，热点因其他原因关闭卡片就自动消失；处于成功视图时不随热点关闭，直到点"确定"或倒数结束。
- **配对码卡片**：有人请求配对时显示在顶层，带 2 分钟倒计时；配对成功或过期后消失。
- **恢复出厂确认框**：复用现有确认框样式。
- 所有卡片都按基础 spec 5.6 的安全区规则布局。二维码用 LVGL 自带的 `lv_qrcode` 控件生成（需要在 `lv_conf.h` 中开启 `LV_USE_QRCODE`）。

### 8.2 串口命令

真机走 USB 串口的 stdin，模拟器走进程 stdin，按行读取：

| 命令 | 作用 |
|---|---|
| `wifi status` | 打印当前状态 |
| `wifi add <ssid> <password>` | 添加网络（SSID 中有空格时用双引号括起来） |
| `wifi forget <ssid>` | 删除网络 |
| `wifi ap on` / `wifi ap off` | 开启 / 关闭配网热点 |

命令解析放在可移植层，读取输入的部分放在平台层。输入的密码不回显，也不写进日志。

### 8.3 设备端多语言

- **范围**：只覆盖系统自带的界面——自检界面、本子项目的网络区块与各卡片，以及子项目 8 的主场景外壳。Agent 生成的场景不在范围内，由场景自己决定显示什么文字。
- **语言设置**：`SystemSettings` 的 `language`（存为 `sys`/`lang`，`zh`/`ja`/`en`，默认 `zh`）。后台"系统"页可以修改；修改后发布 `system.settings` 事件，设备界面在 UI 线程上立即重建文字，不需要重启。设备语言与网页语言相互独立：网页跟随浏览器（7.4），设备跟随这个设置。
- **字符串表**：`wisp_ui` 提供 `ui::tr(StrId)`，三种语言的文字编译进固件（`components/wisp_ui/src/strings_{zh,ja,en}.cpp`），缺失时退回中文。单元测试检查每个 `StrId` 在三种语言里都有非空文字，并且全部字形都能由内置字体渲染。
- **字体**：仍只用一套 16 px、4 bpp 的内置字体，字符集为 GB2312 ∪ 日文假名与 JIS X 0208 符号区 ∪ JIS X 0208 第一水准汉字（实测比原字体多约 222 KiB Flash：除日文字符外，还补齐了原字体缺少的 GB2312 非汉字各区——°、×、→、①、希腊字母、拼音等，Agent 输出中常见）。字体由 `tools/gen_cjk_font.py` 生成（固定版本的 Noto Sans CJK SC 与 `lv_font_conv`），生成命令与来源写进 `docs/references.md`。日文汉字暂用 SC 字形；按语言加载日文字形字体（放在固件或 `storage` 中）留到子项目 8。

---

## 9. 测试与验收

### 9.1 测试分层（TDD，全部能在主机上运行）

| 层 | 测什么 | 怎么测 |
|---|---|---|
| `wisp_net` 服务 | Wi-Fi 状态机（选最强网络、退避间隔、热点超时、配网成功后的热点关闭时机（页面读到结果后 3 秒、热点上连续 3 秒没有设备、最长 30 秒）、待机断开与唤醒重连）；配对（过期、锁定、令牌哈希、淘汰最旧令牌）；WispPack 解析（损坏或截断的包） | GoogleTest，用 `FakeWifi` 和可注入的假时钟 |
| `wisp_core` 服务 | `ModelProfileService` 的只写语义、Key 绑定源与沿用、按类型激活与删除规则、旧格式迁移；`SystemSettings` 的校验与立即生效 | GoogleTest |
| `wisp_admin` 路由 | 每个接口的鉴权、CSRF 请求头、Host 校验、错误格式、`/pair/ap` 只接受热点网段；**`GET /ai/profiles` 等所有响应里都不出现 Key** | 对内存中的假请求直接调路由表，不经过 socket |
| 模拟器 HTTP 服务 | HTTP/1.1 解析、gzip 静态文件与 304、WebSocket 握手和帧收发、连接上限 | 用真实 socket 连 `127.0.0.1` |
| 串口命令 | 命令解析、引号、错误提示 | GoogleTest |
| 端到端冒烟 | 启动模拟器 → 配对 → 走一遍主要接口 → WebSocket 收到日志 → 新建 `llm` 配置并 `/ai/profiles/{id}/test` 成功 | 扩展 `tests/smoke/`，只用 Python 标准库；LLM 用本机起的假 OpenAI 兼容服务器（只实现 `/models`），不访问外网 |
| 前端与资源包 | import 能否解析、体积预算 | `tools/pack_assets.py` 的检查（含 256 KB 预算），纳入 `tools/ci.sh` |

### 9.2 真机验收清单（由用户执行）

1. 不配网开机：各项功能正常，网络区块显示"未配网"。
2. 点"开始配网"：屏幕显示二维码；手机扫码连上热点后自动弹出配网页；配网成功后手机页面和设备屏幕都显示新地址，热点在页面读到结果约 3 秒后关闭（最长 30 秒）；设备屏幕的成功视图倒数 15 秒后自动关闭，或点"确定"立即关闭。
3. 浏览器访问 `http://wisp.local:8900` 或 `http://<显示的 IP 地址>:8900`，用屏幕上的配对码完成配对。
4. 各页面正常显示；修改亮度后屏幕立即变化；日志页实时滚动。
5. 在"对话"标签页从预设新建一套配置并填 Key，"测试连接"成功并列出模型；刷新页面后 Key 只显示"已设置"。再新建第二套并设为当前，第一套的激活标记移走；"语音识别"和"语音合成"标签页的激活不受影响。在"语音识别"标签页用"沿用 Key"新建同平台配置，测试连接成功。
6. 关掉路由器：设备退避重试，界面不卡；路由器恢复后自动重连。
7. 待机超过 10 分钟后 Wi-Fi 断开，唤醒后自动重连。
8. 恢复出厂设置：网页发起后，必须在设备上确认才会执行；执行后已配对的浏览器全部失效。
9. 性能：后台页面首次加载不超过 2 秒；Wi-Fi 连接期间自检界面滚动的 `WISP_PERF` 帧率不低于基础 spec 约定的基准（发布构建，渲染路径 `-O2`）。
10. 网页上传固件：上传一个自己构建的固件 → 设备弹确认框 → 点"是"后重启进入新版本，设置与数据仍在；再上传一次并点"否"，设备继续运行旧版本；上传其他板子的固件被拒绝。

### 9.3 实施顺序

实施计划按以下顺序拆分，每块完成后都能在模拟器中独立验证：

1. 板级分区表与 sdkconfig 迁移、分区约定
2. `hal::Wifi` 接口与 `wisp_net` 服务（Wi-Fi、配对、WispPack）
3. `ModelProfileService`、`SystemSettings`
4. 两套平台实现（HTTP 服务端/客户端、Wi-Fi、mDNS）
5. `wisp_admin` 路由与 WebSocket
6. 前端与打包脚本
7. 设备端界面与串口命令
8. 真机联调

---

## 10. 风险与对策

| 风险 | 对策 |
|---|---|
| Wi-Fi 占用内部 RAM，挤压 LVGL 和其他任务 | Wi-Fi/LWIP 缓冲尽量放进 PSRAM（`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`）；联调时记录空闲内存，写入硬件文档 |
| Wi-Fi 射频与 Flash 访问影响帧率 | 用验收第 9 项对比 `WISP_PERF`；必要时降低 HTTP 任务优先级 |
| 手机系统对强制门户的探测方式各不相同 | 覆盖主流探测路径；即使门户没有自动弹出，配网卡片上也显示配网页地址，可以手动打开 |
| 模拟器的手写 HTTP 服务与 `esp_http_server` 行为不一致 | 路由层只依赖 `HttpServer` 接口的最小语义；端到端冒烟在模拟器上跑，关键接口在真机验收时再走一遍 |
| 明文 HTTP 下令牌被同网段抓取 | 见 6.4；令牌可撤销，HTTPS 留给子项目 7 |
