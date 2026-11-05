#pragma once

// Helpers shared by the system layer's source files (system_layer.cpp,
// status_capsule.cpp, notification_panel.cpp, banner.cpp).

#include <cstdint>
#include <string>
#include <string_view>

#include "lvgl.h"
#include "wisp/hal/display_geometry.hpp"

namespace wisp::ui::detail {

// The visible columns common to screen rows y1..y2, `inset` further in
// from each side; empty if some row shows nothing.
hal::Span visible_extent(const hal::ScreenShape& shape, int32_t y1, int32_t y2, int32_t inset);

// A round badge `size` pixels across standing for a scene: `text` (one
// character) on a colour picked from the theme by `key`'s hash, or
// LV_SYMBOL_BELL on a neutral fill when `text` is empty (a system
// notice). It takes no touches.
lv_obj_t* make_badge(lv_obj_t* parent, std::string_view text, std::string_view key, int32_t size);

// The first UTF-8 character of `s` ("" for an empty string).
std::string first_char(std::string_view s);

// How a press that went from `from` to `to` without scrolling anything
// ended: a tap (it barely moved), a swipe up or to the left (it moved at
// least `swipe` pixels, mostly that way), or something else.
enum class PressEnd : uint8_t { Tap, SwipeUp, SwipeLeft, Other };
PressEnd classify_press(lv_point_t from, lv_point_t to, int32_t swipe);

}  // namespace wisp::ui::detail
