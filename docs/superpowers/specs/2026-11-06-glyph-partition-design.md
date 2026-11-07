# Wisp 正文位图字分区设计文档

- 日期：2026-11-06
- 状态：已批准（2026-11-06），已实施
- 范围：16 px 正文位图字 `lv_font_cjk_16` 从应用镜像挪进单独的 flash 分区 `glyphs`，运行时映射读取；分区表随之调整。显示效果不变。属于基础设施，插在 MVP 计划（`2026-11-06-mvp.md`）Task 4 之前，在 `feat/agent-core` 上实施。
- 前置：矢量字体（`2026-11-05-vector-font-design.md`，下文"矢量字体 spec"）、单一 flash 线程（`2026-11-06-flash-thread-design.md`）

---

## 1. 背景与目标

### 1.1 现状

- 应用镜像 4,209,475 B，占 5 MB OTA 槽的 80.3%，已超过 80% 的预算（`docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`"镜像大小"）。MVP 还要加 Agent 和语音。
- 只读数据 1.92 MB，其中 `lv_font_cjk_16`（Noto Sans CJK SC 16 px、4 bpp、8709 个码位，`tools/gen_cjk_font.py` 用 `lv_font_conv` 生成）约 1.08 MB：位图 996,827 B、字形描述 8710 × 8 B、码位表与 69 个 cmap、kern 对。
- 本板开了 XIP（`CONFIG_SPIRAM_XIP_FROM_PSRAM`），启动时整个镜像拷进 PSRAM。8 MB PSRAM 里程序占约 4 MB，堆只剩约 4 MB。
- 字体数据几乎不变，却随每次 OTA 上传、每次 XIP 拷贝。

### 1.2 为什么不改成矢量渲染

2026-11-06 在主机上用设备同款 stb_truetype 渲染 16 px 对比：没有 hinting 的矢量字横笔常落在两行像素之间（"三"、"番"、"醒"发灰、发虚）；对 alpha 做对比度拉伸或伽马加深都不能把笔画挪回整像素。`lv_font_conv` 生成位图时用了 FreeType 自动 hinting，横笔清晰。换 FreeType 做全矢量是另一项较大的改动（约 +300 KB 代码、首屏光栅化开销），不在本 spec 范围。

### 1.3 目标

1. `lv_font_cjk_16` 的数据不再链接进应用镜像，改放 `glyphs` 分区；渲染出的像素与现在逐字节相同。
2. 镜像减约 1.08 MB；XIP 下 PSRAM 堆多出约 1 MB。
3. `nvs`、`fonts` 两个分区的偏移和大小不变：设置和矢量字体在换表后保留。OTA 槽取整为 4.5 MB，代价是 `storage` 缩小 128 KB、换表时重新格式化一次（第 2 节，用户 2026-11-06 选定）。
4. 分区缺失或镜像无效时只降级，不阻止启动（AGENTS.md 第 3 节"设备模型"）。

### 1.4 不做

- 全矢量正文、FreeType、hinting。
- 管理后台上传字体分区（OTA 仍然不碰 `fonts`/`glyphs`）。
- 其他位图字（图标字 24/32/48 共约 0.1 MB，留在镜像里）。
- 关 XIP 或只开 `SPIRAM_FETCH_INSTRUCTIONS`（另行评估）。

---

## 2. 分区表

`boards/waveshare_esp32_s3_touch_amoled_2_16/partitions.csv`：

| 分区 | 偏移 | 大小 | 变化 |
|---|---|---|---|
| nvs | 0x9000 | 0x6000 | 不变 |
| otadata | 0xF000 | 0x2000 | 不变 |
| phy_init | 0x11000 | 0x1000 | 不变 |
| ota_0 | 0x20000 | 0x480000（4.5 MB） | 原 0x500000 |
| ota_1 | 0x4A0000 | 0x480000 | 原 0x520000 起 |
| glyphs | 0x920000 | 0x120000（1.125 MB） | 新增，data 子类型 0x41 |
| storage | 0xA40000 | 0x2F0000（2.94 MB） | 原 0xA20000 起、0x310000；末尾仍在 0xD30000 |
| fonts | 0xD30000 | 0x2D0000 | 不变 |

- 新镜像约 3.13 MB，占新槽 66%，在 80% 预算内留约 0.61 MB 给 MVP。
- `glyphs` 1,179,648 B，装约 1,119,310 B 的镜像（位图 996,827 B、字形描述 87 KB、kern 22 KB、码位表等 13 KB、头部），余约 60 KB。字符集是固定的 GB2312 + JIS X 0208 一级，不预期增长；超出时构建失败（3.3）。
- 换表必须 USB 烧录一次（`idf.py flash`）：写入新分区表、`ota_data_initial`（从 `ota_0` 启动）、应用、`fonts.bin`、`glyphs.bin`。`nvs` 原样保留，亮度、语言等 NVS 设置都在；已保存的 Wi-Fi 网络和配对记录在 `/int/net/` 下，会随 `/int` 一起清空；旧 `ota_1` 的内容作废。`storage` 的偏移和大小都变了，LittleFS 挂载失败后由 `format_if_mount_failed` 重新格式化：**`/int` 里的 Wi-Fi 网络、配对记录、已安装的场景、场景数据和文件全部清空**，换表后要重新推送需要的场景。SD 卡上的数据不受影响。
- 子类型 0x41 紧接 `fonts` 的 0x40。

## 3. 镜像格式与构建

### 3.1 格式

`glyphs.bin` = 32 字节头 + 载荷，风格与矢量字体镜像一致（矢量字体 spec 3.2）。全部整数小端。

头部：`"WGLY"`、格式版本（`kGlyphImageVersion = 1`）、头长 32、载荷长度、载荷 CRC-32，其余补零。

载荷依次是：

1. 字体度量：`line_height`、`base_line`、`underline_position`、`underline_thickness`、`bpp`（必须为 4）、`kern_scale`、各节的条目数。
2. 字形描述：每个字形 `bitmap_index`(u32)、`adv_w`(u16)、`box_w`/`box_h`(u8)、`ofs_x`/`ofs_y`(i8)，用固定的可移植布局，不照搬 LVGL 结构体的位域。
3. cmap：每项 `range_start`(u32)、`range_length`(u16)、`glyph_id_start`(u16)、`type`(u8)、码位表/偏移表的条目数与在载荷内的偏移。
4. 码位表与偏移表（u16）。
5. kern 对：字形 id 对（u16 × 2）与值（i8）。
6. 位图：与现在 `glyph_bitmap[]` 逐字节相同。

### 3.2 生成

- 新脚本 `tools/pack_glyph_image.py`：读已提交的 `components/wisp_ui/fonts/lv_font_cjk_16.c`（数据源不变，`gen_cjk_font.py` 仍然生成它），解析出各数组，写成 3.1 的格式。只用标准库。
- 离线测试 `tools/pack_glyph_image_test.py`（纳入 `tools/ci.sh`）：头部字段、CRC、各节偏移不越界，样例字形的位图与 `.c` 中的一致。

### 3.3 构建与烧录

- `firmware/main/CMakeLists.txt` 照 `fonts.bin` 的写法：分区表里有 `glyphs` 分区时生成 `build/glyphs.bin`，超过分区大小时构建失败；`idf.py flash` 一并写入；`idf.py fonts-flash` 同时写 `fonts.bin` 与 `glyphs.bin`。分区表没有 `glyphs` 的板子两者都没有。
- `components/wisp_ui/CMakeLists.txt` 在固件构建里不再编译 `fonts/lv_font_cjk_16.c`。主机构建继续把它编进 `wisp_ui`（宏 `WISP_UI_BUILTIN_CJK16`，只在 `NOT ESP_PLATFORM` 时定义），作为内置回退：几百个不传镜像就启动 `UiRuntime` 的主机测试照常有中文字形，等价性测试也以它为参照。固件从不链接它。
- 模拟器：构建时同样生成 `build/sim/glyphs.bin`，启动时读进内存，`--glyph-image <路径|none>` 可以覆盖或关闭（与现有 `--font-image` 一致）。`none` 同时关掉内置回退（`builtin_glyphs = false`），用来在模拟器里看真机缺分区时的样子（4.3 第 2 步）。

## 4. 运行时

### 4.1 校验与加载

- 新增可移植模块 `wisp/ui/glyph_image.hpp`：`Result<GlyphImage> open_glyph_image(std::span<const uint8_t> raw)`。检查魔数、版本、长度、CRC，再检查每个字形的 `bitmap_index + ceil(box_w × box_h × 4 / 8)` 不超过位图节、cmap 的条目和偏移都在载荷内、kern 的字形 id 都小于字形数。错误码与 `open_font_image()` 相同：NotFound（空或没有魔数）、Unsupported（版本不对）、InvalidArgument（其余）。
- 校验通过后，除位图以外的各节（字形描述、cmap、码位表、kern，约 130 KB）解码进 PSRAM（`os::alloc(Mem::Large)`），组装成 LVGL 的 `lv_font_fmt_txt_dsc_t`。只有 `glyph_bitmap` 指向映射区。
- 理由：渲染时从 flash 读到的只有位图字节。映射读取即使赶上擦写读错（本板有自动挂起，正常不会发生），也只是画错像素，不会因为读到错误的索引或长度而越界——与矢量字体 spec 把 cmap 计数拷出来的做法同理。

### 4.2 平台与接入

- 平台层新增 `wisp::port::esp::map_glyph_partition()`（`esp_font_partition.cpp`），与 `map_font_partition()` 相同：整个分区只读、永久映射，没有该分区时返回空。
- `Platform`/`UiConfig` 新增 `glyph_image` 字段（与 `font_image` 并列）和 `builtin_glyphs`（缺省 true，是否允许用主机的内置字体），`UiRuntime::start()` 在装好矢量字体镜像之后调 `open_glyph_image()`，按 4.3 选定正文的 CJK 回退。
- `default_font()` 的回退链保持"Montserrat 16 → CJK 16"，CJK 16 换成由镜像组装的字体，仍外包 `a8_font()` 的 8 位字形缓存。像素与现在相同。`default_font()` 返回的指针始终不变，每次 `start()` 只改它的 A8 外包字体的 `fallback`；`stop()` 后恢复为启动前的回退（主机是内置字体，固件为空），所以任何 runtime 启动之前，主机上的 `default_font()` 已经能显示中文。
- 由镜像组装的字体一旦用过就不释放（`a8_font()` 以字体地址为键、终身保留外包），下次 `start()` 拿到同一段字节（地址、长度、CRC 都相同）时复用，反复启停不增长 PSRAM。

### 4.3 降级

`glyph_image` 为空或校验失败时记一条警告（写明 `open_glyph_image()` 给出的原因），然后：

1. 主机构建且 `builtin_glyphs` 为 true：CJK 回退到内置的 `lv_font_cjk_16`（同样外包 `a8_font()`）。固件没有这一步。
2. 矢量字体镜像可用：CJK 回退到 16 px 的 Tiny TTF 字体（`start()` 时创建，行框与查字过滤与矢量字体 spec 4.3 的大字相同，缓存条数按 `font_cache_entries(16)`；`stop()` 时销毁）。它自己不再回退，免得与正文字体构成回环。能显示，但比位图字发虚。
3. 矢量字体也不可用：只剩 Montserrat 16，中文显示为占位框。界面仍然能启动、能操作。

### 4.4 资源

- 内部 RAM：映射句柄约 80 B，其余不变。
- PSRAM：堆多出约 1 MB（XIP 拷贝变小），减去 4.1 的约 130 KB。
- 启动：多一次约 1.08 MB 的 CRC，预计十几毫秒，以 `boot_ms` 实测为准。
- 虚拟地址：多映射 1.125 MB 数据地址空间，与 `fonts` 分区一样占到关机。

## 5. 测试

主机（GoogleTest，TDD）：

- `open_glyph_image()`：正常镜像通过；魔数、版本、长度、CRC 错误各自返回对应错误码；字形位图越界、cmap 偏移越界、kern id 越界都被拒绝。
- 等价性：主机构建的 `wisp_ui` 里仍有原来的 `lv_font_cjk_16.c`（3.3），以它为参照，对 8709 个码位逐个比较字形描述、kern 值和位图，结果必须完全相同。
- 降级：`glyph_image` 为空时，有矢量镜像则中文字符走 16 px Tiny TTF；两者都没有时 `default_font()` 仍然可用、不崩溃。

真机（交给用户，硬件文档新增一项）：

1. 换表烧录后，NVS 设置和矢量大字都还在，Wi-Fi 网络和配对需要重新设置；`/int` 已重新格式化（场景列表为空），重新推送一个示例场景能正常安装和打开；启动日志有 glyphs 镜像校验通过的一行。
2. `idf.py size`：镜像约 3.13 MB。自检页 PSRAM"共"约 5 MB。
3. `bench scroll 10` 不开热点与开热点各测，fps 不低于当前基准（硬件文档"性能基准"）。
4. 正文中文显示与改动前一致（目测自检页、番茄钟、待办）。
5. 只烧应用、擦掉 `glyphs` 分区：能启动，中文以矢量 16 px 显示，日志有警告。
6. OTA 上传新镜像期间滚动正文，显示正常、不崩溃。

## 6. 文档

- `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`：分区表、镜像大小、验收项。
- `docs/porting-guide.md`"字体分区"：`glyphs` 分区可选；开 XIP 又有 `glyphs` 的板子同样要求 flash 自动挂起。
- `docs/architecture.md`：启动流程里的映射。
- 矢量字体 spec 的"降级"与 16 px 描述加一句指向本 spec。
