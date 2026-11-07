#include "wisp/shell/settings_page.hpp"

#include <algorithm>
#include <utility>

#include "page_frame.hpp"
#include "wisp/core/log.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/theme.hpp"
#include "wisp/ui/theme_tokens.hpp"

namespace wisp::shell {

namespace {

constexpr const char* kTag = "shell";
// A slider's track, and how far past it a touch still takes it, so the
// whole row is a touch target.
constexpr int32_t kSliderPx = 12;
constexpr int32_t kSliderReachPx = (detail::kMinTouchPx - kSliderPx) / 2;

std::string choice_text(int seconds) {
    if (seconds == 0) return ui::tr(ui::StrId::SettingsNever);
    if (seconds < 60) return ui::tr_fill(ui::StrId::SettingsSeconds, std::to_string(seconds));
    return ui::tr_fill(ui::StrId::SettingsMinutes, std::to_string(seconds / 60));
}

lv_obj_t* heading(lv_obj_t* card, ui::StrId id) { return detail::make_text(card, ui::tr(id)); }

// A row of text with a control at its right end.
lv_obj_t* make_row(lv_obj_t* card) {
    lv_obj_t* row = detail::make_box(card);
    lv_obj_set_style_min_height(row, detail::kMinTouchPx, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, ui::theme::kGapSm, 0);
    return row;
}

}  // namespace

SettingsPage::SettingsPage(SettingsDeps deps) : deps_(std::move(deps)) {}

SettingsPage::~SettingsPage() {
    if (timer_ != nullptr) lv_timer_delete(timer_);
}

lv_obj_t* SettingsPage::add_slider(lv_obj_t* card, int min, int max, int value) {
    lv_obj_t* box = detail::make_box(card);
    lv_obj_set_height(box, detail::kMinTouchPx);
    lv_obj_t* s = lv_slider_create(box);
    lv_obj_set_size(s, LV_PCT(100), kSliderPx);
    lv_obj_center(s);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, std::clamp(value, min, max), LV_ANIM_OFF);
    lv_obj_set_ext_click_area(s, kSliderReachPx);
    // Dragging it sideways is the slider's, not a page drag.
    lv_obj_add_flag(s, detail::kNoSwipe);
    lv_obj_add_event_cb(s, &SettingsPage::event_cb, LV_EVENT_RELEASED, this);
    return s;
}

void SettingsPage::build(lv_obj_t* list) {
    list_ = list;
    const core::SystemSettingsValues now =
        deps_.settings ? deps_.settings() : core::SystemSettingsValues{};

    lv_obj_t* card = detail::make_card(list_);
    heading(card, ui::StrId::SettingsBrightness);
    brightness_ = add_slider(card, 10, 255, deps_.settings ? now.brightness : 200);

    card = detail::make_card(list_);
    heading(card, ui::StrId::SettingsVolume);
    volume_ = add_slider(card, 0, 100, deps_.settings ? now.volume : 60);

    card = detail::make_card(list_);
    heading(card, ui::StrId::SettingsScreenOff);
    lv_obj_t* choices = detail::make_box(card);
    lv_obj_set_flex_flow(choices, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(choices, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(choices, ui::theme::kGapSm, 0);
    for (size_t i = 0; i < kScreenOffChoices.size(); ++i) {
        lv_obj_t* b = lv_button_create(choices);
        // Three to a row. No flex grow: LVGL leaves growing items out when
        // it decides where a row wraps, which put all six on one row.
        lv_obj_set_size(b, LV_PCT(32), detail::kMinTouchPx);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, choice_text(kScreenOffChoices[i]).c_str());
        lv_obj_center(l);
        lv_obj_add_event_cb(b, &SettingsPage::event_cb, LV_EVENT_CLICKED, this);
        off_[i] = b;
    }
    mark_screen_off(deps_.settings ? now.screen_off_s : -1);

    card = detail::make_card(list_);
    lv_obj_t* row = make_row(card);
    lv_obj_t* label = detail::make_text(row, ui::tr(ui::StrId::SettingsTtsMute));
    lv_obj_set_width(label, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(label, 1);
    mute_ = lv_switch_create(row);
    if (deps_.settings && now.tts_mute) lv_obj_add_state(mute_, LV_STATE_CHECKED);
    lv_obj_add_flag(mute_, detail::kNoSwipe);
    lv_obj_add_event_cb(mute_, &SettingsPage::event_cb, LV_EVENT_VALUE_CHANGED, this);

    wifi_row_ = detail::make_card(list_);
    lv_obj_add_flag(wifi_row_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_min_height(wifi_row_, detail::kMinTouchPx, 0);
    if (detail::colour_panel(list_))
        lv_obj_set_style_bg_color(wifi_row_, lv_color_hex(ui::theme::kSurface3), LV_STATE_PRESSED);
    std::string wifi = ui::tr(ui::StrId::SettingsWifi);
    wifi += "  " LV_SYMBOL_RIGHT;
    detail::make_text(wifi_row_, wifi.c_str());
    wifi_label_ = detail::make_text(wifi_row_, "", true);
    lv_obj_add_event_cb(wifi_row_, &SettingsPage::event_cb, LV_EVENT_CLICKED, this);

    card = detail::make_card(list_);
    heading(card, ui::StrId::SettingsAbout);
    std::string version = ui::tr(ui::StrId::AboutFirmware);
    version += "  ";
    version += deps_.firmware_version ? deps_.firmware_version() : std::string();
    version_ = detail::make_text(card, version.c_str(), true);
    address_ = detail::make_text(card, "", true);
    detail::make_text(card, ui::tr(ui::StrId::AboutPairHint), true);
    diag_ = lv_button_create(card);
    lv_obj_set_size(diag_, LV_PCT(100), detail::kMinTouchPx);
    ui::set_button_role(diag_, ui::ButtonRole::Secondary);
    label = lv_label_create(diag_);
    lv_label_set_text(label, ui::tr(ui::StrId::AboutDiagnostics));
    lv_obj_center(label);
    lv_obj_add_event_cb(diag_, &SettingsPage::event_cb, LV_EVENT_CLICKED, this);

    detail::bubble_tree(list_);
    refresh();
    timer_ = lv_timer_create(
        [](lv_timer_t* t) { static_cast<SettingsPage*>(lv_timer_get_user_data(t))->refresh(); },
        kRefreshMs, this);
}

void SettingsPage::refresh() {
    if (wifi_label_ == nullptr) return;
    std::string wifi = deps_.wifi_status ? deps_.wifi_status() : std::string();
    if (wifi != shown_wifi_) {
        shown_wifi_ = wifi;
        lv_label_set_text(wifi_label_, wifi.c_str());
    }
    const std::string url = deps_.admin_url ? deps_.admin_url() : std::string();
    std::string address = ui::tr(ui::StrId::AboutAddress);
    address += "  ";
    address += url.empty() ? ui::tr(ui::StrId::AboutAddressNone) : url;
    if (address != shown_address_) {
        shown_address_ = address;
        lv_label_set_text(address_, address.c_str());
    }
}

void SettingsPage::mark_screen_off(int seconds) {
    for (size_t i = 0; i < kScreenOffChoices.size(); ++i) {
        ui::set_button_role(off_[i], kScreenOffChoices[i] == seconds ? ui::ButtonRole::Primary
                                                                     : ui::ButtonRole::Secondary);
    }
}

void SettingsPage::write(std::map<std::string, Value> changes) {
    if (!deps_.set_settings) return;
    if (!deps_.submit_io) {
        deps_.set_settings(std::move(changes));
        return;
    }
    if (!deps_.submit_io([set = deps_.set_settings, changes = std::move(changes)]() mutable {
            set(std::move(changes));
        })) {
        WLOG_W(kTag, "setting not saved (storage queue full)");
    }
}

void SettingsPage::event_cb(lv_event_t* e) {
    auto* self = static_cast<SettingsPage*>(lv_event_get_user_data(e));
    lv_obj_t* obj = lv_event_get_current_target_obj(e);
    if (obj == self->brightness_) {
        self->write({{"brightness", Value(static_cast<int64_t>(lv_slider_get_value(obj)))}});
    } else if (obj == self->volume_) {
        self->write({{"volume", Value(static_cast<int64_t>(lv_slider_get_value(obj)))}});
    } else if (obj == self->mute_) {
        self->write({{"tts_mute", Value(lv_obj_has_state(obj, LV_STATE_CHECKED))}});
    } else if (obj == self->wifi_row_) {
        if (self->deps_.start_wifi_setup) self->deps_.start_wifi_setup();
    } else if (obj == self->diag_) {
        if (self->deps_.open_diagnostics) self->deps_.open_diagnostics();
    } else {
        for (size_t i = 0; i < self->off_.size(); ++i) {
            if (obj != self->off_[i]) continue;
            const int seconds = kScreenOffChoices[i];
            self->mark_screen_off(seconds);
            self->write({{"screen_off_s", Value(static_cast<int64_t>(seconds))}});
        }
    }
}

}  // namespace wisp::shell
