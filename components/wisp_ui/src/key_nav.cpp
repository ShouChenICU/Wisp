#include "wisp/ui/key_nav.hpp"

#include <algorithm>

#include "theme_internal.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::ui {

namespace {

// The nearest ancestor that can scroll vertically now.
lv_obj_t* scrolling_parent(lv_obj_t* obj) {
    for (lv_obj_t* p = lv_obj_get_parent(obj); p != nullptr; p = lv_obj_get_parent(p)) {
        if (!lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE)) continue;
        if (lv_obj_get_scroll_top(p) > 0 || lv_obj_get_scroll_bottom(p) > 0) return p;
    }
    return nullptr;
}

lv_obj_t* focus_row(lv_obj_t* obj) {
    for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o))
        if (lv_obj_has_flag(o, kFocusRow)) return o;
    return obj;
}

// Whether `row`, moved down by `dy`, lies inside `clip` and on the visible
// part of `shape`. Its top and bottom pixel rows are enough: the visible
// part of every supported shape is convex.
bool fits(const hal::ScreenShape& shape, const lv_area_t& clip, const lv_area_t& row, int32_t dy) {
    const int32_t y1 = row.y1 + dy;
    const int32_t y2 = row.y2 + dy;
    if (y1 < clip.y1 || y2 > clip.y2) return false;
    for (int32_t y : {y1, y2}) {
        const hal::Span span = hal::row_span(shape, y);
        if (span.empty() || span.x1 > row.x1 || span.x2 < row.x2) return false;
    }
    return true;
}

}  // namespace

bool key_focusable(lv_obj_t* obj) {
    if (lv_obj_has_flag(obj, kKeyFocusable)) return true;
    if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) || !lv_obj_is_group_def(obj)) return false;
    return !lv_obj_check_type(obj, &lv_msgbox_class) &&
           !lv_obj_check_type(obj, &lv_msgbox_backdrop_class);
}

void collect_focusable(lv_obj_t* root, std::vector<lv_obj_t*>& out) {
    const uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* child = lv_obj_get_child(root, static_cast<int32_t>(i));
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
        if (!key_focusable(child)) {
            collect_focusable(child, out);
            continue;
        }
        out.push_back(child);
        // A row focusable by the flag may hold a control of its own (a
        // network row's info button); a widget's own children are parts of it.
        if (lv_obj_has_flag(child, kKeyFocusable)) collect_focusable(child, out);
    }
}

void make_key_focusable(lv_obj_t* obj) {
    lv_obj_add_flag(obj, kKeyFocusable);
    const bool colour = hal::panel_is_colour(display_panel(lv_obj_get_display(obj)));
    lv_obj_add_style(obj, detail::focus_row_style(colour), LV_STATE_FOCUS_KEY);
}

void show_focus(lv_group_t* group, lv_obj_t* obj, const hal::ScreenShape& shape) {
    if (obj == nullptr) return;
    lv_obj_update_layout(obj);
    lv_obj_t* list = scrolling_parent(obj);
    if (list == nullptr) return;
    lv_area_t row;
    lv_obj_get_coords(focus_row(obj), &row);
    lv_area_t clip;
    lv_obj_get_coords(list, &clip);
    // The range scroll_by() can move the content by: down to the top,
    // up to the bottom.
    const int32_t down = std::max<int32_t>(0, lv_obj_get_scroll_top(list));
    const int32_t up = std::max<int32_t>(0, lv_obj_get_scroll_bottom(list));
    const uint32_t count = lv_group_get_obj_count(group);

    bool found = false;
    int32_t dy = 0;
    if (count > 0 && obj == lv_group_get_obj_by_index(group, 0) && fits(shape, clip, row, down)) {
        dy = down;
        found = true;
    } else if (count > 0 && obj == lv_group_get_obj_by_index(group, count - 1) &&
               fits(shape, clip, row, -up)) {
        dy = -up;
        found = true;
    }
    for (int32_t d = 0; !found && d <= shape.height; ++d) {
        if (d <= down && fits(shape, clip, row, d)) {
            dy = d;
            found = true;
        } else if (d <= up && fits(shape, clip, row, -d)) {
            dy = -d;
            found = true;
        }
    }
    if (!found) return;
    // LVGL's scroll toward its own target may be under way; the content
    // is moved from where it is now.
    lv_obj_stop_scroll_anim(list);
    lv_obj_scroll_by(list, 0, dy, LV_ANIM_ON);
}

}  // namespace wisp::ui
