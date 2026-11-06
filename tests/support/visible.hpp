#pragma once

// Test-only: whether an LVGL area lies wholly on the visible part of a
// screen, for the screen-shape checks of the system's own dialogs and
// screens (AGENTS.md, screen shape).

#include "lvgl.h"
#include "wisp/hal/display_geometry.hpp"

namespace wisp::test {

// Whether all four corners of `a` are hal::visible() on `s`.
inline bool box_visible(const hal::ScreenShape& s, const lv_area_t& a) {
    return hal::visible(s, a.x1, a.y1) && hal::visible(s, a.x2, a.y1) &&
           hal::visible(s, a.x1, a.y2) && hal::visible(s, a.x2, a.y2);
}

}  // namespace wisp::test
