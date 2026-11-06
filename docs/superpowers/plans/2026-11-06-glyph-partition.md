# 正文位图字分区 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the 16 px body font `lv_font_cjk_16` out of the firmware image into a mapped `glyphs` partition, pixel-identical, with a degrade path when the partition is missing.

**Architecture:** A build-time packer turns the committed `lv_font_cjk_16.c` into `glyphs.bin` (32-byte header + portable little-endian payload). At UI start `open_glyph_image()` checks it and builds an LVGL `lv_font_fmt_txt_dsc_t` whose index tables live in PSRAM and whose `glyph_bitmap` points into the mapping; `default_font()` falls back to it through the existing A8 cache. The firmware stops linking the `.c`; host builds keep it as a built-in fallback and as the equivalence reference.

**Tech Stack:** C++20 (no exceptions/RTTI), LVGL 9.5 `lv_font_fmt_txt`, ESP-IDF v6.0.3 partitions/mmap, Python 3 stdlib, GoogleTest.

**Spec:** `docs/superpowers/specs/2026-11-06-glyph-partition-design.md`

## Global Constraints

- Pixels, metrics and kerning of the 16 px body font must be byte-identical to today's `lv_font_cjk_16` (spec 1.3 item 1).
- Partition table (spec 2), exactly: `ota_0` 0x20000/0x480000, `ota_1` 0x4A0000/0x480000, `glyphs` 0x920000/0x120000 data subtype 0x41, `storage` 0xA40000/0x2F0000, `fonts` 0xD30000/0x2D0000; `nvs`/`otadata`/`phy_init` unchanged.
- A missing or invalid glyph image never stops boot: warn, then fall back (spec 4.3).
- Only bitmap bytes are read from the mapping at draw time; every index/length/offset the renderer follows is copied into `os::alloc(Mem::Large)` memory after validation (spec 4.1).
- Portable layer: no platform headers, no chip/board names (AGENTS.md 3). Mapping lives in `wisp_port_esp`.
- AGENTS.md 5 comment rules: no task/review ids, no `.superpowers/` references. Commit by explicit paths only; trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Deletions only on literal absolute paths inside the scratchpad or this worktree; never `git clean -fdx`.

### Image format (shared by Task 1 and Task 2; document it in both `tools/pack_glyph_image.py`'s docstring and `glyph_image.hpp`)

All integers little-endian, no padding between fields or sections. Offsets are bytes from the start of the payload.

Header (32 bytes, `struct.Struct("<4sHHII16x")`, same layout as `fonts.bin`): magic `b"WGLY"`, u16 version `1`, u16 header length `32`, u32 payload length, u32 CRC-32 of the payload (`zlib.crc32` = `wisp::crc32`), 16 reserved zero bytes.

Payload, in order:

| Section | Layout |
|---|---|
| metrics (32 B) | u16 line_height, i16 base_line, i8 underline_position, i8 underline_thickness, u8 bpp (=4), u8 reserved 0, u16 kern_scale, u16 cmap_count, u32 glyph_count (including reserved id 0), u32 list_bytes, u32 kern_pair_count, u32 bitmap_bytes, u32 reserved 0 |
| glyphs | glyph_count × 10 B: u32 bitmap_index, u16 adv_w, u8 box_w, u8 box_h, i8 ofs_x, i8 ofs_y |
| cmaps | cmap_count × 20 B: u32 range_start, u16 range_length, u16 glyph_id_start, u8 type (LVGL `lv_font_fmt_txt_cmap_type_t` value), u8 reserved 0, u16 list_length, u32 unicode_list_offset, u32 glyph_id_ofs_offset (each 0xFFFFFFFF when absent) |
| lists | list_bytes: the cmaps' `unicode_list` (u16 each) and `glyph_id_ofs_list` (u8 each for FORMAT0_FULL, u16 for SPARSE_FULL) arrays, concatenated in cmap order |
| kern | kern_pair_count × 4 B glyph-id pairs (u16 left, u16 right), then kern_pair_count × i8 values |
| bitmap | bitmap_bytes, identical to `glyph_bitmap[]` |

The source font uses `kern_pairs` with `glyph_ids_size = 1` (u16 ids) and no kern classes; the packer rejects any other kern form.

## Review Focus

1. A `glyphs` partition that was never flashed (all 0xFF) or was cut short → `NotFound`/`InvalidArgument`, UI starts with the vector 16 px fallback (Task 2 test `RejectsErasedFlash`, Task 3 test `DegradesToVector16`).
2. An image from a future packer (version 2) → `Unsupported`, same degrade (Task 2 test `RejectsOtherVersion`).
3. UiRuntime started and stopped repeatedly with the same image → the same font object is reused, no growth in PSRAM, glyphs still correct (Task 3 test `RestartReusesGlyphFont`).
4. A character the body font lacks (Korean 한) with the image installed → glyph lookup returns false through the whole chain, no crash (Task 3 test `MissingCharacterFallsThrough`).
5. A crafted image whose glyph points past the bitmap or whose cmap list runs past `list_bytes` → rejected before any lookup (Task 2 tests `RejectsGlyphPastBitmap`, `RejectsCmapListPastLists`).

---

### Task 1: Glyph image packer

**Files:**
- Create: `tools/pack_glyph_image.py`, `tools/pack_glyph_image_test.py`
- Modify: `tools/ci.sh` (run the test next to `pack_font_image_test.py`), `simulator/CMakeLists.txt` (pack `glyphs.bin` next to `wisp_sim`, mirroring the `fonts.bin` block at lines 106-120)

**Interfaces:**
- Produces: `pack_glyph_image.py <lv_font_c> -o <out> [--max-bytes N]`; `build/sim/glyphs.bin` (target `wisp_sim_glyphs`, a dependency of `wisp_sim`); importable `pack(c_source_text: str) -> bytes` and `PackError` for tests.

- [ ] **Step 1: Write the failing tests** in `tools/pack_glyph_image_test.py` (unittest, stdlib only, packs the real `components/wisp_ui/fonts/lv_font_cjk_16.c` once in `setUpClass`):
  - `test_header`: magic `b"WGLY"`, version 1, header length 32, payload length = file length − 32, CRC = `zlib.crc32(payload)`.
  - `test_metrics`: line_height 31, base_line 9, underline_position −2, underline_thickness 1, bpp 4, kern_scale 16, cmap_count 69, glyph_count 8710, kern_pair_count 4516, bitmap_bytes 996827.
  - `test_glyph_and_bitmap_match_source`: for U+4F60 (你) and U+0041 (A), the decoded glyph record equals the `glyph_dsc` entry in the `.c`, and the bitmap slice `[bitmap_index, bitmap_index + ceil(box_w*box_h*4/8))` equals the bytes under that glyph's `/* U+XXXX */` comment.
  - `test_sections_in_bounds`: every glyph's bitmap slice ends ≤ bitmap_bytes; every cmap's list offsets + lengths lie inside the lists section.
  - `test_deterministic`: packing twice gives identical bytes.
  - `test_max_bytes`: `--max-bytes 1000` exits non-zero with a message on stderr; `--max-bytes 1179648` (the `glyphs` partition) succeeds.
  - `test_rejects_kern_classes`: a source with `.kern_classes = 1` raises `PackError`.
- [ ] **Step 2: Run** `python3 tools/pack_glyph_image_test.py` — expect failures (module missing).
- [ ] **Step 3: Implement `tools/pack_glyph_image.py`**: parse the `.c` with regular expressions over `glyph_bitmap[]`, `glyph_dsc[]`, `unicode_list_N[]`, `glyph_id_ofs_list_N[]`, `cmaps[]`, `kern_pair_glyph_ids[]`, `kern_pair_values[]`, `font_dsc` and the public `lv_font_t` fields; emit the format in Global Constraints; module docstring documents the format like `pack_font_image.py` does.
- [ ] **Step 4: Run** `python3 tools/pack_glyph_image_test.py` — expect OK. Add it to `tools/ci.sh`.
- [ ] **Step 5: Simulator build**: add the `glyphs.bin` custom command/target in `simulator/CMakeLists.txt`; `cmake --build build/sim` produces `build/sim/glyphs.bin` of about 1.12 MB.
- [ ] **Step 6: Commit** `feat(tools): pack the 16 px body font into a glyph image`.

### Task 2: Glyph image reader

**Files:**
- Create: `components/wisp_ui/include/wisp/ui/glyph_image.hpp`, `components/wisp_ui/src/glyph_image.cpp`, `tests/ui/glyph_image_test.cpp`
- Modify: `components/wisp_ui/CMakeLists.txt` (add the source), `tests/CMakeLists.txt` (`glyph_image_test`, `LIBS wisp_ui wisp_port_sim_os`, `WISP_GLYPH_IMAGE="${CMAKE_BINARY_DIR}/glyphs.bin"`, depends on `wisp_sim_glyphs`)

**Interfaces:**
- Consumes: `build/sim/glyphs.bin` (Task 1).
- Produces, in namespace `wisp::ui`:
  - `inline constexpr uint32_t kGlyphImageVersion = 1;`
  - `struct GlyphFont { lv_font_t font; uint32_t crc32; };` — `font.dsc` points at an `lv_font_fmt_txt_dsc_t` and tables owned with the object (PSRAM via `os::alloc(Mem::Large)`), `font.dsc->glyph_bitmap` points into the caller's bytes. Non-copyable; destructor frees the tables.
  - `Result<std::unique_ptr<GlyphFont>> open_glyph_image(std::span<const uint8_t> raw);` — errors: `NotFound` (empty or no magic), `Unsupported` (version ≠ 1), `InvalidArgument` (header length, payload length, CRC, bpp ≠ 4, any out-of-range offset/length/glyph id); `NoMemory` when PSRAM allocation fails.
  - The resulting font uses LVGL's own `lv_font_get_glyph_dsc_fmt_txt`/`lv_font_get_bitmap_fmt_txt`, `bitmap_format` PLAIN, `stride` 0, so `a8_font()` accepts it.

- [ ] **Step 1: Write the failing tests** in `tests/ui/glyph_image_test.cpp` (read `WISP_GLYPH_IMAGE` into a vector; corrupt copies re-CRC where the test targets a structural check):
  - `OpensBuildImage`: succeeds; `font.line_height == lv_font_cjk_16.line_height`, `base_line`, underline fields equal.
  - `MatchesBuiltinExactly`: against `extern "C" const lv_font_t lv_font_cjk_16` (host wisp_ui still links it): `cmap_num`, `bpp`, `kern_scale` equal; every `glyph_dsc[i]` field equal for all 8710 ids; every cmap's fields and list contents equal; kern `pair_cnt`, ids and values equal; bitmap bytes `memcmp` equal over 996827 bytes.
  - `RejectsEmpty` → NotFound; `RejectsErasedFlash` (1,179,648 × 0xFF) → NotFound; `RejectsOtherVersion` (version 2) → Unsupported; `RejectsBadCrc` (flip one bitmap byte) → InvalidArgument; `RejectsTruncated` (drop last byte) → InvalidArgument.
  - `RejectsGlyphPastBitmap` (glyph 1's bitmap_index = bitmap_bytes, CRC fixed) → InvalidArgument; `RejectsCmapListPastLists` (cmap 0's list offset = list_bytes) → InvalidArgument; `RejectsKernIdPastGlyphs` (first kern id = glyph_count) → InvalidArgument.
  - `TablesNotInMapping`: `glyph_dsc`, `cmaps`, lists and kern pointers all lie outside `[raw.data(), raw.data()+raw.size())`; `glyph_bitmap` lies inside.
- [ ] **Step 2: Run** `cmake --build build/sim && ctest --test-dir build/sim -R glyph_image_test --output-on-failure` — expect failures.
- [ ] **Step 3: Implement** `open_glyph_image()` in `glyph_image.cpp`: bounds-check the whole layout before allocating, decode every section into PSRAM, fill the LVGL structures; header comment states the format and why only the bitmap stays in the mapping (spec 4.1).
- [ ] **Step 4: Run** the test — expect PASS, no warnings.
- [ ] **Step 5: Commit** `feat(ui): read the glyph image into an LVGL font`.

### Task 3: Wire the glyph font into the UI, app and platforms

**Files:**
- Modify: `components/wisp_ui/include/wisp/ui/ui_runtime.hpp` (UiConfig), `components/wisp_ui/src/ui_runtime.cpp` (start/stop), `components/wisp_ui/src/fonts.cpp`, `components/wisp_ui/src/fonts_internal.hpp`, `components/wisp_ui/include/wisp/ui/fonts.hpp` (comments), `components/wisp_ui/src/a8_font.hpp/.cpp`, `components/wisp_ui/CMakeLists.txt`, `components/wisp_app/include/wisp/app/app.hpp` (Platform), `components/wisp_app/src/app.cpp`, `simulator/sim_options.{hpp,cpp}`, `simulator/main.cpp`, `components/wisp_port_esp/include/wisp/port/esp/esp_font_partition.hpp`, `components/wisp_port_esp/ui/esp_font_partition.cpp`, `firmware/main/main.cpp`, spec 3.3/4.2 text
- Test: `tests/ui/glyph_font_test.cpp` (new), `tests/ui/a8_font_test.cpp`, `tests/sim/sim_options_test.cpp`, existing UI/app/scene tests must stay green

**Interfaces:**
- Consumes: `open_glyph_image()`, `GlyphFont` (Task 2); `build/sim/glyphs.bin` (Task 1).
- Produces:
  - `UiConfig::glyph_image` (`std::span<const uint8_t>`) and `UiConfig::builtin_glyphs` (`bool`, default `true`); `app::Platform::glyph_image` and `app::Platform::builtin_glyphs` (same types/defaults), passed through in `app.cpp` next to `font_image`.
  - `void a8_set_fallback(const lv_font_t* wrapper, const lv_font_t* fallback);` in `a8_font.hpp` — `wrapper` must be a pointer returned by `a8_font()`; sets its `fallback` (wrapped by `a8_font()` when supported).
  - `void detail::install_glyph_font(std::span<const uint8_t> raw, bool builtin_allowed);` called by `UiRuntime::start()` after `install_font_image()`; `release_vector_fonts()` also drops a Tiny TTF 16 px fallback.
  - `std::span<const uint8_t> wisp::port::esp::map_glyph_partition();` — like `map_font_partition()` for partition `"glyphs"`.
  - Simulator option `--glyph-image <path|none>`; default `<exe dir>/glyphs.bin`; `none` gives an empty image and `builtin_glyphs = false`.

Ruling recorded for the spec: host builds keep compiling `lv_font_cjk_16.c` into `wisp_ui` (macro `WISP_UI_BUILTIN_CJK16`, only when `NOT ESP_PLATFORM`) as a built-in fallback, so the hundreds of host tests that start a UiRuntime without an image keep their CJK glyphs and Task 2 has its reference; the firmware never links it. Update spec 3.3 (simulator line) and 4.2/4.3 to say so.

Body fallback chain chosen by `install_glyph_font()` (spec 4.3):
1. `raw` opens → `a8_font(&glyph_font->font)`; log `fonts: glyph image ok, <n> bytes, crc <hex>`.
2. else `builtin_allowed` and built-in compiled in → `a8_font(&lv_font_cjk_16)`.
3. else the vector image is installed → a Tiny TTF font at 16 px created the same way as `create_vector()` (same line-box adjustment, `vector_glyph_dsc` filter), destroyed by `release_vector_fonts()`.
4. else none (Montserrat 16 only).
Cases 2-4 log one warning naming the reason from `open_glyph_image()`. The `GlyphFont` from case 1 is kept (never freed) and reused when the next start passes the same bytes (same data pointer, size and CRC); different bytes build a new one. Before any runtime starts, `default_font()` falls back to the built-in when compiled in.

- [ ] **Step 1: Write the failing tests** in `tests/ui/glyph_font_test.cpp` (`WISP_GLYPH_IMAGE` and `WISP_FONT_IMAGE` compile definitions):
  - `UsesGlyphImage`: start with `glyph_image` = build image; `lv_font_get_glyph_dsc(default_font(), …, 0x4F60)` succeeds, `resolved_font` is an A8 wrapper whose base is the glyph font (not `lv_font_cjk_16`); its `adv_w`/`box_*`/`ofs_*` equal those from `lv_font_cjk_16`.
  - `BuiltinWhenNoImage`: empty image, `builtin_glyphs = true` → 你 resolves to the A8 wrapper of `lv_font_cjk_16`.
  - `DegradesToVector16`: empty image, `builtin_glyphs = false`, vector image present → 你 resolves to a font whose `get_glyph_dsc` is not the fmt_txt one and whose glyph height is ≤ 16 + 2.
  - `NoCjkWithoutAnyImage`: empty image, `builtin_glyphs = false`, no vector image → 你 lookup returns false; `A` still resolves; start succeeds.
  - `RestartReusesGlyphFont`: start/stop three times with the same bytes → the resolved base font pointer is the same each time.
  - `MissingCharacterFallsThrough`: image installed, U+D55C (한) → false, no crash.
  - In `a8_font_test.cpp`: `SetFallbackRewires`: after `a8_set_fallback(w, &lv_font_cjk_16)` a CJK lookup through `w` resolves to the A8 wrapper of `lv_font_cjk_16`; `a8_set_fallback(w, nullptr)` makes it fail.
  - In `sim_options_test.cpp`: `--glyph-image none` and `--glyph-image /x/y.bin` parse into the options.
- [ ] **Step 2: Run** `cmake --build build/sim && ctest --test-dir build/sim -R "glyph_font_test|a8_font_test|sim_options_test" --output-on-failure` — expect failures.
- [ ] **Step 3: Implement** the interfaces above. `default_font()` keeps returning the same pointer forever; only its wrapper's fallback changes. Device: `main.cpp` sets `platform.glyph_image = wisp::port::esp::map_glyph_partition();` right after the `font_image` line (same thread rule as the comment there). `wisp_ui/CMakeLists.txt` adds `fonts/lv_font_cjk_16.c` and `WISP_UI_BUILTIN_CJK16` only when `NOT ESP_PLATFORM`.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -j16 --output-on-failure` — all tests pass, no new warnings; `python3 tools/check_layering.py` passes.
- [ ] **Step 5: Run** the simulator once with defaults and once with `--glyph-image none`: the log shows `glyph image ok` the first time and the warning the second; Chinese text renders in both (vector in the second).
- [ ] **Step 6: Commit** `feat(ui): draw the body font from the glyph image` (code, tests, spec amendment).

### Task 4: Partition table, firmware packaging and docs

**Files:**
- Modify: `boards/waveshare_esp32_s3_touch_amoled_2_16/partitions.csv` (+ its header comment), `firmware/main/CMakeLists.txt` (glyphs block beside the fonts block at lines 66-90), `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md` (partition table, image size, new acceptance row from spec 5), `docs/porting-guide.md` ("字体分区": optional `glyphs` partition, same auto-suspend rule with XIP), `docs/architecture.md` (boot mapping), `docs/superpowers/specs/2026-11-05-vector-font-design.md` (one-line pointers in 降级 and the 16 px description)

**Interfaces:**
- Consumes: `tools/pack_glyph_image.py` (Task 1), `map_glyph_partition()` (Task 3).
- Produces: `firmware/build/glyphs.bin`; `idf.py flash` writes it; `idf.py fonts-flash` writes `fonts.bin` and `glyphs.bin`.

- [ ] **Step 1: Partition table** exactly as Global Constraints; header comment says why the OTA slots are 4.5 MB and that moving to this table reformats `/int` (spec 2).
- [ ] **Step 2: Firmware CMake**: when the table has a `glyphs` partition, generate `build/glyphs.bin` with `--max-bytes` = its size and attach it to `flash` and `fonts-flash` (reuse `esptool_py_flash_to_partition`).
- [ ] **Step 3: Build** `bash -c '. ~/esp/esp-idf-v6.0.3/export.sh >/dev/null && cd firmware && idf.py build'` — succeeds; `idf.py size` Total image size ≈ 3.13 MB (record the exact number); `build/glyphs.bin` ≤ 1,179,648 B; `flash_args` lists `0x920000 glyphs.bin`.
- [ ] **Step 4: Docs** listed above; `bash tools/ci.sh` green.
- [ ] **Step 5: Commit** `feat(boards): add the glyph partition and round the OTA slots to 4.5 MB` (`docs/hardware` and porting guide in the same commit).

### Device acceptance (user runs; controller records results in the hardware doc)

Spec 5 items 1-6: USB flash (settings and vector font survive, `/int` reformatted, re-push one example scene), image size and self-check PSRAM total ≈ 5 MB, `bench scroll 10` with and without the hotspot against the current baseline, body text unchanged by eye, erase only `glyphs` → boots with vector 16 px and a warning, OTA while scrolling body text.
