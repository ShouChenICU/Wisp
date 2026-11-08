#pragma once

// The settings page (MVP spec 6.4): a grouped list, each control a thin
// front for a service (AGENTS.md section 3). The groups, in order:
//   - display and sound: brightness (10-255), volume and alert volume
//     (notify_vol, 0-100) as sliders, written once the finger lets go;
//     spoken replies (a switch, on while tts_mute is off);
//   - screen and power: when the screen goes off (kScreenOffChoices, "never"
//     being 0) and when deep standby starts (kDeepStandbyChoices), each a
//     value row opening a choice page; power-saving sleep (light_sleep) as
//     a switch with a line saying what it does;
//   - network: Wi-Fi, its status; a tap starts the device's own
//     provisioning (WifiService::start_provisioning(); the card itself is
//     the system duties');
//   - language and time: the system language, the time zone
//     (kTimeZones) and the time format (time_24h), each a value row opening
//     a choice page;
//   - system: the firmware version, the admin page's address (with the
//     IP), how to pair (AboutPairHint) and "hardware diagnostics", which
//     opens the self-check screen.
// A value row's choice page is the shell's (SettingsDeps::open_choice,
// wisp/shell/choice_page.hpp): a pick writes the setting and the row
// shows it at once; going back without one writes nothing. A setting
// stored with a value the list lacks (set on the admin page) is listed
// too, in its place.
//
// Every write goes through SettingsDeps::set_settings inside a
// SettingsDeps::submit_io job: SystemSettings::set() reaches flash. The
// values shown at build come from SettingsDeps::settings (a cache), and
// the page keeps what it wrote since. The Wi-Fi status and the address
// are read again every kRefreshMs. A language change rebuilds the page
// (the shell follows ui::lang()).
//
// Threading: everything on the UI thread, the destructor included.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "lvgl.h"
#include "wisp/base/value.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/shell/choice_page.hpp"
#include "wisp/ui/strings.hpp"

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
    // The storage worker; empty: set_settings runs here.
    std::function<bool(std::function<void()>)> submit_io;
};

struct TimeZoneChoice {
    int utc_offset_min;
    ui::StrId city;
};

class SettingsPage {
public:
    static constexpr std::array<int, 6> kScreenOffChoices = {15, 30, 60, 300, 600, 0};
    static constexpr std::array<int, 6> kDeepStandbyChoices = {60, 300, 600, 1800, 3600, 0};
    // The language codes of the language choices, in their order.
    static constexpr std::array<const char*, 3> kLanguages = {"zh", "en", "ja"};
    // UTC-12:00 to UTC+14:00 as the world uses them (the admin page's
    // list), each with a city in it.
    static const std::vector<TimeZoneChoice>& time_zones();
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
        Count,
    };

    // For tests and the simulator's checks.
    lv_obj_t* brightness() const { return brightness_; }
    lv_obj_t* volume() const { return volume_; }
    lv_obj_t* notify_volume() const { return notify_; }
    // The sliders' "60%".
    lv_obj_t* brightness_value() const { return brightness_value_; }
    lv_obj_t* volume_value() const { return volume_value_; }
    lv_obj_t* notify_value() const { return notify_value_; }
    lv_obj_t* speak() const { return speak_; }
    lv_obj_t* light_sleep() const { return light_sleep_; }
    lv_obj_t* row(Row r) const { return rows_[static_cast<size_t>(r)].row; }
    // nullptr for the diagnostics row, which shows no value.
    lv_obj_t* value(Row r) const { return rows_[static_cast<size_t>(r)].value; }
    // The groups in order: their headings and their cards.
    size_t groups() const { return headings_.size(); }
    lv_obj_t* heading(size_t i) const { return i < headings_.size() ? headings_[i] : nullptr; }
    lv_obj_t* card(size_t i) const { return i < cards_.size() ? cards_[i] : nullptr; }
    lv_obj_t* list() const { return list_; }
    // What the page now takes the settings to be.
    const core::SystemSettingsValues& values() const { return now_; }

private:
    struct RowParts {
        lv_obj_t* row = nullptr;
        lv_obj_t* value = nullptr;
    };
    lv_obj_t* add_group(ui::StrId heading);
    // A slider block: the name and the value on one line, the slider with
    // an icon at each end under them.
    lv_obj_t* add_slider(lv_obj_t* card, ui::StrId name, const char* low, const char* high, int min,
                         int max, int value, lv_obj_t** value_label);
    lv_obj_t* add_switch(lv_obj_t* card, const char* icon, ui::StrId name, bool on);
    void add_value_row(lv_obj_t* card, Row r, const char* icon, ui::StrId name, bool opens);
    void show_values();
    void open(Row r);
    void write(std::map<std::string, Value> changes);
    static void event_cb(lv_event_t* e);

    SettingsDeps deps_;
    core::SystemSettingsValues now_{};
    lv_obj_t* list_ = nullptr;
    lv_obj_t* brightness_ = nullptr;
    lv_obj_t* volume_ = nullptr;
    lv_obj_t* notify_ = nullptr;
    lv_obj_t* brightness_value_ = nullptr;
    lv_obj_t* volume_value_ = nullptr;
    lv_obj_t* notify_value_ = nullptr;
    lv_obj_t* speak_ = nullptr;
    lv_obj_t* light_sleep_ = nullptr;
    std::array<RowParts, static_cast<size_t>(Row::Count)> rows_{};
    std::vector<lv_obj_t*> headings_, cards_;
    lv_timer_t* timer_ = nullptr;
    std::string shown_wifi_, shown_address_;
};

}  // namespace wisp::shell
