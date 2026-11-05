#include "self_check_layout.hpp"

#include <cstring>
#include <utility>

namespace wisp::app {

namespace {

constexpr int32_t kCardRadius = 12;
constexpr int32_t kCardPad = 10;
constexpr int32_t kCardRowGap = 6;
constexpr int32_t kTableColumnGap = 12;
constexpr int32_t kTableRowGap = 4;

// Grid descriptors must outlive the grids that use them. A table of n rows
// uses the last n + 1 entries of kTableRows.
const int32_t kTableColumns[] = {LV_GRID_CONTENT, LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
const int32_t kTableRows[] = {LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT,
                              LV_GRID_TEMPLATE_LAST};
static_assert(sizeof(kTableRows) / sizeof(kTableRows[0]) == kMaxTableRows + 1);

}  // namespace

void PageText::bind(lv_obj_t* label, ui::StrId id, std::string prefix) {
    bound_.push_back(Bound{label, id, std::move(prefix)});
    render(bound_.back());
}

bool PageText::set_lang(ui::Lang lang) {
    if (lang == lang_) {
        return false;
    }
    lang_ = lang;
    for (const Bound& bound : bound_) {
        render(bound);
    }
    return true;
}

void PageText::render(const Bound& bound) const {
    if (bound.prefix.empty()) {
        set_text_if_changed(bound.label, tr(bound.id));
        return;
    }
    set_text_if_changed(bound.label, (bound.prefix + tr(bound.id)).c_str());
}

uint32_t state_color(core::DeviceState state) {
    switch (state) {
        case core::DeviceState::Ok:
            return kColorGood;
        case core::DeviceState::Degraded:
            return kColorWarning;
        case core::DeviceState::Missing:
            return kColorMissing;
        case core::DeviceState::Failed:
            return kColorError;
    }
    return kColorError;
}

uint32_t level_color(SummaryLevel level) {
    switch (level) {
        case SummaryLevel::Good:
            return kColorGood;
        case SummaryLevel::Warning:
            return kColorWarning;
        case SummaryLevel::Error:
            return kColorError;
        case SummaryLevel::Neutral:
            return kColorNeutralChip;
    }
    return kColorNeutralChip;
}

lv_obj_t* make_card(lv_obj_t* col, ui::StrId heading, PageText& text) {
    lv_obj_t* card = lv_obj_create(col);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), 0);
    lv_obj_set_style_radius(card, kCardRadius, 0);
    lv_obj_set_style_pad_all(card, kCardPad, 0);
    lv_obj_set_style_pad_row(card, kCardRowGap, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = make_wrap_label(card);
    text.bind(title, heading);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorDimText), 0);
    return card;
}

lv_obj_t* make_wrap_label(lv_obj_t* parent) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    return label;
}

lv_obj_t* make_plain_row(lv_obj_t* parent, lv_flex_flow_t flow) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, flow);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

lv_obj_t* make_table(lv_obj_t* parent, int rows) {
    lv_obj_t* table = lv_obj_create(parent);
    lv_obj_remove_style_all(table);
    lv_obj_set_size(table, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(table, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(table, kTableColumnGap, 0);
    lv_obj_set_style_pad_row(table, kTableRowGap, 0);
    rows = rows < 1 ? 1 : rows > kMaxTableRows ? kMaxTableRows : rows;
    lv_obj_set_grid_dsc_array(table, kTableColumns, kTableRows + (kMaxTableRows - rows));
    return table;
}

void add_table_key(lv_obj_t* table, int row, ui::StrId label, PageText& text) {
    lv_obj_t* key = lv_label_create(table);
    text.bind(key, label);
    lv_obj_set_style_text_color(key, lv_color_hex(kColorDimText), 0);
    lv_obj_set_grid_cell(key, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_START, row, 1);
}

lv_obj_t* add_table_row(lv_obj_t* table, int row, ui::StrId label, PageText& text) {
    add_table_key(table, row, label, text);
    lv_obj_t* value = lv_label_create(table);
    lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_grid_cell(value, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_START, row, 1);
    return value;
}

void set_text_if_changed(lv_obj_t* label, const char* text) {
    const char* shown = lv_label_get_text(label);
    if (shown != nullptr && std::strcmp(shown, text) == 0) {
        return;
    }
    lv_label_set_text(label, text);
}

void set_text_color_if_changed(lv_obj_t* obj, uint32_t rgb) {
    lv_color_t color = lv_color_hex(rgb);
    if (lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), color)) {
        return;
    }
    lv_obj_set_style_text_color(obj, color, 0);
}

void set_bg_color_if_changed(lv_obj_t* obj, uint32_t rgb) {
    lv_color_t color = lv_color_hex(rgb);
    if (lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), color)) {
        return;
    }
    lv_obj_set_style_bg_color(obj, color, 0);
}

}  // namespace wisp::app
