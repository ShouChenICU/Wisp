#pragma once

// The fonts partition image (vector-font spec 3.2): a 32-byte header
// ("WFNT", format version, header length, TTF length, CRC-32) followed by
// a TrueType font. open_font_image() checks it before anything hands the
// TTF to LVGL's Tiny TTF: stb_truetype does no bounds checking, so a
// damaged image could make it read past the data.
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
};

// Checks a fonts partition image (spec 3.2, 3.6) and returns its TrueType
// payload. Errors: NotFound (empty, or no "WFNT" magic), Unsupported (other
// format version), InvalidArgument (header, length, CRC or sfnt structure).
Result<FontImage> open_font_image(std::span<const uint8_t> raw);

}  // namespace wisp::ui
