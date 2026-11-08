#pragma once

// The system's duties on the screen that no page may take away (mvp spec
// 6.6): on from boot whatever screen is up -- the home screen, a sub-page,
// the hardware diagnostics or a scene.
//   - The power key: a short press toggles standby, a long press asks to
//     power off (spec 8 of the base spec), through PowerKeyController.
//   - The provisioning and pairing cards (connectivity spec 8.1), on the
//     top layer.
//   - The factory-reset and firmware-update confirm dialogs, on the system
//     layer; with the power-off dialog, one at a time.
//   - The WISP_PERF line every 5 s (docs/architecture.md).
//   - The system language (ui::lang()) following the stored one, so the
//     cards and the dialogs reopen in a new language.
//
// Every event handler posts its work onto the UI thread, which reads the
// services' current state rather than trusting a possibly stale payload;
// a 100 ms lv_timer resyncs the network services and the pending
// factory-reset and firmware requests once a second, for an event the bus
// dropped.
//
// Threading: start() and stop() are UI thread only, like every member that
// touches LVGL. The bus handlers run on the dispatcher thread and only
// post; the WISP_PERF timer runs on its own os::Timer thread and reads
// nothing that needs the UI thread. A post still queued when stop() runs
// finds the duties stopped and does nothing (alive_).

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "lvgl.h"
#include "net_cards.hpp"
#include "wisp/app/power_key_controller.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/hal/power.hpp"
#include "wisp/os/os.hpp"

namespace wisp::ui {
class SystemLayer;
class UiRuntime;
}  // namespace wisp::ui

namespace wisp::app {

class App;

class SystemDuties {
public:
    // How often the WISP_PERF line is emitted.
    static constexpr uint32_t kPerfReportMs = 5000;

    // `app` must have a UiRuntime and a system layer, both outliving this
    // object.
    explicit SystemDuties(App& app);
    // Requires stop() first when started (its LVGL objects are only
    // released on the UI thread).
    ~SystemDuties();

    SystemDuties(const SystemDuties&) = delete;
    SystemDuties& operator=(const SystemDuties&) = delete;

    // UI thread. Subscribes, takes the power key from PowerManager, starts
    // the timers and shows whatever the services already announced.
    void start();
    // UI thread. Undoes start(): no handler runs and no dialog or card
    // stays up once it returns. Idempotent.
    void stop();

    // Whether power_off() can do anything: a power device, or the
    // platform's fallback.
    bool can_power_off() const;
    // UI thread. Powers off exactly as a confirmed long press of the power
    // key does (the settings page asks first itself).
    void power_off() { do_power_off(); }

private:
    // Runs `fn` on the UI thread unless stop() has run by then. False when
    // the post queue was full.
    bool post(std::function<void()> fn);

    // The safe area (spec 5.6) the cards are laid out in.
    hal::Area safe_area() const;

    void emit_perf_line();

    void on_button_event(const core::Event& e);
    void on_settings_event(const core::Event& e);
    // net.wifi.state / net.ap.state / net.pairing.code (connectivity spec
    // 5.5) and system.factory_reset {state:"requested"}: each posts the
    // matching sync onto the UI thread.
    void on_net_event(const core::Event& e);
    void on_reset_event(const core::Event& e);
    // system.firmware: any state may open or close the update dialog.
    void on_firmware_event(const core::Event& e);
    // UI thread: opens, refreshes or closes the provisioning card to match
    // the Wi-Fi service.
    void sync_wifi();
    // UI thread: shows the service's active pairing code (if any) with a
    // countdown to its expiry, or closes the card.
    void sync_pairing();
    // Keeps the stacking order on the top layer: provisioning card, then
    // pairing card, then the system layer (the dialogs last).
    void raise_overlays();
    // Turns a sleeping screen on for something the user is waiting to see
    // (a pairing code, the factory-reset question). UI thread.
    void wake_screen();
    // UI thread: takes a pending factory-reset request (see
    // take_factory_reset_request()) and shows the dialog for it.
    void sync_reset();
    void show_reset_dialog();
    void close_reset_dialog();
    // UI thread: takes a pending firmware request and shows the dialog for
    // it, or closes the dialog once nothing awaits an answer (declined
    // elsewhere, timed out).
    void sync_firmware();
    void show_firmware_dialog();
    void close_firmware_dialog();
    // Brings ui::lang() (the system UI's language) in line with the stored
    // language, reopening the open network cards and confirm dialog in it
    // if it changed. UI thread only.
    void sync_system_language();
    void handle_power_key_action(PowerKeyController::Action action);
    void show_confirm_dialog();
    void close_confirm_dialog();
    void do_power_off();

    static void refresh_timer_cb(lv_timer_t* timer);
    // Answers to the three dialogs, called by the system layer after it
    // closed the box.
    void on_power_off_answer(bool yes);
    void on_reset_answer(bool yes);
    void on_firmware_answer(bool yes);

    App& app_;
    ui::UiRuntime& ui_;
    // Holds the confirm dialogs and keeps one open at a time; outlives
    // this object (see app_impl.hpp).
    ui::SystemLayer& system_;
    std::shared_ptr<hal::Display> display_;
    std::shared_ptr<hal::Power> power_;

    PowerKeyController power_key_;
    ProvisioningCard setup_card_;
    PairingCard pair_card_;

    // SystemSettings::revision() as of the last sync_system_language();
    // empty until the first one, which always reads the stored language.
    // UI thread only.
    std::optional<uint32_t> language_revision_;
    // The power-off confirmation's box while it is open.
    lv_obj_t* msgbox_ = nullptr;
    // The factory-reset confirmation, kept apart from the power-off dialog
    // above so neither one's answer (or the power key) can be taken for
    // the other. The system layer opens one dialog at a time.
    lv_obj_t* reset_box_ = nullptr;
    // Set once the user confirmed and the reset task started: later
    // requests are ignored while the reset runs towards its restart.
    bool reset_confirmed_ = false;
    // The firmware-update confirmation; the third kind of dialog, likewise
    // never stacked on the other two.
    lv_obj_t* fw_box_ = nullptr;
    // os::now_ms() of the last periodic network resync (see kNetResyncMs).
    uint64_t last_net_resync_ms_ = 0;

    // Cleared by stop(): a post queued before it then does nothing. Read
    // and cleared on the UI thread only.
    std::shared_ptr<bool> alive_;
    lv_timer_t* refresh_timer_ = nullptr;
    std::unique_ptr<os::Timer> perf_timer_;

    core::SubscriptionId button_sub_ = 0;
    core::SubscriptionId settings_sub_ = 0;
    core::SubscriptionId net_sub_ = 0;
    core::SubscriptionId reset_sub_ = 0;
    core::SubscriptionId fw_sub_ = 0;
};

}  // namespace wisp::app
