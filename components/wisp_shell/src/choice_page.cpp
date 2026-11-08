#include "wisp/shell/choice_page.hpp"

#include <algorithm>
#include <utility>

#include "page_frame.hpp"

namespace wisp::shell {

ChoicePage::ChoicePage(ChoiceSpec spec) : spec_(std::move(spec)) {}

void ChoicePage::build(lv_obj_t* list) {
    list_ = list;
    card_ = detail::make_row_card(list_);
    for (size_t i = 0; i < spec_.labels.size(); ++i) {
        detail::ListRow r = detail::make_list_row(card_, nullptr, "", true);
        // The label takes the row and is cut short rather than wrapped.
        lv_obj_set_flex_grow(r.name, 1);
        lv_obj_set_height(r.name, detail::line_height(r.row));
        lv_label_set_long_mode(r.name, LV_LABEL_LONG_DOT);
        lv_label_set_text(r.name, spec_.labels[i].c_str());
        lv_obj_t* check = lv_label_create(r.row);
        lv_label_set_text(check, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(check, detail::accent_colour(r.row), 0);
        lv_obj_add_event_cb(r.row, &ChoicePage::event_cb, LV_EVENT_CLICKED, this);
        rows_.push_back(r.row);
        checks_.push_back(check);
    }
    mark(spec_.current);
    detail::bubble_tree(list_);
    if (spec_.current >= 0 && static_cast<size_t>(spec_.current) < rows_.size()) {
        // The current choice in the middle of the screen, as near as the
        // list's ends let it be.
        lv_obj_update_layout(list_);
        lv_area_t row, list;
        lv_obj_get_coords(rows_[static_cast<size_t>(spec_.current)], &row);
        lv_obj_get_coords(list_, &list);
        const int32_t at = lv_obj_get_scroll_y(list_);
        const int32_t y = at + (row.y1 + row.y2) / 2 - (list.y1 + list.y2) / 2;
        const int32_t end = std::max<int32_t>(0, at + lv_obj_get_scroll_bottom(list_));
        lv_obj_scroll_to_y(list_, std::clamp<int32_t>(y, 0, end), LV_ANIM_OFF);
    }
}

void ChoicePage::mark(int current) {
    spec_.current = current;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const bool on = static_cast<int>(i) == current;
        lv_obj_t* name = lv_obj_get_child(rows_[i], 0);
        lv_obj_set_style_text_color(
            name, on ? detail::accent_colour(rows_[i]) : detail::text_colour(rows_[i]), 0);
        if (on)
            lv_obj_remove_flag(checks_[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(checks_[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void ChoicePage::event_cb(lv_event_t* e) {
    auto* self = static_cast<ChoicePage*>(lv_event_get_user_data(e));
    lv_obj_t* obj = lv_event_get_current_target_obj(e);
    for (size_t i = 0; i < self->rows_.size(); ++i) {
        if (obj != self->rows_[i]) continue;
        self->mark(static_cast<int>(i));
        if (self->spec_.on_pick) self->spec_.on_pick(i);
        return;
    }
}

}  // namespace wisp::shell
