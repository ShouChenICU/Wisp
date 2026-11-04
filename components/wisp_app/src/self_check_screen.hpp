#pragma once

// The self-check screen (spec 8) itself: a scrollable column of a header
// with summary chips and titled section cards, shown right after boot on
// any board with a display. See self_check.hpp for how App::start()/
// App::stop() own this object's lifetime.
//
// The class is split over two files: self_check_screen.cpp holds the
// page framing, the lifecycle, the bus subscriptions and the timers;
// self_check_overlays.cpp what goes on the top layer (the network cards
// and the confirm dialogs) and the power key's actions. Each
// section's widgets are a class of their own (self_check_header.hpp,
// self_check_devices.hpp, self_check_sensors.hpp, self_check_readouts.hpp,
// self_check_tests.hpp, net_section.hpp, scene_section.hpp). Nothing outside
// wisp_app's own sources includes this header.
//
// The screen is built once on the screen that is active then and stays on
// it: while a scene is open (its own screen loaded over this one) the
// screen keeps running hidden, and a rebuild (a language change) still
// builds on this screen, never on the scene's.

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "loopback.hpp"
#include "lvgl.h"
#include "net_cards.hpp"
#include "net_section.hpp"
#include "scene_section.hpp"
#include "scene_wiring.hpp"
#include "self_check_devices.hpp"
#include "self_check_header.hpp"
#include "self_check_model.hpp"
#include "self_check_readouts.hpp"
#include "self_check_sampler.hpp"
#include "self_check_sensors.hpp"
#include "self_check_tests.hpp"
#include "wisp/app/power_key_controller.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/hal/clock.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/hal/imu.hpp"
#include "wisp/hal/power.hpp"
#include "wisp/os/os.hpp"

namespace wisp::ui {
class SystemLayer;
class UiRuntime;
}  // namespace wisp::ui

namespace wisp::app {

class App;

class SelfCheck {
public:
    // `scenes` (null: no "场景" rows, only the empty card) lists the
    // installed scenes; it must outlive this screen.
    SelfCheck(App& app, ui::SystemLayer& system, SceneWiring* scenes);
    ~SelfCheck();

    // Must be called once, from the UI thread, right after construction
    // (see start_self_check()).
    void build();

    // Rebuilds every widget in the current ui::lang() (spec 8.3), keeping
    // the scroll position, an open confirm dialog and the interactive
    // tests' results. Must be called from the UI thread.
    void rebuild();

    // Re-renders the header's "启动 .. ms" text from BootReport's current
    // boot_ms. Must be called from the UI thread; see
    // refresh_self_check_header() in self_check.hpp for why this exists --
    // build() runs before App::start() has called BootReport::set_boot_ms()
    // (spec 6.1's boot sequence only finishes, and boot_ms is only final,
    // once this screen is already built), so the header initially shows a
    // stale (zero) value that this corrects once the real one is known.
    void refresh_header();

private:
    // BootReport's boot_ms (0 until App::start() sets it).
    uint64_t boot_ms() const;

    // The safe area (spec 5.6) to lay the root column out in: that of
    // the system layer's screen shape, which App::start() derived from
    // display_'s caps (an invalid shape falls back to a circle).
    hal::Area compute_safe_area() const;

    // Creates root_ and every widget under it (build()/rebuild()).
    void build_widgets();
    void build_devices(lv_obj_t* col);

    void start_perf_timer();
    void emit_perf_line();

    void subscribe_buttons();
    void on_button_event(const core::Event& e);
    void subscribe_power();
    void on_power_event(const core::Event& e);
    void subscribe_settings();
    void on_settings_event(const core::Event& e);
    // net.wifi.state / net.ap.state / net.pairing.code (connectivity spec
    // 5.5) and system.factory_reset {state:"requested"}: each posts the
    // matching sync onto the UI thread, which reads the services' current
    // state rather than trusting a possibly stale payload.
    void subscribe_net();
    void on_net_event(const core::Event& e);
    void on_reset_event(const core::Event& e);
    // system.firmware: any state may open or close the update dialog.
    void on_firmware_event(const core::Event& e);
    // UI thread: re-reads the Wi-Fi state into the network section and
    // opens, refreshes or closes the provisioning card to match.
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
    // Brings ui::lang() in line with the stored language, rebuilding if it
    // changed. UI thread only.
    void sync_language();
    void handle_power_key_action(PowerKeyController::Action action);
    void show_confirm_dialog();
    void close_confirm_dialog();
    void do_power_off();

    void start_loopback();
    void toggle_perf_overlay();
    // UI thread: records a passed interactive test and updates its mark
    // and the summary chip.
    void pass_test(InteractiveTest test);

    void refresh_ui();
    void refresh_storage();
    // Any thread: asks the storage worker for the scene list; the "场景"
    // card takes it on the UI thread.
    void request_scene_list();
    // UI thread: a row was tapped.
    void open_scene(const std::string& id);

    static void refresh_timer_cb(lv_timer_t* timer);
    // Answers to the three dialogs, called by the system layer after it
    // closed the box.
    void on_power_off_answer(bool yes);
    void on_reset_answer(bool yes);
    void on_firmware_answer(bool yes);

    App& app_;
    ui::UiRuntime* ui_;
    // Holds the confirm dialogs and keeps one open at a time; outlives
    // this screen (see app_impl.hpp).
    ui::SystemLayer& system_;
    std::shared_ptr<hal::Display> display_;
    std::shared_ptr<hal::Imu> imu_;
    std::shared_ptr<hal::Power> power_;
    std::shared_ptr<hal::Clock> clock_;
    core::Settings ui_settings_;

    PowerKeyController power_key_;
    bool overlay_enabled_ = false;

    // --- Sections (their widgets are valid only while this App's
    // UiRuntime is alive). UI thread only. ---
    SelfCheckHeader header_;
    DeviceSection devices_;
    // connectivity spec 8.1.
    NetSection net_section_;
    // scene-runtime-core spec 8.
    SceneSection scenes_section_;
    SensorSection sensors_;
    PowerClockSection power_clock_;
    StorageSection storage_;
    TestSection tests_section_;
    PerfSection perf_;
    ProvisioningCard setup_card_;
    PairingCard pair_card_;

    // Whether the device list's "storage.sd" row follows StorageManager::
    // status() live (see build_devices()).
    bool sd_row_live_ = false;
    // This boot's interactive test results; kept across rebuilds. UI
    // thread only.
    InteractiveTests tests_;
    // The last loopback status shown, re-rendered by rebuild(). UI thread
    // only.
    LoopbackStatus loop_status_;
    // SystemSettings::revision() as of the last sync_language(); empty
    // until the first one, which always reads the stored language. UI
    // thread only.
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
    // os::now_ms() of the last periodic scene list (see kSceneResyncMs).
    uint64_t last_scene_list_ms_ = 0;

    SceneWiring* scenes_;
    // Cleared by the UI-thread teardown: a scene list still on its way to
    // the UI thread then finds this screen gone. UI thread only.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    // The screen everything is built on (see the file header).
    lv_obj_t* screen_ = nullptr;

    lv_timer_t* refresh_timer_ = nullptr;
    // The single root container build() creates everything else under.
    // Deleted (recursively, with every child and so every LV_EVENT_*
    // callback registered with a section as user_data) in ~SelfCheck()'s
    // UI-thread teardown, so that no widget callback can still fire after
    // this SelfCheck is freed: a touch or click between stop_self_check()
    // returning and UiRuntime::stop() running would otherwise call back
    // into freed memory.
    lv_obj_t* root_ = nullptr;

    SelfCheckSampler sampler_;

    // --- WISP_PERF emission. ---
    std::unique_ptr<os::Timer> perf_timer_;

    // --- Bus subscriptions. ---
    core::SubscriptionId button_sub_ = 0;
    core::SubscriptionId power_sub_ = 0;
    core::SubscriptionId settings_sub_ = 0;
    core::SubscriptionId net_sub_ = 0;
    core::SubscriptionId reset_sub_ = 0;
    core::SubscriptionId fw_sub_ = 0;
    core::SubscriptionId scene_sub_ = 0;
};

}  // namespace wisp::app
