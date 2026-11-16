#pragma once

// Decodes a baseline JPEG back to RGB888 for tests, with the TJpgDec that
// LVGL bundles (src/libs/tjpgd), as components/wisp_port_sim/ui/image_decoders.cpp
// does.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include "src/libs/tjpgd/tjpgd.h"

namespace wisp::test {

struct DecodedJpeg {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgb{};  // R, G, B per pixel, no row padding

    const uint8_t* at(uint32_t x, uint32_t y) const { return &rgb[(y * width + x) * 3]; }
};

namespace jpeg_detail {

struct Session {
    std::span<const uint8_t> data{};
    size_t at = 0;
    DecodedJpeg* out = nullptr;
};

inline size_t input(JDEC* jd, uint8_t* buf, size_t n) {
    auto* s = static_cast<Session*>(jd->device);
    n = std::min(n, s->data.size() - s->at);
    if (buf != nullptr) std::memcpy(buf, s->data.data() + s->at, n);
    s->at += n;
    return n;
}

// TJpgDec hands out RGB888 in LVGL's byte order (B, G, R).
inline int output(JDEC* jd, void* bitmap, JRECT* rect) {
    auto* s = static_cast<Session*>(jd->device);
    const auto* px = static_cast<const uint8_t*>(bitmap);
    for (uint32_t y = rect->top; y <= rect->bottom; ++y) {
        for (uint32_t x = rect->left; x <= rect->right; ++x, px += 3) {
            uint8_t* d = &s->out->rgb[(y * s->out->width + x) * 3];
            d[0] = px[2];
            d[1] = px[1];
            d[2] = px[0];
        }
    }
    return 1;
}

}  // namespace jpeg_detail

inline std::optional<DecodedJpeg> decode_jpeg(std::span<const uint8_t> jpeg) {
    std::vector<uint8_t> work(4096);
    DecodedJpeg out;
    jpeg_detail::Session s{jpeg, 0, &out};
    JDEC jd;
    if (jd_prepare(&jd, jpeg_detail::input, work.data(), work.size(), &s) != JDR_OK) {
        return std::nullopt;
    }
    out.width = jd.width;
    out.height = jd.height;
    out.rgb.assign(static_cast<size_t>(jd.width) * jd.height * 3, 0);
    if (jd_decomp(&jd, jpeg_detail::output, 0) != JDR_OK) return std::nullopt;
    return out;
}

}  // namespace wisp::test
