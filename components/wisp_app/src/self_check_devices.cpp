#include "self_check_devices.hpp"

#include <utility>

#include "self_check_layout.hpp"
#include "wisp/app/format.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::app {

namespace {

constexpr int32_t kDotSize = 10;
constexpr int32_t kDotGap = 8;
constexpr int32_t kWordGap = 8;

// A label sized to its text but wrapping at `max_width` pixels.
lv_obj_t* make_word_label(lv_obj_t* parent, const std::string& text, int32_t max_width) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_max_width(label, max_width, 0);
    lv_label_set_text(label, text.c_str());
    return label;
}

}  // namespace

void DeviceSection::build(lv_obj_t* col, PageText& text, std::vector<DeviceRowInfo> devices) {
    text_ = &text;
    card_ = make_card(col, ui::StrId::SectionDevices, text);
    // Content-sized labels only wrap at a max width in pixels (a percentage
    // is not applied while measuring the text): one layout pass gives the
    // width every row's text has.
    lv_obj_update_layout(card_);
    const int32_t text_width = lv_obj_get_content_width(card_) - kDotSize - kDotGap;

    rows_.clear();
    rows_.reserve(devices.size());
    for (DeviceRowInfo& info : devices) {
        core::DeviceState state = info.state;
        rows_.push_back(Row{std::move(info), state});
    }
    for (size_t i : device_display_order(states())) {
        build_row(rows_[i], text_width);
    }
}

void DeviceSection::build_row(Row& row, int32_t text_width) {
    row.row = make_plain_row(card_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row.row, kDotGap, 0);

    // Centred on the first text line.
    const lv_font_t* font = lv_obj_get_style_text_font(row.row, LV_PART_MAIN);
    row.dot = lv_obj_create(row.row);
    lv_obj_remove_style_all(row.dot);
    lv_obj_set_size(row.dot, kDotSize, kDotSize);
    lv_obj_set_style_radius(row.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(row.dot, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_top(row.dot, (lv_font_get_line_height(font) - kDotSize) / 2, 0);
    lv_obj_remove_flag(row.dot, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* body = lv_obj_create(row.row);
    lv_obj_remove_style_all(body);
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(body, kWordGap, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    make_word_label(body, row.boot.id, text_width);
    lv_obj_t* driver = make_word_label(body, row.boot.model, text_width);
    lv_obj_set_style_text_color(driver, lv_color_hex(kColorDimText), 0);
    // Full width, so it always starts a line of its own.
    row.reason = make_wrap_label(body);
    lv_obj_set_style_text_color(row.reason, lv_color_hex(kColorDimText), 0);

    render(row);
}

void DeviceSection::render(Row& row) {
    set_bg_color_if_changed(row.dot, state_color(row.state));
    if (row.state == core::DeviceState::Ok) {
        lv_obj_add_flag(row.reason, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    // The boot report's reason only explains the state it was recorded
    // with; a state reached later (a card taken out) shows just its name.
    std::string text = state_label(row.state, text_->lang());
    if (row.state == row.boot.state && !row.boot.reason.empty()) {
        text += ": " + row.boot.reason;
    }
    set_text_if_changed(row.reason, text.c_str());
    lv_obj_remove_flag(row.reason, LV_OBJ_FLAG_HIDDEN);
}

void DeviceSection::retext() {
    for (Row& row : rows_) {
        if (row.row != nullptr) render(row);
    }
}

bool DeviceSection::set_state(const std::string& id, core::DeviceState state) {
    for (Row& row : rows_) {
        if (row.boot.id != id) {
            continue;
        }
        if (row.state == state) {
            return false;
        }
        row.state = state;
        if (row.row != nullptr) {
            render(row);
            reorder();
        }
        return true;
    }
    return false;
}

void DeviceSection::reorder() {
    // Index 0 is the card's heading.
    int32_t index = 1;
    for (size_t i : device_display_order(states())) {
        if (lv_obj_get_index(rows_[i].row) != index) {
            lv_obj_move_to_index(rows_[i].row, index);
        }
        ++index;
    }
}

std::vector<core::DeviceState> DeviceSection::states() const {
    std::vector<core::DeviceState> out;
    out.reserve(rows_.size());
    for (const Row& row : rows_) {
        out.push_back(row.state);
    }
    return out;
}

DeviceSummary DeviceSection::summary() const { return summarize_devices(states()); }

bool DeviceSection::scroll_to_first_problem() {
    std::vector<core::DeviceState> current = states();
    std::vector<size_t> order = device_display_order(current);
    if (order.empty() || current[order.front()] == core::DeviceState::Ok) {
        return false;
    }
    lv_obj_t* row = rows_[order.front()].row;
    if (row == nullptr) {
        return false;
    }
    lv_obj_scroll_to_view_recursive(row, LV_ANIM_ON);
    return true;
}

}  // namespace wisp::app
