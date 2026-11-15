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
//   - The system language (ui::lang()) following the settings cell, so the
//     cards and the dialogs reopen in a new language. While a confirmed
//     factory reset runs, or once it is announced done, the reset reseeds
//     the cell to the defaults: the screen keeps its language until the
//     restart. A reset that failed or was cancelled lets it follow again.
//
// The language, the cards and the dialogs follow their owners' state cells
// (the system settings, the Wi-Fi and hotspot, the pairing code, the reset
// status, the firmware progress), subscribed on the UiRuntime: each call
// runs on the UI thread and re-reads the services' current state, so a
// call that stands for several changes still ends on the last one, and
// the first call shows what was announced before start(). Nothing
// re-reads the services or the settings on a timer: a 100 ms lv_timer
// only runs the cards' countdowns.
//
// The power key's short and long presses reach the duties through a
// ButtonService listener and an EdgeInbox on the UiRuntime ("power_key"),
// neither of which needs the bus or a free post: every press is handled,
// in order, also with wisp.bus flooded, the post queue full or the UI
// loop paused in standby. An overflow of the inbox is counted (App's
// dispatch_report()) and logged; the inbox logs that warning (rate-limited)
// on wisp.button, under ButtonService's lock, from inside the listener's
// push.
//
// Order against PowerManager's wake: its follower (a cell, coalescing,
// moved by every edge of every key) and this inbox both run on the UI
// dispatcher, each in the place its first pending edge gave it, not edge
// by edge across the two. "Power key off, another key, power key on"
// pressed within one UI stall: the follower, queued by the first press's
// down edge, runs first and finds the screen still on (the other key's
// wake does nothing), then the inbox runs off, on: the screen ends on.
//
// Threading: start() and stop() are UI thread only, like every member that
// touches LVGL. The listener runs on wisp.button and only pushes into the
// inbox, whose handler runs on the UI thread; the WISP_PERF timer runs on
// its own os::Timer thread and reads nothing that needs the UI thread. A
// post still queued when stop() runs finds the duties stopped and does
// nothing (alive_).

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "factory_reset.hpp"
#include "lvgl.h"
#include "net_cards.hpp"
#include "wisp/app/power_key_controller.hpp"
#include "wisp/core/button_service.hpp"
#include "wisp/core/edge_inbox.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/firmware_update.hpp"
#include "wisp/core/state_cell.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/hal/power.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/strings.hpp"

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
    // released on the UI thread, and it removes the button listener).
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

    // Any thread while this object exists: the power-key inbox's dropped
    // edges since construction (an atomic).
    uint32_t power_key_overflows() const {
        return power_key_inbox_ ? power_key_inbox_->overflows() : 0;
    }

private:
    // Runs `fn` on the UI thread unless stop() has run by then. False when
    // the post queue was full.
    bool post(std::function<void()> fn);

    // The safe area (spec 5.6) the cards are laid out in.
    hal::Area safe_area() const;

    void emit_perf_line();

    // UI thread: the power-key inbox's handler, once per short or long
    // press, in order.
    void on_power_key(const core::ButtonEdge& edge);
    // UI thread: the firmware progress cell's callback. Any phase may open
    // or close the update dialog.
    void on_firmware_progress(const core::FirmwareProgress& progress);
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
    // UI thread: the reset status cell's callback.
    void on_reset_status(const ResetStatus& status);
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
    // UI thread: the settings cell's callback. Brings ui::lang() (the
    // system UI's language) in line with `values`' language, reopening the
    // open network cards and confirm dialog in it if it changed -- unless
    // reset_holds_language().
    void sync_system_language(const core::SystemSettingsValues& values);
    void apply_system_language(std::optional<ui::Lang> lang);
    // Whether a confirmed factory reset still running (factory_reset_
    // running()) or announced done owns the settings cell's next values:
    // its reseed to the defaults must not switch the screen before the
    // restart.
    bool reset_holds_language() const;
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
    // The id of the key with role power, resolved once: the registry does
    // not change after boot. Empty: no power key, button edges are
    // ignored.
    std::string power_key_id_;

    PowerKeyController power_key_;
    // Built with this object, so dispatch_report() never races its
    // creation; its handler runs only once start() added the listener.
    // Null without a power key or when Mem::Large is exhausted (the key
    // then does nothing). Destroyed first by the destructor, after stop()
    // removed the only producer.
    os::LargeUnique<core::EdgeInbox<core::ButtonEdge, 8>> power_key_inbox_;
    // App's ButtonService listener; 0 while stopped.
    uint32_t power_key_listener_ = 0;
    ProvisioningCard setup_card_;
    PairingCard pair_card_;

    // The power-off confirmation's box while it is open.
    lv_obj_t* msgbox_ = nullptr;
    // The factory-reset confirmation, kept apart from the power-off dialog
    // above so neither one's answer (or the power key) can be taken for
    // the other. The system layer opens one dialog at a time.
    lv_obj_t* reset_box_ = nullptr;
    // The firmware-update confirmation; the third kind of dialog, likewise
    // never stacked on the other two.
    lv_obj_t* fw_box_ = nullptr;

    // Cleared by stop(): a post queued before it then does nothing. Read
    // and cleared on the UI thread only.
    std::shared_ptr<bool> alive_;
    lv_timer_t* refresh_timer_ = nullptr;
    std::unique_ptr<os::Timer> perf_timer_;

    // On the UiRuntime; released by stop(), before App::stop() destroys
    // the networking (Wi-Fi, hotspot, pairing) and the firmware service.
    // Wi-Fi, hotspot and pairing stay empty without Wi-Fi.
    core::CellSubscription settings_sub_;
    core::CellSubscription wifi_sub_;
    core::CellSubscription ap_sub_;
    core::CellSubscription pairing_sub_;
    core::CellSubscription reset_sub_;
    core::CellSubscription fw_sub_;
};

// The App's duties; null without a screen or while App is stopped. For
// tests that stop() and start() them (on the UI thread) around a change,
// to show what a late start finds.
SystemDuties* system_duties(App& app);

}  // namespace wisp::app
