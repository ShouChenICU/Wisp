// What the self-check screen shows on the top layer: the provisioning and
// pairing cards (connectivity spec 8.1) and the confirm dialogs (power off,
// factory reset, firmware update; all on ui::SystemLayer, one at a time),
// plus the power key's actions and the two languages: the page's own and
// the system one the dialogs follow. See self_check_screen.hpp.

#include <optional>
#include <string>
#include <utility>

#include "app_impl.hpp"
#include "factory_reset.hpp"
#include "firmware_confirm.hpp"
#include "self_check_screen.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_layer.hpp"

namespace wisp::app {

void SelfCheck::sync_wifi() {
    const uint64_t now = os::now_ms();
    net_section_.refresh(now);
    net::WifiService* wifi = app_.wifi();
    std::optional<net::ApSession> ap = wifi != nullptr ? wifi->ap_session() : std::nullopt;
    if (!ap) {
        setup_card_.close();
        return;
    }
    const net::ApSession& shown = setup_card_.session();
    if (setup_card_.is_open() && shown.ssid == ap->ssid && shown.password == ap->password &&
        shown.ip == ap->ip) {
        return;
    }
    setup_card_.open(*ap, compute_safe_area(), ui::lang());
    raise_overlays();
}

void SelfCheck::sync_pairing() {
    const uint64_t now = os::now_ms();
    net::PairingService* pairing = app_.pairing();
    uint64_t expires_ms = 0;
    std::optional<std::string> code =
        pairing != nullptr ? pairing->active_code(now, &expires_ms) : std::nullopt;
    if (!code) {
        pair_card_.close();
        return;
    }
    if (pair_card_.is_open() && pair_card_.code() == *code &&
        pair_card_.expires_ms() == expires_ms) {
        return;
    }
    pair_card_.open(std::move(*code), expires_ms, now, compute_safe_area(), ui::lang());
    raise_overlays();
    wake_screen();
}

void SelfCheck::set_page_lang(ui::Lang lang) {
    if (!text_.set_lang(lang)) {
        return;
    }
    // Everything not bound in text_ is re-rendered from what the sections
    // (or this screen) last showed; each compares before it sets. The
    // network cards and the dialogs are system UI and keep the system
    // language (see sync_system_language()).
    board_.show_lang(lang);
    header_.show_devices(devices_.summary());
    header_.show_tests(tests_.passed_count());
    devices_.retext();
    net_section_.retext(os::now_ms());
    tests_section_.show(tests_);
    tests_section_.set_loopback(loop_status_);
    tests_section_.retext();
    scenes_section_.retext();
    if (last_storage_) show_storage(*last_storage_);
    refresh_ui();
}

void SelfCheck::sync_system_language() {
    // The revision is read before the value: a change stored in between
    // moves it again, so the next sync reads the value once more.
    uint32_t revision = app_.system_settings().revision();
    if (language_revision_ == revision) {
        return;
    }
    language_revision_ = revision;
    const core::SystemSettingsValues values = app_.system_settings().get();
    utc_offset_min_ = values.utc_offset_min;
    refresh_time_sync();
    std::optional<ui::Lang> lang = ui::parse_lang(values.language);
    if (!lang || *lang == ui::lang()) {
        return;
    }
    // The page keeps its own language; the system UI (the network cards
    // and the system layer's dialogs) follows this one, so whatever of it
    // is open is reopened in it. The cards go first and raise_overlays()
    // restores their stacking order; the dialogs open above both.
    ui::set_lang(*lang);
    const uint64_t now = os::now_ms();
    if (setup_card_.is_open()) {
        net::ApSession session = setup_card_.session();
        setup_card_.open(session, compute_safe_area(), *lang);
    }
    if (pair_card_.is_open()) {
        std::string code = pair_card_.code();
        uint64_t expires = pair_card_.expires_ms();
        pair_card_.open(std::move(code), expires, now, compute_safe_area(), *lang);
    }
    raise_overlays();
    if (msgbox_ != nullptr) {
        close_confirm_dialog();
        show_confirm_dialog();
    }
    if (reset_box_ != nullptr) {
        close_reset_dialog();
        show_reset_dialog();
    }
    if (fw_box_ != nullptr) {
        close_firmware_dialog();
        show_firmware_dialog();
    }
}

void SelfCheck::raise_overlays() {
    pair_card_.raise();
    system_.raise();
}

void SelfCheck::wake_screen() {
    PowerManager* pm = app_.power_manager();
    if (pm != nullptr && pm->standby()) {
        pm->exit_standby();
    }
}

void SelfCheck::sync_reset() {
    if (take_factory_reset_request(app_)) {
        show_reset_dialog();
    }
}

void SelfCheck::show_reset_dialog() {
    if (reset_box_ != nullptr || reset_confirmed_) {
        return;  // one dialog per request at a time; nothing once confirmed
    }
    reset_box_ = system_.open_confirm(
        ui::StrId::ResetTitle, ui::tr(ui::StrId::ResetPrompt),
        [this](bool yes) { on_reset_answer(yes); }, ui::ButtonRole::Danger);
    if (reset_box_ == nullptr) {
        // One confirm dialog at a time (spec 8). The person at the device
        // is already answering another question, so the browser gets a
        // definite "no" now rather than a dialog that may never show.
        WLOG_I("self_check", "factory reset request refused: another dialog is open");
        cancel_factory_reset(app_);
        return;
    }
    wake_screen();
    WLOG_I("self_check", "factory reset: waiting for confirmation on the screen");
}

void SelfCheck::close_reset_dialog() {
    if (reset_box_ == nullptr) {
        return;
    }
    system_.close_confirm(reset_box_);
    reset_box_ = nullptr;
}

void SelfCheck::sync_firmware() {
    if (take_firmware_request(app_)) {
        show_firmware_dialog();
    }
    if (fw_box_ != nullptr && !pending_firmware(app_)) {
        close_firmware_dialog();
    }
}

void SelfCheck::show_firmware_dialog() {
    if (fw_box_ != nullptr) {
        return;
    }
    std::optional<core::FirmwarePending> pending = pending_firmware(app_);
    if (!pending) {
        return;
    }
    if (system_.confirm_open()) {
        // As for the factory reset: one confirm dialog at a time, and the
        // browser hears "rejected" now.
        WLOG_I("self_check", "firmware update refused: another dialog is open");
        decline_firmware(app_);
        return;
    }
    // One version per line: a git-described version is about as wide as
    // the box, and LVGL would otherwise break it mid-number.
    std::string text = ui::tr(ui::StrId::FirmwarePrompt);
    text += "\n" + pending->current + "\n→ " + pending->next;
    if (pending->downgrade) {
        text += "\n";
        text += ui::tr(ui::StrId::FirmwareDowngrade);
    }
    fw_box_ = system_.open_confirm(ui::StrId::FirmwareTitle, text,
                                   [this](bool yes) { on_firmware_answer(yes); });
    wake_screen();
    WLOG_I("self_check", "firmware %s: waiting for confirmation on the screen",
           pending->next.c_str());
}

void SelfCheck::close_firmware_dialog() {
    if (fw_box_ == nullptr) {
        return;
    }
    system_.close_confirm(fw_box_);
    fw_box_ = nullptr;
}

void SelfCheck::handle_power_key_action(PowerKeyController::Action action) {
    // Screen on/off now goes through PowerManager (panel/touch sleep, UI
    // pause, button poll widening, the platform DFS/light-sleep hook --
    // spec 6.7), not a direct hal::Display::set_power() call.
    PowerManager* pm = app_.power_manager();
    switch (action) {
        case PowerKeyController::Action::None:
            break;
        case PowerKeyController::Action::ScreenOff:
            if (pm != nullptr) {
                pm->enter_standby();
            }
            break;
        case PowerKeyController::Action::ScreenOn:
            if (pm != nullptr) {
                pm->exit_standby();
            }
            break;
        case PowerKeyController::Action::ShowConfirm:
            show_confirm_dialog();
            break;
        case PowerKeyController::Action::ScreenOnAndConfirm:
            if (pm != nullptr) {
                pm->exit_standby();
            }
            show_confirm_dialog();
            break;
        case PowerKeyController::Action::PowerOff:
            do_power_off();
            break;
        case PowerKeyController::Action::CancelConfirm:
            close_confirm_dialog();
            break;
    }
}

void SelfCheck::show_confirm_dialog() {
    if (msgbox_ != nullptr) {
        return;  // Already up -- spec 8: no stacked confirm dialogs.
    }
    msgbox_ = system_.open_confirm(ui::StrId::PowerOffTitle, ui::tr(ui::StrId::PowerOffPrompt),
                                   [this](bool yes) { on_power_off_answer(yes); });
    if (msgbox_ == nullptr) {
        // Nor on top of another question (the factory reset, a firmware
        // update): the long press is ignored, and the key controller is
        // told no dialog came up, so the next short press turns the screen
        // off as usual.
        WLOG_I("self_check", "power-off dialog not shown: another dialog is open");
        (void)power_key_.on_confirm(false);
    }
}

void SelfCheck::close_confirm_dialog() {
    if (msgbox_ == nullptr) {
        return;
    }
    system_.close_confirm(msgbox_);
    msgbox_ = nullptr;
}

void SelfCheck::do_power_off() {
    if (power_) {
        auto result = power_->power_off();
        if (result) {
            return;
        }
        // Logged, rather than silently falling through, so a device
        // that ends up stuck running the deep-sleep fallback below (the
        // PMIC can't be told to cut power) leaves a trace of *why* --
        // otherwise the only symptom is the screen going dark and
        // nothing else happening.
        WLOG_E("self_check", "Power::power_off() failed: %s", result.error().message.c_str());
        // Fall through to the platform fallback below
        // rather than silently doing nothing -- the user already
        // confirmed shutdown, so a failed Power::power_off() shouldn't
        // leave the device stuck on.
    }
    if (app_.platform().request_power_off) {
        // The ESP fallback (esp_deep_sleep_start(), no wakeup source)
        // does not power down the AMOLED panel or its PMIC rail (see
        // docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md) -- deep sleep only
        // stops the SoC. Without turning the panel off first, this
        // confirm dialog (its own GRAM contents) would stay lit,
        // indistinguishable from a hang, and keep drawing power for as
        // long as the battery lasts. set_power(Standby) runs here, on
        // this (UI) thread, as spec 5.3 requires for every Display
        // call.
        if (display_) {
            display_->set_power(hal::PowerLevel::Standby);
        }
        app_.platform().request_power_off();
    }
}

void SelfCheck::on_power_off_answer(bool yes) {
    msgbox_ = nullptr;
    PowerKeyController::Action action = power_key_.on_confirm(yes);
    handle_power_key_action(action);
}

void SelfCheck::on_reset_answer(bool yes) {
    reset_box_ = nullptr;
    if (yes) {
        // Off the UI thread: the reset waits for the network and writes
        // flash before it restarts the device. If the task could not
        // start, the reset was answered "cancelled" and a later request
        // may ask again.
        reset_confirmed_ = start_factory_reset(app_);
    } else {
        cancel_factory_reset(app_);
    }
}

void SelfCheck::on_firmware_answer(bool yes) {
    fw_box_ = nullptr;
    if (yes) {
        // Off the UI thread: switching the boot slot re-verifies the image.
        (void)start_firmware_install(app_);
    } else {
        decline_firmware(app_);
    }
}

}  // namespace wisp::app
