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
#include "wisp/net/pairing_service.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"
#include "wisp/ui/wisp_model.hpp"

namespace wisp::app {

namespace {

constexpr const char* kTag = "system";
constexpr uint32_t kUiRefreshMs = 100;
// How often the refresh timer re-reads the network services (and the
// pending factory-reset and firmware requests) even without an event: the
// bus drops its oldest events when full, and a lost net.ap.state,
// net.pairing.code, system.factory_reset or system.firmware must not leave
// a card up (or missing) for good.
// The reads are a few short lock-protected copies and an atomic.
constexpr uint64_t kNetResyncMs = 1000;

}  // namespace

SystemDuties::SystemDuties(App& app)
    : app_(app),
      ui_(*app.ui()),
      system_(*app.system_layer()),
      display_(app.registry().get<hal::Display>("display.main")),
      power_(app.registry().get<hal::Power>("power.main")),
      setup_card_([this] {
          if (net::WifiService* wifi = app_.wifi()) {
              if (auto r = wifi->stop_provisioning(); !r) {
                  WLOG_W(kTag, "stopping the setup hotspot failed: %s", r.error().message.c_str());
              }
          }
      }),
      pair_card_([this] {
          if (net::PairingService* pairing = app_.pairing()) pairing->cancel_code();
      }) {}

SystemDuties::~SystemDuties() = default;

bool SystemDuties::post(std::function<void()> fn) {
    return ui_.post([alive = alive_, fn = std::move(fn)] {
        if (*alive) fn();
    });
}

hal::Area SystemDuties::safe_area() const { return hal::safe_rect(system_.shape()); }

void SystemDuties::start() {
    if (alive_) return;
    alive_ = std::make_shared<bool>(true);

    button_sub_ =
        app_.bus().subscribe("button.", [this](const core::Event& e) { on_button_event(e); });
    // From here on, this object's PowerKeyController is the *only* thing
    // that decides what a "button.power" event does to the screen --
    // PowerManager's own generic "any button wakes standby" handler
    // (power_manager.cpp) must leave button.power alone, or the two could
    // each independently decide to flip the screen for the very same
    // physical press (one reacting to `down`, the other -- with its own,
    // now-stale idea of "is the screen on" -- reacting to the
    // `short`/`long` that follows it), producing a wake-then-immediately-
    // resleep flash. See PowerManager::set_power_key_owned()'s doc comment.
    if (PowerManager* pm = app_.power_manager()) {
        pm->set_power_key_owned(true);
    }
    settings_sub_ = app_.bus().subscribe("system.settings",
                                         [this](const core::Event& e) { on_settings_event(e); });
    net_sub_ = app_.bus().subscribe("net.", [this](const core::Event& e) { on_net_event(e); });
    reset_sub_ = app_.bus().subscribe("system.factory_reset",
                                      [this](const core::Event& e) { on_reset_event(e); });
    fw_sub_ = app_.bus().subscribe("system.firmware",
                                   [this](const core::Event& e) { on_firmware_event(e); });

    // Anything announced before the subscriptions above (networking starts
    // first).
    sync_system_language();
    sync_wifi();
    sync_pairing();
    sync_reset();
    sync_firmware();
    last_net_resync_ms_ = os::now_ms();
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
    // Each handler is guaranteed not to run again once its unsubscribe
    // returns (EventBus::unsubscribe()); whatever they already posted
    // finds alive_ cleared below.
    for (core::SubscriptionId* sub :
         {&button_sub_, &settings_sub_, &net_sub_, &reset_sub_, &fw_sub_}) {
        if (*sub != 0) {
            app_.bus().unsubscribe(*sub);
            *sub = 0;
        }
    }
    if (perf_timer_) {
        perf_timer_->stop();
        perf_timer_.reset();
    }
    // Give PowerManager's generic "any button wakes standby" handler back
    // ownership of "button.power" now that this object's PowerKeyController
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

void SystemDuties::on_button_event(const core::Event& e) {
    if (e.source != "button.power") return;
    // on_button_event() itself runs on the EventBus dispatcher thread, but
    // PowerKeyController is not thread-safe against concurrent callers --
    // on_power_off_answer() also drives it, from the UI thread, via
    // on_confirm(). Posting the *whole* decision (on_short()/on_long()
    // included, not just the resulting Action) onto the UI thread is what
    // makes every PowerKeyController call happen on one single thread.
    std::string topic = e.topic;
    // A dropped post() here silently loses whatever power-key action
    // (screen off/on) this event was about to decide -- log it so that
    // isn't invisible (see event_bus.hpp for why this must not block
    // waiting for a slot instead).
    if (!post([this, topic] {
            // PowerManager::standby() -- not any state PowerKeyController
            // tracks itself -- is the single source of truth for "is the
            // screen on"; read it fresh right before deciding, on this
            // same UI thread (the only thread that ever calls into
            // PowerManager's enter_standby()/exit_standby() on the power
            // key's behalf, so there is no race between this read and
            // handle_power_key_action() below acting on the result).
            PowerManager* pm = app_.power_manager();
            bool screen_on = pm == nullptr || !pm->standby();
            PowerKeyController::Action action = PowerKeyController::Action::None;
            if (topic == "button.power.short") {
                action = power_key_.on_short(screen_on);
            } else if (topic == "button.power.long") {
                action = power_key_.on_long(screen_on);
            }
            if (action != PowerKeyController::Action::None) {
                handle_power_key_action(action);
            }
        })) {
        WLOG_W(kTag, "post(power key action) dropped (post queue full)");
    }
}

void SystemDuties::on_settings_event(const core::Event& e) {
    if (e.payload.find("language") == e.payload.end()) {
        return;
    }
    // The language itself is read back from SystemSettings on the UI
    // thread rather than taken from the payload, so a stale event queued
    // behind a newer change can never switch the screen back. If this
    // post is dropped, the refresh timer's sync_system_language() still
    // catches the change.
    if (!post([this] { sync_system_language(); })) {
        WLOG_W(kTag, "post(language change) dropped (post queue full)");
    }
}

void SystemDuties::on_net_event(const core::Event& e) {
    bool posted = true;
    if (e.topic == "net.wifi.state" || e.topic == "net.ap.state") {
        posted = post([this] { sync_wifi(); });
    } else if (e.topic == "net.pairing.code") {
        posted = post([this] { sync_pairing(); });
    }
    if (!posted) {
        WLOG_W(kTag, "post(%s) dropped (post queue full)", e.topic.c_str());
    }
}

void SystemDuties::on_reset_event(const core::Event& e) {
    auto it = e.payload.find("state");
    if (it == e.payload.end() || it->second.type() != Value::Type::String ||
        it->second.as_string() != "requested") {
        return;
    }
    // The pending flag is taken on the UI thread, not here: if this post
    // is dropped, the flag is still set for the periodic resync.
    if (!post([this] { sync_reset(); })) {
        WLOG_W(kTag, "post(factory reset dialog) dropped (post queue full)");
    }
}

void SystemDuties::on_firmware_event(const core::Event&) {
    // The state is re-read on the UI thread; a dropped post is made up for
    // by the periodic resync.
    if (!post([this] { sync_firmware(); })) {
        WLOG_W(kTag, "post(firmware dialog) dropped (post queue full)");
    }
}

void SystemDuties::refresh_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<SystemDuties*>(lv_timer_get_user_data(timer));
    // The fallback for a lost system.settings event: one atomic load per
    // tick while nothing changes.
    self->sync_system_language();
    const uint64_t now = os::now_ms();
    if (now - self->last_net_resync_ms_ >= kNetResyncMs) {
        self->last_net_resync_ms_ = now;
        self->sync_wifi();
        self->sync_pairing();
        self->sync_reset();
        self->sync_firmware();
    }
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

void SystemDuties::sync_system_language() {
    // The revision is read before the value: a change stored in between
    // moves it again, so the next sync reads the value once more.
    uint32_t revision = app_.system_settings().revision();
    if (language_revision_ == revision) {
        return;
    }
    language_revision_ = revision;
    std::optional<ui::Lang> lang = ui::parse_lang(app_.system_settings().get().language);
    if (!lang || *lang == ui::lang()) {
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

void SystemDuties::sync_reset() {
    if (take_factory_reset_request(app_)) {
        show_reset_dialog();
    }
}

void SystemDuties::show_reset_dialog() {
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
        reset_confirmed_ = start_factory_reset(app_);
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

}  // namespace wisp::app
