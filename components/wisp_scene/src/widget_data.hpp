#pragma once

// The table-valued widget data the scene task sends the UI thread as
// bytes (scene-runtime-core spec 4.3). Both ends are the same build, so
// values are copied as they are laid out in memory; the reader still
// checks every size and offset.
//
// Canvas: what draw(g) produced, as CanvasDraw::commands: a uint32_t
// count, `count` DrawCommands, then the bytes of every text.
//
// Chart: the series, as the string value of SetProp{chart, series}: a
// uint8_t series count, then per series a uint16_t point count and that
// many doubles.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

#include "wisp/os/large.hpp"
#include "wisp/ui/theme.hpp"

namespace wisp::scene {

enum class DrawOp : uint8_t { Line, Circle, Arc, Rect, Fill, Text };

inline constexpr size_t kMaxDrawCommands = 512;
// Coordinates, sizes and angles are held to this before they reach LVGL.
inline constexpr int32_t kMaxDrawCoord = 100000;

struct DrawCommand {
    DrawOp op = DrawOp::Fill;
    uint8_t fill = 0;      // rect, circle: filled instead of outlined
    uint8_t font_px = 16;  // text: kFontPxMin..kFontPxMax, a token already in pixels
    uint8_t reserved = 0;
    uint32_t rgb = ui::theme::kText;  // the `text` colour unless the scene names one
    int32_t width = 1;
    int32_t radius = 0;
    // line: x1 y1 x2 y2; circle: cx cy r; arc: cx cy r start end
    // (degrees, clockwise from 3 o'clock); rect: x y w h; text: x y.
    int32_t v[5] = {};
    uint32_t text_offset = 0;  // into the text bytes
    uint32_t text_size = 0;
};

inline constexpr size_t kDrawHeaderBytes = sizeof(uint32_t);

// Commands in `bytes`, or 0 if the bytes are not a whole command list.
inline size_t draw_command_count(std::span<const uint8_t> bytes) {
    if (bytes.size() < kDrawHeaderBytes) return 0;
    uint32_t count = 0;
    std::memcpy(&count, bytes.data(), sizeof(count));
    if (count > kMaxDrawCommands || bytes.size() < kDrawHeaderBytes + count * sizeof(DrawCommand))
        return 0;
    return count;
}

// Command `i` (below draw_command_count()); false if its text lies
// outside the text bytes.
inline bool read_draw_command(std::span<const uint8_t> bytes, size_t i, DrawCommand& out) {
    std::memcpy(&out, bytes.data() + kDrawHeaderBytes + i * sizeof(DrawCommand), sizeof(out));
    size_t texts = kDrawHeaderBytes + draw_command_count(bytes) * sizeof(DrawCommand);
    size_t text_bytes = bytes.size() - texts;
    return out.text_offset <= text_bytes && out.text_size <= text_bytes - out.text_offset;
}

inline std::string_view draw_text(std::span<const uint8_t> bytes, const DrawCommand& c) {
    size_t texts = kDrawHeaderBytes + draw_command_count(bytes) * sizeof(DrawCommand);
    return {reinterpret_cast<const char*>(bytes.data() + texts + c.text_offset), c.text_size};
}

inline constexpr size_t kMaxSeries = 4;
inline constexpr size_t kMaxSeriesPoints = 128;

using SeriesData = os::LargeVector<os::LargeVector<double>>;

// Encoded series, or false (out unchanged) if `bytes` is not one.
inline bool decode_series(std::string_view bytes, SeriesData& out) {
    if (bytes.empty()) return false;
    const auto count = static_cast<uint8_t>(bytes[0]);
    if (count > kMaxSeries) return false;
    size_t at = 1;
    SeriesData series(count);
    for (auto& s : series) {
        uint16_t n = 0;
        if (bytes.size() - at < sizeof(n)) return false;
        std::memcpy(&n, bytes.data() + at, sizeof(n));
        at += sizeof(n);
        if (n > kMaxSeriesPoints || bytes.size() - at < n * sizeof(double)) return false;
        s.resize(n);
        std::memcpy(s.data(), bytes.data() + at, n * sizeof(double));
        at += n * sizeof(double);
    }
    if (at != bytes.size()) return false;
    out = std::move(series);
    return true;
}

}  // namespace wisp::scene
