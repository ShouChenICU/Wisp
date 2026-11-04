#include "scene_section.hpp"

#include <utility>

#include "self_check_layout.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::app {

namespace {

constexpr int32_t kRowGap = 8;
constexpr int32_t kRowPad = 4;
constexpr int32_t kRowRadius = 8;
constexpr int32_t kChipPadHor = 8;
constexpr int32_t kChipPadVer = 2;
constexpr uint32_t kChipText = 0xFFFFFF;
// A pressed row, a shade darker than the card.
constexpr uint32_t kColorPressed = 0xE0E2E6;

}  // namespace

void SceneSection::build(lv_obj_t* col, StatusFn status, OpenFn on_open) {
    status_ = std::move(status);
    on_open_ = std::move(on_open);
    card_ = make_card(col, ui::StrId::Scenes);
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
    if (!listed_) return;
    if (entries_.empty()) {
        lv_obj_t* empty = make_wrap_label(card_);
        lv_label_set_text(empty, ui::tr(ui::StrId::ScenesEmpty));
        lv_obj_set_style_text_color(empty, lv_color_hex(kColorDimText), 0);
        return;
    }
    rows_.reserve(entries_.size());
    for (const SceneEntry& entry : entries_) {
        Row row;
        row.entry = entry;
        lv_obj_t* obj = make_plain_row(card_, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(obj, kRowGap, 0);
        lv_obj_set_style_pad_ver(obj, kRowPad, 0);
        lv_obj_set_style_radius(obj, kRowRadius, 0);
        lv_obj_set_style_bg_color(obj, lv_color_hex(kColorPressed), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(obj, &SceneSection::row_cb, LV_EVENT_CLICKED, this);

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

void SceneSection::render(Row& row) {
    row.rendered = true;
    switch (row.state) {
        case core::SceneState::Running:
        case core::SceneState::Error: {
            const bool running = row.state == core::SceneState::Running;
            set_text_if_changed(row.status, ui::tr(running ? ui::StrId::SceneStateRunning
                                                           : ui::StrId::SceneStateError));
            set_text_color_if_changed(row.status, kChipText);
            set_bg_color_if_changed(row.chip, running ? kColorGood : kColorError);
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
        if (lv_obj_get_parent(row.chip) == target) {
            // A copy: opening may come back here (a list refresh) before
            // the callback returns.
            const std::string id = row.entry.id;
            if (self->on_open_) self->on_open_(id);
            return;
        }
    }
}

}  // namespace wisp::app
