#include "wisp/ui/dialog.hpp"

namespace wisp::ui {

lv_obj_t* make_dialog_actions(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, kDialogButtonGap, 0);
    return row;
}

lv_obj_t* add_dialog_button(lv_obj_t* row, const char* text, ButtonRole role) {
    lv_obj_t* btn = lv_button_create(row);
    set_button_role(btn, role);
    // Flex growth alone sets the width: every button gets the same share
    // of the row whatever its label's length.
    lv_obj_set_width(btn, 0);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(btn, kDialogButtonMinHeight, 0);
    lv_obj_t* label = lv_label_create(btn);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return btn;
}

}  // namespace wisp::ui
