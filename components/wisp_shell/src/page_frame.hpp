#pragma once

// What the shell's sub-pages (history, scene list, settings) share: the
// screen with its title, the scrolling list under it, the look of their
// rows, and the event plumbing the shell's drag tracking relies on.
//
// Shape (AGENTS.md, screen shape; spec 5.6 of the base spec). The list
// covers the whole screen and the title is its first item, so the title
// scrolls with the rows rather than holding a band at the top. The list's
// padding keeps the content inside the safe area's columns, starts it
// below the status capsule and ends it at the safe area's bottom row:
// scrolled to the top, the title and the first row lie wholly in the safe
// area; scrolled to the bottom, the last row does; in between, the title
// and the rows may pass through the rows the screen does not show.
//
// A page adds its rows after the title (they start at kFirstRow), keeps
// it when it rebuilds them (clear_rows(), not lv_obj_clean()) and puts
// anything that must come first at kFirstRow, not at index 0.
//
// The page's content (the list) sits in `root`, a plain container
// as large as the screen at its top left, so the shell can slide the page
// as one while a finger drags it, and show it over another page's screen
// meanwhile (wisp/shell/shell.hpp). Anything that must not move with it
// (a card over the page) goes on the screen itself.
//
// Gestures. The shell tracks drags on the page's screen, which hears its
// children's presses only if every object between them bubbles its
// events: bubble_tree() sets LV_OBJ_FLAG_EVENT_BUBBLE on a subtree, and a
// page calls it on whatever it adds. A handler on a container therefore
// also hears its children's events and must check the target. An object
// with kNoSwipe (a slider) starts no drag: dragging it sideways is its
// own gesture.
//
// UI thread only, like every lv_* call.

#include <cstdint>

#include "lvgl.h"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::shell::detail {

// A press that starts on an object with this flag starts no page drag.
inline constexpr lv_obj_flag_t kNoSwipe = LV_OBJ_FLAG_USER_1;
// The least height of anything a finger taps.
inline constexpr int32_t kMinTouchPx = 48;
// A sub-page's title.
inline constexpr int kTitleFontPx = 28;

struct PageFrame {
    lv_obj_t* screen = nullptr;
    // The page's content: everything below is in it.
    lv_obj_t* root = nullptr;
    // Scrolls vertically; a flex column of the title and the page's rows.
    lv_obj_t* list = nullptr;
    // The list's first item.
    lv_obj_t* title = nullptr;
};

// The index in the list of a page's first row: the title is at 0.
inline constexpr int32_t kFirstRow = 1;

// A new screen (not loaded) with the title `title` and the list.
PageFrame make_page(const hal::ScreenShape& shape, ui::StrId title);

// A page's root on `parent`: as large as the screen, transparent, neither
// clickable nor scrollable, bubbling its children's events.
lv_obj_t* make_root(lv_obj_t* parent, const hal::ScreenShape& shape);

// Deletes the list's rows, keeping its title.
void clear_rows(lv_obj_t* list);

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

// Grouped lists (the settings page and its choice pages, mvp spec 6.4): a
// dim heading over each group, the group a card of rows.
//
// A heading above a group's card.
lv_obj_t* make_group_heading(lv_obj_t* parent, const char* text);
// A card for rows: like make_card() but without padding, so that a row's
// pressed state reaches the card's edges; the rows pad themselves.
lv_obj_t* make_row_card(lv_obj_t* card_parent);
// A part of a row card that is not a row (a slider, a hint): a box padded
// like a row.
lv_obj_t* make_row_box(lv_obj_t* card);
// Parts `obj` from what is above it in its card: a hairline on colour
// panels, a gap on two-tone ones (which have no grey for a hairline).
void add_divider(lv_obj_t* obj);

struct ListRow {
    lv_obj_t* row = nullptr;
    lv_obj_t* name = nullptr;
};
// A row of a row card, at least kMinTouchPx high: `icon` (an LV_SYMBOL_*
// or WISP_SYMBOL_*, or nullptr) and `name` at its left, under a divider
// unless it is the card's first. A `clickable` row shows a pressed state.
// Whatever is added to the row afterwards goes after the name, at its
// right end.
ListRow make_list_row(lv_obj_t* card, const char* icon, const char* name, bool clickable);
// Pads `row` so that content `content_h` high sits in the middle of a
// kMinTouchPx row.
void fit_row(lv_obj_t* row, int32_t content_h);
// A value at the right end of `row`: dim, one line, cut short with dots
// when the row has no room for it all, and followed by a chevron when the
// row opens something.
lv_obj_t* add_row_value(lv_obj_t* row, const char* text, bool chevron);
// The height of one line of the body font.
int32_t line_height(lv_obj_t* obj);
// The accent colour of `obj`'s display (the text colour on two-tone
// panels).
lv_color_t accent_colour(lv_obj_t* obj);

}  // namespace wisp::shell::detail
