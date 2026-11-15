// The always-on system duties: the power key, the network cards, the
// confirm dialogs and WISP_PERF. See system_duties.hpp.

#include "system_duties.hpp"

#include <optional>
#include <string>
#include <utility>

#include "app_impl.hpp"
#include "factory_reset.hpp"
#include "firmware_confirm.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/format.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/hal/button.hpp"
#include "wisp/hal/roles.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"
#include "wisp/ui/wisp_model.hpp"

namespace wisp::app {

namespace {

constexpr const char* kTag = "system";
// The pairing and provisioning cards' countdowns.
constexpr uint32_t kUiRefreshMs = 100;

}  // namespace

SystemDuties::SystemDuties(App& app)
    : app_(app),
      ui_(*app.ui()),
      system_(*app.system_layer()),
      display_(app.registry().by_role<hal::Display>(hal::role::kMain)),
      power_(app.registry().by_role<hal::Power>(hal::role::kMain)),
      power_key_id_([&app] {
          auto key = app.registry().by_role<hal::Button>(hal::role::kPower);
          return key ? key->info().id : std::string();
      }()),
      setup_card_([this] {
          if (net::WifiService* wifi = app_.wifi()) {
              if (auto r = wifi->stop_provisioning(); !r) {
                  WLOG_W(kTag, "stopping the setup hotspot failed: %s", r.error().message.c_str());
              }
          }
      }),
      pair_card_([this] {
          if (net::PairingService* pairing = app_.pairing()) pairing->cancel_code();
      }) {
    if (!power_key_id_.empty()) {
        power_key_inbox_ = os::make_large<core::EdgeInbox<core::ButtonEdge, 8>>(
            "power_key", ui_, [this](const core::ButtonEdge& e) { on_power_key(e); });
        if (!power_key_inbox_) WLOG_E(kTag, "no memory for the power key's inbox");
    }
}

SystemDuties::~SystemDuties() {
    // First, while every member its handler reads is alive: off the UI
    // thread this waits for a run in progress. stop() has removed the
    // listener, its only producer.
    power_key_inbox_.reset();
}

bool SystemDuties::post(std::function<void()> fn) {
    return ui_.post([alive = alive_, fn = std::move(fn)] {
        if (*alive) fn();
    });
}

hal::Area SystemDuties::safe_area() const { return hal::safe_rect(system_.shape()); }

void SystemDuties::start() {
    if (alive_) return;
    alive_ = std::make_shared<bool>(true);

    if (power_key_inbox_) {
        // On wisp.button under ButtonService's lock: a compare and a push
        // (which, on an overflow, also logs the inbox's rate-limited
        // warning there). Down and up edges would only take room in the
        // ring.
        power_key_listener_ = app_.buttons().add_listener([this](const core::ButtonEdge& e) {
            if (e.kind != core::ButtonEdge::Kind::Short && e.kind != core::ButtonEdge::Kind::Long) {
                return;
            }
            if (e.id.view() != power_key_id_) return;
            power_key_inbox_->push(e);
        });
    }
    // From here on, this object's PowerKeyController is the *only* thing
    // that decides what a power-key event does to the screen --
    // PowerManager's own generic "any button wakes standby" handler
    // (power_manager.cpp) must leave that key alone, or the two could
    // each independently decide to flip the screen for the very same
    // physical press (one reacting to `down`, the other -- with its own,
    // now-stale idea of "is the screen on" -- reacting to the
    // `short`/`long` that follows it), producing a wake-then-immediately-
    // resleep flash. See PowerManager::set_power_key_owned()'s doc comment.
    if (PowerManager* pm = app_.power_manager()) {
        pm->set_power_key_owned(true);
    }
    // On the UI dispatcher: each callback runs here, on the UI thread, and
    // first with the cell's current value, so the cards show what was
    // announced before (networking starts first) one UI iteration after
    // start(). The calls below still show the rest at once; their
    // callbacks then find nothing new.
    settings_sub_ = app_.system_settings().cell().subscribe(
        [this](const core::SystemSettingsValues& v) { sync_system_language(v); }, &ui_);
    if (net::WifiService* wifi = app_.wifi()) {
        wifi_sub_ =
            wifi->wifi_cell().subscribe([this](const net::WifiSnapshot&) { sync_wifi(); }, &ui_);
        ap_sub_ = wifi->ap_cell().subscribe([this](const net::ApSnapshot&) { sync_wifi(); }, &ui_);
    }
    if (net::PairingService* pairing = app_.pairing()) {
        pairing_sub_ = pairing->pairing_cell().subscribe(
            [this](const net::PairingSnapshot&) { sync_pairing(); }, &ui_);
    }
    reset_sub_ = reset_status(app_).subscribe(
        [this](const ResetStatus& status) { on_reset_status(status); }, &ui_);
    fw_sub_ = firmware_progress(app_).subscribe(
        [this](const core::FirmwareProgress& p) { on_firmware_progress(p); }, &ui_);

    sync_reset();
    sync_firmware();
    refresh_timer_ = lv_timer_create(&SystemDuties::refresh_timer_cb, kUiRefreshMs, this);

    perf_timer_ = std::make_unique<os::Timer>("wisp.perf", kPerfReportMs, /*periodic=*/true,
                                              [this] { emit_perf_line(); });
    perf_timer_->start();
    // The LVGL performance overlay, switched in the diagnostics' performance
    // card, stays on across restarts and over every screen.
    if (core::Settings(*app_.platform().settings, "ui").get_bool("perf_overlay", false)) {
        ui_.set_perf_overlay(true);
    }
}

void SystemDuties::stop() {
    if (!alive_) return;
    // No handler runs again once its release returns (here on the UI
    // thread a cell callback's release returns at once). The listener goes
    // first: once removed nothing pushes into the power-key inbox, which
    // stays for the next start() and is destroyed with this object. Edges
    // still waiting in it run after stop() and find alive_ cleared below.
    if (power_key_listener_ != 0) {
        app_.buttons().remove_listener(power_key_listener_);
        power_key_listener_ = 0;
    }
    for (core::CellSubscription* sub :
         {&settings_sub_, &wifi_sub_, &ap_sub_, &pairing_sub_, &reset_sub_, &fw_sub_}) {
        sub->reset();
    }
    if (perf_timer_) {
        perf_timer_->stop();
        perf_timer_.reset();
    }
    // Give PowerManager's generic "any button wakes standby" handler back
    // ownership of the power key now that this object's PowerKeyController
    // stops listening -- see PowerManager::set_power_key_owned()'s doc
    // comment. Without it the power key would no longer wake the device
    // from standby.
    if (PowerManager* pm = app_.power_manager()) {
        pm->set_power_key_owned(false);
    }
    *alive_ = false;
    alive_.reset();
    close_confirm_dialog();
    close_reset_dialog();
    close_firmware_dialog();
    // The cards live on the top layer.
    setup_card_.close();
    pair_card_.close();
    if (refresh_timer_ != nullptr) {
        lv_timer_delete(refresh_timer_);
        refresh_timer_ = nullptr;
    }
}

void SystemDuties::emit_perf_line() {
    ui::UiPerf perf = ui_.perf();
    const ui::WispPerf wisp = ui::wisp_perf(os::now_ms());

    Value heap = app_.platform().heap_info ? app_.platform().heap_info() : Value();
    int64_t heap_internal_free = 0;
    int64_t heap_large_free = 0;
    if (const Value* v = heap.find("internal_free")) {
        heap_internal_free = v->as_int();
    }
    if (const Value* v = heap.find("large_free")) {
        heap_large_free = v->as_int();
    }

    Map payload{
        {"fps", static_cast<double>(perf.fps)},
        {"cpu", static_cast<int64_t>(perf.cpu_percent)},
        {"flush_ms", static_cast<double>(perf.avg_flush_ms)},
        {"frames", static_cast<int64_t>(perf.frames)},
        {"heap_internal_free", heap_internal_free},
        {"heap_large_free", heap_large_free},
        {"wisp_fps", wisp.fps},
        {"wisp_ms", wisp.ms},
    };
    app_.platform().emit_line("WISP_PERF " + to_json(Value(payload)));
}

void SystemDuties::on_power_key(const core::ButtonEdge& edge) {
    // A press that waited in the inbox while stop() ran does nothing.
    if (!alive_) return;
    // On the UI thread, like on_power_off_answer(), the other caller of
    // PowerKeyController, which is not thread-safe. PowerManager::standby()
    // -- not any state PowerKeyController tracks itself -- is the single
    // source of truth for "is the screen on"; read it fresh right before
    // deciding, on this same UI thread (the only thread that ever calls
    // into PowerManager's enter_standby()/exit_standby() on the power
    // key's behalf, so there is no race between this read and
    // handle_power_key_action() below acting on the result).
    PowerManager* pm = app_.power_manager();
    const bool screen_on = pm == nullptr || !pm->standby();
    const PowerKeyController::Action action = edge.kind == core::ButtonEdge::Kind::Short
                                                  ? power_key_.on_short(screen_on)
                                                  : power_key_.on_long(screen_on);
    if (action != PowerKeyController::Action::None) {
        handle_power_key_action(action);
    }
}

void SystemDuties::on_firmware_progress(const core::FirmwareProgress& progress) {
    // The service moves this cell to "received" just before it raises the
    // request flag that sync_firmware() takes, so a call running in between
    // would find no flag, and nothing else would open the dialog: "received"
    // opens it itself. show_firmware_dialog() shows only what
    // pending_firmware() still holds, once (fw_box_); the flag, taken
    // later, then finds the dialog open or the image answered (an answer
    // clears it).
    if (progress.phase == core::FirmwareProgress::Phase::Received) show_firmware_dialog();
    sync_firmware();
}

void SystemDuties::refresh_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<SystemDuties*>(lv_timer_get_user_data(timer));
    // The cards' own countdowns: a pairing code's expiry and the success
    // view's timer move no cell.
    const uint64_t now = os::now_ms();
    self->pair_card_.tick(now);
    self->setup_card_.tick(now);
}

void SystemDuties::sync_wifi() {
    const uint64_t now = os::now_ms();
    net::WifiService* wifi = app_.wifi();
    std::optional<net::ApSession> ap = wifi != nullptr ? wifi->ap_session() : std::nullopt;
    // The session the open QR view shows has ended in success: show the new
    // address instead. The result outlives the hotspot, so this holds even
    // when the hotspot closed before this sync ran.
    if (setup_card_.is_open() && !setup_card_.showing_success()) {
        std::optional<net::ProvisioningResult> done =
            wifi != nullptr ? wifi->provisioning_result() : std::nullopt;
        if (done && done->session == setup_card_.session().id) {
            setup_card_.show_success(displayable_ssid(done->ssid),
                                     admin_url(done->ip, app_.platform().http_port), now,
                                     safe_area(), ui::lang());
            raise_overlays();
            wake_screen();
            return;
        }
    }
    // The success view stays until OK or its own timer, whatever the
    // hotspot does; only a new session replaces it.
    if (setup_card_.showing_success() && (!ap || ap->id == setup_card_.session().id)) return;
    if (!ap) {
        setup_card_.close();
        return;
    }
    const net::ApSession& shown = setup_card_.session();
    if (setup_card_.is_open() && !setup_card_.showing_success() && shown.id == ap->id &&
        shown.ssid == ap->ssid && shown.password == ap->password && shown.ip == ap->ip) {
        return;
    }
    setup_card_.open(*ap, safe_area(), ui::lang());
    raise_overlays();
}

void SystemDuties::sync_pairing() {
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
    pair_card_.open(std::move(*code), expires_ms, now, safe_area(), ui::lang());
    raise_overlays();
    wake_screen();
}

bool SystemDuties::reset_holds_language() const {
    // Asked of the reset itself, at the time of the call, not of the last
    // status this object was called with: the reseed's delivery may run
    // before or after the reset status's ("done" is set before the
    // reseed), and a failure may reach this object folded into a newer
    // request. A reset still running holds it, "done" included; one that
    // failed does not.
    return factory_reset_running(app_) ||
           reset_status(app_).get().phase == ResetStatus::Phase::Done;
}

void SystemDuties::sync_system_language(const core::SystemSettingsValues& values) {
    apply_system_language(ui::parse_lang(values.language));
}

void SystemDuties::apply_system_language(std::optional<ui::Lang> lang) {
    // The first delivery usually finds the language App::start() applied
    // before the first frame, and changes nothing. A reseed of the cell
    // (after a batch's failed rollback) is followed like any change: the
    // screen shows what is stored.
    if (!lang || *lang == ui::lang() || reset_holds_language()) {
        return;
    }
    // The system UI (the network cards and the system layer's dialogs)
    // follows this language, so whatever of it is open is reopened in it.
    // The cards go first and raise_overlays() restores their stacking
    // order; the dialogs open above both.
    ui::set_lang(*lang);
    const uint64_t now = os::now_ms();
    setup_card_.reopen(safe_area(), *lang, now);
    if (pair_card_.is_open()) {
        std::string code = pair_card_.code();
        uint64_t expires = pair_card_.expires_ms();
        pair_card_.open(std::move(code), expires, now, safe_area(), *lang);
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

void SystemDuties::raise_overlays() {
    pair_card_.raise();
    system_.raise();
}

void SystemDuties::wake_screen() {
    PowerManager* pm = app_.power_manager();
    if (pm != nullptr && pm->standby()) {
        pm->exit_standby();
    }
}

void SystemDuties::on_reset_status(const ResetStatus&) {
    sync_reset();
    // A confirmed reset that failed lets the language follow again: what
    // was stored meanwhile is shown now. Only the language is read.
    apply_system_language(app_.system_settings().cell().read(
        [](const core::SystemSettingsValues& v) { return ui::parse_lang(v.language); }));
}

void SystemDuties::sync_reset() {
    if (take_factory_reset_request(app_)) {
        show_reset_dialog();
    }
}

void SystemDuties::show_reset_dialog() {
    if (reset_box_ != nullptr || factory_reset_running(app_)) {
        return;  // one dialog per request at a time; nothing once confirmed
    }
    reset_box_ = system_.open_confirm(
        ui::StrId::ResetTitle, ui::tr(ui::StrId::ResetPrompt),
        [this](bool yes) { on_reset_answer(yes); }, ui::ButtonRole::Danger);
    if (reset_box_ == nullptr) {
        // One confirm dialog at a time (spec 8). The person at the device
        // is already answering another question, so the browser gets a
        // definite "no" now rather than a dialog that may never show.
        WLOG_I(kTag, "factory reset request refused: another dialog is open");
        cancel_factory_reset(app_);
        return;
    }
    wake_screen();
    WLOG_I(kTag, "factory reset: waiting for confirmation on the screen");
}

void SystemDuties::close_reset_dialog() {
    if (reset_box_ == nullptr) {
        return;
    }
    system_.close_confirm(reset_box_);
    reset_box_ = nullptr;
}

void SystemDuties::sync_firmware() {
    if (take_firmware_request(app_)) {
        show_firmware_dialog();
    }
    if (fw_box_ != nullptr && !pending_firmware(app_)) {
        close_firmware_dialog();
    }
}

void SystemDuties::show_firmware_dialog() {
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
        WLOG_I(kTag, "firmware update refused: another dialog is open");
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
    WLOG_I(kTag, "firmware %s: waiting for confirmation on the screen", pending->next.c_str());
}

void SystemDuties::close_firmware_dialog() {
    if (fw_box_ == nullptr) {
        return;
    }
    system_.close_confirm(fw_box_);
    fw_box_ = nullptr;
}

void SystemDuties::handle_power_key_action(PowerKeyController::Action action) {
    // Screen on/off goes through PowerManager (panel/touch sleep, UI
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

void SystemDuties::show_confirm_dialog() {
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
        WLOG_I(kTag, "power-off dialog not shown: another dialog is open");
        (void)power_key_.on_confirm(false);
    }
}

void SystemDuties::close_confirm_dialog() {
    if (msgbox_ == nullptr) {
        return;
    }
    system_.close_confirm(msgbox_);
    msgbox_ = nullptr;
}

bool SystemDuties::can_power_off() const {
    return power_ != nullptr || static_cast<bool>(app_.platform().request_power_off);
}

void SystemDuties::do_power_off() {
    // What still waits for flash goes first (spec 3.8 of the flash-thread
    // design); the screen stays as it is for those few seconds at most.
    app_.flush_storage();
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
        WLOG_E(kTag, "Power::power_off() failed: %s", result.error().message.c_str());
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

void SystemDuties::on_power_off_answer(bool yes) {
    msgbox_ = nullptr;
    PowerKeyController::Action action = power_key_.on_confirm(yes);
    handle_power_key_action(action);
}

void SystemDuties::on_reset_answer(bool yes) {
    reset_box_ = nullptr;
    if (yes) {
        // Off the UI thread: the reset waits for the network and writes
        // flash before it restarts the device. If the task could not
        // start, the reset was answered "cancelled" and a later request
        // may ask again.
        (void)start_factory_reset(app_);
    } else {
        cancel_factory_reset(app_);
    }
}

void SystemDuties::on_firmware_answer(bool yes) {
    fw_box_ = nullptr;
    if (yes) {
        // Off the UI thread: switching the boot slot re-verifies the image.
        (void)start_firmware_install(app_);
    } else {
        decline_firmware(app_);
    }
}

SystemDuties* system_duties(App& app) { return app.impl_->duties.get(); }

}  // namespace wisp::app
