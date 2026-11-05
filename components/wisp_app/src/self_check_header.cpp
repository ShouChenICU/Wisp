#include "self_check_header.hpp"

#include <utility>

#include "wisp/ui/fonts.hpp"
#include "wisp/ui/theme.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

namespace {

constexpr int32_t kChipPadHor = 10;
constexpr int32_t kChipPadVer = ui::theme::kGapXs;
constexpr int32_t kChipGap = ui::theme::kGapSm;

// The title's size (vector-font spec 5.1): large enough to read at a glance,
// in the page's current language.
constexpr int kTitlePx = 32;

// A pill-shaped container with one label; `label` receives the label.
lv_obj_t* make_pill(lv_obj_t* parent, lv_obj_t** label) {
    lv_obj_t* chip = lv_obj_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(chip, kChipPadHor, 0);
    lv_obj_set_style_pad_ver(chip, kChipPadVer, 0);
    lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    *label = lv_label_create(chip);
    return chip;
}

// Colours `chip` for `level`, as a badge: the level's colour on its own
// soft tint.
void paint_chip(lv_obj_t* chip, lv_obj_t* label, SummaryLevel level) {
    set_bg_color_if_changed(chip, level_tint(level));
    set_text_color_if_changed(label, level_color(level));
}

std::string fraction(int part, int whole) {
    return std::to_string(part) + "/" + std::to_string(whole);
}

// A language's own name, the same in every table.
ui::StrId lang_name(ui::Lang lang) {
    switch (lang) {
        case ui::Lang::Zh:
            return ui::StrId::LangNameZh;
        case ui::Lang::Ja:
            return ui::StrId::LangNameJa;
        case ui::Lang::En:
            return ui::StrId::LangNameEn;
    }
    return ui::StrId::LangNameEn;
}

}  // namespace

void SelfCheckHeader::build(lv_obj_t* col, PageText& text, std::function<void()> on_devices_tap) {
    text_ = &text;
    on_devices_tap_ = std::move(on_devices_tap);

    lv_obj_t* title = make_wrap_label(col);
    lv_obj_set_style_text_font(title, ui::font(kTitlePx), 0);
    text.bind(title, ui::StrId::SelfCheckTitle);

    lv_obj_t* chips = make_plain_row(col, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(chips, kChipGap, 0);
    lv_obj_set_style_pad_row(chips, kChipGap, 0);
    devices_chip_ = make_pill(chips, &devices_label_);
    lv_obj_add_event_cb(devices_chip_, &SelfCheckHeader::devices_chip_cb, LV_EVENT_CLICKED, this);
    tests_chip_ = make_pill(chips, &tests_label_);
}

void SelfCheckHeader::show_devices(const DeviceSummary& summary) {
    if (devices_label_ == nullptr) {
        return;
    }
    set_text_if_changed(
        devices_label_,
        text_->tr_fill(ui::StrId::SummaryDevices, fraction(summary.ok, summary.total)).c_str());
    paint_chip(devices_chip_, devices_label_, summary.level);
}

void SelfCheckHeader::show_tests(int passed) {
    if (tests_label_ == nullptr) {
        return;
    }
    set_text_if_changed(
        tests_label_,
        text_->tr_fill(ui::StrId::SummaryTests, fraction(passed, kInteractiveTestCount)).c_str());
    paint_chip(tests_chip_, tests_label_,
               passed == kInteractiveTestCount ? SummaryLevel::Good : SummaryLevel::Neutral);
}

void SelfCheckHeader::devices_chip_cb(lv_event_t* e) {
    auto* self = static_cast<SelfCheckHeader*>(lv_event_get_user_data(e));
    if (self->on_devices_tap_) {
        self->on_devices_tap_();
    }
}

void BoardSection::build(lv_obj_t* col, PageText& text, const Info& info,
                         std::function<void(ui::Lang)> on_lang) {
    on_lang_ = std::move(on_lang);
    lv_obj_t* card = make_card(col, ui::StrId::SectionBoard, text);

    lv_obj_t* table = make_table(card, info.target.empty() ? 3 : 4);
    int row = 0;
    lv_label_set_text(add_table_row(table, row++, ui::StrId::BoardName, text), info.name.c_str());
    if (!info.target.empty()) {
        lv_label_set_text(add_table_row(table, row++, ui::StrId::BoardTarget, text),
                          info.target.c_str());
    }
    lv_label_set_text(add_table_row(table, row++, ui::StrId::Firmware, text), info.version.c_str());
    boot_ = add_table_row(table, row, ui::StrId::BootTime, text);
    lv_label_set_text(boot_, "");

    lv_obj_t* langs = make_plain_row(card, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(langs, kChipGap, 0);
    lv_obj_set_style_pad_row(langs, kChipGap, 0);
    size_t i = 0;
    for (ui::Lang lang : kLangs) {
        lv_obj_t* label = nullptr;
        lv_obj_t* button = make_pill(langs, &label);
        // Not translated: each name is written in its own language.
        lv_label_set_text(label, ui::tr(lang_name(lang), lang));
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
        // A segmented choice: the current language is an accent pill, the
        // others raised ones.
        namespace theme = ui::theme;
        lv_obj_set_style_bg_color(button, lv_color_hex(theme::kSurface3), 0);
        lv_obj_set_style_text_color(button, lv_color_hex(theme::kText), 0);
        lv_obj_set_style_bg_color(
            button, lv_color_hex(theme::pressed(theme::kSurface3, theme::kText)), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(button, lv_color_hex(theme::kAccent), LV_STATE_CHECKED);
        lv_obj_set_style_text_color(button, lv_color_hex(theme::kAccentInk), LV_STATE_CHECKED);
        lv_obj_add_event_cb(button, &BoardSection::lang_cb, LV_EVENT_CLICKED, this);
        lang_buttons_[i++] = button;
    }
    show_lang(text.lang());
}

void BoardSection::set_boot_ms(uint64_t boot_ms) {
    if (boot_ != nullptr) {
        set_text_if_changed(boot_, (std::to_string(boot_ms) + " ms").c_str());
    }
}

void BoardSection::show_lang(ui::Lang lang) {
    size_t i = 0;
    for (ui::Lang each : kLangs) {
        lv_obj_t* button = lang_buttons_[i++];
        if (button == nullptr) continue;
        if (each == lang) {
            lv_obj_add_state(button, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(button, LV_STATE_CHECKED);
        }
    }
}

void BoardSection::lang_cb(lv_event_t* e) {
    auto* self = static_cast<BoardSection*>(lv_event_get_user_data(e));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    size_t i = 0;
    for (ui::Lang lang : kLangs) {
        if (self->lang_buttons_[i++] == target && self->on_lang_) {
            self->on_lang_(lang);
            return;
        }
    }
}

}  // namespace wisp::app
