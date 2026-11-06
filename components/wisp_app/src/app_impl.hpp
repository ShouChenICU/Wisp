#pragma once

// App::Impl's full definition, shared (via #include, not inheritance) by
// every one of wisp_app's own .cpp files that needs it: app.cpp (which
// defines App's own member functions), and self_check_screen.cpp /
// loopback.cpp (granted access to App's private members, including
// Impl's layout, via the friend declarations in wisp/app/app.hpp --
// friendship grants *permission*, but a TU still needs this header for
// Impl's *complete type* before it can actually name a member of it).
//
// Splitting this out of app.cpp itself (rather than defining `struct
// App::Impl { ... };` inline there, as earlier tasks did before the
// self-check screen and loopback needed to share it) is what lets
// SelfCheck's and the "wisp.loop" task handle's lifetimes live directly
// on the one App::Impl instance per App -- see app.hpp's Lifetime note
// on why that (rather than a process-wide static) is required for tests
// that construct several Apps in one process, one after another.

#include <atomic>
#include <memory>
#include <utility>

#include "net_wiring.hpp"
#include "notification_router.hpp"
#include "scene_wiring.hpp"
#include "self_check.hpp"
#include "status_shell.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/app/system_notices.hpp"
#include "wisp/core/button_service.hpp"
#include "wisp/core/firmware_update.hpp"
#include "wisp/core/model_profiles.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/occupancy.hpp"
#include "wisp/core/power_monitor.hpp"
#include "wisp/core/sensor_service.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/core/tone_player.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

struct App::Impl {
    Platform platform;
    core::DeviceRegistry registry;
    core::BootReport report;
    core::EventBus bus;
    // Wake locks (scene-device spec 4.3); PowerManager follows them.
    core::WakeLockRegistry wakelocks{bus};
    // Device occupancy (scene-device spec 6.3); SensorService records its
    // holders here.
    core::OccupancyRegistry occupancy;
    core::StorageManager storage;
    core::ButtonService buttons;
    // Profile ids come from Platform::random (a weak fallback without it).
    core::ModelProfileService models{storage, bus, platform.random};
    // Brightness goes through PowerManager so a change made in standby
    // waits for the wake-up (spec 5.4). power_manager is only reset after
    // the HTTP server and the net task have stopped (App::stop()), so no
    // caller races with it going away. Every stored change is published
    // as system.settings (spec 8.3); the self-check screen moves the
    // system language (ui::lang()) and an open confirm dialog to a new
    // language from it.
    core::SystemSettings system_settings{
        *platform.settings,
        [this](uint8_t level) {
            if (power_manager) power_manager->set_brightness(level);
        },
        [this](const core::SystemSettingsValues& now) {
            bus.publish({"system.settings", "app", 0, core::to_map(now)});
        }};

    // Only constructed by start() -- see App::start()/App::stop().
    std::unique_ptr<ui::UiRuntime> ui;
    // The system layer (wisp/ui/system_layer.hpp) on `ui`: built and
    // destroyed on the UI thread, right after `ui` starts and right after
    // the self-check screen (whose dialogs it holds) is torn down.
    std::unique_ptr<ui::SystemLayer> system_layer;
    std::unique_ptr<os::Timer> storage_timer;

    // Constructed just before power_manager (which holds a
    // non-owning pointer into it -- see PowerManager::enter_standby()/
    // exit_standby()'s PowerMonitor::set_standby() hook), via its own new
    // "power.main" device lookup in App::start() -- separate from, and
    // parallel to, the existing "imu.main" lookup that App::start()
    // already did for power_manager's own `imu` parameter. Owns its own
    // internal sample Timer (see power_monitor.hpp), so -- unlike
    // storage_timer below -- there is no separate App-owned Timer object
    // for it. Torn down in App::stop() right after power_manager (which
    // points into it), same relative order as storage/storage_timer vs.
    // power_manager.
    std::unique_ptr<core::PowerMonitor> power_monitor;

    // Constructed once `ui` is known either way (started, or
    // headless), right before the self-check screen (which depends on
    // it) -- see App::start()'s Step 4. power_timer drives its
    // periodic tick() (the idle-timeout check); both are torn down in
    // App::stop(), before `buttons`/`ui`/`bus` (which power_manager
    // holds pointers/a reference into) are themselves stopped.
    std::unique_ptr<PowerManager> power_manager;
    std::unique_ptr<os::Timer> power_timer;

    // Built by App::start() right after power_manager, torn down by
    // App::stop() right after the self-check screen and before
    // power_manager -- see net_wiring.hpp.
    std::unique_ptr<NetWiring> net;

    // Notifications (scene-device spec 5), built by start() once the
    // storage worker runs and loaded on it. stop() unsubscribes and closes
    // it (no more timer saves) before the final flush, and resets it only
    // after power_timer (which posts wake-lock reminders) has stopped.
    std::unique_ptr<core::NotificationService> notifications;
    // The notification sounds over "audio_out.main" (scene-device spec
    // 5.4). Built before power_manager, whose activity hook silences it,
    // and reset after it.
    std::unique_ptr<core::TonePlayer> tones;
    // The IMU shared between the scenes (wisp.sensor) and the self-check
    // screen (scene-device spec 6.1). Built by start() before the scene
    // runtime and the screen, which hold pointers to it; stopped and reset
    // by stop() once both are gone. Follows power.state (sensor_sub).
    std::unique_ptr<core::SensorService> sensors;
    core::SubscriptionId sensor_sub = 0;
    // audio/notify_vol and sys/tz_min, kept from system.settings: the
    // router reads the volume on the posting thread (the scene task) and
    // the status shell the time zone on the UI thread, neither of which
    // may read Settings.
    std::atomic<uint8_t> notify_vol{60};
    std::atomic<int> utc_offset_min{480};
    core::SubscriptionId settings_sub = 0;
    // What a posted, tapped or acted-on notification does (wake, sound,
    // banner, its scene). Built once the scene runtime is, stopped first
    // by App::stop().
    std::unique_ptr<NotificationRouter> router;
    // The status capsule, the pull-down panel and the banners (with a
    // screen): built once networking is, stopped first by App::stop()
    // after the console, before the router and the system layer.
    os::LargeUnique<StatusShell> status_shell;
    core::SubscriptionId battery_sub = 0;
    core::SubscriptionId charging_sub = 0;
    core::SubscriptionId scene_state_sub = 0;

    // The storage worker, the scene repository and (with a screen) the
    // scene runtime: built by App::start() right after storage, reset by
    // App::stop() after networking and before the storage-refresh timer --
    // see scene_wiring.hpp.
    // In Mem::Large (scene_wiring.hpp); null without the memory for it.
    os::LargeUnique<SceneWiring> scenes;

    // The self-check screen's own teardown hook -- see
    // self_check.hpp. Constructed (via start_self_check(), posted onto
    // `ui`) only when `ui` is started; destroyed by App::stop() before
    // `ui`/`bus` are stopped, so every task/timer/subscription it owns
    // (the "wisp.sample" task, its lv_timer, its bus subscriptions,
    // the 5000ms WISP_PERF timer) is torn down while `ui`/`bus` are still
    // alive to be torn down against.
    std::unique_ptr<SelfCheck> self_check;

    // The "wisp.loop" task state for the self-check screen's "Mic test"
    // button (see loopback.cpp's run_loopback()/join_loopback()/
    // reset_loopback()). Lives here -- on this App's own Impl -- rather
    // than in a process-wide static, again so tests constructing several
    // Apps in one process never have one App's loopback state bleed into
    // another's.
    //
    // loop_mutex guards all three fields below. join_loopback() sets
    // loop_stopping under this lock as its very first action (before
    // even taking loop_task to join), so a run_loopback() call racing
    // against it is fully serialized by the same lock: it either
    // completes spawning a task entirely before loop_stopping flips (and
    // that task is then exactly the one join_loopback() goes on to join),
    // or it observes loop_stopping already true and refuses outright --
    // there is no window in which a new "wisp.loop" task gets spawned
    // that nothing will ever join -- a click triggering "Mic test"
    // concurrently with App::stop() must not be able to start a task
    // that outlives SelfCheck.
    os::Mutex loop_mutex;
    bool loop_running = false;
    bool loop_stopping = false;
    std::unique_ptr<os::Task> loop_task;

    // The one-shot "wisp.reset" task that runs a confirmed factory reset
    // (see factory_reset.hpp), joined by App::stop() before networking is
    // torn down. reset_mutex guards reset_task.
    os::Mutex reset_mutex;
    std::unique_ptr<os::Task> reset_task;
    // What starts reset_task; empty means os::Task::spawn. Tests swap in
    // one that fails (set_reset_spawner()).
    std::function<std::unique_ptr<os::Task>(const os::TaskConfig&, std::function<void()>)>
        reset_spawner;
    // A factory reset was requested and no screen has taken it up yet (see
    // take_factory_reset_request()). Kept beside the event so a request
    // made before the screen subscribes, or whose event the bus dropped,
    // still reaches the screen.
    std::atomic<bool> reset_pending{false};

    // The firmware update service (connectivity spec 6.5) over
    // Platform::firmware, built by start() before networking (the admin
    // route streams into it) and reset by stop() after networking and the
    // "wisp.fwup" task are gone. firmware_mutex guards firmware_task; see
    // firmware_confirm.hpp for the rest.
    std::unique_ptr<core::FirmwareUpdateService> firmware;
    os::Mutex firmware_mutex;
    std::unique_ptr<os::Task> firmware_task;
    // An image awaits the user's answer and no screen has taken it up yet
    // (take_firmware_request()), kept for the same reason as reset_pending.
    std::atomic<bool> firmware_pending{false};

    // The "wisp.console" task (console_task.hpp): started last by
    // App::start(), stopped first by App::stop().
    std::unique_ptr<os::Task> console_task;
    std::atomic<bool> console_stopping{false};

    bool started = false;

    // Both the constructor and destructor are declared here but defined
    // out-of-line in self_check_screen.cpp, the one TU where SelfCheck's
    // full definition is visible: `self_check` above is only
    // forward-declared in this header, and a member-wise constructor
    // body still needs every member's destructor available (for the
    // compiler-generated "destroy already-constructed members" path if
    // a later member's initialization fails) -- not just the destructor
    // itself -- so both must wait for SelfCheck to be complete.
    explicit Impl(Platform p);
    ~Impl();
};

}  // namespace wisp::app
