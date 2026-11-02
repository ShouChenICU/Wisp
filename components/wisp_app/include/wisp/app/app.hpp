#pragma once

// Boot orchestration: wires every wisp_core service (and, when a display
// is present, wisp_ui) together behind one App object that both the PC
// simulator (Task 13) and the ESP firmware (Task 15) construct from a
// Platform. See AGENTS.md's layering rule -- wisp_app sits above wisp_ui,
// so this header may include wisp_core/wisp_ui headers freely, but (like
// every portable layer) must never include platform headers.
//
// App::start() follows spec 6.1's boot sequence:
//   1. (Re)initializes logging (wisp::log::init()).
//   2. Calls Platform::board's init(), then fills in a
//      DeviceState::Ok report entry for every device that's now in the
//      registry but that init() didn't already record an entry for
//      (board-recorded entries, e.g. a Failed or Missing device, are left
//      untouched) -- see board.hpp.
//   3. Starts the EventBus, then StorageManager::init() (a failure here
//      sets the "storage" boot-report note to "readonly" and boot
//      continues read-only rather than failing), starts a periodic
//      2000ms StorageManager::refresh() timer, and starts ButtonService.
//   4. If "display.main" is registered: applies the `display` settings
//      namespace's `brightness` key (default 200) to it, starts a
//      UiRuntime over it (plus "touch.main" if also registered).
//      Otherwise records the "mode": "headless" boot-report note.
//      Either way, constructs the PowerMonitor (Task 19a, spec 6.3/6.7)
//      and starts its own internal sample Timer, then constructs the
//      PowerManager (Task 18b, spec 6.7, handed a pointer to the
//      PowerMonitor just built) and starts its periodic idle-check
//      Timer, then -- if a UiRuntime was started -- builds the
//      self-check screen (see src/self_check.hpp's start_self_check(),
//      spec 8) on it, blocking until that has actually run. PowerManager
//      is constructed before the self-check screen specifically because
//      the screen's power-key handling calls into it (see
//      src/self_check_screen.cpp).
//   5. Records the elapsed boot_ms (now that step 4 -- the self-check
//      screen included -- has actually finished, per spec 6.1; the
//      screen's header, which already rendered once with boot_ms still
//      at its default, is then re-rendered with this final value), emits
//      "WISP_BOOT_REPORT <json>" and "WISP_MANIFEST <json>" (in that
//      order) via Platform::emit_line, then publishes
//      "system.boot.completed".

#include <functional>
#include <memory>
#include <string>

#include "wisp/app/board.hpp"
#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/boot_report.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/storage_manager.hpp"

namespace wisp::ui {
class UiRuntime;
}

namespace wisp::core {
class ButtonService;
}

namespace wisp::app {

class PowerManager;

// Everything App needs that varies by port/board. Constructed by the
// simulator main (Task 13) or the ESP firmware main (Task 15) and handed
// to App by value (App takes ownership of `board`/`settings`).
struct Platform {
    std::unique_ptr<Board> board;
    std::unique_ptr<core::SettingsBackend> settings;

    // Writes one whole line of diagnostics output (e.g. to the serial
    // console or stdout) -- no trailing newline expected to be added by
    // the caller or present in `line`; the implementation adds whatever
    // line ending its destination needs.
    std::function<void(const std::string& line)> emit_line;

    // Fallback shutdown, for the self-check UI's power-off handling (Task
    // 12) when no "power.*" device capable of powering itself off is
    // registered. May be empty if the platform has no such fallback.
    std::function<void()> request_power_off;

    // Returns heap diagnostics, e.g. {"internal_free": ..., "large_free":
    // ...}, for the self-check UI (Task 12). May be empty (no heap
    // diagnostics available).
    std::function<Value()> heap_info;

    // Task 18b/spec 6.7: called by wisp::app::PowerManager with `true`
    // when entering standby, `false` when exiting. On real hardware,
    // the implementation keeps dynamic frequency scaling (DFS) always
    // on and only toggles automatic tickless light sleep on top of it
    // (see firmware/main/main.cpp's esp_pm_configure() call); the
    // simulator just logs. May be empty (no platform power hook at
    // all -- PowerManager still does everything else: panel/touch
    // sleep, UI pause, button poll widening).
    std::function<void(bool standby)> set_standby;

    std::string firmware_version = "0.1.0";
};

class App {
public:
    explicit App(Platform platform);

    // Calls stop() if start() succeeded and stop() wasn't already called.
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Runs the boot sequence described in the file header above. Fails
    // with ErrorCode::Busy, doing nothing else, if this App is already
    // started (i.e. a previous start() succeeded and stop() hasn't been
    // called since) -- start() is not re-entrant/restart-on-call; call
    // stop() first to run the sequence again.
    Result<void> start();

    // Stops (in reverse order) the storage-refresh timer, ButtonService,
    // UiRuntime (if one was started) and EventBus. Idempotent, and safe
    // to call even if start() was never called or didn't succeed -- only
    // what was actually started is torn down. Stopping the UiRuntime
    // here (rather than leaving it to the destructor of some other
    // object) matters because LVGL 9.5 under this build only supports one
    // started UiRuntime per process at a time (see ui_runtime.hpp): tests
    // that construct Apps one after another rely on each one's stop()
    // (direct or via ~App()) fully releasing LVGL before the next App
    // starts its own UiRuntime.
    void stop();

    core::DeviceRegistry& registry();
    core::BootReport& report();
    core::EventBus& bus();
    core::StorageManager& storage();
    core::ButtonService& buttons();

    // nullptr in headless mode (no "display.main" device registered).
    ui::UiRuntime* ui();

    // Task 18b: non-null for the whole lifetime of a started App (see
    // start()'s Step 4) -- constructed regardless of headless mode,
    // since the idle-timeout/platform-power-hook half of PowerManager's
    // job (see power_manager.hpp) is useful even without a display.
    // Used by the self-check screen's power-key handling (see
    // src/self_check_screen.cpp) to route screen on/off through it
    // instead of calling hal::Display::set_power() directly. nullptr
    // before start() (or after stop()).
    PowerManager* power_manager();

    // The Platform this App was constructed from (Task 12 controller
    // ruling: the self-check screen needs it for request_power_off,
    // heap_info, firmware_version, board->name() and the settings
    // backend -- see src/self_check_screen.cpp). Valid for the whole
    // lifetime of this App, independent of start()/stop().
    const Platform& platform() const;

private:
    // Grants loopback.cpp's run_loopback()/join_loopback() (spec 8's
    // "录音回放" button) access to a per-App task handle stored in Impl,
    // so each App's "wisp.loop" task is tracked on that App itself
    // (torn down by App::stop() before EventBus/UiRuntime stop -- see
    // app.hpp's Lifetime note) rather than in a process-wide static.
    friend void run_loopback(App&, std::function<void(const char* status)>);
    friend void join_loopback(App&);
    // Clears the "stopping" flag join_loopback() sets, so loopback works
    // again after a start()/stop()/start() cycle on the same App -- see
    // app.cpp's start() and loopback.cpp.
    friend void reset_loopback(App&);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wisp::app
