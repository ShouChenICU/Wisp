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
//      UiRuntime over it (plus "touch.main" if also registered), and
//      posts build_self_check() (see src/self_check.hpp; an empty
//      placeholder until Task 12) onto it. Otherwise records the "mode":
//      "headless" boot-report note.
//   5. Records the elapsed boot_ms, emits "WISP_BOOT_REPORT <json>" and
//      "WISP_MANIFEST <json>" (in that order) via Platform::emit_line,
//      then publishes "system.boot.completed".

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

namespace wisp::app {

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

    // nullptr in headless mode (no "display.main" device registered).
    ui::UiRuntime* ui();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wisp::app
