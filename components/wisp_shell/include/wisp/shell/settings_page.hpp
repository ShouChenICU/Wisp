#pragma once

// The settings page (MVP spec 6.4): a grouped list, each control a thin
// front for a service (AGENTS.md section 3). The setting rows come from
// setting_rows(), a hand-written layout list of {group, key, widget}
// (settings-schema spec 3.8); everything else about them (range, choices,
// label, the key written, the value's text) comes from the key's row of
// core::kSettingsSchema. The groups, in order:
//   - display and sound: brightness, volume and alert volume as sliders
//     showing percent(), written once the finger lets go; spoken replies
//     (an inverted switch, on while tts_mute is off);
//   - screen and power: when the screen goes off and when deep standby
//     starts, each a value row opening a choice page (the row's
//     int_choices, "never" (0) last);
//   - network: Wi-Fi, its status; a tap starts the device's own
//     provisioning (WifiService::start_provisioning(); the card itself is
//     the system duties');
//   - models: the active model of each kind;
//   - language and time: the system language (kLanguageOrder), the time
//     zone (kUtcOffsetChoices, each with its city from time_zones()) and
//     the time format (time_24h, a choice of 24 h / 12 h), each a value row
//     opening a choice page;
//   - system: the firmware version, the admin page's address (with the
//     IP), how to pair (AboutPairHint), "hardware diagnostics", which
//     opens the self-check screen, then restart and power off, each asking
//     first in the system layer's confirm dialog (a row only when its
//     SettingsDeps callback is set and there is a layer).
// A value row's choice page is the shell's (SettingsDeps::open_choice,
// wisp/shell/choice_page.hpp): a pick writes the setting and the row
// shows it at once; going back without one writes nothing. A setting
// stored with a value the list lacks (set on the admin page) is listed
// too, in its place.
//
// Every write goes through SettingsDeps::set_settings inside a
// SettingsDeps::submit_io job: SystemSettings::set() reaches flash. The
// values shown at build come from SettingsDeps::settings (a cache; without
// one, the table's defaults), and the page keeps what it wrote since. The
// Wi-Fi status and the address are read again every kRefreshMs. A language
// change rebuilds the page (the shell follows ui::lang()).
//
// A confirm dialog still open goes with the page (closed unanswered), so
// its answer never reaches a page that is gone.
//
// Threading: everything on the UI thread, the destructor included.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "lvgl.h"
#include "wisp/base/value.hpp"
#include "wisp/core/model_profiles.hpp"
#include "wisp/core/settings_schema.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/shell/choice_page.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::ui {
class SystemLayer;
}

namespace wisp::shell {

struct SettingsDeps {
    std::function<core::SystemSettingsValues()> settings;
    std::function<void(std::map<std::string, Value>)> set_settings;
    std::function<std::string()> wifi_status, admin_url, firmware_version;
    std::function<void()> start_wifi_setup;
    std::function<void()> open_diagnostics;
    // Shows a choice page over this one. Empty: the value rows open
    // nothing.
    std::function<void(ChoiceSpec)> open_choice;
    // Restarts the device / powers it off, once the user said yes; called
    // on the UI thread. Empty: no such row.
    std::function<void()> restart, power_off;
    // Where the confirm dialogs open; nullptr: no restart or power-off row.
    ui::SystemLayer* layer = nullptr;
    // The storage worker; empty: set_settings runs here.
    std::function<bool(std::function<void()>)> submit_io;
    std::function<void(core::ModelKind)> open_model;
    std::function<std::string(core::ModelKind)> model_name;
};

struct TimeZoneChoice {
    int utc_offset_min = 0;
    ui::StrId city = ui::StrId::TzEast0000;
};

// The groups that hold settings; the others (network, models, system) have
// no settings row.
enum class SettingGroup : uint8_t { DisplaySound, ScreenPower, LangTime };
// How a setting is shown. Not a function of its type: tts_mute is shown
// inverted ("spoken replies"), time_24h as a choice of 24 h / 12 h.
enum class SettingWidget : uint8_t { Slider, Switch, InvertedSwitch, Choice };
struct SettingRow {
    SettingGroup group = SettingGroup::DisplaySound;
    core::SettingId id = core::SettingId::Brightness;
    SettingWidget widget = SettingWidget::Slider;
};

class SettingsPage {
public:
    // The settings rows, in page order (settings-schema spec 3.8's
    // hand-written layout list); a test checks it against the table's
    // kSettingDevicePage keys.
    static std::span<const SettingRow> setting_rows();
    // The languages in their display order; the schema's order is the
    // stored meaning, not a display order.
    static constexpr std::array<core::LangCode, 3> kLanguageOrder = {
        core::LangCode::Zh, core::LangCode::En, core::LangCode::Ja};
    // A city for each of core::kUtcOffsetChoices (UTC-12:00 to UTC+14:00).
    static const std::vector<TimeZoneChoice>& time_zones();
    // A Level or Percent value as the page shows it: (value - min) * 100 /
    // (max - min), rounded to nearest, so the lowest level reads 0 %
    // (settings-schema spec decision 3).
    static int percent(int value, const core::SettingSpec& spec);
    static constexpr uint32_t kRefreshMs = 1000;

    // "UTC+08:00", "UTC-03:30".
    static std::string utc_label(int utc_offset_min);
    // A screen-off or deep standby time as its choice reads: "15 秒",
    // "5 分钟", "1 小时", "从不".
    static std::string duration_text(int seconds);

    explicit SettingsPage(SettingsDeps deps);
    // Deletes the refresh timer; the controls go with their screen.
    ~SettingsPage();

    SettingsPage(const SettingsPage&) = delete;
    SettingsPage& operator=(const SettingsPage&) = delete;

    void build(lv_obj_t* list);
    // Re-reads the Wi-Fi status and the address.
    void refresh();

    // The value rows and their values.
    enum class Row : uint8_t {
        ScreenOff,
        DeepStandby,
        Wifi,
        Language,
        TimeZone,
        TimeFormat,
        Version,
        Address,
        Diagnostics,
        // nullptr rows when SettingsDeps cannot do them.
        Restart,
        PowerOff,
        ModelLlm,
        ModelAsr,
        ModelTts,
        Count,
    };

    // For tests and the simulator's checks.
    lv_obj_t* brightness() const { return control(core::SettingId::Brightness); }
    lv_obj_t* volume() const { return control(core::SettingId::Volume); }
    lv_obj_t* notify_volume() const { return control(core::SettingId::NotifyVol); }
    // The sliders' "60%".
    lv_obj_t* brightness_value() const { return shown(core::SettingId::Brightness); }
    lv_obj_t* volume_value() const { return shown(core::SettingId::Volume); }
    lv_obj_t* notify_value() const { return shown(core::SettingId::NotifyVol); }
    lv_obj_t* speak() const { return control(core::SettingId::TtsMute); }
    lv_obj_t* row(Row r) const { return rows_[static_cast<size_t>(r)].row; }
    // nullptr for the rows that show no value (diagnostics, restart, power
    // off).
    lv_obj_t* value(Row r) const { return rows_[static_cast<size_t>(r)].value; }
    // The groups in order: their headings and their cards.
    size_t groups() const { return headings_.size(); }
    lv_obj_t* heading(size_t i) const { return i < headings_.size() ? headings_[i] : nullptr; }
    lv_obj_t* card(size_t i) const { return i < cards_.size() ? cards_[i] : nullptr; }
    lv_obj_t* list() const { return list_; }
    // The restart or power-off question while it is open, else nullptr.
    lv_obj_t* confirm() const { return confirm_; }
    // What the page now takes the settings to be.
    const core::SettingsSnapshot& values() const { return now_; }

private:
    struct RowParts {
        lv_obj_t* row = nullptr;
        lv_obj_t* value = nullptr;
    };
    // A setting's control (slider, switch or value row) and its value
    // label; nullptr when it has none.
    struct SettingParts {
        lv_obj_t* control = nullptr;
        lv_obj_t* value = nullptr;
    };
    lv_obj_t* control(core::SettingId id) const {
        return settings_[static_cast<size_t>(id)].control;
    }
    lv_obj_t* shown(core::SettingId id) const { return settings_[static_cast<size_t>(id)].value; }
    lv_obj_t* add_group(ui::StrId heading);
    // Adds setting_rows()'s rows of `group` to `card`, in order.
    void add_setting_rows(lv_obj_t* card, SettingGroup group);
    // A slider block: the name and the value on one line, the slider with
    // an icon at each end under them.
    void add_slider(lv_obj_t* card, core::SettingId id);
    void add_switch(lv_obj_t* card, const SettingRow& row);
    void add_value_row(lv_obj_t* card, Row r, const char* icon, ui::StrId name, bool opens);
    void show_values();
    void open(core::SettingId id);
    void ask(Row r);
    void write(std::map<std::string, Value> changes);
    static void event_cb(lv_event_t* e);

    SettingsDeps deps_;
    core::SettingsSnapshot now_{};
    lv_obj_t* list_ = nullptr;
    std::array<SettingParts, static_cast<size_t>(core::SettingId::Count)> settings_{};
    std::array<RowParts, static_cast<size_t>(Row::Count)> rows_{};
    std::vector<lv_obj_t*> headings_, cards_;
    lv_timer_t* timer_ = nullptr;
    lv_obj_t* confirm_ = nullptr;
    std::string shown_wifi_, shown_address_;
};

}  // namespace wisp::shell
