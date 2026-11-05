#include "scene_section.hpp"

#include <utility>

#include "self_check_layout.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/theme.hpp"

namespace wisp::app {

namespace {

constexpr int32_t kRowGap = ui::theme::kGapSm;
constexpr int32_t kRowPad = ui::theme::kGapXs;
constexpr int32_t kRowRadius = ui::theme::kRadiusSm;
// A row is a tap target: never shorter than a fingertip.
constexpr int32_t kRowMinHeight = 48;
constexpr int32_t kChipPadHor = 8;
constexpr int32_t kChipPadVer = 2;
// A pressed row: the card lit by its own text.
constexpr uint32_t kColorPressed = ui::theme::pressed(kColorCard, ui::theme::kText);

}  // namespace

void SceneSection::build(lv_obj_t* col, PageText& text, StatusFn status, OpenFn on_open) {
    text_ = &text;
    status_ = std::move(status);
    on_open_ = std::move(on_open);
    card_ = make_card(col, ui::StrId::Scenes, text);
    build_rows();
}

void SceneSection::set_entries(std::vector<SceneEntry> entries) {
    if (listed_ && entries == entries_) {
        update_states();
        return;
    }
    listed_ = true;
    entries_ = std::move(entries);
    if (card_ != nullptr) build_rows();
}

void SceneSection::build_rows() {
    // Index 0 is the heading.
    while (lv_obj_get_child_count(card_) > 1) lv_obj_delete(lv_obj_get_child(card_, 1));
    rows_.clear();
    empty_ = nullptr;
    if (!listed_) return;
    if (entries_.empty()) {
        empty_ = make_wrap_label(card_);
        lv_label_set_text(empty_, text_->tr(ui::StrId::ScenesEmpty));
        lv_obj_set_style_text_color(empty_, lv_color_hex(kColorDimText), 0);
        return;
    }
    rows_.reserve(entries_.size());
    for (const SceneEntry& entry : entries_) {
        Row row;
        row.entry = entry;
        // The tap target: a column at least kRowMinHeight tall that centres
        // the name-and-state line in it. (A content-sized flex row always
        // puts its line at the top, whatever its min height.)
        lv_obj_t* tap = make_plain_row(card_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_ver(tap, kRowPad, 0);
        lv_obj_set_style_min_height(tap, kRowMinHeight, 0);
        lv_obj_set_style_radius(tap, kRowRadius, 0);
        lv_obj_set_style_bg_color(tap, lv_color_hex(kColorPressed), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(tap, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(tap, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tap, &SceneSection::row_cb, LV_EVENT_CLICKED, this);
        row.row = tap;

        lv_obj_t* obj = make_plain_row(tap, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(obj, kRowGap, 0);
        // Clicks land on the tap target, not on the line inside it.
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* name = lv_label_create(obj);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_flex_grow(name, 1);
        lv_label_set_text(name, entry.name.c_str());

        row.chip = lv_obj_create(obj);
        lv_obj_remove_style_all(row.chip);
        lv_obj_set_size(row.chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(row.chip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_hor(row.chip, kChipPadHor, 0);
        lv_obj_set_style_pad_ver(row.chip, kChipPadVer, 0);
        lv_obj_remove_flag(row.chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(row.chip, LV_OBJ_FLAG_CLICKABLE);
        row.status = lv_label_create(row.chip);
        rows_.push_back(std::move(row));
    }
    update_states();
}

void SceneSection::update_states() {
    for (Row& row : rows_) {
        const core::SceneState state =
            status_ ? status_(row.entry.id).state : core::SceneState::Stopped;
        if (row.rendered && state == row.state) continue;
        row.state = state;
        render(row);
    }
}

void SceneSection::retext() {
    if (empty_ != nullptr) set_text_if_changed(empty_, text_->tr(ui::StrId::ScenesEmpty));
    for (Row& row : rows_) {
        if (row.rendered) render(row);
    }
}

void SceneSection::render(Row& row) {
    row.rendered = true;
    switch (row.state) {
        case core::SceneState::Running:
        case core::SceneState::Error: {
            const bool running = row.state == core::SceneState::Running;
            set_text_if_changed(row.status, text_->tr(running ? ui::StrId::SceneStateRunning
                                                              : ui::StrId::SceneStateError));
            // A badge: the state's colour on its soft tint.
            set_text_color_if_changed(row.status, running ? kColorGood : kColorError);
            set_bg_color_if_changed(row.chip, running ? ui::theme::kOkSoft : ui::theme::kErrSoft);
            lv_obj_set_style_bg_opa(row.chip, LV_OPA_COVER, 0);
            break;
        }
        case core::SceneState::Stopped:
            set_text_if_changed(row.status, row.entry.version.c_str());
            set_text_color_if_changed(row.status, kColorDimText);
            lv_obj_set_style_bg_opa(row.chip, LV_OPA_TRANSP, 0);
            break;
    }
}

void SceneSection::row_cb(lv_event_t* e) {
    auto* self = static_cast<SceneSection*>(lv_event_get_user_data(e));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    for (const Row& row : self->rows_) {
        if (row.row == target) {
            // A copy: opening may come back here (a list refresh) before
            // the callback returns.
            const std::string id = row.entry.id;
            if (self->on_open_) self->on_open_(id);
            return;
        }
    }
}

}  // namespace wisp::app
