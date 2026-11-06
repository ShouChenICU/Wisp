#pragma once

// The glyphs partition image (glyph-partition spec 3.1, 4.1): the 16 px
// body font, packed by tools/pack_glyph_image.py from the generated
// lv_font_cjk_16.c, read back into an LVGL lv_font_fmt_txt font.
//
// Format (all integers little-endian, no padding between fields or
// sections; "payload offset" means bytes from the first byte after the
// header):
//
//   header, 32 bytes: magic "WGLY", u16 version (kGlyphImageVersion),
//     u16 header length (32), u32 payload length, u32 CRC-32 of the
//     payload (wisp::crc32), 16 reserved zero bytes.
//   metrics, 32 bytes: u16 line_height, i16 base_line,
//     i8 underline_position, i8 underline_thickness, u8 bpp (4),
//     u8 reserved, u16 kern_scale, u16 cmap_count, u32 glyph_count
//     (including the reserved id 0), u32 list_bytes, u32 kern_pair_count,
//     u32 bitmap_bytes, u32 reserved.
//   glyphs, glyph_count x 10 bytes: u32 bitmap_index (into the bitmap
//     section), u16 adv_w, u8 box_w, u8 box_h, i8 ofs_x, i8 ofs_y.
//   cmaps, cmap_count x 20 bytes: u32 range_start, u16 range_length,
//     u16 glyph_id_start, u8 type (lv_font_fmt_txt_cmap_type_t), u8
//     reserved, u16 list_length, u32 unicode_list payload offset, u32
//     glyph_id_ofs_list payload offset (0xFFFFFFFF when absent).
//   lists, list_bytes: the cmaps' unicode_list (u16 each, SPARSE_TINY)
//     and glyph_id_ofs_list (u8 each, FORMAT0_FULL) arrays.
//   kern, kern_pair_count x (u16 left, u16 right) glyph ids, then
//     kern_pair_count x i8 values.
//   bitmap, bitmap_bytes: identical to lv_font_conv's glyph_bitmap[].
//
// Only FORMAT0_FULL, FORMAT0_TINY and SPARSE_TINY cmaps exist in the
// format; anything else is rejected.
//
// Portable: no platform headers. The bytes come from the platform (a
// mapped partition on the device, a file read into memory in the
// simulator).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "lvgl.h"
#include "wisp/base/result.hpp"
#include "wisp/os/large.hpp"

namespace wisp::ui {

inline constexpr uint32_t kGlyphImageVersion = 1;
inline constexpr size_t kGlyphImageHeaderBytes = 32;

// An LVGL font built from a glyph image. `font.dsc` and every table it
// points at (glyph descriptors, cmaps, their lists, kern pairs) live in
// `tables`, in Mem::Large memory; only `glyph_bitmap` points into the
// caller's bytes, which must outlive this object. Neither copyable nor
// movable: LVGL and the font fallback chain hold `&font`.
struct GlyphFont {
    lv_font_t font{};
    uint32_t crc32 = 0;
    os::LargeBuffer tables;

    GlyphFont() = default;
    GlyphFont(const GlyphFont&) = delete;
    GlyphFont& operator=(const GlyphFont&) = delete;
};

// Checks a glyphs partition image and builds its font. Every offset,
// length and glyph id the renderer will follow is validated before
// anything is allocated. Errors: NotFound (empty, or no "WGLY" magic --
// an erased partition), Unsupported (other format version),
// InvalidArgument (header or payload length, CRC, bpp other than 4,
// cmap type, or any out-of-range offset, length or glyph id), NoMemory.
Result<std::unique_ptr<GlyphFont>> open_glyph_image(std::span<const uint8_t> raw);

}  // namespace wisp::ui
