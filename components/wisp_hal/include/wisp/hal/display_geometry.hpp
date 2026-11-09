#pragma once

// Portable display-shape/safe-area geometry (spec 5.6): a screen is not
// necessarily a full rectangle (rounded-corner or circular panels exist),
// so every layer that places UI content -- the self-check screen, the
// scene runtime, DeviceRegistry::manifest()'s "safe_area" -- goes through
// these pure functions instead of hardcoding margins or assuming the
// whole screen is visible. See AGENTS.md's "Screen shape" rule.
//
// This header (like display_geometry.cpp) must stay free of LVGL and
// platform headers -- it depends only on wisp_base -- and must never log
// (wisp_hal has no wisp_core dependency); callers that need a WLOG_W on
// the fallback path below do that themselves (see
// DeviceRegistry::manifest()/the self-check screen).

#include <cstdint>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/hal/display.hpp"

namespace wisp::hal {

enum class ScreenShapeKind { Rect, RoundedRect, Circle };

// A screen's shape, parsed from Display caps (see screen_shape_from_caps()
// below). `corner_radius` is only meaningful for RoundedRect (always 0
// for Rect/Circle), and is clamped to at most min(width, height) / 2 by
// screen_shape_from_caps().
struct ScreenShape {
    ScreenShapeKind kind = ScreenShapeKind::Rect;
    int32_t width = 0;
    int32_t height = 0;
    int32_t corner_radius = 0;
};

// `percent` of the screen's shorter side, rounded to the nearest pixel.
// Touch distances (edge zones, swipe and drag thresholds) use it, so a
// gesture feels the same on screens of other resolutions.
constexpr int32_t share_of_screen(const ScreenShape& s, int percent) {
    const int64_t side = s.width < s.height ? s.width : s.height;
    return static_cast<int32_t>((side * percent + 50) / 100);
}

// Parses a Display's caps ("width", "height", "shape", "corner_radius" --
// see display.hpp) into a ScreenShape. Fails with ErrorCode::InvalidArgument
// if:
//   - "width" or "height" is missing or <= 0;
//   - "shape" is present but isn't one of "rect"/"rounded_rect"/"circle"
//     (the error message includes the offending value);
//   - "shape" is "rounded_rect" but "corner_radius" is missing or < 0.
// "shape" missing entirely defaults to Rect. A "rounded_rect" whose
// corner_radius exceeds min(width, height) / 2 is clamped to that value
// rather than rejected.
Result<ScreenShape> screen_shape_from_caps(const Properties& caps);

// Whether the pixel at (x, y) -- taken at its center, (x + 0.5, y + 0.5) --
// is part of the visible screen area. Always false outside
// [0, width) x [0, height). For RoundedRect, a pixel inside one of the
// four corner_radius x corner_radius corner squares is visible only if
// its center is within corner_radius of that corner's circle center; for
// Circle, a pixel is visible only if its center is within
// min(width, height) / 2 of the screen's center.
bool visible(const ScreenShape& shape, int32_t x, int32_t y);

// Returns the largest axis-aligned rectangle, centered on the screen, that
// is entirely visible() -- i.e. the area UI content can safely occupy
// without being clipped by the screen's physical shape. RoundedRect and
// Circle each carry a fixed kSafeCornerMargin (4 px) beyond the exact
// visible boundary, so a corner glyph placed at the safe rect's edge
// isn't flush against the real screen's shape (real panels' corners are
// measured by eye -- see docs/porting-guide.md -- and the exact radius
// is rarely known to the pixel):
//   - Rect: the whole screen, no margin.
//   - RoundedRect: each edge inset by
//     ceil(corner_radius - (corner_radius - kSafeCornerMargin) / sqrt(2)).
//   - Circle: inset horizontally by
//     ceil(width/2 - (R - kSafeCornerMargin) / sqrt(2)) and vertically by
//     ceil(height/2 - (R - kSafeCornerMargin) / sqrt(2)), where
//     R = min(width, height) / 2.
Area safe_rect(const ScreenShape& shape);

// A row of visible pixel columns, [x1, x2] inclusive (same convention as
// Area). empty() is true for a fully invisible/out-of-range row (and is
// the default-constructed value, so a Span can be returned by value for
// that case without the caller needing to special-case it before calling
// empty()).
struct Span {
    int32_t x1 = 0;
    int32_t x2 = -1;
    bool empty() const { return x1 > x2; }
};

// Returns the leftmost/rightmost visible() pixel columns of row `y` (used
// to lay out content row-by-row on a Circle/RoundedRect screen, inset
// further than safe_rect() would allow near the top/bottom of the safe
// area). Returns an empty Span if `y` is out of [0, height) or the entire
// row is invisible.
Span row_span(const ScreenShape& shape, int32_t y);

// What the panel can show, from caps "panel", and so how to draw on it:
// colour panels anti-alias and animate; a mono panel has two colours and
// a slow refresh; e-paper keeps its picture without power and redraws
// slowly, so it shows still pictures. A missing or unknown value is Lcd.
enum class PanelKind { Lcd, Amoled, Mono, EPaper };
PanelKind panel_kind(const Properties& caps);
// Lcd and Amoled.
inline bool panel_is_colour(PanelKind k) { return k == PanelKind::Lcd || k == PanelKind::Amoled; }

}  // namespace wisp::hal
