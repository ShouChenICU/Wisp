# Wisp 矢量字体（大字号文字）设计文档

- 日期：2026-11-05
- 状态：待审阅
- 范围：设备界面的大字号文字（20 px 及以上）改用矢量字体渲染；属于子项目 3a（场景运行时）之后的界面增补，不是路线图里单列的子项目
- 前置：子项目 3a 场景运行时核心（`2026-11-04-scene-runtime-core-design.md`，下文简称"场景 spec"）；子项目 2 连接与管理后台（`2026-11-03-connectivity-admin-design.md`，下文简称"连接 spec"）
- 依据：可行性验证（spike，本地分支 `spike/vector-font` 的 `c5f8550`、`727be0d`，代码已丢弃，只取结论）的真机实测数据，下文"实测"均指它

---

## 1. 背景与目标

### 1.1 现状

- 正文是 16 px 位图字：`lv_font_montserrat_16` 回退到 `lv_font_cjk_16`（Noto Sans CJK SC 的 16 px、4 bpp 子集，`tools/gen_cjk_font.py` 生成），外面包一层 8 位字形缓存（`components/wisp_ui/src/a8_font.cpp`）。
- 大字号只有 Montserrat 24/32/48，只含 ASCII 和 LVGL 的图标符号（`LV_SYMBOL_*`）。`ui::font(px)` 把它们回退到 16 px 的 CJK 字：**大字号里的中文、日文仍然只有 16 px**。场景的 `font = "lg"/"xl"/"xxl"` 也是这样。
- Montserrat 24/32/48 三个字体共占应用镜像约 171 KB。

### 1.2 目标

1. 20 px 及以上的文字（中、日、英）用矢量字体渲染，字形清晰、字号真实。
2. 系统界面和场景用同一个入口 `ui::font(px)` 取大字号字体；场景的 `font` 属性可以直接写像素数。
3. 字体数据放在独立的 `fonts` flash 分区，经内存映射读取，不占应用镜像、不占内部 RAM。
4. 分区缺失、镜像损坏或格式不符时，大字号文字退回位图字并记警告，绝不阻止启动（AGENTS.md 第 3 节"设备模型"）。
5. 去掉 Montserrat 24/32/48，大号图标改由专门的图标位图字提供。

### 1.3 已定决策

用户已定（2026-11-05）：

| 议题 | 决策 |
|---|---|
| 方案 | 方案 A：Noto Sans CJK SC（与思源黑体字形相同；`gen_cjk_font.py` 锁定的同一个文件）放在专用的 `fonts` flash 分区。批准重新分区，重新分区会清空 `storage` |
| 渲染 | LVGL 自带的 Tiny TTF（stb_truetype），字形缓存在 PSRAM |
| 正文 | 16 px 正文保持位图字，与现在相同 |
| 大号英文 | 英文大字也用同一个矢量字体（拉丁字母是 Source Sans 风格，与 16 px 正文的 Montserrat 不同，接受） |
| 自检页标题 | 用大号矢量字，只显示当前语言（spike 里中日英三语并排的固定标题只是测试） |

由实测得出、本设计采用的结论：

| 议题 | 决策 | 依据 |
|---|---|---|
| 字体格式 | TrueType 轮廓（`glyf`），不用 CFF | 设备上 CFF 冷渲染慢 3–5 倍（48 px 汉字 0.84 ms 对 2.7 ms，整屏冷渲染 101 ms 对 184 ms）；热态相同。TTF 只多 0.55 MB flash |
| 字符集 | 与 16 px 位图字相同，再加拉丁字母（3.1） | 0 缺字 |
| 分区大小 | spike 的数字：`storage` 0x310000、`fonts` 0x2D0000，只写在板目录的 `partitions.csv` 里 | AGENTS.md 第 3 节"板子无关" |
| 读取方式 | 整个分区 `esp_partition_mmap` 后直接读 | 映射 70–78 µs；瓶颈在光栅化而不在 flash（镜像拷进 PSRAM 再测，数字相同）；内部 RAM 只多 80 B |
| UI 任务栈 | `wisp_ui` 从 8 KB 加到 10 KB，在场景负载下用 `mem` 复测 | stb 光栅化在栈上约 0.5 KB 扫描线缓冲；实测余量从 1208 B 降到 808 B |
| OTA | OTA 不碰 `fonts` 分区；镜像带头部，开机校验 | 3.2、3.6 |

### 1.4 不在本期范围内

- 管理后台上传字体镜像（要流式写分区、要占 httpd 的内部 RAM 预算，留给以后的子项目；本期只用 USB 写入）。
- 网页固件上传顺带更新 `fonts` 分区。
- 16 px 正文改成矢量字；hinting；粗体、字重、可变字体；彩色 emoji；竖排；日文字形（日文汉字继续用 SC 字形，与 16 px 一致）。
- 空闲时预热常用字形（3.6 节的冷渲染尖峰先实测，必要时另行设计）。
- 字符集以外的字（例如韩文）：按码位回退到 16 px 位图字，再没有就显示占位框，与现在相同。

---

## 2. 组件与分层

### 2.1 改动一览

| 层 | 位置 | 改动 |
|---|---|---|
| 工具 | `tools/gen_vector_font.py`（新） | 从锁定的上游 OTF 生成子集 TTF：pyftsubset → 三次转二次（cu2qu）→ `post` 表改为 format 3。fontTools 锁定 4.55.3，只在生成时用 |
| 工具 | `tools/pack_font_image.py`（新） | 给 TTF 加 32 字节头部，打成分区镜像 `fonts.bin`（3.2）。只用 Python 标准库，构建时运行 |
| 工具 | `tools/gen_icon_font.py`（新） | 从已拉取的 LVGL 源码里的 FontAwesome 生成 24/32/48 px 图标位图字（4.6） |
| 数据 | `components/wisp_ui/fonts/noto_sans_cjk_sc_subset.ttf`（新，提交） | 子集 TTF，约 2.4 MB |
| 数据 | `components/wisp_ui/fonts/lv_font_wisp_icons_{24,32,48}.c`（新，提交） | 图标位图字 |
| 板级 | `boards/<板名>/partitions.csv` | 参考板加 `fonts` 分区、缩小 `storage` |
| `wisp_ui` | `wisp/ui/font_image.hpp`（新） | 镜像头部与 sfnt 结构的校验，纯函数 |
| `wisp_ui` | `wisp/ui/fonts.hpp`（新）、`src/fonts.cpp`（新） | `default_font()`、`font(px)` 迁到这里并扩展：字号档位、Tiny TTF 字体、回退链、行高、缓存上限；`ui_runtime.hpp` 继续 include 它，现有调用方不用改 |
| `wisp_ui` | `UiConfig`、`UiRuntime` | `UiConfig::font_image`；`start()` 校验并装入镜像，`stop()` 在 `lv_deinit()` 之前销毁矢量字体；UI 任务栈默认 10 KB |
| `wisp_ui` | `config/lv_conf.h` | `LV_USE_TINY_TTF 1`（不开文件读取）；`LV_FONT_MONTSERRAT_24/32/48` 改为 0 |
| `wisp_port_esp` | `wisp/port/esp/esp_font_partition.hpp`（新） | 找到并映射 `fonts` 分区，返回只读字节区间 |
| `wisp_app` | `Platform::font_image` | 平台交给 App 的字体镜像（与 `Platform::assets` 同样是一段 `std::span`），App 原样转交 `UiConfig` |
| `wisp_app` | 自检页标题、配对卡片 | 标题改用 `ui::font(32)`；配对码数字改用 `ui::font(24)`（不再直接引用 Montserrat 24） |
| `wisp_scene` | 白名单与主题 | `font` 属性接受像素数（5.3）；画布 `g:text` 的 `font` 选项同样 |
| 固件 | `firmware/main/CMakeLists.txt`、`main.cpp` | 构建时打包 `fonts.bin`、加烧录目标；`app_main` 映射分区填入 `Platform::font_image` |
| 模拟器 | `simulator/CMakeLists.txt`、`main.cpp`、`sim_options` | 构建时打包同一个 `fonts.bin` 放在 `wisp_sim` 旁边；`--font-image <路径>|none` |

### 2.2 平台边界

- `wisp_ui` 不 include 任何平台头文件（AGENTS.md 第 3 节）。它只拿到一段 `std::span<const uint8_t>`：真机上是映射好的分区，模拟器上是读进内存的文件。取得这段字节的代码在平台层：真机在 `wisp_port_esp`，模拟器在 `simulator/main.cpp`（与网页资源包 `assets.wpak` 的加载方式相同）。
- 校验逻辑是可移植的纯函数（`ui::open_font_image()`），主机测试覆盖；平台层不解析镜像。
- 分区名 `fonts` 是约定，偏移、大小只在板目录里；代码按名字找分区，找不到就是"没有矢量字"，照常启动。
- 芯片、板子名都不出现在可移植层里。

### 2.3 内存与 flash 访问（AGENTS.md 第 5 节）

- **映射**：`esp_partition_mmap` 属于"映射 flash"，只能在内部 RAM 栈的任务里做。真机在 `app_main`（主任务，栈在内部 RAM）里、`App::start()` 之前映射，映射句柄一直保留到关机。
- **读取**：之后 UI 任务读映射区不算访问 flash（"读已经映射进地址空间的常量不算"），UI 任务的 `flash_access` 声明不因字体改变。
- **内部 RAM**：映射句柄 80 B；Tiny TTF 的字体对象、字形缓存、stb 光栅化的临时缓冲都经 `lv_malloc` 走 LVGL 的堆，在 PSRAM（`wisp/ui/lv_heap.hpp`）。实测 `lv_heap_internal_bytes()` 全程不增长。UI 任务栈加 2 KB 是唯一的内部 RAM 增量，必须真机复测（7.3）。
- **PSRAM**：每个字号档位的字形缓存有上限（4.4），所有档位合计最多约 1.25 MB；镜像本身不占 PSRAM（读的是映射的 flash）。模拟器把镜像读进普通内存。
- **地址空间**：映射整个 2.81 MB 分区要占数据总线虚拟地址空间（实测映射到 `0x3C800000`）；开了 XIP 的板子上代码和只读数据也映射在 PSRAM 里，两者共用这段地址空间，新板子要确认放得下（`docs/porting-guide.md`）。

---

## 3. 字体镜像

### 3.1 字形来源与子集

- 源字体：Noto Sans CJK SC Regular，`notofonts/noto-cjk` 发布标签 `Sans2.004` 的 `Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf`，SHA-256 `2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b`，与 `tools/gen_cjk_font.py` 锁定的是同一个文件；下载与校验复用它的 `fetch_font()`。
- 字符集：`gen_cjk_font.build_charset()` 的全部码位（ASCII、GB2312、JIS X 0208 第 1–8 区与第 16–47 区、U+3000–303F、U+FF01–FF5E、字符串表里的字，减去 U+2252；目前 8709 个），再加：
  - Latin-1 补充 U+00A0–U+00FF 全部（源字体全有）；
  - 常用标点 U+2010–U+2027、U+2030–U+203A、U+20AC（€）、U+2122（™）中源字体有字形的那些。源字体缺的码位（实测 U+2017、U+201B、U+201F、U+2023、U+2024、U+2031、U+2034、U+2036–U+2038）在脚本里按 `gen_cjk_font.ABSENT_FROM_SOURCE` 的方式显式列出、排除；列表之外有缺字就失败。
  - 不加拉丁扩展-A：源字体缺其中 98 个码位。
- 子集参数（与实测一致）：`pyftsubset --layout-features=kern --no-hinting --desubroutinize --drop-tables+=GSUB,vhea,vmtx,VORG,DSIG --notdef-outline --name-IDs=0,1,2,3,4,5,6,13,14 --name-languages=1033`。保留 `kern` 的 GPOS（实测 stb 支持的 PairPos 格式 1，`To`、`AV`、`LT` 生效）；`palt`、竖排和连字用不到，去掉。
- 转换：CFF 轮廓用 fontTools 的 `Cu2QuPen` 转二次曲线（误差 1.0 font unit，肉眼与 CFF 无差别），写成 `glyf`/`loca`，`maxp` 改 1.0 版，`post` 改 format 3（去掉约 96 KB 的字形名），sfnt 版本 `0x00010000`。脚本改编自 fontTools 的 `Snippets/otf2ttf.py`（MIT），文件头注明。
- 名字：保留上游的 name 表（`Noto Sans CJK SC`）。上游许可证没有声明保留字体名（第 6 节），不需要改名。
- 结果约 2.42 MB（实测 2,419,288 B，加拉丁字符后略增），提交为 `components/wisp_ui/fonts/noto_sans_cjk_sc_subset.ttf`。仓库已经提交了 7.3 MB 的 `lv_font_cjk_16.c`，同样的做法；提交生成物可以让主机测试、CI 和固件构建都离线进行。
- 重新生成：`<装了 fonttools==4.55.3 的 venv>/bin/python tools/gen_vector_font.py --workdir <仓库外的新空目录>`。字符串表加了新字时，与 `gen_cjk_font.py` 一起重新生成（4.2：新字在矢量字里缺失时只是逐字回退到 16 px，不会出错）。
- 离线检查 `tools/gen_vector_font_test.py`（纳入 `tools/ci.sh`，只用标准库）：已提交的 TTF 是 `glyf` 格式（版本 `0x00010000`、有 `glyf`/`loca`、没有 `CFF `）、没有 hinting 表（`fpgm`/`prep`/`cvt `）、`cmap` 覆盖的码位恰好等于脚本当前的字符集。

### 3.2 镜像格式

分区里存的是"头部 + TTF"。所有整数小端：

| 偏移 | 长度 | 内容 |
|---|---|---|
| 0 | 4 | 魔数 `WFNT` |
| 4 | 2 | 格式版本，目前为 1 |
| 6 | 2 | 头部长度，目前为 32；TTF 从这里开始 |
| 8 | 4 | TTF 长度（字节） |
| 12 | 4 | TTF 的 CRC-32（与 zip/gzip 相同，`zlib.crc32`，即 `wisp::crc32`） |
| 16 | 16 | 保留，写 0 |

- `tools/pack_font_image.py <ttf> -o <fonts.bin> [--max-bytes N]` 生成镜像：检查输入是 `glyf` 格式的 TTF（拒绝 `OTTO`，即 CFF），超过 `--max-bytes`（分区大小）就失败。输出逐字节可复现。
- 格式版本只在头部或载荷的解释方式改变时加 1。固件只认自己的版本；版本不符按"镜像无效"处理（3.6）。

### 3.3 构建与打包

- **模拟器**：`simulator/CMakeLists.txt` 加一个自定义命令，把已提交的 TTF 打包成 `build/sim/fonts.bin`（依赖 TTF 和打包脚本，变了才重打），放在 `wisp_sim` 旁边；主机测试也读这个文件（经编译定义 `WISP_FONT_IMAGE`），所以测试和截图用的都是真字体。
- **固件**：`firmware/main/CMakeLists.txt` 同样打包到 `build/fonts.bin`。用 `partition_table_get_partition_info()` 读出当前板子分区表里 `fonts` 的大小，作为 `--max-bytes`；板子的分区表没有 `fonts` 分区时不打包、不加烧录目标，构建照常（这块板就没有矢量字）。

### 3.4 分区

参考板（`waveshare_esp32_s3_touch_amoled_2_16`，16 MB flash）的新分区表：

| 名字 | 类型 | 子类型 | 偏移 | 大小 |
|---|---|---|---|---|
| nvs | data | nvs | 0x9000 | 0x6000（不变） |
| otadata | data | ota | 0xF000 | 0x2000（不变） |
| phy_init | data | phy | 0x11000 | 0x1000（不变） |
| ota_0 | app | ota_0 | 0x20000 | 0x500000（不变） |
| ota_1 | app | ota_1 | 0x520000 | 0x500000（不变） |
| storage | data | littlefs | 0xA20000 | **0x310000**（3.06 MB，原 0x5E0000） |
| **fonts** | data | 0x40（自定义） | **0xD30000** | **0x2D0000**（2.81 MB） |

- `fonts` 的大小 = 镜像约 2.42 MB × 1.2 向上取整到 64 KB，排到 16 MB 末尾；余量约 0.5 MB，够加字。
- `storage` 偏移不变、变小。LittleFS 的超级块记着块数，挂载时与分区大小不符就失败，`esp_storage.cpp` 已有的 `format_if_mount_failed` 会重新格式化：**`/int` 里的数据（已安装的场景、场景数据、文件）全部清空**，用户已批准。`nvs` 不动，Wi-Fi、配对、亮度、语言等设置都保留。
- 新板子是否要 `fonts` 分区、给多大，由板子自己决定（`docs/porting-guide.md`）；没有 PSRAM 的板子不应加，否则字形缓存会落进内部 RAM。

### 3.5 写入设备

- `idf.py -p <端口> flash` 同时写入 `fonts.bin`（与引导程序、分区表、应用一起），新设备一条命令就完整。
- `idf.py -p <端口> fonts-flash` 只写字体分区（ESP-IDF 的 `esptool_py_flash_target()` + `esptool_py_flash_to_partition()`，与 `spiffs_create_partition_image()` 的做法相同；`idf.py` 把未知命令当作构建目标执行）。
- `idf.py -p <端口> app-flash` 只写应用，日常改代码时用它，省掉每次约 1.5 MB（压缩后）的字体写入。
- 网页固件上传（OTA）只换应用，不碰 `fonts`。因此固件里的镜像格式版本（3.2）与分区里的必须一致；以后改格式时，新固件要能在旧镜像上降级运行，并在日志里提示用 USB 重写字体分区。
- 从旧分区表升级（现有设备）：必须用 USB 完整烧录一次 `idf.py flash`（写入新分区表、应用和字体）；之后才能继续用网页上传。

### 3.6 启动校验与降级

`UiRuntime::start()` 在建 LVGL 显示之前调用 `ui::open_font_image(config.font_image)`：

1. 区间为空：`NotFound`，"no font image"（模拟器 `--font-image none`、板子没有 `fonts` 分区或映射失败）。
2. 魔数不是 `WFNT`：`NotFound`，"fonts partition holds no font image"（新擦除的分区全是 0xFF）。
3. 版本不是 1：`Unsupported`，"font image format N, firmware reads 1"。
4. 头部长度小于 32、TTF 长度为 0 或超出区间：`InvalidArgument`。
5. CRC-32 不符：`InvalidArgument`，"font image CRC mismatch"（写入中途断开等）。
6. sfnt 结构：版本必须是 `0x00010000`；表目录在载荷之内；每张表的偏移与长度都在载荷之内；必须有 `cmap`、`glyf`、`head`、`hhea`、`hmtx`、`loca`、`maxp`。不满足就 `InvalidArgument`。stb_truetype 读字体时不做边界检查，损坏的数据可能让它越界读，所以这些检查必须在把数据交给 Tiny TTF 之前做完。

- 通过：记 `WLOG_I`"fonts: image ok, <字节数> bytes, crc <8 位十六进制>"，矢量字可用。
- 不通过：记 `WLOG_W`"fonts: <原因>; large text uses bitmap fonts"，按 4.2 的降级规则运行。`start()` 照常成功——字体问题永远不会让 UI 起不来。
- 某个字号的 `lv_tiny_ttf_create_data_ex()` 返回空（内存不够等）：这一档记一次警告，此后一直用降级字体，不反复重试。
- 校验时间：CRC 要把约 2.4 MB 读一遍（经 cache 读映射区），估计 0.1–0.15 s，计入 `boot_ms`，真机实测记录（参考板现在 `boot_ms` 约 1.83 s，上限 3 s）。
- `WISP_BOOT_REPORT` 不加字段；字体状态只在日志里。

---

## 4. 字体服务 `ui::font(px)`

### 4.1 字号档位

| 请求 | 返回 |
|---|---|
| `px < 20` | `default_font()`：16 px 位图正文字，与现在相同 |
| `px ≥ 20` | 矢量字，字号取档位表里不大于 `px` 的最大一档：**20、24、28、32、40、48、64、80、96、128**；超过 128 取 128 |

- 每一档第一次被请求时在 UI 线程上创建 Tiny TTF 字体（实测 90–116 µs），之后一直复用；同一档永远是同一个指针，所以场景的属性比较、样式缓存都不受影响。
- 用档位而不是任意字号：字体对象和字形缓存的个数有上限（4.4），PSRAM 用量可预期；场景和以后的 Agent 写任意像素数都能落到某一档。
- 纯函数 `ui::font_step(int px)` 给出档位（`px < 20` 时为 16），供测试和文档使用。
- `ui::vector_fonts_ready()`（UI 线程）：本次 `start()` 装入的镜像是否通过校验，供测试与诊断。

### 4.2 回退链

```
矢量字(档位) → 图标字(不大于档位的最大一个：24/32/48；档位 20 没有) → default_font()（Montserrat 16 → CJK 16）
```

- Tiny TTF 找不到字形时返回 false，LVGL 按 `fallback` 逐字往下找。Noto 没有 `LV_SYMBOL_*`（FontAwesome 的私用区码位），这些字由图标字给出；字符集以外的字回退到 16 px。
- 混排时回退字形画在主字体的基线上，实测没有错位。
- **降级**（镜像缺失或无效，或该档创建失败）：`font(px)` 返回"图标字 → `default_font()`"，即图标仍是大号、文字退到 16 px。Montserrat 24/32/48 删除之后没有大号拉丁位图字可退（4.6），这是降级模式下可以接受的样子。

### 4.3 行高

Tiny TTF 按 `hhea` 的上伸 1160、下沉 −288（单位 1/1000 em）算行高，行高 = 1.448 em，32 px 字行高 46 px，比同字号 Montserrat 高很多。改为：

- `line_height = ceil(0.28 × 档位) + ceil(0.99 × 档位)`，即约 1.27 em（24 px → 31，32 px → 41，48 px → 62，128 px → 163）；`base_line = ceil(0.28 × 档位) − 1`。
- 减 1 的原因：Tiny TTF 给每个字形位图多分配一行（`lv_tiny_ttf.c` 里 `box_h = y2 − y1 + 1`、`ofs_y = −y2`），LVGL 按 `ofs_y` 摆放时整个字形比按基线算的位置高一行，多出的一行在位图底部、是空白的。基线下移一行正好抵消，字形墨迹的上下界都落在行框内。
- 理由：LVGL 的标签按自己的文字区域裁剪（`lv_label.c` 的 `draw_main` 把裁剪区设为文字区），字形超出行框的部分在首行、末行会被裁掉，中间行会压到相邻行。字符集里最高的拉丁字形是带重音的大写字母（`Å` 0.983 em，`À`/`É` 等 0.970 em），最低的是 `|`、`¦`（−0.279 em）与 `ç`、`g`（约 −0.25 em）；汉字与假名在 −0.12…0.88 em 之内。行框取 −0.28…0.99 em 正好全部装下，汉字在行内基本居中（偏上约 0.025 em）。
- 实现：创建字体后直接改 `lv_font_t` 的 `line_height` 与 `base_line` 两个公开字段（不调 `lv_tiny_ttf_set_size()`，它会改回去）。余量只有 0–1 px，由主机测试逐档检查（7.1）；若某档差 1 px，把 0.99 调到 1.0，并在计划里记录。
- 字符集里另有少数超出这个范围的符号（如 U+3031 〱、全角 `＾`），按设计接受，不为它们加大行高。
- **请用户确认**（第 9 节第 1 条）：brief 提议约 1.2×；1.2× 会裁掉 `À`/`Å`/`É` 的重音和 `|`、`g` 的下端。若接受这些裁切，可改为 1.2×。

### 4.4 缓存与内存上限

- Tiny TTF 的缓存按条数计，每个字体对象一份（`lv_cache_class_lru_rb_count`）。实测每条（位图 + 缓存节点）约 `0.6 × px² + 250` 字节：24 px 590 B、32 px 872 B、48 px 1643 B、64 px 2708 B。
- 每档条数 = `clamp(128 KiB / (0.6 × px² + 250), 8, 256)`，使每档上限约 128 KB：20 px 256 条、24 px 220、28 px 181、32 px 151、40 px 108、48 px 80、64 px 48、80 px 32、96 px 22、128 px 13。纯函数 `ui::font_cache_entries(int step)` 给出。
- 十档全部用满时合计约 1.25 MB PSRAM；常见界面只用 2–3 档，约 0.3 MB。字距缓存沿用 `LV_TINY_TTF_CACHE_KERNING_CNT`（256 条，很小）。
- 不用 Tiny TTF 的 0 缓存模式（每次绘制都重新光栅化）。
- 大字号档位条数少：128 px 只缓存 13 个字形，适合时钟数字这类短文本；128 px 的长段中文每次重绘都要重新光栅化（实测 64 px 汉字冷渲染约 1.1 ms/字，128 px 预计 3–5 ms/字），文档里说明。

### 4.5 线程与生命周期

- `font(px)` 只能在 UI 线程上、`UiRuntime::start()` 与 `stop()` 之间调用（与现在的说明相同；场景的 `theme_font` 也在 UI 线程上）。`default_font()` 仍然任何线程都可以调用。
- 矢量字体对象由 LVGL 的堆分配，缓存用 LVGL 的 `lv_cache`，与 LVGL 实例同生共死。`UiRuntime::stop()` 在 `lv_deinit()` 之前销毁全部矢量字体（`lv_tiny_ttf_destroy`）并清空档位表；下一次 `start()` 后重新按需创建。
- 镜像区间由平台持有，必须活得比 `UiRuntime` 久（真机是永久映射，模拟器是 `main()` 里的一个缓冲）。

### 4.6 图标字与移除 Montserrat 24/32/48

- `tools/gen_icon_font.py` 读 LVGL 9.5.0 源码（`build/sim/_deps/lvgl-src` 或 `firmware/managed_components/lvgl__lvgl`，与 `gen_pinyin_dict.py` 的查找方式相同；不下载）里的 `scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff`，码位取自 `src/font/lv_symbol_def.h` 的全部 `LV_SYMBOL_*`（私用区 U+E000–U+F8FF，去掉 `LV_SYMBOL_DUMMY` U+F8FF），用锁定的 `lv_font_conv@1.5.3`（经 `npx`）生成 24、32、48 px、4 bpp、不压缩的 `lv_font_wisp_icons_{24,32,48}.c`。图标集合与原来 Montserrat 自带的相同，现有界面的图标不会变化。
- 字体名不含"Font Awesome"：FontAwesome 的字体许可（OFL 1.1）保留了这个名字，修改版不能用（第 6 节）。
- 4 bpp 不压缩，可以和 16 px 字一样经 A8 包装缓存 8 位字形。
- `tools/gen_icon_font.py --check`（纳入 `tools/ci.sh`，在 cmake 配置之后、离线）检查已提交的三个文件的码位与当前 LVGL 的 `LV_SYMBOL_*` 一致。
- 之后 `lv_conf.h` 关掉 `LV_FONT_MONTSERRAT_24/32/48`，代码里不再直接引用这三个字体（配对卡片的数字改用 `ui::font(24)`，自检标题改用 `ui::font(32)`）。镜像净省约 70 KB（三个 Montserrat 约 171 KB，减去图标字约 100 KB；以 `idf.py size` 实测为准）。
- 档位 64 及以上的图标用 48 px 图标字，与现在（`font(px)` 超过 48 一律取 Montserrat 48）相同。

---

## 5. 使用者

### 5.1 自检页标题

- 标题用 `ui::font(32)`，文字仍绑定 `StrId::SelfCheckTitle`，只显示当前界面语言：中文"自检"、日文"セルフチェック"、英文"Self Check"；切换语言时随之更新。出厂默认语言是中文（连接 spec 8.3），所以默认显示"自检"。
- 删掉 `self_check_header.cpp` 里的 `title_font()`（Montserrat 24 回退到正文字）。
- 降级时标题是 16 px 正文字，布局不需要特殊处理。

### 5.2 配对卡片

配对码数字从 `&lv_font_montserrat_24` 改为 `ui::font(24)`（数字在矢量字里，字距与字形略有变化）。

### 5.3 场景：`font` 属性接受像素数

- `font` 取值扩展为：令牌 `xs`/`sm`/`md`（16 px 正文）、`lg`（24）、`xl`（32）、`xxl`（48），或 **16 到 128 的整数像素**。数字按 4.1 取档：16–19 用正文字，20 及以上用矢量字的档位。
- 越界、非整数、其他类型都在建树时报错，沿用场景 spec 4.5 的格式（行号、控件名、属性名），期望值写作 `one of xs, sm, md, lg, xl, xxl, or an integer 16 to 128 (pixels)`。
- 画布 `g:text(x, y, s, {font = ...})` 的 `font` 选项接受同样的取值。
- 令牌保留，现有场景不用改；`lg`/`xl`/`xxl` 自动变成矢量字，行高随之变化（4.3），示例测试会检查布局仍在安全区内。
- `docs/scene-api.md` 由 `schema_markdown()` 生成，补充数字字号、档位表和"128 px 适合短文本"的说明；`Schema.DocIsUpToDate` 逐字节比对。

### 5.4 示例场景

| 场景 | 改动 |
|---|---|
| 计数器 | 数字单独一行，`font = 96`；"已点击 … 次"的说明文字用正文字 |
| 番茄钟 | 剩余时间从 `xxl` 改为按表盘大小取字号（表盘直径 ≥ 280 时 72，否则 48） |
| 时钟表盘 | 表盘上加大号数字时间（如 `font = 56`）：在画布上叠一层 `ui.stack` 里的标签，放在表盘下三分之一；星期仍用小字 |

示例测试（`tests/scene/examples_test.cpp`）在矩形、圆角矩形、圆形屏上检查控件四角都可见，覆盖这些改动。

### 5.5 留给子项目 4

Agent 生成场景时，提示词里给出 `font` 的数字写法与档位表（来自 `docs/scene-api.md`），说明 128 px 只适合短文本、大字号首次显示有几十毫秒的冷渲染。

---

## 6. 许可

- **Noto Sans CJK SC**：SIL Open Font License 1.1。已对照上游许可证原文核实（`notofonts/noto-cjk` 标签 `Sans2.004` 根目录的 `LICENSE`，SHA-256 `6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2`）：文件是 OFL 1.1 全文，**没有版权行，也没有声明保留字体名**。版权声明在字体的 name 表里：ID 0 "© 2014-2021 Adobe (http://www.adobe.com/)."，ID 13/14 是许可证说明与链接。spike 报告里的"with Reserved Font Name 'Source'"是思源黑体（Source Han Sans）仓库的写法，不适用于 Noto 版本——这也是选 Noto 版本的原因：子集化、转 TTF 都属于 OFL 意义上的修改版，若基于思源黑体，修改版就不能再用含 "Source" 的名字。
  - 子集保留 name 表 ID 0、13、14，满足"随字体附带版权与许可"。
  - 随固件分发，不单独出售。
  - `THIRD_PARTY_NOTICES.md` 现有的 "Noto Sans CJK SC (via `lv_font_cjk_16.c`)" 一节扩展到子集 TTF 与 `fonts` 分区镜像，`docs/references.md` 补一行生成方式。
- **Font Awesome Free 5**（图标字）：字体部分 SIL OFL 1.1，版权 "Copyright (c) 2022 Fonticons, Inc. (https://fontawesome.com) with Reserved Font Name: "Font Awesome"."（LVGL 源码 `scripts/built_in_font/font_license/FontAwesome5/LICENSE.txt`）。生成的位图字是修改版，名字不用"Font Awesome"。`THIRD_PARTY_NOTICES.md` 新增一节，许可证文字从上述文件原样复制。原来 Montserrat 24/32/48 里的同一批图标由 LVGL 自带，现在改由本仓库生成，所以要单独登记。
- **stb_truetype**（`stb_truetype_htcw.h`，Tiny TTF 使用）：随 LVGL 9.5.0 一起获取，MIT / 公有领域双许可；在 `THIRD_PARTY_NOTICES.md` 的 LVGL 一节里与 LodePNG、TJpgDec 并列，许可证文字取自该头文件末尾。
- **fontTools 4.55.3**（MIT）：只在生成脚本里用，不进固件；`tools/gen_vector_font.py` 的转换部分改编自其 `Snippets/otf2ttf.py`，文件头注明，并登记到 `docs/references.md` 与 `THIRD_PARTY_NOTICES.md`（AGENTS.md 第 6 节）。

---

## 7. 测试与验收

### 7.1 主机测试（GoogleTest，TDD）

| 对象 | 内容 |
|---|---|
| `open_font_image` | 合成镜像：正常；空区间；全 0xFF；魔数错；版本 2；头部长度 < 32；长度越界；CRC 不符；CFF（`OTTO`）；表目录或某张表越界；缺 `glyf`。构建产出的 `fonts.bin` 通过 |
| 档位与缓存 | `font_step` 的边界（19→16、20→20、31→28、47→40、500→128）；`font_cache_entries` 的数值 |
| 矢量字 | 真镜像下 `font(32)` 的"自"和"A"都由它自己解析、高度明显大于 16 px；同档同指针；行高与基线按 4.3；ASCII、Latin-1、假名、字符集内全部汉字在 20/48/128 三档都完整落在行框内 |
| 回退 | `LV_SYMBOL_OK` 在 48 档由 48 px 图标字解析、在 20 档由正文字解析；字符集外的字（如"한"）回退到 16 px |
| 降级 | 没有镜像、CRC 损坏两种情况：`start()` 成功，`vector_fonts_ready()` 为 false，`font(48)` 的"自"由 16 px CJK 解析、图标仍是 48 px |
| 生命周期 | 启动、停止、再启动后 `font(32)` 正常渲染；停止后 LVGL 堆回到启动前的水平 |
| 自检标题 | 标题字体是 `font(32)`；文字随语言切换为"自检"/"セルフチェック"/"Self Check" |
| 场景 `font` | 白名单接受令牌和 16–128 整数，拒绝 15、129、32.5、`"huge"`；数字按档位映射；画布 `font` 选项同样；`scene-api.md` 最新 |
| 示例 | 计数器、番茄钟、时钟在三种屏形上控件四角可见 |

Python（标准库）：`tools/pack_font_image_test.py`（头部字段、CRC 与 `zlib.crc32` 一致、可复现、拒绝 CFF 与超大）、`tools/gen_vector_font_test.py`（3.1）、`tools/gen_icon_font.py --check`（4.6），全部纳入 `tools/ci.sh`。

### 7.2 模拟器

- 默认加载 `fonts.bin`，截图里自检标题是 32 px 矢量字。
- `sim_smoke` 加一个用例：`--font-image none` 启动正常、日志有降级警告、截图正常。

### 7.3 真机验收清单（由用户执行，控制者负责烧录）

1. `idf.py -p <端口> flash` 完整烧录：日志 `fonts: image ok`；`storage.int` 状态 ok（已重新格式化）；Wi-Fi、语言、亮度等设置仍在；`boot_ms` < 3000，记录比改动前多了多少。
2. 自检标题在中、日、英三种语言下清晰、不缺字、不被裁切；配对卡片的数字正常。
3. 重新安装示例场景：计数器、番茄钟、时钟的大字正常；一个临时测试场景同时显示 `font = 128` 的中文、`xxl` 的混排文字和 `lg`/`xl`/`xxl` 图标，图标是大号的。
4. 降级：`parttool.py erase_partition --partition-name=fonts` 后重启，日志有警告，大字退到 16 px、图标仍大，不崩溃；`idf.py fonts-flash` 后恢复。写入一个截断的镜像（只写前 1 MB）同样降级。
5. 网页上传新固件后字体仍正常（OTA 不碰 `fonts`）。
6. 性能：自检列表滚动的 `WISP_PERF` 不低于基准（不开热点 22.7 fps、开热点 28.2 fps）；记录首次进入大字号页面的冷渲染时间。
7. 内存：空闲与场景负载（番茄钟运行 + 128 px 测试页）下执行 `mem`，记录内部 RAM 空闲/最小/最大块与 `wisp_ui` 栈的 high-water mark（剩余应 ≥ 1 KB，否则继续加栈）、PSRAM 空闲；内部 RAM 仍满足 `wisp/app/ram_budget.hpp`；结果写入 `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`。
8. `idf.py size`：应用镜像变化（去掉 Montserrat 24/32/48、加入 Tiny TTF 与图标字）写入硬件文档。

### 7.4 实施顺序（供计划参考）

子集生成与 TTF → 镜像打包、分区与烧录目标 → 镜像校验与平台接线 → 字体服务（档位、回退、行高、缓存、生命周期）→ 图标字与移除 Montserrat → 自检标题 → 场景 `font` 数字与示例 → 文档与许可 → 真机验收。

---

## 8. 风险与对策

| 风险 | 对策 |
|---|---|
| 损坏或写了一半的镜像让 stb_truetype 越界读、崩溃 | 头部 CRC 覆盖整个 TTF；sfnt 表目录与每张表的边界在交给 Tiny TTF 之前检查（3.6） |
| 开机多出 CRC 校验时间 | 实测并记录；若超过 0.2 s，再考虑只在首次启动或分区内容变化时完整校验（需另行设计，不在本期） |
| UI 任务栈溢出（stb 光栅化的栈缓冲） | 栈加到 10 KB；场景负载下用 `mem` 复测 high-water mark |
| 首次显示大字号页面的冷渲染尖峰（实测 48 px 约 40 个字多 55 ms） | 记录实测值；可接受则不处理，必要时以后加空闲预热 |
| 字形缓存占用 PSRAM，与场景 VM、图片像素内存竞争 | 每档条数上限，合计最多约 1.25 MB；PSRAM 用完时 LVGL 的堆退回内部 RAM 并单独计数（`lv_heap_internal_bytes()`），真机验收时检查 |
| 行高变化让现有界面（场景 `lg`/`xl`/`xxl`、配对卡片）溢出 | 示例测试在三种屏形上检查可见性；真机目视 |
| 字体分区与固件版本不一致（只做了 OTA，没重写字体） | 镜像格式版本只在格式变化时加 1，字符集变化只会逐字回退；版本不符时降级并在日志提示用 USB 重写 |
| 重新分区清空 `storage` | 用户已批准；验收第 1 条确认设置保留、场景需要重新安装 |
| 映射 2.8 MB 占用数据总线地址空间（与 XIP、PSRAM 映射共享） | 参考板实测可行；移植指南提醒新板子确认 |

---

## 9. 需要用户确认的裁定

1. **行高**：约 1.27×（`ceil(0.28 px) + ceil(0.99 px)`，基线为 `ceil(0.28 px) − 1`），而不是 brief 提议的约 1.2×；理由见 4.3（标签会裁切超出行框的字形）。
2. **字号档位**：20、24、28、32、40、48、64、80、96、128；20 以下仍是 16 px 位图字。
3. **缓存上限**：每档约 128 KB，十档合计最多约 1.25 MB PSRAM。
4. **烧录方式**：`idf.py flash` 每次都写字体分区；`idf.py fonts-flash` 只写字体，`idf.py app-flash` 只写应用；管理后台上传字体留给以后。
5. **场景字号写法**：沿用 `font` 属性，令牌之外接受 16–128 的整数像素，不另加属性。
6. **加入的拉丁字符**：Latin-1 补充全部与常用标点；不加拉丁扩展-A（源字体缺 98 个）。
7. **图标字**：24/32/48 三个尺寸，包含全部 `LV_SYMBOL_*`（与原 Montserrat 相同）；64 档以上用 48 px 图标。
8. **开机完整 CRC 校验**（约 0.1–0.15 s，实测为准）；字体状态只写日志，不进 `WISP_BOOT_REPORT`。
9. **提交生成物**：子集 TTF（约 2.4 MB）提交进仓库，构建时只做打包，离线可构建。
