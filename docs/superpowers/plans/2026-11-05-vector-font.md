# Wisp 矢量字体（大字号文字）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 20 px 及以上的文字（中、日、英）改由 Noto Sans CJK SC 的矢量子集渲染：字体镜像放在专用 `fonts` flash 分区、映射后由 LVGL Tiny TTF 读取，系统界面和场景都经 `ui::font(px)` 取字，镜像缺失或损坏时只降级不阻止启动；同时去掉 Montserrat 24/32/48，大号图标改由图标位图字提供。

**Architecture:** 工具链生成子集 TTF（提交进仓库）并在构建时加 32 字节头部打成 `fonts.bin`；真机 `app_main` 映射 `fonts` 分区、模拟器读构建目录里的同一个文件，都以 `std::span` 经 `Platform::font_image` → `UiConfig::font_image` 交给 `wisp_ui`。`UiRuntime::start()` 用可移植的 `ui::open_font_image()` 校验（魔数、版本、长度、CRC-32、sfnt 表边界），`ui::font(px)` 按档位在 UI 线程上懒建 Tiny TTF 字体，回退链"矢量字 → 图标字 → 16 px 正文字"，`stop()` 在 `lv_deinit()` 之前销毁。

**Tech Stack:** LVGL 9.5.0 Tiny TTF（stb_truetype）、Noto Sans CJK SC `Sans2.004`、fontTools 4.55.3（只在生成脚本里）、lv_font_conv 1.5.3（经 `npx`）、ESP-IDF v6.0.3（`esp_partition_mmap`、`esptool_py_flash_to_partition`）、GoogleTest、Python 3 标准库。

**Spec:** `docs/superpowers/specs/2026-11-05-vector-font-design.md`（下文"spec N"均指它；"场景 spec"指 `2026-11-04-scene-runtime-core-design.md`）

## 分支

本分支 `feat/vector-font` 从 `feat/scene-runtime-core` 的 `a99045b` 切出（该分支尚未合入 `main`）。spike 分支 `spike/vector-font`（`c5f8550`、`727be0d`）只作参考，不合并、不 cherry-pick：它把映射写在 `main.cpp`、标题写死三语、校验只看 sfnt 头，这些都按 spec 重做。

## 相对 spec 的细化（写计划时裁定，执行前请用户确认）

1. **字体服务的文件位置**：`default_font()`、`font(px)` 从 `ui_runtime.{hpp,cpp}` 迁到新的 `wisp/ui/fonts.hpp` 与 `src/fonts.cpp`，`ui_runtime.hpp` 继续 `#include "wisp/ui/fonts.hpp"`，调用方不改。装入与释放镜像的函数 `detail::install_font_image()`/`detail::release_vector_fonts()` 放在私有头 `src/fonts_internal.hpp`，只有 `UiRuntime` 调用。
2. **模拟器读镜像的位置**：与 `assets.wpak` 一样写在 `simulator/main.cpp`（`load_font_image()`），不进 `wisp_port_sim`：只有可执行文件需要它，测试经 `tests/support/font_image.hpp` 直接读同一个文件。
3. **测试拿镜像的方式**：`simulator/CMakeLists.txt` 生成 `build/sim/fonts.bin`（目标 `wisp_sim_fonts`）；要用真字体的测试 `add_dependencies(<test> wisp_sim_fonts)` 并定义 `WISP_FONT_IMAGE="${CMAKE_BINARY_DIR}/fonts.bin"`，经 `tests/support/font_image.hpp` 的 `test_font_image()` 读取（读一次、缓存在静态 `std::vector`）。
4. **现有测试的预期随之改变**：`UiRuntime.FontSizesWrapA8AndFallBackToCjk` 断言 `font(20) == font(16)`，20 现在是矢量档位。该测试改名为 `UiRuntime.SmallSizesUseBodyFont`，只保留 `px < 20` 的断言；矢量部分移到新的 `vector_font_test`。
5. **UI 任务栈**：`UiConfig::task_stack` 默认值改为 10240；`flash_access` 不变（UI 任务替自检界面读写 Settings，栈仍在内部 RAM），内部 RAM 多 2 KB，Task 9 实测。
6. **降级字体在 Task 3–4 与 Task 5 之间不同**：Task 4 时降级仍返回现有的 Montserrat 24/32/48 包装；Task 5 加入图标字、删掉 Montserrat 之后，降级才变成"图标字 → 正文字"。

## Global Constraints

逐字摘自 AGENTS.md 的适用规则：

- **可移植层禁止 include 平台头文件**：`esp_*`、`freertos/*`、`driver/*`、IDF 的 `hal/`、`soc/*`、`rom/*`、`lwip/*`、`mbedtls/*`、`curl/*`、`sdkconfig.h`、`nvs.h`/`nvs_flash.h`、`sdmmc_cmd.h`、`SDL*`，以及平台层的 `wisp/port/*`。另外每一层只能依赖上面顺序中排在它之前的层，不得反向依赖。这条规则由 `tools/check_layering.py` 强制检查。
- **板子无关**：可移植层和平台层里不得出现具体板子的引脚、芯片型号或常量。这些只能写在 `boards/` 和 `drivers/` 中，通过设备的 caps 对外暴露。……可移植层（含 `lv_conf.h`）不出现芯片名。
- **屏幕形状**：……任何界面（包括场景、Agent 生成的界面）都必须按 Display caps 的 `shape` 和 `hal::safe_rect()`/`row_span()` 布局，不得写死边距或假设整屏可见。
- **UI 单线程**：只有 UI 线程可以调用 LVGL，其他线程一律通过 `UiRuntime::post()` 把操作交给 UI 线程；脚本也不直接调用 LVGL。
- **设备模型**：……设备缺失或初始化失败时只降级运行，不得阻止启动。
- **语言与错误处理**：C++20，编译选项 `-fno-exceptions -fno-rtti`。不使用 `dynamic_cast` 和 `typeid`，错误一律通过 `wisp::Result<T>` 返回。
- **命名**：组件前缀 `wisp_`，命名空间 `wisp::<层>`，头文件路径 `wisp/<层>/<名>.hpp`，日志宏 `WLOG_D/I/W/E`。
- **组件 CMake**：使用 `wisp_component(...)`，让同一份 CMakeLists 同时兼容 idf.py 和普通 CMake。
- **格式与语言**：格式以 `.clang-format` 为准。代码标识符、注释和提交信息用英文；面向用户的界面文字可以用中文。
- **测试**：使用 GoogleTest，坚持 TDD（先写失败的测试，再实现）。测试输出要干净，不能有告警。
- **依赖**：外部依赖锁定精确版本，固件提交 `dependencies.lock`。
- **内存分配**：大块缓冲用 `os::alloc(Mem::Large)`，DMA 缓冲用 `Mem::Dma`，不要直接调用平台的分配函数。
- 内部 SRAM 只留给必须放在内部的东西：DMA 缓冲；会读、写、擦除或映射内部 flash 的任务的栈；ISR 和 IRAM 代码/数据。其余超过几 KB 的一律用 `Mem::Large`（PSRAM）。
- 新任务都要声明它会不会访问内部 flash（`os::TaskConfig::flash_access`）……读、写、擦除或映射内部 flash 都算访问……读已经映射进地址空间的常量和网页资源包不算。……开了 XIP 的板子上普通读写不再停 cache，只有映射 flash（`esp_partition_mmap`、OTA 收尾校验）还会 abort，所以违规代码在这类板子上大多测不出来，模拟器也测不出来。栈的大小按 `mem` 命令实测的 high-water mark 加余量来定。
- 预算：……消耗内部 RAM 的改动必须上真机重新测量。
- **注释**：只解释现在的代码为什么这样写（约束、不变量、踩过的坑），不写"哪一轮评审/哪次修复改了什么"……禁止引用 `.superpowers/` 下的任何文件……禁止评审编号式引用……`tools/ci.sh` 对此有 grep 守卫。
- 移植或改编外部代码时，在文件头注明来源和许可，并登记到 `docs/references.md` 与 `THIRD_PARTY_NOTICES.md`。
- 能在模拟器里验证的，就不要依赖真机。需要真机的验证步骤要明确写出来交给用户执行，不得声称已经验证过。
- 机器可读的输出（`WISP_BOOT_REPORT`、`WISP_MANIFEST`、`WISP_PERF`）都是"前缀 + 单行 JSON"。修改这些输出的格式时，要同步更新冒烟测试和文档。（本计划不改它们。）
- 改动渲染路径时，要对比 `WISP_PERF` 的数值，不得让帧率低于 spec 和计划中约定的基准。
- **生成代码**：`idf.py bmgr` 生成的代码已被 git 忽略，不要手改。
- **分支**：不在 `main` 上直接开发，使用功能分支。push、merge、发布之前先征得用户同意。
- **提交**：采用 Conventional Commits 风格。每个提交结尾附 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`。**不 push。不烧录**（烧录由控制者在 Task 9 做）。

本计划的固定约束：

- 数值（spec 原文）：镜像头部 32 字节、魔数 `WFNT`、格式版本 1、CRC-32 同 `zlib.crc32`；档位 20、24、28、32、40、48、64、80、96、128，`px < 20` 用 16 px 正文字；每档缓存条数 `clamp(131072 / (0.6·px² + 250), 8, 256)`；`line_height = ceil(0.28·px) + ceil(0.99·px)`、`base_line = ceil(0.28·px) − 1`（抵消 Tiny TTF 把字形画高一行，spec 4.3）；图标字 24/32/48；场景 `font` 数字 16–128；参考板 `storage` 0xA20000/0x310000、`fonts` 0xD30000/0x2D0000、子类型 0x40；UI 任务栈 10240。
- 分区大小与偏移只出现在 `boards/<板名>/partitions.csv`；代码只按分区名 `fonts` 查找。
- `esp_partition_mmap` 只在 `app_main` 里调用（主任务栈在内部 RAM）。`wisp_ui` 只接触 `std::span<const uint8_t>`。
- 镜像在交给 `lv_tiny_ttf_create_data_ex()` 之前必须通过 `ui::open_font_image()`（stb_truetype 不做边界检查）。
- 生成脚本需要的网络与 fontTools/npx 只在开发者重新生成时用；`tools/ci.sh`、主机构建、固件构建全部离线。
- 验证命令：主机 `cmake -S simulator -B build/sim -G Ninja && cmake --build build/sim && ctest --test-dir build/sim -R '<正则>' --output-on-failure`；提交前 `bash tools/ci.sh`；固件 `bash -c '. ~/esp/esp-idf-v6.0.3/export.sh >/dev/null && cd firmware && idf.py build'`。
- 删除文件只用仓库内的字面绝对路径，不用变量拼出的路径，不 `git clean`。

## Review Focus

1. **损坏、截断、全 0xFF 或格式不符的镜像**：期望 `UiRuntime::start()` 照常成功、记一条警告、大字退到降级字体，Tiny TTF 从未拿到这段数据（没有越界读、没有崩溃）。测试：Task 3 `FontImage.*`（11 种坏镜像）、Task 4 `VectorFont.CorruptImageDegrades`、Task 3 `App.BootsWithoutFontImage`、`sim_smoke` 的 `--font-image none` 用例。
2. **`UiRuntime` 停止后再启动**：矢量字体由 LVGL 的堆分配、挂在 `lv_cache` 上，`lv_deinit()` 之后旧指针不能再用。期望 `stop()` 先销毁全部矢量字体，重启后重新创建，无悬空指针、无泄漏。测试：Task 4 `VectorFont.RestartRecreatesFonts`、`VectorFont.StopReleasesLvglHeap`。
3. **字形被行框裁切**：标签按文字区裁剪。期望在 20/48/128 三档上 ASCII、Latin-1、假名、字符集内全部汉字都落在 `[0, line_height)` 之内。测试：Task 4 `VectorFont.GlyphsFitLineBox`。
4. **flash 访问规则**：`esp_partition_mmap` 只在 `app_main`；UI 任务只读映射区；没有新任务。审查 Task 3 的 `map_font_partition()` 调用点与注释。
5. **场景 `font` 数字的边界与文档**：15、129、32.5、字符串非令牌都报错且信息含行号与期望值；`docs/scene-api.md` 与 schema 同步。测试：Task 7 `Schema.FontAcceptsTokensAndPixels`、`Schema.FontRejectsOutOfRange`、`Schema.DocIsUpToDate`。

---

## 文件结构总览

```
tools/
  gen_vector_font.py  gen_vector_font_test.py          # 子集 TTF（fontTools，开发时）+ 离线检查
  pack_font_image.py  pack_font_image_test.py          # TTF -> fonts.bin（标准库，构建时）
  gen_icon_font.py                                      # LVGL 的 FontAwesome -> 图标位图字；--check 离线
components/wisp_ui/
  fonts/noto_sans_cjk_sc_subset.ttf                     # 提交的生成物
  fonts/lv_font_wisp_icons_{24,32,48}.c                 # 提交的生成物
  include/wisp/ui/{font_image,fonts}.hpp
  src/{font_image,fonts}.cpp  src/fonts_internal.hpp
  config/lv_conf.h                                      # LV_USE_TINY_TTF 1；Montserrat 24/32/48 -> 0
components/wisp_port_esp/
  include/wisp/port/esp/esp_font_partition.hpp  ui/esp_font_partition.cpp
components/wisp_app/  include/wisp/app/app.hpp (Platform::font_image)  src/{app,self_check_header,net_cards}.cpp
components/wisp_scene/ src/{schema,node,widgets,theme,scene_view,scene_view_canvas}.cpp  src/widget_data.hpp
boards/waveshare_esp32_s3_touch_amoled_2_16/partitions.csv
firmware/main/{CMakeLists.txt,main.cpp}
simulator/{CMakeLists.txt,main.cpp,sim_options.{hpp,cpp}}
tests/ui/{font_image_test,vector_font_test}.cpp  tests/support/font_image.hpp
scenes/examples/{counter,pomodoro,clock}/main.lua
docs/{scene-api.md,architecture.md,porting-guide.md,references.md,hardware/waveshare_esp32_s3_touch_amoled_2_16.md}
THIRD_PARTY_NOTICES.md
```

---

### Task 1: 子集 TTF 生成脚本与提交的字体

**Files:**
- Create: `tools/gen_vector_font.py`、`tools/gen_vector_font_test.py`、`components/wisp_ui/fonts/noto_sans_cjk_sc_subset.ttf`
- Modify: `tools/ci.sh`（在 `gen_cjk_font_test.py` 之后运行 `gen_vector_font_test.py`）

**Interfaces:**
- `gen_vector_font.py`：
  - 复用 `gen_cjk_font` 的 `build_charset()`、`fetch_font()`、`FONT_FILE`、`FONT_SHA256`（`import gen_cjk_font`，同目录）。
  - `LATIN_EXTRA`：U+00A0–U+00FF；U+2010–U+2027、U+2030–U+203A、U+20AC、U+2122。`LATIN_ABSENT_FROM_SOURCE = {U+2017, U+201B, U+201F, U+2023, U+2024, U+2031, U+2034, U+2036, U+2037, U+2038}`（写计划时用源字体核实过；执行时若 fontTools 报出的缺字不同，以实际为准并在本计划记录）。
  - `def vector_charset() -> list[str]`：`build_charset()[0]` ∪ (`LATIN_EXTRA` − `LATIN_ABSENT_FROM_SOURCE`)，按码位排序。只用标准库，供离线测试 import。
  - `def main(argv)`：`--workdir <仓库外的新空目录>`（规则同 `gen_cjk_font.py`）、`--font <已下载的 OTF>`、`--output`（默认 `components/wisp_ui/fonts/noto_sans_cjk_sc_subset.ttf`）。在 `main()` 里才 `import fontTools`，版本必须恰为 `4.55.3`，否则退出并打印安装命令 `python3 -m venv <dir> && <dir>/bin/pip install fonttools==4.55.3`。
  - 步骤：写 `unicodes.txt` → `fontTools.subset.main([...])`，参数为 spec 3.1 所列 → 转 glyf（`Cu2QuPen` 误差 1.0、`reverse_direction=True`、`TTGlyphPen`，新 `loca`/`glyf`，删 `CFF `、`VORG`，`maxp` 1.0 版、`maxZones=1` 其余 0，`sfntVersion = "\0\1\0\0"`）→ `post.formatType = 3.0` → 检查 cmap 覆盖全部请求码位（缺任何一个就退出并列出）→ 写输出。转换函数文件头注明 "Adapted from fontTools Snippets/otf2ttf.py (MIT)"。
- `gen_vector_font_test.py`（只用标准库）：解析 sfnt 表目录与 `cmap` 的 format 4 / format 12 子表（自写约 40 行）。

- [ ] **Step 1: 写失败的测试** `tools/gen_vector_font_test.py`：

```python
class Charset(unittest.TestCase):
    def test_vector_charset_extends_bitmap_charset(self):
        base, _ = gen_cjk_font.build_charset()
        chars = gen_vector_font.vector_charset()
        self.assertTrue(set(base) <= set(chars))
        for c in "ÀÅéü€™–—‰": self.assertIn(c, chars)
        self.assertNotIn("‗", chars)
        self.assertEqual(chars, sorted(set(chars), key=ord))

class CommittedFont(unittest.TestCase):
    def test_is_truetype_without_hinting(self):   # sfnt 版本 0x00010000；有 glyf、loca、cmap、head、hhea、hmtx、maxp、GPOS、name
                                                  # 没有 'CFF '、fpgm、prep、'cvt '、GSUB
    def test_cmap_matches_charset(self):          # cmap 码位集合 == set(map(ord, vector_charset()))
```

- [ ] **Step 2: 运行，确认失败** — `python3 tools/gen_vector_font_test.py`（模块与字体都还不存在）。
- [ ] **Step 3: 实现脚本**，在仓库外的空目录里生成（字体从上游下载并校验 SHA-256；fontTools 装在该目录的 venv 里）：
  `python3 -m venv /tmp/claude-1000/<…>/scratchpad/vf/venv && …/venv/bin/pip install fonttools==4.55.3 && …/venv/bin/python tools/gen_vector_font.py --workdir /tmp/claude-1000/<…>/scratchpad/vf/work`。记录输出大小（预计约 2.45 MB）与 SHA-256，写进本计划的执行记录。
- [ ] **Step 4: 验证** — `python3 tools/gen_vector_font_test.py`；`bash tools/ci.sh`。
- [ ] **Step 5: Commit**（显式路径）— `feat(tools): generate the vector CJK font subset as TrueType`

---

### Task 2: 镜像打包、分区表与烧录目标

**Files:**
- Create: `tools/pack_font_image.py`、`tools/pack_font_image_test.py`
- Modify: `tools/ci.sh`（运行 `pack_font_image_test.py`）、`boards/waveshare_esp32_s3_touch_amoled_2_16/partitions.csv`、`firmware/main/CMakeLists.txt`、`simulator/CMakeLists.txt`

**Interfaces:**
- `pack_font_image.py <ttf> -o <out> [--max-bytes N]`：输出 = 头部（spec 3.2：`b"WFNT"`、`u16 1`、`u16 32`、`u32 len`、`u32 crc32`、16 字节 0，`struct.pack("<4sHHII16x", ...)`）+ TTF 原样。输入前 4 字节必须是 `00 01 00 00`（`OTTO` 报 "CFF fonts are not supported; convert to TrueType"）；输出超过 `--max-bytes` 报 "font image is N bytes, the partition holds M"。函数 `pack(ttf_bytes) -> bytes` 供测试 import。
- 分区表（spec 3.4）：

```
storage,   data, littlefs, 0xA20000, 0x310000
fonts,     data, 0x40,     0xD30000, 0x2D0000
```
  文件头注释改为说明：`fonts` 保存矢量字体镜像（spec 3.4），由 `idf.py flash`/`fonts-flash` 写入、OTA 不碰；`storage` 从 0x5E0000 缩小到 0x310000，换成这张表时 `/int` 会被重新格式化，`nvs` 保持原偏移和大小所以设置保留。
- `firmware/main/CMakeLists.txt`（`idf_component_register` 之后）：

```cmake
partition_table_get_partition_info(_wisp_fonts_size "--partition-name fonts" "size")
if(_wisp_fonts_size)
    set(WISP_FONT_IMAGE ${CMAKE_BINARY_DIR}/fonts.bin)
    set(_wisp_font_ttf ${CMAKE_CURRENT_LIST_DIR}/../../components/wisp_ui/fonts/noto_sans_cjk_sc_subset.ttf)
    add_custom_command(OUTPUT ${WISP_FONT_IMAGE}
        COMMAND ${_wisp_python} ${CMAKE_CURRENT_LIST_DIR}/../../tools/pack_font_image.py
                ${_wisp_font_ttf} -o ${WISP_FONT_IMAGE} --max-bytes ${_wisp_fonts_size}
        DEPENDS ${_wisp_font_ttf} ${CMAKE_CURRENT_LIST_DIR}/../../tools/pack_font_image.py
        COMMENT "Packing the vector font into fonts.bin")
    add_custom_target(wisp_fonts_bin ALL DEPENDS ${WISP_FONT_IMAGE})
    idf_component_get_property(_wisp_main_args esptool_py FLASH_ARGS)
    idf_component_get_property(_wisp_sub_args esptool_py FLASH_SUB_ARGS)
    esptool_py_flash_target(fonts-flash "${_wisp_main_args}" "${_wisp_sub_args}" ALWAYS_PLAINTEXT)
    esptool_py_flash_to_partition(fonts-flash fonts ${WISP_FONT_IMAGE})
    add_dependencies(fonts-flash wisp_fonts_bin)
    esptool_py_flash_to_partition(flash fonts ${WISP_FONT_IMAGE})
    add_dependencies(flash wisp_fonts_bin)
endif()
```
  （`_wisp_python` 沿用文件里打包网页时已有的 Python 变量；若名字不同按实际改。这几个函数与 `spiffs_create_partition_image()` 用的是同一组，已在 v6.0.3 的 `components/esptool_py/project_include.cmake` 与 `components/spiffs/project_include.cmake` 核实。）
- `simulator/CMakeLists.txt`：`set(WISP_SIM_FONTS ${CMAKE_CURRENT_BINARY_DIR}/fonts.bin)`，同样的 `add_custom_command`（不带 `--max-bytes`），`add_custom_target(wisp_sim_fonts DEPENDS ${WISP_SIM_FONTS})`，`add_dependencies(wisp_sim wisp_sim_fonts)`。

- [ ] **Step 1: 写失败的测试** `tools/pack_font_image_test.py`：

```python
def test_header_fields(self):        # magic、version 1、header 32、length、crc == zlib.crc32(ttf)、保留 16 字节为 0、载荷原样
def test_reproducible(self):         # 同一输入两次输出逐字节相同
def test_rejects_cff(self):          # b"OTTO..." -> 非零退出，stderr 含 "CFF"
def test_rejects_non_sfnt(self):     # b"\x00\x00\x00\x00..." -> 非零退出
def test_max_bytes(self):            # --max-bytes 小于输出 -> 非零退出，消息含两者字节数
def test_committed_font_fits_reference_partition(self):  # 打包已提交的 TTF，长度 <= 0x2D0000
```

- [ ] **Step 2: 运行，确认失败** — `python3 tools/pack_font_image_test.py`。
- [ ] **Step 3: 实现**脚本、分区表与两处 CMake。
- [ ] **Step 4: 验证** — `bash tools/ci.sh`（模拟器构建产出 `build/sim/fonts.bin`）；固件 `idf.py build` 产出 `firmware/build/fonts.bin`，`ninja -C firmware/build -t targets all | grep fonts-flash` 能看到目标，`firmware/build/flash_args` 里有 `0xd30000 fonts.bin`。不烧录。
- [ ] **Step 5: Commit** — `feat(boards): add the fonts partition and pack the vector font image at build time`

---

### Task 3: 镜像校验、平台提供与启动接线

**Files:**
- Create: `components/wisp_ui/include/wisp/ui/font_image.hpp`、`components/wisp_ui/src/font_image.cpp`、`components/wisp_port_esp/include/wisp/port/esp/esp_font_partition.hpp`、`components/wisp_port_esp/ui/esp_font_partition.cpp`、`tests/ui/font_image_test.cpp`、`tests/support/font_image.hpp`
- Modify: `components/wisp_ui/CMakeLists.txt`、`components/wisp_ui/include/wisp/ui/ui_runtime.hpp`（`UiConfig::font_image`）、`components/wisp_ui/src/ui_runtime.cpp`、`components/wisp_port_esp/CMakeLists.txt`（`esp_partition`）、`components/wisp_app/include/wisp/app/app.hpp`（`Platform::font_image`）、`components/wisp_app/src/app.cpp`、`firmware/main/main.cpp`、`simulator/main.cpp`、`simulator/sim_options.{hpp,cpp}`、`tests/CMakeLists.txt`、`tests/sim/sim_options_test.cpp`、`tests/app/app_boot_test.cpp`、`tests/smoke/sim_smoke.py`

**Interfaces:**

```cpp
// wisp/ui/font_image.hpp -- no LVGL, no platform headers.
namespace wisp::ui {
inline constexpr uint32_t kFontImageVersion = 1;
inline constexpr size_t kFontImageHeaderBytes = 32;
struct FontImage {
    std::span<const uint8_t> ttf;  // points into the caller's bytes
    uint32_t crc32 = 0;
};
// Checks a fonts partition image (spec 3.2, 3.6) and returns its TrueType
// payload. Errors: NotFound (empty, or no "WFNT" magic), Unsupported (other
// format version), InvalidArgument (header, length, CRC or sfnt structure).
Result<FontImage> open_font_image(std::span<const uint8_t> raw);
}  // namespace wisp::ui

// wisp/port/esp/esp_font_partition.hpp
namespace wisp::port::esp {
// Maps the whole "fonts" data partition read-only and keeps it mapped for
// good. Maps flash, so only on a task whose stack is in internal RAM
// (app_main). Empty, with a warning, when there is no such partition or
// the mapping fails.
std::span<const uint8_t> map_font_partition();
}

// wisp::ui::UiConfig
std::span<const uint8_t> font_image;  // raw partition bytes; must outlive the UiRuntime
// wisp::app::Platform
std::span<const uint8_t> font_image;  // empty: large text uses bitmap fonts
```

- `open_font_image` 按 spec 3.6 的顺序检查；sfnt 检查：`numTables` ≥ 1，表目录 `12 + 16·numTables` 在载荷内，每张表 `offset + length` 不溢出且在载荷内，必须有 `cmap glyf head hhea hmtx loca maxp`。错误消息就是 spec 3.6 列出的英文短句。
- `UiRuntime::start()`：在 `lv_init()` 之后、建显示之前调用 `open_font_image(config.font_image)`；通过时 `WLOG_I("ui", "fonts: image ok, %u bytes, crc %08x", ...)` 并保存 `FontImage`（本 Task 只保存，Task 4 用）；不通过时 `WLOG_W("ui", "fonts: %s; large text uses bitmap fonts", msg)`。无论结果如何 `start()` 都继续。
- `App::start()`：`ui_config.font_image = impl_->platform.font_image;`。
- `firmware/main/main.cpp`：在构造 `Platform` 处 `platform.font_image = wisp::port::esp::map_font_partition();`，注释写明为什么必须在 `app_main` 里（映射 flash，主任务栈在内部 RAM；AGENTS.md 第 5 节）。
- `map_font_partition()`：`esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "fonts")`；`esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, &handle)`；成功 `WLOG_I("fonts", "mapped %u bytes in %lld us", ...)`，返回 `{ptr, part->size}`；句柄不释放。
- 模拟器：`SimOptions::font_image`（默认空 = 可执行文件旁边的 `fonts.bin`；`"none"` = 不加载）；`simulator/main.cpp` 的 `load_font_image(opts, argv[0])` 返回 `std::vector<uint8_t>`（读不到时 `WLOG_W` 并返回空），活到 `main()` 结束，`platform.font_image` 指向它。`--help`/用法说明同步。
- `tests/support/font_image.hpp`：`std::span<const uint8_t> test_font_image();`——读 `WISP_FONT_IMAGE`，读不到时 `ADD_FAILURE()` 并返回空。

- [ ] **Step 1: 写失败的测试**

```cpp
// tests/ui/font_image_test.cpp（合成镜像由测试内的 make_image(ttf) 生成：头部 + 一个最小 sfnt，
// 表目录列出 cmap glyf head hhea hmtx loca maxp，各 4 字节内容）
TEST(FontImage, AcceptsWellFormedImage)       // ttf.data() == raw.data() + 32；crc32 == wisp::crc32(ttf)
TEST(FontImage, EmptyIsNotFound)              // code NotFound，message "no font image"
TEST(FontImage, ErasedPartitionIsNotFound)    // 4096 字节全 0xFF -> NotFound，message "fonts partition holds no font image"
TEST(FontImage, OtherVersionIsUnsupported)    // version 2 -> Unsupported，message "font image format 2, firmware reads 1"
TEST(FontImage, ShortHeaderIsInvalid)         // header_bytes 16 -> InvalidArgument
TEST(FontImage, LengthBeyondDataIsInvalid)    // length = raw.size() -> InvalidArgument
TEST(FontImage, CrcMismatchIsInvalid)         // 改载荷一个字节 -> InvalidArgument，message "font image CRC mismatch"
TEST(FontImage, CffIsInvalid)                 // 载荷以 "OTTO" 开头（CRC 正确）-> InvalidArgument
TEST(FontImage, TableOutOfBoundsIsInvalid)    // 某表 offset+length 超出载荷 -> InvalidArgument
TEST(FontImage, TableDirectoryOutOfBoundsIsInvalid)  // numTables 1000 -> InvalidArgument
TEST(FontImage, MissingGlyfIsInvalid)         // 目录里没有 glyf -> InvalidArgument，message 含 "glyf"
TEST(FontImage, BuiltImageIsValid)            // test_font_image() 通过，ttf.size() > 2'000'000
// tests/sim/sim_options_test.cpp
TEST(SimOptions, FontImage)                   // --font-image a.bin -> "a.bin"；--font-image none -> "none"；缺值报错
// tests/app/app_boot_test.cpp
TEST(App, BootsWithoutFontImage)              // Platform::font_image 为空：start() 成功，WISP_BOOT_REPORT 照常输出
```
  `sim_smoke.py` 新增用例 `font_image_none`：`--font-image none --screenshot <png> --exit-after-ms 3000` 启动，退出码 0，输出含 `fonts: no font image; large text uses bitmap fonts`，`check_screenshot()` 通过。

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'FontImage|SimOptions|App\.' --output-on-failure`。
- [ ] **Step 3: 实现**；`tests/CMakeLists.txt` 注册 `font_image_test`（`LIBS wisp_ui wisp_port_sim_os`，`add_dependencies(font_image_test wisp_sim_fonts)`，定义 `WISP_FONT_IMAGE`）。
- [ ] **Step 4: 验证** — 全量 ctest；`bash tools/ci.sh`（分层检查：`wisp_ui` 没有 include 平台头）；固件 `idf.py build`。
- [ ] **Step 5: Commit** — `feat(ui): validate the font image at UI start and map the fonts partition`

---

### Task 4: 字体服务：档位、Tiny TTF、回退、行高、缓存与生命周期

**Files:**
- Create: `components/wisp_ui/include/wisp/ui/fonts.hpp`、`components/wisp_ui/src/fonts.cpp`、`components/wisp_ui/src/fonts_internal.hpp`、`tests/ui/vector_font_test.cpp`
- Modify: `components/wisp_ui/config/lv_conf.h`（`LV_USE_TINY_TTF 1`，`LV_TINY_TTF_FILE_SUPPORT 0` 不变）、`components/wisp_ui/include/wisp/ui/ui_runtime.hpp`（删掉 `default_font`/`font` 的声明，改为 include `fonts.hpp`；`UiConfig::task_stack = 10240`）、`components/wisp_ui/src/ui_runtime.cpp`（删掉两个函数的实现；`start()` 调 `detail::install_font_image()`，`stop()` 在 `lv_deinit()` 之前调 `detail::release_vector_fonts()`）、`components/wisp_ui/CMakeLists.txt`、`tests/CMakeLists.txt`、`tests/ui/ui_runtime_test.cpp`

**Interfaces:**

```cpp
// wisp/ui/fonts.hpp
namespace wisp::ui {
inline constexpr int kBodyFontPx = 16;
inline constexpr int kVectorMinPx = 20;
inline constexpr std::array<int, 10> kFontSteps = {20, 24, 28, 32, 40, 48, 64, 80, 96, 128};
const lv_font_t* default_font();        // unchanged; any thread
int font_step(int px);                  // < 20 -> 16; else the largest step <= px; above 128 -> 128
size_t font_cache_entries(int step);    // clamp(131072 / (0.6*step*step + 250), 8, 256)
const lv_font_t* font(int px);          // UI thread, between UiRuntime::start() and stop()
bool vector_fonts_ready();              // UI thread: this start()'s image passed open_font_image()
}
// src/fonts_internal.hpp
namespace wisp::ui::detail {
void install_font_image(const FontImage* image);  // nullptr: none; called by UiRuntime::start()
void release_vector_fonts();                      // lv_tiny_ttf_destroy() each, clear the table; before lv_deinit()
}
```

- `font(px)`：`step = font_step(px)`；16 → `default_font()`；否则查档位表（`std::array<const lv_font_t*, 10>`，按 `kFontSteps` 下标）。没有就创建：镜像可用时 `lv_tiny_ttf_create_data_ex(ttf.data(), ttf.size(), step, LV_FONT_KERNING_NORMAL, font_cache_entries(step))`，设 `line_height = d + a`、`base_line = d - 1`，其中 `d = (28*step + 99) / 100`（即 `ceil(0.28·step)`）、`a = (99*step + 99) / 100`（即 `ceil(0.99·step)`），`fallback = degraded(step)`；创建失败或镜像不可用时用 `degraded(step)`，并且创建失败只 `WLOG_W` 一次（该档记为降级，不再重试）。
- `degraded(step)`：本 Task 里是现有的 Montserrat 包装（24/28 → 24，32/40 → 32，≥48 → 48，20 → `default_font()`）；Task 5 改为图标字。
- 档位表与镜像指针都是 `fonts.cpp` 的文件内静态变量，只在 UI 线程上读写（`install` 在 `start()` 里、UI 任务开始之前）。
- 注释写明：为什么用档位（字体对象与缓存个数有上限，spec 4.1、4.4）；为什么改行高且不能调 `lv_tiny_ttf_set_size()`（spec 4.3，标签按文字区裁剪）；为什么 `stop()` 要先销毁（字体与缓存在 LVGL 的堆和 `lv_cache` 上）。

- [ ] **Step 1: 写失败的测试**

```cpp
// tests/ui/vector_font_test.cpp（用 test_font_image() 起一个 480x480 FakeDisplay 的 UiRuntime）
TEST(VectorFont, StepsAndThreshold)       // font_step: 0->16, 19->16, 20->20, 23->20, 31->28, 47->40, 63->48, 128->128, 500->128
TEST(VectorFont, CacheEntriesPerStep)     // 20->256, 24->220, 32->151, 48->80, 64->48, 128->13；所有档位 * 估算字节 <= 131072 + 一条
TEST(VectorFont, LargeSizesAreVectorWithCjk)
    // vector_fonts_ready()；font(32) 解析 '自' 与 'A'：resolved_font == font(32)，'自' 的 box_h >= 24；
    // font(33) == font(32)；font(16) == default_font()；font(19) == default_font()
TEST(VectorFont, LineHeightAndBaseline)   // font(24): base 6, line 31；font(48): 13, 62；font(128): 35, 163
TEST(VectorFont, GlyphsFitLineBox)
    // 20/48/128 三档，码位 U+0021–007E、U+00A1–00FF、U+3041–3096、U+30A1–30FA，以及 U+4E00–9FFF 中字体有的全部：
    // 墨迹行（位图最后一行是 Tiny TTF 多分配的空白行，不算）：top = line_height - base_line - box_h - ofs_y >= 0
    // 且 top + box_h - 1 <= line_height；某档差 1 px 时按 spec 4.3 把 0.99 调到 1.0 并记录
TEST(VectorFont, OutsideCharsetFallsBackToBody)   // font(48) 解析 '한'(U+D55C)：resolved_font != font(48)，box_h <= 16
TEST(VectorFont, CorruptImageDegrades)    // 镜像副本改一个载荷字节：start() 成功，vector_fonts_ready() == false，
                                          // font(48) 解析 '自' 的 box_h <= 16（16 px CJK）
TEST(VectorFont, NoImageDegrades)         // 空 span：同上
TEST(VectorFont, RestartRecreatesFonts)   // start/stop/start：第二次的 font(32) 能解析并取到 '自' 的位图（lv_font_get_glyph_bitmap 非空）
TEST(VectorFont, StopReleasesLvglHeap)    // start 后记 lv_heap_bytes()；建 4 档并各解析 20 个汉字；stop；再 start：lv_heap_bytes() 与第一次 start 后相差 < 1 KB
// tests/ui/ui_runtime_test.cpp
TEST(UiRuntime, SmallSizesUseBodyFont)    // 原 FontSizesWrapA8AndFallBackToCjk 改名：font(8)/font(16)/font(19) == default_font()，default_font 的 fallback 是 CJK 16
TEST(UiRuntime, DefaultUiStackIs10K)      // UiConfig{}.task_stack == 10240
```

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'VectorFont|UiRuntime' --output-on-failure`。
- [ ] **Step 3: 实现**。
- [ ] **Step 4: 验证** — 全量 ctest（场景测试里 `theme_font("xl") == ui::font(32)` 一类断言仍成立）；`bash tools/ci.sh`；固件 `idf.py build`，记录应用镜像增量（预计 Tiny TTF + stb 约 +19.5 KB）。
- [ ] **Step 5: Commit** — `feat(ui): render 20 px and larger text with Tiny TTF from the font image`

---

### Task 5: 图标位图字，移除 Montserrat 24/32/48

**Files:**
- Create: `tools/gen_icon_font.py`、`components/wisp_ui/fonts/lv_font_wisp_icons_{24,32,48}.c`
- Modify: `components/wisp_ui/CMakeLists.txt`（加三个 `.c`）、`components/wisp_ui/src/fonts.cpp`（`degraded()` 与回退链）、`components/wisp_ui/config/lv_conf.h`（`LV_FONT_MONTSERRAT_24/32/48 0`）、`components/wisp_app/src/net_cards.cpp`（`ui::font(24)`）、`tools/ci.sh`（cmake 配置之后运行 `python3 tools/gen_icon_font.py --check`）、`tests/ui/vector_font_test.cpp`、`tests/app/net_ui_test.cpp`

**Interfaces:**
- `gen_icon_font.py [--lvgl <dir>] [--check]`：LVGL 源码查找与版本校验照搬 `gen_pinyin_dict.py` 的 `find_lvgl()`/`lvgl_version()`（必须 9.5.0）。码位：解析 `src/font/lv_symbol_def.h` 里 `#define LV_SYMBOL_<名> "\xEF\x80\x81"` 形式的 UTF-8 字面量，取 U+E000–U+F8FF 内的、去掉 U+F8FF。生成：在仓库外的临时目录里对每个尺寸执行 `npx --yes lv_font_conv@1.5.3 --font <lvgl>/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff -r <码位列表> --size <N> --format lvgl --bpp 4 --no-compress -o lv_font_wisp_icons_<N>.c --lv-include lvgl.h --lv-font-name lv_font_wisp_icons_<N>`，再加文件头（来源、LVGL 版本、许可 "Font Awesome Free 5, SIL OFL 1.1; Reserved Font Name 'Font Awesome' not used"、命令）写入仓库。`--check`：只读已提交文件里 `/* U+XXXX` 注释的码位，与 `lv_symbol_def.h` 的集合比较，不一致则失败（离线）。
- `degraded(step)` 与 `fallback`：图标字 = `a8_font(&copy)`，`copy` 是 `lv_font_wisp_icons_N` 的可变副本、`fallback = default_font()`；`icon_font(step)`：20 → 无（直接 `default_font()`），24/28 → 24，32/40 → 32，≥48 → 48。矢量字 `fallback = icon_font(step) ?: default_font()`；降级字体 = `icon_font(step) ?: default_font()`。
- `net_cards.cpp`：`lv_obj_set_style_text_font(digits, ui::font(24), 0)`，删掉 "needs no CJK fallback" 的注释。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(VectorFont, IconsResolveFromIconFont)   // font(48) 解析 LV_SYMBOL_OK(U+F00C)：resolved_font 不是 font(48)、不是 default_font()，box_h >= 30；
                                             // font(32) 的 box_h 在 [20, 32]；font(20) 的 box_h <= 16
TEST(VectorFont, DegradedKeepsLargeIcons)    // 空镜像：font(48) 解析 U+F00C 的 box_h >= 30，解析 'A' 的 box_h <= 16
TEST(NetUi, PairingCodeUsesFont24)           // 配对卡片数字标签的字体 == ui::font(24)
```
  `NetUi.PairingCardCountsDownAndCloses` 等现有用例继续通过。

- [ ] **Step 2: 运行，确认失败**。
- [ ] **Step 3: 实现**：运行 `gen_icon_font.py`（需要 `npx`；在仓库外的空目录里生成），提交三个 `.c`；改 `lv_conf.h`；`grep -rn 'montserrat_\(24\|32\|48\)' components tests` 为空。
- [ ] **Step 4: 验证** — `bash tools/ci.sh`（含 `--check`）；固件 `idf.py build`，`idf.py size` 记录应用镜像变化（预计净减约 70 KB），写进本计划执行记录。
- [ ] **Step 5: Commit** — `feat(ui): large icons from an icon-only bitmap font, drop Montserrat 24/32/48`

---

### Task 6: 自检页标题

**Files:**
- Modify: `components/wisp_app/src/self_check_header.cpp`、`tests/app/self_check_screen_test.cpp`

**Interfaces:**
- 标题：`lv_obj_set_style_text_font(title, ui::font(kTitlePx), 0); text.bind(title, ui::StrId::SelfCheckTitle);`，`kTitlePx = 32`；删掉 `title_font()`。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(SelfCheckScreen, TitleUsesLargeFontInCurrentLanguage)
    // 用 test_font_image() 起 UI：标题标签字体 == ui::font(32)；
    // 依次 set_lang(Zh/Ja/En)，标题文字分别为 "自检"、"セルフチェック"、"Self Check"；
    // 解析标题首字的 resolved_font == ui::font(32)（是矢量字而非回退）
```
  `SelfCheckScreen.NoContentOutsideSafeRect` 在新行高下继续通过。

- [ ] **Step 2: 运行，确认失败**。
- [ ] **Step 3: 实现**。
- [ ] **Step 4: 验证** — 全量 ctest；模拟器截图（`./build/sim/wisp_sim --screenshot /tmp/claude-1000/<…>/scratchpad/sc.png --exit-after-ms 3000`）目视标题，截图不进仓库。
- [ ] **Step 5: Commit** — `feat(app): show the self-check title in the large vector font`

---

### Task 7: 场景 `font` 接受像素数，示例场景用大字

**Files:**
- Modify: `components/wisp_scene/src/schema.cpp`（期望值文字、文档段落）、`components/wisp_scene/src/node.cpp`（`PropType::Font` 的检查）、`components/wisp_scene/include/wisp/scene/theme.hpp`、`components/wisp_scene/src/theme.cpp`、`components/wisp_scene/src/scene_view.cpp`（`Prop::Font` 接受整数）、`components/wisp_scene/src/widget_data.hpp`、`components/wisp_scene/src/widgets.cpp`、`components/wisp_scene/src/scene_view_canvas.cpp`、`docs/scene-api.md`（由测试输出重新生成）、`scenes/examples/{counter,pomodoro,clock}/main.lua`、`tests/scene/{schema_test,scene_view_test,canvas_test}.cpp`

**Interfaces:**

```cpp
// wisp/scene/theme.hpp
inline constexpr int kFontPxMin = 16;
inline constexpr int kFontPxMax = 128;
int theme_font_px(std::string_view token);   // xs/sm/md -> 16, lg 24, xl 32, xxl 48; unknown -> 16
const lv_font_t* theme_font(std::string_view token);  // ui::font(theme_font_px(token))
const lv_font_t* theme_font(int px);                  // ui::font(px), px already validated
```
- `check_value(Prop::Font)`：字符串必须是令牌；数字必须是整数（`lua_isinteger`，或值为整数的浮点数）且在 16–128；其他类型 `WrongType`。期望值文字：`one of xs, sm, md, lg, xl, xxl, or an integer 16 to 128 (pixels)`。
- 界面改动里的 `font` 值：令牌照旧是字符串，数字是 `int64_t`；`SceneView` 两种都接受。
- 画布：`DrawCommand::font` 改为 `uint8_t font_px = 16`（存像素，令牌在 `options()` 里转像素）；`scene_view_canvas.cpp` 用 `theme_font(int)`。
- `schema_markdown()`：`font` 一行与属性表更新；新增一段 "Font sizes"：16–19 是正文位图字，20 起是矢量字，档位 20、24、28、32、40、48、64、80、96、128（取不大于给定值的一档），大号适合短文本、首次显示有冷渲染。
- 示例（spec 5.4）：计数器数字 `font = 96`，说明文字正文字；番茄钟剩余时间 `font = DIAL >= 280 and 72 or 48`；时钟在画布外包 `ui.stack`，叠一个 `font = 56` 的数字时间标签（`bind(now, ...)`），放在表盘下三分之一，星期仍是 `sm`。

- [ ] **Step 1: 写失败的测试**

```cpp
TEST(Schema, FontAcceptsTokensAndPixels)    // "xl"、16、20、64、128、64.0 均 Ok
TEST(Schema, FontRejectsOutOfRange)         // 15、129、32.5 -> BadValue；"huge" -> BadValue；true -> WrongType；
                                            // 建树报错含 main.lua 行号、"label"、"font"、期望值文字
TEST(SceneView, NumericFontUsesVectorStep)  // label font = 50 -> 字体 == ui::font(48)；font = 18 -> ui::font(16)
TEST(SceneView, FontTokenStillMaps)         // 现有 FontsAndColorsMapToTheme 保持；lg -> ui::font(24)
TEST(Canvas, TextAcceptsPixelFont)          // g:text(..., {font = 64}) 的命令 font_px == 64；{font = 200} 报 "canvas: font expects ..."
TEST(Schema, DocIsUpToDate)                 // 失败后用 build/sim/scene-api.md 覆盖 docs/scene-api.md 再跑
```
  `ExamplesTest.*`（三种屏形上控件四角可见）对改过的三个示例继续通过。

- [ ] **Step 2: 运行，确认失败** — `ctest --test-dir build/sim -R 'Schema|SceneView|Canvas|Examples' --output-on-failure`。
- [ ] **Step 3: 实现**；用 `DocIsUpToDate` 的输出更新 `docs/scene-api.md`。
- [ ] **Step 4: 验证** — 全量 ctest；`bash tools/ci.sh`；模拟器 `--scenes-dir scenes/examples --open-scene counter`（以及 `pomodoro`、`clock`）截图目视。
- [ ] **Step 5: Commit** — `feat(scene): accept pixel sizes for font and use large text in the examples`

---

### Task 8: 文档与许可

**Files:**
- Modify: `THIRD_PARTY_NOTICES.md`、`docs/references.md`、`docs/architecture.md`、`docs/porting-guide.md`、`docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`、本计划

- [ ] **Step 1: `THIRD_PARTY_NOTICES.md`**
  - "Noto Sans CJK SC" 一节标题与正文扩展到 `components/wisp_ui/fonts/noto_sans_cjk_sc_subset.ttf` 与 `fonts` 分区镜像：由 `tools/gen_vector_font.py` 子集化并转为 TrueType（修改版）；保留 name 表 ID 0/13/14；上游 `LICENSE`（标签 `Sans2.004`，SHA-256 `6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2`）没有版权行、没有保留字体名，字体名保持 "Noto Sans CJK SC"。
  - 新增 "Font Awesome Free 5 (via `lv_font_wisp_icons_*.c`)"：来源 LVGL 9.5.0 的 `scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff`，许可证文字从 `scripts/built_in_font/font_license/FontAwesome5/LICENSE.txt` 原样复制；写明生成的字体不用保留名 "Font Awesome"。
  - LVGL 一节的 bundled 列表加 stb_truetype（`src/libs/tiny_ttf/stb_truetype_htcw.h`，MIT / Public Domain，许可证文字取自该文件末尾）。
  - 新增 "fontTools"（4.55.3，MIT，`tools/gen_vector_font.py` 的转换改编自 `Snippets/otf2ttf.py`；不进固件），许可证文字取自 fontTools 4.55.3 发行包的 `LICENSE`。
- [ ] **Step 2: `docs/references.md`**：新增三行——子集 TTF（来源、字符集、pyftsubset 参数、cu2qu 误差、post v3、命令、`gen_vector_font_test.py`）、图标位图字（来源、码位来源、lv_font_conv 命令、`--check`）、fontTools（改编片段）。
- [ ] **Step 3: `docs/architecture.md`**：启动流程第 4 步写明 `Platform::font_image` → `UiConfig::font_image` → `UiRuntime::start()` 校验；"相对 spec 的细化"第 7 条（字体）改写：正文 16 px 位图、20 px 起矢量档位、回退链、降级规则、行高；停止顺序写明 `UiRuntime::stop()` 先销毁矢量字体。
- [ ] **Step 4: `docs/porting-guide.md`**：分区约定表加 `fonts`（可选：矢量大字；没有时大字退到 16 px；没有 PSRAM 的板子不要加）；"改分区表的规则"补一句：缩小 `storage` 会让 `/int` 重新格式化，必须事先告知用户；固件与分区一节加 `idf.py flash` 写字体、`fonts-flash`、`app-flash`；提醒映射 2.8 MB 要占数据总线地址空间。
- [ ] **Step 5: 硬件文档**：分区表换成 spec 3.4 的表；改写原来"`nvs` 和 `storage` 保持了偏移和大小"那段（`storage` 缩小、换表时 `/int` 被清空、`nvs` 保留）；"内部 RAM"一节先写入 spike 的实测（`wisp_ui` 栈余量 1208 → 808 B，映射 80 B），Task 9 再补正式数据。
- [ ] **Step 6: 验证** — `bash tools/ci.sh`（注释与文档守卫覆盖 `docs/`）。
- [ ] **Step 7: Commit** — `docs: register the vector font and icon font, document the fonts partition`

---

### Task 9: 真机验收与实测（控制者烧录，用户执行检查）

**Files:**
- Modify: `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md`（"真机验收清单"表追加行，"内部 RAM"、"性能基准"两节补数据）、本计划

- [ ] **Step 1: 控制者烧录**：`bash -c '. ~/esp/esp-idf-v6.0.3/export.sh >/dev/null && cd firmware && idf.py build && idf.py -p /dev/ttyACM0 flash'`（写入新分区表、应用和 `fonts.bin`；设备当前跑的是 spike 固件，`storage` 已被擦过一次）。
- [ ] **Step 2: 在验收表末尾追加下列行**（编号接现有最后一行，"结果"栏留空，来源写 "vector-font spec 7.3.n"），交给用户逐项执行并填写：

| 项目 | 怎么测 | 预期结果 |
|---|---|---|
| 完整烧录与设置保留（7.3.1） | 看启动日志与 `WISP_BOOT_REPORT`；检查 Wi-Fi 是否自动连上、语言与亮度 | 有 `fonts: image ok, … bytes`；`storage.int` 为 ok（已重新格式化）；设置都在；`boot_ms` < 3000，记下比改动前（1826–1828 ms）多多少 |
| 自检标题（7.3.2） | 在自检页把语言依次切到中文、日文、英文；打开配网卡片看配对码 | 标题分别是 32 px 的"自检""セルフチェック""Self Check"，清晰、不缺字、上下不被裁；配对码数字正常 |
| 场景大字（7.3.3） | 重新安装示例场景（`storage` 已清空），打开计数器、番茄钟、时钟；再推送一个测试场景：一行 `font = 128` 的"小鬼火"、一行 `font = "xxl"` 的中日英混排、`ui.icon` 的 `lg`/`xl`/`xxl` 各一个 | 大字清晰、字距正常、不被裁；图标是大号的，不是 16 px |
| 降级（7.3.4） | `parttool.py -p /dev/ttyACM0 erase_partition --partition-name=fonts` 后重启；再 `idf.py -p /dev/ttyACM0 fonts-flash` 后重启；再把 `firmware/build/fonts.bin` 的前 1 MB 写进分区后重启 | 擦除与截断两种情况：不崩溃，日志有 `fonts: …; large text uses bitmap fonts`，大字退到 16 px、图标仍大；`fonts-flash` 后恢复 |
| OTA 不碰字体（7.3.5） | 改一点代码重新构建，在网页"系统"页上传 `wisp.bin` 并在设备上确认 | 重启后 `fonts: image ok`，标题仍是矢量字 |
| 帧率（7.3.6） | 按"UI 帧率基准"一行滚动自检列表，不开热点和开热点各一次；首次打开 128 px 测试场景时看 `WISP_PERF` 或日志里的帧耗时 | fps 不开热点 ≥ 22.7、开热点 ≥ 28.2；记下首次大字页面的冷渲染耗时 |
| 内存（7.3.7） | 分别在 (a) 开机空闲、(b) 番茄钟运行 + 128 px 测试场景打开后执行 `mem` | 记录内部 RAM 空闲 / `min` / 最大块，`wisp_ui` 的栈大小与 high-water mark（剩余应 ≥ 1024 B），PSRAM 空闲；内部 RAM 满足 `wisp/app/ram_budget.hpp`（空闲 ≥ 60 KB、最大块 ≥ 20 KB） |
| 镜像大小（7.3.8） | `idf.py size` | 记下应用镜像大小与占 `ota_0` 的比例，对比改动前 3,169,136 B |

- [ ] **Step 3: 记录结果**：用户给出的数值写进硬件文档的"内部 RAM"（`wisp_ui` 10 KB 栈的 high-water mark、两种状态的内部 RAM）与"性能基准"（帧率、冷渲染、`boot_ms` 增量、CRC 校验耗时）。`wisp_ui` 剩余 < 1024 B 时回到 Task 4 加栈；帧率低于基准时停下来报告，不自行改回。
- [ ] **Step 4: 验证** — `bash tools/ci.sh`。
- [ ] **Step 5: Commit** — `docs(hardware): record the vector font acceptance checks and measurements`
