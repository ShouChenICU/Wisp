#pragma once

// The settings page (MVP spec 6.4), each control a thin front for a
// service (AGENTS.md section 3):
//   - brightness (10-255) and volume (0-100) sliders, written once the
//     finger lets go;
//   - when the screen goes off: kScreenOffChoices, "never" being 0;
//   - mute spoken replies (tts_mute);
//   - Wi-Fi: its status; a tap starts the device's own provisioning
//     (WifiService::start_provisioning(); the card itself is the system
//     duties');
//   - about: the firmware version, the admin page's address (with the IP),
//     how to pair (AboutPairHint) and "hardware diagnostics", which opens
//     the self-check screen.
// Every write goes through SettingsDeps::set_settings inside a
// SettingsDeps::submit_io job: SystemSettings::set() reaches flash. The
// values shown at build come from SettingsDeps::settings (a cache). The
// Wi-Fi status and the address are read again every kRefreshMs.
//
// Threading: everything on the UI thread, the destructor included.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "lvgl.h"
#include "wisp/base/value.hpp"
#include "wisp/core/system_settings.hpp"

namespace wisp::shell {

struct SettingsDeps {
    std::function<core::SystemSettingsValues()> settings;
    std::function<void(std::map<std::string, Value>)> set_settings;
    std::function<std::string()> wifi_status, admin_url, firmware_version;
    std::function<void()> start_wifi_setup;
    std::function<void()> open_diagnostics;
    // The storage worker; empty: set_settings runs here.
    std::function<bool(std::function<void()>)> submit_io;
};

class SettingsPage {
public:
    static constexpr std::array<int, 6> kScreenOffChoices = {15, 30, 60, 300, 600, 0};
    static constexpr uint32_t kRefreshMs = 1000;

    explicit SettingsPage(SettingsDeps deps);
    // Deletes the refresh timer; the controls go with their screen.
    ~SettingsPage();

    SettingsPage(const SettingsPage&) = delete;
    SettingsPage& operator=(const SettingsPage&) = delete;

    void build(lv_obj_t* list);
    // Re-reads the Wi-Fi status and the address.
    void refresh();

    // For tests and the simulator's checks.
    lv_obj_t* brightness() const { return brightness_; }
    lv_obj_t* volume() const { return volume_; }
    lv_obj_t* screen_off(size_t i) const { return i < off_.size() ? off_[i] : nullptr; }
    lv_obj_t* mute() const { return mute_; }
    lv_obj_t* wifi_row() const { return wifi_row_; }
    lv_obj_t* wifi_label() const { return wifi_label_; }
    lv_obj_t* version_label() const { return version_; }
    lv_obj_t* address_label() const { return address_; }
    lv_obj_t* diagnostics() const { return diag_; }
    lv_obj_t* list() const { return list_; }

private:
    void write(std::map<std::string, Value> changes);
    void mark_screen_off(int seconds);
    lv_obj_t* add_slider(lv_obj_t* card, int min, int max, int value);
    static void event_cb(lv_event_t* e);

    SettingsDeps deps_;
    lv_obj_t* list_ = nullptr;
    lv_obj_t* brightness_ = nullptr;
    lv_obj_t* volume_ = nullptr;
    std::array<lv_obj_t*, kScreenOffChoices.size()> off_{};
    lv_obj_t* mute_ = nullptr;
    lv_obj_t* wifi_row_ = nullptr;
    lv_obj_t* wifi_label_ = nullptr;
    lv_obj_t* version_ = nullptr;
    lv_obj_t* address_ = nullptr;
    lv_obj_t* diag_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    std::string shown_wifi_, shown_address_;
};

}  // namespace wisp::shell
