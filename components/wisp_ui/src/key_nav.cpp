#include "wisp/ui/key_nav.hpp"

#include "theme_internal.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::ui {

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
        if (key_focusable(child)) {
            out.push_back(child);
        } else {
            collect_focusable(child, out);
        }
    }
}

void make_key_focusable(lv_obj_t* obj) {
    lv_obj_add_flag(obj, kKeyFocusable);
    const bool colour = hal::panel_is_colour(display_panel(lv_obj_get_display(obj)));
    lv_obj_add_style(obj, detail::focus_row_style(colour), LV_STATE_FOCUS_KEY);
}

}  // namespace wisp::ui
