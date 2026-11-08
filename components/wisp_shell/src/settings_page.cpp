#include "wisp/shell/settings_page.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "page_frame.hpp"
#include "wisp/core/log.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/symbols.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/theme_tokens.hpp"

namespace wisp::shell {

namespace {

constexpr const char* kTag = "shell";
// A slider's track, and how far past it a touch still takes it, so the
// slider's line is a touch target.
constexpr int32_t kSliderPx = 12;
constexpr int32_t kSliderReachPx = (detail::kMinTouchPx - kSliderPx) / 2;
// A switch, and the slider's end icons' width, so the tracks line up.
constexpr int32_t kSwitchW = 52, kSwitchH = 30;
constexpr int32_t kEndIconPx = 20;

int percent(int value, int max) { return (value * 100 + max / 2) / max; }

std::string percent_text(int value, int max) { return std::to_string(percent(value, max)) + "%"; }

ui::StrId lang_name(const std::string& code) {
    if (code == "en") return ui::StrId::LangFullEn;
    if (code == "ja") return ui::StrId::LangFullJa;
    return ui::StrId::LangFullZh;
}

std::string time_zone_text(int offset) {
    for (const TimeZoneChoice& z : SettingsPage::time_zones()) {
        if (z.utc_offset_min == offset)
            return ui::tr_fill_n(ui::StrId::TzFormat, ui::lang(),
                                 {SettingsPage::utc_label(offset), ui::tr(z.city)});
    }
    return SettingsPage::utc_label(offset);
}

// `base` with `current` put in its place when the list lacks it: before
// "never" (0, last), in order among the rest.
std::vector<int> with_current(const std::array<int, 6>& base, int current) {
    std::vector<int> out(base.begin(), base.end());
    if (std::find(out.begin(), out.end(), current) != out.end()) return out;
    auto at = std::find_if(out.begin(), out.end(), [&](int s) { return s == 0 || s > current; });
    out.insert(at, current);
    return out;
}

int index_of(const std::vector<int>& list, int v) {
    auto it = std::find(list.begin(), list.end(), v);
    return it == list.end() ? -1 : static_cast<int>(it - list.begin());
}

}  // namespace

const std::vector<TimeZoneChoice>& SettingsPage::time_zones() {
    using ui::StrId;
    static const std::vector<TimeZoneChoice> zones = {
        {-720, StrId::TzWest1200}, {-660, StrId::TzWest1100}, {-600, StrId::TzWest1000},
        {-570, StrId::TzWest0930}, {-540, StrId::TzWest0900}, {-480, StrId::TzWest0800},
        {-420, StrId::TzWest0700}, {-360, StrId::TzWest0600}, {-300, StrId::TzWest0500},
        {-240, StrId::TzWest0400}, {-210, StrId::TzWest0330}, {-180, StrId::TzWest0300},
        {-120, StrId::TzWest0200}, {-60, StrId::TzWest0100},  {0, StrId::TzEast0000},
        {60, StrId::TzEast0100},   {120, StrId::TzEast0200},  {180, StrId::TzEast0300},
        {210, StrId::TzEast0330},  {240, StrId::TzEast0400},  {270, StrId::TzEast0430},
        {300, StrId::TzEast0500},  {330, StrId::TzEast0530},  {345, StrId::TzEast0545},
        {360, StrId::TzEast0600},  {390, StrId::TzEast0630},  {420, StrId::TzEast0700},
        {480, StrId::TzEast0800},  {525, StrId::TzEast0845},  {540, StrId::TzEast0900},
        {570, StrId::TzEast0930},  {600, StrId::TzEast1000},  {630, StrId::TzEast1030},
        {660, StrId::TzEast1100},  {720, StrId::TzEast1200},  {765, StrId::TzEast1245},
        {780, StrId::TzEast1300},  {840, StrId::TzEast1400},
    };
    return zones;
}

std::string SettingsPage::utc_label(int offset) {
    const int a = std::abs(offset);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "UTC%c%02d:%02d", offset < 0 ? '-' : '+', a / 60 % 100, a % 60);
    return buf;
}

std::string SettingsPage::duration_text(int seconds) {
    if (seconds == 0) return ui::tr(ui::StrId::SettingsNever);
    if (seconds < 60) return ui::tr_fill(ui::StrId::SettingsSeconds, std::to_string(seconds));
    if (seconds % 3600 == 0)
        return ui::tr_fill(ui::StrId::SettingsHours, std::to_string(seconds / 3600));
    return ui::tr_fill(ui::StrId::SettingsMinutes, std::to_string(seconds / 60));
}

SettingsPage::SettingsPage(SettingsDeps deps) : deps_(std::move(deps)) {
    if (deps_.settings) {
        now_ = deps_.settings();
    } else {
        now_.brightness = 200;
        now_.volume = 60;
        now_.notify_vol = 60;
        now_.screen_off_s = static_cast<int>(core::kDefaultScreenOffS);
        now_.deep_after_s = static_cast<int>(core::kDefaultDeepAfterS);
        now_.light_sleep = core::kDefaultLightSleep;
        now_.language = "zh";
        now_.utc_offset_min = 480;
        now_.time_24h = true;
    }
}

SettingsPage::~SettingsPage() {
    if (timer_ != nullptr) lv_timer_delete(timer_);
    if (confirm_ != nullptr && deps_.layer != nullptr) deps_.layer->close_confirm(confirm_);
}

lv_obj_t* SettingsPage::add_group(ui::StrId heading) {
    headings_.push_back(detail::make_group_heading(list_, ui::tr(heading)));
    cards_.push_back(detail::make_row_card(list_));
    return cards_.back();
}

lv_obj_t* SettingsPage::add_slider(lv_obj_t* card, ui::StrId name, const char* low,
                                   const char* high, int min, int max, int value,
                                   lv_obj_t** value_label) {
    lv_obj_t* box = detail::make_row_box(card);
    if (lv_obj_get_index(box) > 0) detail::add_divider(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* head = detail::make_box(box);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, ui::theme::kGapSm, 0);
    lv_obj_t* l = lv_label_create(head);
    lv_label_set_text(l, ui::tr(name));
    *value_label = detail::add_row_value(head, "", false);

    lv_obj_t* line = detail::make_box(box);
    lv_obj_set_height(line, detail::kMinTouchPx);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(line, ui::theme::kGapMd, 0);
    lv_obj_t* lo = lv_label_create(line);
    lv_label_set_text(lo, low);
    lv_obj_set_width(lo, kEndIconPx);
    lv_obj_set_style_text_align(lo, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lo, detail::dim_colour(card), 0);
    lv_obj_t* s = lv_slider_create(line);
    lv_obj_set_height(s, kSliderPx);
    lv_obj_set_flex_grow(s, 1);
    lv_slider_set_range(s, min, max);
    const int v = std::clamp(value, min, max);
    lv_slider_set_value(s, v, LV_ANIM_OFF);
    lv_obj_set_ext_click_area(s, kSliderReachPx);
    // Dragging it sideways is the slider's, not a page drag.
    lv_obj_add_flag(s, detail::kNoSwipe);
    lv_obj_add_event_cb(s, &SettingsPage::event_cb, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(s, &SettingsPage::event_cb, LV_EVENT_RELEASED, this);
    lv_obj_t* hi = lv_label_create(line);
    lv_label_set_text(hi, high);
    lv_obj_set_width(hi, kEndIconPx);
    lv_obj_set_style_text_align(hi, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(*value_label, percent_text(v, max).c_str());
    return s;
}

lv_obj_t* SettingsPage::add_switch(lv_obj_t* card, const char* icon, ui::StrId name, bool on) {
    detail::ListRow r = detail::make_list_row(card, icon, ui::tr(name), false);
    lv_obj_set_flex_grow(r.name, 1);
    lv_label_set_long_mode(r.name, LV_LABEL_LONG_WRAP);
    lv_obj_t* sw = lv_switch_create(r.row);
    lv_obj_set_size(sw, kSwitchW, kSwitchH);
    detail::fit_row(r.row, kSwitchH);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_flag(sw, detail::kNoSwipe);
    lv_obj_add_event_cb(sw, &SettingsPage::event_cb, LV_EVENT_VALUE_CHANGED, this);
    return sw;
}

void SettingsPage::add_value_row(lv_obj_t* card, Row which, const char* icon, ui::StrId name,
                                 bool opens) {
    detail::ListRow r = detail::make_list_row(card, icon, ui::tr(name), opens);
    RowParts& parts = rows_[static_cast<size_t>(which)];
    parts.row = r.row;
    if (which == Row::Restart || which == Row::PowerOff) {
        // An action, no page behind it: no chevron.
        lv_obj_set_flex_grow(r.name, 1);
    } else if (which != Row::Diagnostics) {
        parts.value = detail::add_row_value(r.row, "", opens);
    } else {
        // Nothing to show, only somewhere to go.
        lv_obj_set_flex_grow(r.name, 1);
        lv_obj_t* c = lv_label_create(r.row);
        lv_label_set_text(c, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(c, detail::dim_colour(r.row), 0);
    }
    if (opens) lv_obj_add_event_cb(r.row, &SettingsPage::event_cb, LV_EVENT_CLICKED, this);
}

void SettingsPage::build(lv_obj_t* list) {
    list_ = list;

    lv_obj_t* card = add_group(ui::StrId::SettingsGroupDisplay);
    brightness_ = add_slider(card, ui::StrId::SettingsBrightness, WISP_SYMBOL_MOON, WISP_SYMBOL_SUN,
                             10, 255, now_.brightness, &brightness_value_);
    volume_ = add_slider(card, ui::StrId::SettingsVolume, LV_SYMBOL_MUTE, LV_SYMBOL_VOLUME_MAX, 0,
                         100, now_.volume, &volume_value_);
    notify_ = add_slider(card, ui::StrId::SettingsNotifyVol, LV_SYMBOL_MUTE, LV_SYMBOL_BELL, 0, 100,
                         now_.notify_vol, &notify_value_);
    speak_ = add_switch(card, LV_SYMBOL_AUDIO, ui::StrId::SettingsSpeak, !now_.tts_mute);

    card = add_group(ui::StrId::SettingsGroupPower);
    add_value_row(card, Row::ScreenOff, WISP_SYMBOL_MOON, ui::StrId::SettingsScreenOff, true);
    add_value_row(card, Row::DeepStandby, WISP_SYMBOL_HOURGLASS, ui::StrId::SettingsDeepStandby,
                  true);
    light_sleep_ =
        add_switch(card, LV_SYMBOL_CHARGE, ui::StrId::SettingsLightSleep, now_.light_sleep);
    lv_obj_t* hint = detail::make_row_box(card);
    lv_obj_set_style_pad_top(hint, 0, 0);
    detail::make_text(hint, ui::tr(ui::StrId::SettingsLightSleepHint), true);

    card = add_group(ui::StrId::SettingsGroupNetwork);
    add_value_row(card, Row::Wifi, LV_SYMBOL_WIFI, ui::StrId::SettingsWifi, true);

    card = add_group(ui::StrId::SettingsGroupLangTime);
    add_value_row(card, Row::Language, WISP_SYMBOL_GLOBE, ui::StrId::SettingsLanguage, true);
    add_value_row(card, Row::TimeZone, WISP_SYMBOL_MAP_MARKER, ui::StrId::SettingsTimeZone, true);
    add_value_row(card, Row::TimeFormat, WISP_SYMBOL_CLOCK, ui::StrId::SettingsTimeFormat, true);

    card = add_group(ui::StrId::SettingsGroupSystem);
    add_value_row(card, Row::Version, WISP_SYMBOL_INFO, ui::StrId::AboutFirmware, false);
    add_value_row(card, Row::Address, WISP_SYMBOL_LINK, ui::StrId::AboutAddress, false);
    hint = detail::make_row_box(card);
    lv_obj_set_style_pad_top(hint, 0, 0);
    detail::make_text(hint, ui::tr(ui::StrId::AboutPairHint), true);
    add_value_row(card, Row::Diagnostics, LV_SYMBOL_SETTINGS, ui::StrId::AboutDiagnostics, true);
    if (deps_.layer != nullptr && deps_.restart)
        add_value_row(card, Row::Restart, LV_SYMBOL_REFRESH, ui::StrId::SettingsRestart, true);
    if (deps_.layer != nullptr && deps_.power_off)
        add_value_row(card, Row::PowerOff, LV_SYMBOL_POWER, ui::StrId::SettingsPowerOff, true);

    lv_label_set_text(value(Row::Version),
                      deps_.firmware_version ? deps_.firmware_version().c_str() : "");
    show_values();
    detail::bubble_tree(list_);
    refresh();
    timer_ = lv_timer_create(
        [](lv_timer_t* t) { static_cast<SettingsPage*>(lv_timer_get_user_data(t))->refresh(); },
        kRefreshMs, this);
}

void SettingsPage::show_values() {
    lv_label_set_text(value(Row::ScreenOff), duration_text(now_.screen_off_s).c_str());
    lv_label_set_text(value(Row::DeepStandby), duration_text(now_.deep_after_s).c_str());
    lv_label_set_text(value(Row::Language), ui::tr(lang_name(now_.language)));
    lv_label_set_text(value(Row::TimeZone), time_zone_text(now_.utc_offset_min).c_str());
    lv_label_set_text(value(Row::TimeFormat), ui::tr(now_.time_24h ? ui::StrId::SettingsClock24
                                                                   : ui::StrId::SettingsClock12));
}

void SettingsPage::refresh() {
    if (value(Row::Wifi) == nullptr) return;
    std::string wifi = deps_.wifi_status ? deps_.wifi_status() : std::string();
    if (wifi != shown_wifi_) {
        shown_wifi_ = wifi;
        lv_label_set_text(value(Row::Wifi), wifi.c_str());
    }
    const std::string url = deps_.admin_url ? deps_.admin_url() : std::string();
    const std::string address = url.empty() ? ui::tr(ui::StrId::AboutAddressNone) : url;
    if (address != shown_address_) {
        shown_address_ = address;
        lv_label_set_text(value(Row::Address), address.c_str());
    }
}

void SettingsPage::open(Row r) {
    if (!deps_.open_choice) return;
    ChoiceSpec spec;
    switch (r) {
        case Row::ScreenOff:
        case Row::DeepStandby: {
            const bool off = r == Row::ScreenOff;
            const std::vector<int> times =
                with_current(off ? kScreenOffChoices : kDeepStandbyChoices,
                             off ? now_.screen_off_s : now_.deep_after_s);
            spec.title = off ? ui::StrId::SettingsScreenOff : ui::StrId::SettingsDeepStandby;
            for (int s : times) spec.labels.push_back(duration_text(s));
            spec.current = index_of(times, off ? now_.screen_off_s : now_.deep_after_s);
            spec.on_pick = [this, off, times](size_t i) {
                const char* key = off ? "screen_off_s" : "deep_after_s";
                (off ? now_.screen_off_s : now_.deep_after_s) = times[i];
                show_values();
                write({{key, Value(static_cast<int64_t>(times[i]))}});
            };
            break;
        }
        case Row::Language: {
            spec.title = ui::StrId::SettingsLanguage;
            for (size_t i = 0; i < kLanguages.size(); ++i) {
                spec.labels.push_back(ui::tr(lang_name(kLanguages[i])));
                if (now_.language == kLanguages[i]) spec.current = static_cast<int>(i);
            }
            spec.on_pick = [this](size_t i) {
                now_.language = kLanguages[i];
                show_values();
                write({{"language", Value(std::string(kLanguages[i]))}});
            };
            break;
        }
        case Row::TimeZone: {
            std::vector<int> offsets;
            for (const TimeZoneChoice& z : time_zones()) offsets.push_back(z.utc_offset_min);
            if (index_of(offsets, now_.utc_offset_min) < 0) {
                offsets.insert(
                    std::upper_bound(offsets.begin(), offsets.end(), now_.utc_offset_min),
                    now_.utc_offset_min);
            }
            spec.title = ui::StrId::SettingsTimeZone;
            for (int m : offsets) spec.labels.push_back(time_zone_text(m));
            spec.current = index_of(offsets, now_.utc_offset_min);
            spec.on_pick = [this, offsets](size_t i) {
                now_.utc_offset_min = offsets[i];
                show_values();
                write({{"utc_offset_min", Value(static_cast<int64_t>(offsets[i]))}});
            };
            break;
        }
        case Row::TimeFormat:
            spec.title = ui::StrId::SettingsTimeFormat;
            spec.labels = {ui::tr(ui::StrId::SettingsClock24), ui::tr(ui::StrId::SettingsClock12)};
            spec.current = now_.time_24h ? 0 : 1;
            spec.on_pick = [this](size_t i) {
                now_.time_24h = i == 0;
                show_values();
                write({{"time_24h", Value(now_.time_24h)}});
            };
            break;
        default:
            return;
    }
    deps_.open_choice(std::move(spec));
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

void SettingsPage::ask(Row r) {
    if (confirm_ != nullptr || deps_.layer == nullptr) return;
    const bool restart = r == Row::Restart;
    // The layer keeps one dialog at a time: with another question up
    // (a factory reset, a firmware update) this one is not asked.
    confirm_ = deps_.layer->open_confirm(
        restart ? ui::StrId::RestartTitle : ui::StrId::PowerOffTitle,
        ui::tr(restart ? ui::StrId::RestartPrompt : ui::StrId::PowerOffPrompt),
        [this, restart](bool yes) {
            confirm_ = nullptr;
            if (!yes) return;
            WLOG_I(kTag, "settings: %s", restart ? "restart" : "power off");
            if (restart && deps_.restart) deps_.restart();
            if (!restart && deps_.power_off) deps_.power_off();
        });
    if (confirm_ == nullptr) WLOG_I(kTag, "settings: another dialog is open");
}

void SettingsPage::event_cb(lv_event_t* e) {
    auto* self = static_cast<SettingsPage*>(lv_event_get_user_data(e));
    lv_obj_t* obj = lv_event_get_current_target_obj(e);
    const lv_event_code_t code = lv_event_get_code(e);
    struct Slider {
        lv_obj_t* slider;
        lv_obj_t* value;
        int max;
        const char* key;
        int* now;
    };
    const Slider sliders[] = {
        {self->brightness_, self->brightness_value_, 255, "brightness", &self->now_.brightness},
        {self->volume_, self->volume_value_, 100, "volume", &self->now_.volume},
        {self->notify_, self->notify_value_, 100, "notify_vol", &self->now_.notify_vol},
    };
    for (const Slider& s : sliders) {
        if (obj != s.slider) continue;
        const int v = lv_slider_get_value(obj);
        lv_label_set_text(s.value, percent_text(v, s.max).c_str());
        if (code == LV_EVENT_RELEASED) {
            *s.now = v;
            self->write({{s.key, Value(static_cast<int64_t>(v))}});
        }
        return;
    }
    if (obj == self->speak_) {
        self->now_.tts_mute = !lv_obj_has_state(obj, LV_STATE_CHECKED);
        self->write({{"tts_mute", Value(self->now_.tts_mute)}});
    } else if (obj == self->light_sleep_) {
        self->now_.light_sleep = lv_obj_has_state(obj, LV_STATE_CHECKED);
        self->write({{"light_sleep", Value(self->now_.light_sleep)}});
    } else if (obj == self->row(Row::Wifi)) {
        if (self->deps_.start_wifi_setup) self->deps_.start_wifi_setup();
    } else if (obj == self->row(Row::Diagnostics)) {
        if (self->deps_.open_diagnostics) self->deps_.open_diagnostics();
    } else if (obj == self->row(Row::Restart) || obj == self->row(Row::PowerOff)) {
        self->ask(obj == self->row(Row::Restart) ? Row::Restart : Row::PowerOff);
    } else {
        for (Row r :
             {Row::ScreenOff, Row::DeepStandby, Row::Language, Row::TimeZone, Row::TimeFormat}) {
            if (obj == self->row(r)) self->open(r);
        }
    }
}

}  // namespace wisp::shell
