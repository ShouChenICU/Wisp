#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "wisp/base/result.hpp"

namespace wisp {

// Baseline JPEG of tightly packed RGB888 (R, G, B per pixel, no row padding),
// through the vendored stb_image_write (third_party/stb_image_write, spec 3.6):
// 4:2:0 at quality <= 90. Writes into `out` and returns the JPEG's size.
// InvalidArgument: width or height 0 or over 65535, quality outside 1-100, or
// rgb.size() != width * height * 3. QuotaExceeded: the JPEG does not fit `out`
// (the tools' too_large). No heap allocation; about 5 KB of stack.
Result<size_t> encode_jpeg(std::span<const uint8_t> rgb, uint32_t width, uint32_t height,
                           int quality, std::span<uint8_t> out);

struct ImageInfo {
    const char* mime = "";  // "image/jpeg" | "image/png" (static strings)
    uint32_t width = 0;     // 0 when the header does not say
    uint32_t height = 0;
};

// By the bytes alone: JPEG (FF D8 FF; size from the first SOF0-SOF15 marker,
// except DHT C4, JPG C8 and DAC CC) or PNG (the 8-byte signature; size from IHDR).
// nullopt for anything else.
std::optional<ImageInfo> sniff_image(std::span<const uint8_t> bytes);

}  // namespace wisp
