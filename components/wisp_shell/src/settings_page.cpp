#include "wisp/shell/settings_page.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "page_frame.hpp"
#include "wisp/core/log.hpp"
#include "wisp/ui/setting_labels.hpp"
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

using core::SettingId;
using core::SettingSpec;

constexpr SettingRow kSettingRows[] = {
    {SettingGroup::DisplaySound, SettingId::Brightness, SettingWidget::Slider},
    {SettingGroup::DisplaySound, SettingId::Volume, SettingWidget::Slider},
    {SettingGroup::DisplaySound, SettingId::NotifyVol, SettingWidget::Slider},
    {SettingGroup::DisplaySound, SettingId::TtsMute, SettingWidget::InvertedSwitch},
    {SettingGroup::ScreenPower, SettingId::ScreenOffS, SettingWidget::Choice},
    {SettingGroup::ScreenPower, SettingId::DeepAfterS, SettingWidget::Choice},
    {SettingGroup::LangTime, SettingId::Language, SettingWidget::Choice},
    {SettingGroup::LangTime, SettingId::UtcOffsetMin, SettingWidget::Choice},
    {SettingGroup::LangTime, SettingId::Time24h, SettingWidget::Choice},
};

// The icons beside a setting: a row's leading icon, or a slider's two end
// icons (low, high).
struct SettingIcons {
    const char* icon = nullptr;
    const char* high = nullptr;
};

SettingIcons icons_of(SettingId id) {
    switch (id) {
        case SettingId::Brightness:
            return {WISP_SYMBOL_MOON, WISP_SYMBOL_SUN};
        case SettingId::Volume:
            return {LV_SYMBOL_MUTE, LV_SYMBOL_VOLUME_MAX};
        case SettingId::NotifyVol:
            return {LV_SYMBOL_MUTE, LV_SYMBOL_BELL};
        case SettingId::TtsMute:
            return {LV_SYMBOL_AUDIO, nullptr};
        case SettingId::ScreenOffS:
            return {WISP_SYMBOL_MOON, nullptr};
        case SettingId::DeepAfterS:
            return {WISP_SYMBOL_HOURGLASS, nullptr};
        case SettingId::Language:
            return {WISP_SYMBOL_GLOBE, nullptr};
        case SettingId::UtcOffsetMin:
            return {WISP_SYMBOL_MAP_MARKER, nullptr};
        case SettingId::Time24h:
            return {WISP_SYMBOL_CLOCK, nullptr};
        default:
            return {};
    }
}

// The Row a choice setting's value row is known by (for tests and the
// simulator's checks); Row::Count for a setting with no value row.
constexpr SettingsPage::Row row_of(SettingId id) {
    using Row = SettingsPage::Row;
    switch (id) {
        case SettingId::ScreenOffS:
            return Row::ScreenOff;
        case SettingId::DeepAfterS:
            return Row::DeepStandby;
        case SettingId::Language:
            return Row::Language;
        case SettingId::UtcOffsetMin:
            return Row::TimeZone;
        case SettingId::Time24h:
            return Row::TimeFormat;
        default:
            return Row::Count;
    }
}

// A choice row's options and their text come from the row's int_choices,
// except for the two settings shown as words: the language (kLanguageOrder)
// and the time format (24 h / 12 h).
constexpr bool worded_choice(SettingId id) {
    return id == SettingId::Language || id == SettingId::Time24h;
}

// Every Choice row has its own value row (add_value_row indexes rows_ by
// it), and is either worded or an Int row listing int_choices.
consteval bool choice_rows_mapped() {
    for (size_t i = 0; i < std::size(kSettingRows); ++i) {
        const SettingRow& row = kSettingRows[i];
        if (row.widget != SettingWidget::Choice) continue;
        if (row_of(row.id) == SettingsPage::Row::Count) return false;
        if (!worded_choice(row.id) && core::setting_spec(row.id).type != core::SettingType::Int)
            return false;
        for (size_t j = i + 1; j < std::size(kSettingRows); ++j) {
            if (kSettingRows[j].widget == SettingWidget::Choice &&
                row_of(kSettingRows[j].id) == row_of(row.id))
                return false;
        }
    }
    return true;
}
static_assert(choice_rows_mapped(), "a Choice row has no value row of its own or no options");

// Every shown setting has a label (SettingLabels.EveryDeviceShownKeyHasALabel).
ui::StrId label_of(SettingId id) {
    return ui::setting_label(core::setting_spec(id).label).value_or(ui::StrId::SettingsBrightness);
}

std::string percent_text(int value, const SettingSpec& spec) {
    return std::to_string(SettingsPage::percent(value, spec)) + "%";
}

ui::StrId lang_name(core::LangCode code) {
    switch (code) {
        case core::LangCode::En:
            return ui::StrId::LangFullEn;
        case core::LangCode::Ja:
            return ui::StrId::LangFullJa;
        case core::LangCode::Zh:
            break;
    }
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

// An Int row's choices as the page lists them, ascending, with "never"
// (0 on a kSettingZeroNever row) moved to the end, and `current` put in its
// place when the list lacks it: before "never", in order among the rest.
std::vector<int> int_options(const SettingSpec& spec, int current) {
    const bool zero_never = (spec.flags & core::kSettingZeroNever) != 0;
    auto never = [&](int v) { return zero_never && v == 0; };
    std::vector<int> out;
    for (int32_t v : spec.int_choices) {
        if (!never(v)) out.push_back(static_cast<int>(v));
    }
    for (int32_t v : spec.int_choices) {
        if (never(v)) out.push_back(0);
    }
    if (std::find(out.begin(), out.end(), current) != out.end()) return out;
    auto at = std::find_if(out.begin(), out.end(), [&](int v) { return never(v) || v > current; });
    out.insert(at, current);
    return out;
}

// A choice row's value as its row and choice page read.
std::string option_text(SettingId id, const Value& v) {
    if (id == SettingId::Language)
        return ui::tr(lang_name(
            core::parse_lang_code(v.as_string()).value_or(core::setting_init<core::LangCode>(id))));
    if (id == SettingId::Time24h)
        return ui::tr(v.as_bool() ? ui::StrId::SettingsClock24 : ui::StrId::SettingsClock12);
    const SettingSpec& spec = core::setting_spec(id);
    const int n = static_cast<int>(v.as_int());
    if (spec.unit == core::SettingUnit::Seconds) return SettingsPage::duration_text(n);
    if (spec.unit == core::SettingUnit::UtcOffsetMinutes) return time_zone_text(n);
    return std::to_string(n);
}

// The choices of a choice row, as the values written.
std::vector<Value> options_of(SettingId id, const core::SettingsSnapshot& now) {
    std::vector<Value> out;
    if (id == SettingId::Language) {
        for (core::LangCode code : SettingsPage::kLanguageOrder)
            out.emplace_back(std::string(core::lang_code(code)));
    } else if (id == SettingId::Time24h) {
        out = {Value(true), Value(false)};
    } else {
        const int current = static_cast<int>(core::setting_value(now, id).as_int());
        for (int v : int_options(core::setting_spec(id), current))
            out.emplace_back(static_cast<int64_t>(v));
    }
    return out;
}

}  // namespace

std::span<const SettingRow> SettingsPage::setting_rows() { return kSettingRows; }

int SettingsPage::percent(int value, const core::SettingSpec& spec) {
    const int range = spec.max - spec.min;
    if (range <= 0) return 0;
    return ((value - spec.min) * 100 + range / 2) / range;
}

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
    if (deps_.settings != nullptr) now_ = deps_.settings->get();
    writes_->page = this;
}

SettingsPage::~SettingsPage() {
    // On the UI thread, the subscription's dispatcher: no delivery is under
    // way, and none comes after this.
    settings_sub_.reset();
    // A completion still queued finds no page.
    writes_->page = nullptr;
    if (timer_ != nullptr) lv_timer_delete(timer_);
    if (confirm_ != nullptr && deps_.layer != nullptr) deps_.layer->close_confirm(confirm_);
}

lv_obj_t* SettingsPage::add_group(ui::StrId heading) {
    headings_.push_back(detail::make_group_heading(list_, ui::tr(heading)));
    cards_.push_back(detail::make_row_card(list_));
    return cards_.back();
}

void SettingsPage::add_slider(lv_obj_t* card, SettingId id) {
    const SettingSpec& spec = core::setting_spec(id);
    const SettingIcons icons = icons_of(id);
    SettingParts& parts = settings_[static_cast<size_t>(id)];
    lv_obj_t* box = detail::make_row_box(card);
    if (lv_obj_get_index(box) > 0) detail::add_divider(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* head = detail::make_box(box);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, ui::theme::kGapSm, 0);
    lv_obj_t* l = lv_label_create(head);
    lv_label_set_text(l, ui::tr(label_of(id)));
    parts.value = detail::add_row_value(head, "", false);

    lv_obj_t* line = detail::make_box(box);
    lv_obj_set_height(line, detail::kMinTouchPx);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(line, ui::theme::kGapMd, 0);
    lv_obj_t* lo = lv_label_create(line);
    lv_label_set_text(lo, icons.icon);
    lv_obj_set_width(lo, kEndIconPx);
    lv_obj_set_style_text_align(lo, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lo, detail::dim_colour(card), 0);
    lv_obj_t* s = lv_slider_create(line);
    lv_obj_set_height(s, kSliderPx);
    lv_obj_set_flex_grow(s, 1);
    lv_slider_set_range(s, spec.min, spec.max);
    lv_obj_set_ext_click_area(s, kSliderReachPx);
    // Dragging it sideways is the slider's, not a page drag.
    lv_obj_add_flag(s, detail::kNoSwipe);
    lv_obj_add_event_cb(s, &SettingsPage::event_cb, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(s, &SettingsPage::event_cb, LV_EVENT_RELEASED, this);
    lv_obj_t* hi = lv_label_create(line);
    lv_label_set_text(hi, icons.high);
    lv_obj_set_width(hi, kEndIconPx);
    lv_obj_set_style_text_align(hi, LV_TEXT_ALIGN_CENTER, 0);
    parts.control = s;
}

void SettingsPage::add_switch(lv_obj_t* card, const SettingRow& row) {
    detail::ListRow r =
        detail::make_list_row(card, icons_of(row.id).icon, ui::tr(label_of(row.id)), false);
    lv_obj_set_flex_grow(r.name, 1);
    lv_label_set_long_mode(r.name, LV_LABEL_LONG_WRAP);
    lv_obj_t* sw = lv_switch_create(r.row);
    lv_obj_set_size(sw, kSwitchW, kSwitchH);
    detail::fit_row(r.row, kSwitchH);
    lv_obj_add_flag(sw, detail::kNoSwipe);
    lv_obj_add_event_cb(sw, &SettingsPage::event_cb, LV_EVENT_VALUE_CHANGED, this);
    settings_[static_cast<size_t>(row.id)].control = sw;
}

void SettingsPage::add_setting_rows(lv_obj_t* card, SettingGroup group) {
    for (const SettingRow& row : setting_rows()) {
        if (row.group != group) continue;
        switch (row.widget) {
            case SettingWidget::Slider:
                add_slider(card, row.id);
                break;
            case SettingWidget::Switch:
            case SettingWidget::InvertedSwitch:
                add_switch(card, row);
                break;
            case SettingWidget::Choice: {
                const Row r = row_of(row.id);
                add_value_row(card, r, icons_of(row.id).icon, label_of(row.id), true);
                settings_[static_cast<size_t>(row.id)] = {this->row(r), value(r)};
                break;
            }
        }
    }
}

void SettingsPage::add_value_row(lv_obj_t* card, Row which, const char* icon, ui::StrId name,
                                 bool opens) {
    detail::ListRow r = detail::make_list_row(card, icon, ui::tr(name), opens);
    RowParts& parts = rows_[static_cast<size_t>(which)];
    parts.row = r.row;
    if (which == Row::Restart || which == Row::PowerOff) {
        // An action, no page behind it: no chevron.
        lv_obj_set_flex_grow(r.name, 1);
    } else if (which == Row::Address) {
        // What the user types into a browser: never cut off. The address
        // gets its own line under the name and wraps if even that is too
        // narrow (a round screen's rows near the bottom are).
        lv_obj_set_flex_grow(r.name, 1);
        lv_obj_t* box = detail::make_row_box(card);
        lv_obj_set_style_pad_top(box, 0, 0);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box, ui::theme::kGapXs, 0);
        parts.value = detail::make_text(box, "");
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
    add_setting_rows(card, SettingGroup::DisplaySound);

    card = add_group(ui::StrId::SettingsGroupPower);
    add_setting_rows(card, SettingGroup::ScreenPower);
    card = add_group(ui::StrId::SettingsGroupNetwork);
    add_value_row(card, Row::Wifi, LV_SYMBOL_WIFI, ui::StrId::SettingsWifi, true);

    card = add_group(ui::StrId::SettingsGroupModels);
    add_value_row(card, Row::ModelLlm, nullptr, ui::StrId::ModelLlm, true);
    add_value_row(card, Row::ModelAsr, nullptr, ui::StrId::ModelAsr, true);
    add_value_row(card, Row::ModelTts, nullptr, ui::StrId::ModelTts, true);

    card = add_group(ui::StrId::SettingsGroupLangTime);
    add_setting_rows(card, SettingGroup::LangTime);

    card = add_group(ui::StrId::SettingsGroupSystem);
    add_value_row(card, Row::Version, WISP_SYMBOL_INFO, ui::StrId::AboutFirmware, false);
    add_value_row(card, Row::Address, WISP_SYMBOL_LINK, ui::StrId::AboutAddress, false);
    detail::make_text(lv_obj_get_parent(value(Row::Address)), ui::tr(ui::StrId::AboutPairHint),
                      true);
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
    // On the UI dispatcher only: the callback redraws controls. Its first
    // call brings the value as it is then, which the page may already show.
    if (deps_.settings != nullptr && deps_.dispatcher != nullptr) {
        settings_sub_ = deps_.settings->subscribe(
            [this](const core::SettingsSnapshot& v) { follow(v); }, deps_.dispatcher);
    }
}

namespace {

// Copies the field of `id` from `from` into `to`; true if it changed. Field
// by field, so following the cell needs no snapshot copy on the UI stack
// (the UI task has the smallest stack margin) and no allocation (the
// HostList text stays a FixedString).
bool take_setting(core::SettingsSnapshot& to, const core::SettingsSnapshot& from, SettingId id) {
    switch (id) {
#define WISP_SETTING_TAKE(Id, name, ...)        \
    case SettingId::Id:                         \
        if (to.name == from.name) return false; \
        to.name = from.name;                    \
        return true;
        WISP_SETTINGS(WISP_SETTING_TAKE)
#undef WISP_SETTING_TAKE
        case SettingId::Count:
            break;
    }
    return false;
}

}  // namespace

void SettingsPage::follow(const core::SettingsSnapshot& v) {
    // A setting whose write is in flight keeps the page's value until the
    // write finishes (resync()).
    uint32_t changed = 0;
    for (size_t i = 0; i < static_cast<size_t>(SettingId::Count); ++i) {
        const auto id = static_cast<SettingId>(i);
        if (!in_flight(id) && take_setting(now_, v, id)) changed |= uint32_t{1} << i;
    }
    if (changed != 0) show_values(changed);
}

bool SettingsPage::in_flight(SettingId id) const {
    const size_t i = static_cast<size_t>(id);
    return issued_[i] != writes_->done[i].load(std::memory_order_acquire);
}

void SettingsPage::resync() {
    writes_->finished.store(false);
    if (deps_.settings != nullptr) follow(deps_.settings->get());
}

void SettingsPage::show_values(uint32_t changed) {
    for (const SettingRow& row : setting_rows()) {
        if (((changed >> static_cast<unsigned>(row.id)) & 1) == 0) continue;
        lv_obj_t* control = this->control(row.id);
        const Value v = core::setting_value(now_, row.id);
        switch (row.widget) {
            case SettingWidget::Slider: {
                // Held: the value and its label follow the finger, and the
                // release writes it.
                if (lv_obj_has_state(control, LV_STATE_PRESSED)) break;
                const SettingSpec& spec = core::setting_spec(row.id);
                const int n = std::clamp(static_cast<int>(v.as_int()), static_cast<int>(spec.min),
                                         static_cast<int>(spec.max));
                lv_slider_set_value(control, n, LV_ANIM_OFF);
                lv_label_set_text(shown(row.id), percent_text(n, spec).c_str());
                break;
            }
            case SettingWidget::Switch:
            case SettingWidget::InvertedSwitch: {
                const bool on =
                    row.widget == SettingWidget::InvertedSwitch ? !v.as_bool() : v.as_bool();
                lv_obj_set_state(control, LV_STATE_CHECKED, on);
                break;
            }
            case SettingWidget::Choice:
                lv_label_set_text(shown(row.id), option_text(row.id, v).c_str());
                break;
        }
    }
}

void SettingsPage::refresh() {
    // A write whose completion post was lost (or that had no post).
    if (writes_->finished.load()) resync();
    for (auto kind : core::kModelKinds) {
        Row r = kind == core::ModelKind::Llm   ? Row::ModelLlm
                : kind == core::ModelKind::Asr ? Row::ModelAsr
                                               : Row::ModelTts;
        if (value(r)) {
            auto name = deps_.model_name ? deps_.model_name(kind) : std::string();
            lv_label_set_text(value(r), name.empty() ? ui::tr(ui::StrId::ModelNone) : name.c_str());
        }
    }
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

void SettingsPage::open(SettingId id) {
    if (!deps_.open_choice) return;
    const std::vector<Value> options = options_of(id, now_);
    const Value current = core::setting_value(now_, id);
    ChoiceSpec spec;
    spec.title = label_of(id);
    for (size_t i = 0; i < options.size(); ++i) {
        spec.labels.push_back(option_text(id, options[i]));
        if (options[i] == current) spec.current = static_cast<int>(i);
    }
    spec.on_pick = [this, id, options](size_t i) {
        if (!core::assign_setting(now_, id, options[i])) return;
        show_values();
        write({{std::string(core::setting_spec(id).name), options[i]}});
    };
    deps_.open_choice(std::move(spec));
}

void SettingsPage::write(std::map<std::string, Value> changes) {
    if (!deps_.set_settings) return;
    if (!deps_.submit_io) {
        // Stored (or refused) by the time it returns.
        deps_.set_settings(std::move(changes));
        resync();
        return;
    }
    uint32_t ids = 0;
    static_assert(static_cast<size_t>(SettingId::Count) <= 32, "ids is a 32-bit mask");
    for (const auto& [key, value] : changes) {
        if (const SettingSpec* spec = core::find_setting(key)) {
            const SettingId id = core::setting_id_of(*spec);
            ids |= uint32_t{1} << static_cast<unsigned>(id);
            ++issued_[static_cast<size_t>(id)];
        }
    }
    auto done = [ids](Writes& w) {
        for (size_t i = 0; i < w.done.size(); ++i) {
            if ((ids >> i) & 1) w.done[i].fetch_add(1, std::memory_order_release);
        }
    };
    const bool queued = deps_.submit_io([set = deps_.set_settings, changes = std::move(changes),
                                         writes = writes_, post = deps_.post, done]() mutable {
        set(std::move(changes));
        done(*writes);
        writes->finished.store(true);
        // Back on the UI thread; if this post is lost the refresh timer
        // sees `finished`.
        if (post) {
            (void)post([writes] {
                if (writes->page != nullptr) writes->page->resync();
            });
        }
    });
    if (!queued) {
        WLOG_W(kTag, "setting not saved (storage queue full)");
        done(*writes_);
        resync();
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
    for (const SettingRow& row : setting_rows()) {
        if (obj != self->control(row.id)) continue;
        const std::string key(core::setting_spec(row.id).name);
        switch (row.widget) {
            case SettingWidget::Slider: {
                const int v = lv_slider_get_value(obj);
                lv_label_set_text(self->shown(row.id),
                                  percent_text(v, core::setting_spec(row.id)).c_str());
                if (code == LV_EVENT_RELEASED && core::assign_setting(self->now_, row.id, Value(v)))
                    self->write({{key, Value(static_cast<int64_t>(v))}});
                break;
            }
            case SettingWidget::Switch:
            case SettingWidget::InvertedSwitch: {
                const bool checked = lv_obj_has_state(obj, LV_STATE_CHECKED);
                const bool v = row.widget == SettingWidget::InvertedSwitch ? !checked : checked;
                if (core::assign_setting(self->now_, row.id, Value(v)))
                    self->write({{key, Value(v)}});
                break;
            }
            case SettingWidget::Choice:
                self->open(row.id);
                break;
        }
        return;
    }
    if (obj == self->row(Row::Wifi)) {
        if (self->deps_.start_wifi_setup) self->deps_.start_wifi_setup();
    } else if (obj == self->row(Row::ModelLlm) || obj == self->row(Row::ModelAsr) ||
               obj == self->row(Row::ModelTts)) {
        if (self->deps_.open_model)
            self->deps_.open_model(obj == self->row(Row::ModelLlm)   ? core::ModelKind::Llm
                                   : obj == self->row(Row::ModelAsr) ? core::ModelKind::Asr
                                                                     : core::ModelKind::Tts);
    } else if (obj == self->row(Row::Diagnostics)) {
        if (self->deps_.open_diagnostics) self->deps_.open_diagnostics();
    } else if (obj == self->row(Row::Restart) || obj == self->row(Row::PowerOff)) {
        self->ask(obj == self->row(Row::Restart) ? Row::Restart : Row::PowerOff);
    }
}

}  // namespace wisp::shell
