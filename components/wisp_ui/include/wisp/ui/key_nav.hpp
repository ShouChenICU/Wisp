#pragma once

// Keypad navigation (device-model spec 9.1): the keys a board's nav buttons
// map to, and what of a screen those keys can reach. UiRuntime::push_key()
// feeds the keys to LVGL's keypad input device; SystemLayer fills its
// focus group with the focusable objects of what is in front.
//
// UI thread only, like every lv_* call.

#include <cstdint>
#include <vector>

#include "lvgl.h"
#include "wisp/hal/display_geometry.hpp"

namespace wisp::ui {

// Prev and Next move the focus, Ok activates the focused object, Back goes
// back. Back has no LVGL key: only the nav-key hook acts on it.
enum class NavKey : uint8_t { Prev, Next, Ok, Back };

// Marks an object the keys reach although its class is no LVGL group
// default: a clickable row built from a plain lv_obj. The shell's
// detail::kNoSwipe is LV_OBJ_FLAG_USER_1.
inline constexpr lv_obj_flag_t kKeyFocusable = LV_OBJ_FLAG_USER_2;

// Marks the box a key focus inside it brings into view whole: a list row
// whose name and value sit beside, or above, the focused control.
inline constexpr lv_obj_flag_t kFocusRow = LV_OBJ_FLAG_USER_3;

// Whether the keys reach `obj`: it carries kKeyFocusable, or it is
// clickable and of a class LVGL puts in a group by default (a button,
// switch, slider, textarea ...). A message box and its backdrop are such
// classes but only hold the buttons that are reached.
bool key_focusable(lv_obj_t* obj);

// Appends the key-focusable objects under `root` (not `root` itself) to
// `out`, in tree order. A hidden object and all it holds are skipped, and
// so is what a widget focusable by its class holds (a button's label); an
// object focusable by kKeyFocusable is followed by the focusable objects
// it holds (a row's own button).
void collect_focusable(lv_obj_t* root, std::vector<lv_obj_t*>& out);

// Makes `obj` reachable by the keys (kKeyFocusable) and gives it the
// theme's row focus: a raised fill while a key focuses it, or an outline
// on a two-tone panel, which cannot show the fill.
void make_key_focusable(lv_obj_t* obj);

// What a focus move shows, after LVGL's own scroll_to_view (which knows
// neither the screen's shape nor the row around a control): the nearest
// vertically scrollable ancestor of `obj` is scrolled so that `obj`'s
// focus row (its nearest kFocusRow ancestor, or itself) lies wholly on
// the visible part of `shape` (hal::row_span()) and inside that
// ancestor, by the least distance. The first object of `group` takes the
// list to its top and the last to its bottom when the row stays visible
// there, so the wrap shows a list's heading and end padding. Nothing
// changes when no ancestor scrolls (a dialog) or the row cannot fit.
void show_focus(lv_group_t* group, lv_obj_t* obj, const hal::ScreenShape& shape);

}  // namespace wisp::ui
