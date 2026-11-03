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
#include "self_check.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/core/button_service.hpp"
#include "wisp/core/llm_config.hpp"
#include "wisp/core/power_monitor.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

struct App::Impl {
    Platform platform;
    core::DeviceRegistry registry;
    core::BootReport report;
    core::EventBus bus;
    core::StorageManager storage;
    core::ButtonService buttons;
    core::LlmConfigService llm{storage};
    // Brightness goes through PowerManager so a change made in standby
    // waits for the wake-up (spec 5.4). power_manager is only reset after
    // the HTTP server and the net task have stopped (App::stop()), so no
    // caller races with it going away. Every stored change is published
    // as system.settings (spec 8.3); the self-check screen rebuilds its
    // text from it when the language changes.
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

    // The self-check screen's own teardown hook -- see
    // self_check.hpp. Constructed (via start_self_check(), posted onto
    // `ui`) only when `ui` is started; destroyed by App::stop() before
    // `ui`/`bus` are stopped, so every task/timer/subscription it owns
    // (the "wisp.sample" task, its lv_timer, its bus subscriptions,
    // the 5000ms WISP_PERF timer) is torn down while `ui`/`bus` are still
    // alive to be torn down against.
    std::unique_ptr<SelfCheck> self_check;

    // The "wisp.loop" task state for the self-check screen's "录音回放"
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
    // that nothing will ever join -- a click triggering "录音回放"
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
