#pragma once

// What the shell's sub-pages (history, scene list, settings) share: the
// screen with its title, the scrolling list under it, the look of their
// rows, and the event plumbing the shell's swipe tracking relies on.
//
// Shape (AGENTS.md, screen shape; spec 5.6 of the base spec). The title
// sits below the status capsule, as wide as the safe area. The list runs
// from below the title to the bottom of the screen, full width, and its
// padding keeps the content inside the safe area's columns and ends it at
// the safe area's bottom row: scrolled to either end, the first and the
// last row lie wholly in the safe area; in between, rows may pass through
// the rows the screen does not show.
//
// Gestures. The shell tracks swipes on the page's screen, which hears its
// children's presses only if every object between them bubbles its
// events: bubble_tree() sets LV_OBJ_FLAG_EVENT_BUBBLE on a subtree, and a
// page calls it on whatever it adds. A handler on a container therefore
// also hears its children's events and must check the target. An object
// with kNoSwipe (a slider) starts no swipe: dragging it sideways is its
// own gesture.
//
// UI thread only, like every lv_* call.

#include <cstdint>

#include "lvgl.h"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::shell::detail {

// A press that starts on an object with this flag is not a swipe.
inline constexpr lv_obj_flag_t kNoSwipe = LV_OBJ_FLAG_USER_1;
// The least height of anything a finger taps.
inline constexpr int32_t kMinTouchPx = 48;
// A sub-page's title.
inline constexpr int kTitleFontPx = 28;

struct PageFrame {
    lv_obj_t* screen = nullptr;
    lv_obj_t* title = nullptr;
    // Scrolls vertically; a flex column of the page's rows.
    lv_obj_t* list = nullptr;
};

// A new screen (not loaded) with the title `title` and the list.
PageFrame make_page(const hal::ScreenShape& shape, ui::StrId title);

// Sets LV_OBJ_FLAG_EVENT_BUBBLE on `obj` and everything below it.
void bubble_tree(lv_obj_t* obj);

// Whether the panel shows colour (the theme's surfaces) or only the
// wisp's two tones.
bool colour_panel(lv_obj_t* obj);
// The text colour of plain and of secondary text on `obj`'s display.
lv_color_t text_colour(lv_obj_t* obj);
lv_color_t dim_colour(lv_obj_t* obj);

// A plain container: no style, no scrolling, content-sized height,
// `width` wide (LV_PCT(100) for a row of the list).
lv_obj_t* make_box(lv_obj_t* parent, int32_t width = LV_PCT(100));
// A card: a box with the theme's surface (or a hairline frame on panels
// without colour), rounded, padded.
lv_obj_t* make_card(lv_obj_t* parent);
// A label in the body font that wraps at its parent's width.
lv_obj_t* make_text(lv_obj_t* parent, const char* text, bool dim = false);

}  // namespace wisp::shell::detail
