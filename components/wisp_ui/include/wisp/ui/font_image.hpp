#pragma once

// The fonts partition image (vector-font spec 3.2): a 32-byte header
// ("WFNT", format version, header length, TTF length, CRC-32) followed by
// a TrueType font. open_font_image() checks it before anything hands the
// TTF to LVGL's Tiny TTF: stb_truetype does no bounds checking, so a
// damaged image could make it read past the data. The CRC catches
// accidental damage; the structural checks keep stb's font, cmap, metrics
// and loca reads inside the image. Glyph outlines and GPOS/kern data are
// not checked (see font_image.cpp and spec 3.6).
//
// Portable: no LVGL, no platform headers. The bytes come from the
// platform (a mapped partition on the device, a file read into memory in
// the simulator) through UiConfig::font_image.

#include <cstddef>
#include <cstdint>
#include <span>

#include "wisp/base/result.hpp"

namespace wisp::ui {

inline constexpr uint32_t kFontImageVersion = 1;
inline constexpr size_t kFontImageHeaderBytes = 32;

struct FontImage {
    std::span<const uint8_t> ttf;  // points into the caller's bytes
    uint32_t crc32 = 0;
    // The cmap subtable stb_truetype reads (format 4 or 12), checked;
    // inside `ttf`.
    std::span<const uint8_t> cmap;
    // The subtable's format (4 or 12; 0 with no image) and its segment
    // (format 4) or group (format 12) count, copied out of `cmap` when it
    // was checked. On the device `cmap` is a flash mapping, and a read
    // that races a flash erase or write can return 0xFF instead of the
    // real bytes; font_image_has_glyph() steers by these copies so that
    // such a read cannot send it outside the subtable.
    uint32_t cmap_format = 0;
    uint32_t cmap_count = 0;
};

// Checks a fonts partition image (spec 3.2, 3.6) and returns its TrueType
// payload. Errors: NotFound (empty, or no "WFNT" magic), Unsupported (other
// format version), InvalidArgument (header, length, CRC or sfnt structure).
Result<FontImage> open_font_image(std::span<const uint8_t> raw);

// Whether `image` (from open_font_image(), or empty) maps `codepoint` to a
// glyph, the same way stb_truetype's FindGlyphIndex does. Tiny TTF logs an
// LVGL error for every character its font lacks before LVGL tries the
// fallback, so the font service asks this first (fonts.cpp).
bool font_image_has_glyph(const FontImage& image, uint32_t codepoint);

}  // namespace wisp::ui
