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

#include <memory>
#include <utility>

#include "self_check.hpp"
#include "wisp/app/app.hpp"
#include "wisp/core/button_service.hpp"
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

    // Only constructed by start() -- see App::start()/App::stop().
    std::unique_ptr<ui::UiRuntime> ui;
    std::unique_ptr<os::Timer> storage_timer;

    // The self-check screen's own teardown hook (Task 12) -- see
    // self_check.hpp. Constructed (via start_self_check(), posted onto
    // `ui`) only when `ui` is started; destroyed by App::stop() before
    // `ui`/`bus` are stopped, so every task/timer/subscription it owns
    // (the "wisp.sample" task, its lv_timer, the "button." subscription,
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
    // that nothing will ever join (fix round 1, issue 1: a click
    // triggering "录音回放" concurrently with App::stop() must not be
    // able to start a task that outlives SelfCheck).
    os::Mutex loop_mutex;
    bool loop_running = false;
    bool loop_stopping = false;
    std::unique_ptr<os::Task> loop_task;

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
