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
// page follows the settings cell (SettingsDeps::settings) from build() until
// it is destroyed: each delivery replaces what the page takes the settings
// to be and redraws the controls whose value moved, so a change made by
// voice or on the admin page shows at once. What the page writes shows at
// once too, and that setting keeps the page's value while its write is in
// flight (a delivery of an earlier write, a whole snapshot, would otherwise
// flip it back for a moment). Once the write has run (stored, refused or
// failed) the job posts its completion (SettingsDeps::post) and the page
// takes the cell's value again, so a write that did not store puts its row
// back; a write the storage queue does not take puts it back at once. A
// completion whose post is lost is picked up by the kRefreshMs timer.
// Two things a delivery leaves alone: a slider the finger holds (its
// release writes the finger's value) and an open choice page (built from
// the value when it opened; a pick writes the pick). Without a cell the
// page shows the table's defaults, follows nothing and keeps what it wrote.
// The Wi-Fi status and the address are read again every kRefreshMs. A
// language change rebuilds the page (the shell follows ui::lang()).
//
// A confirm dialog still open goes with the page (closed unanswered), so
// its answer never reaches a page that is gone.
//
// Threading: everything on the UI thread, the destructor included.

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "lvgl.h"
#include "wisp/base/value.hpp"
#include "wisp/core/dispatcher.hpp"
#include "wisp/core/model_profiles.hpp"
#include "wisp/core/settings_schema.hpp"
#include "wisp/core/state_cell.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/shell/choice_page.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::ui {
class SystemLayer;
}

namespace wisp::shell {

struct SettingsDeps {
    // SystemSettings::cell(); it must outlive the page. Null: the table's
    // defaults, nothing followed.
    const core::StateCell<core::SettingsSnapshot>* settings = nullptr;
    // Where the cell's deliveries run: the UI thread (the shell passes its
    // UiRuntime). Null: nothing followed.
    core::Dispatcher* dispatcher = nullptr;
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
    // Runs a function on the UI thread soon (UiRuntime::post()); false if
    // it was not taken. A write's completion comes back through it. Empty:
    // completions wait for the refresh timer.
    std::function<bool(std::function<void()>)> post;
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
    // Releases the cell's subscription, deletes the refresh timer; the
    // controls go with their screen.
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
    // Puts now_ on every setting control (with `before`: on those whose
    // value differs from it); a slider the finger holds keeps the finger's
    // value.
    void show_values(const core::SettingsSnapshot* before = nullptr);
    // A delivery of the settings cell.
    void follow(const core::SettingsSnapshot& v);
    // Takes the cell's value again (follow() of a fresh get()) and marks
    // the finished writes as seen. No cell: nothing.
    void resync();
    // `v` with the settings whose writes are in flight at now_'s values.
    core::SettingsSnapshot merged(core::SettingsSnapshot v) const;
    bool in_flight(core::SettingId id) const;
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
    // Writes handed to the storage worker, per setting (UI thread), and
    // what the worker's jobs share with the page: how many of them have
    // run, per setting, and the page while it lives (UI thread only: the
    // destructor clears it, the posted completion reads it).
    struct Writes {
        std::array<std::atomic<uint16_t>, static_cast<size_t>(core::SettingId::Count)> done{};
        std::atomic<bool> finished{false};
        SettingsPage* page = nullptr;
    };
    std::array<uint16_t, static_cast<size_t>(core::SettingId::Count)> issued_{};
    std::shared_ptr<Writes> writes_ = std::make_shared<Writes>();
    core::CellSubscription settings_sub_;
};

}  // namespace wisp::shell
