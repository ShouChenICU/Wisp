// ESP32-S3 firmware entry point (Task 15 -- see
// .superpowers/sdd/2026-11-01-foundation/task-15-brief.md): wires up the
// ESP-only pieces of wisp::app::Platform (EspLogSink, RamSettingsBackend,
// emit_line/request_power_off/heap_info) around whichever board
// firmware/CMakeLists.txt's WISP_BOARD selected, then runs the same
// wisp::app::App boot sequence the PC simulator runs (see
// simulator/main.cpp and wisp/app/app.hpp). This board (Task 15) has zero
// devices (see boards/waveshare_amoled_216/board.cpp), so App::start()
// takes the headless path -- no UI, no self-check screen.

#include <cstdio>
#include <memory>
#include <string>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wisp/app/app.hpp"
#include "wisp/app/board.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"
#include "wisp/port/esp/log_esp.hpp"
#include "wisp/port/esp/ram_settings_backend.hpp"

extern "C" void app_main() {
    // Static storage duration: add_sink() requires `sink` to outlive the
    // logging subsystem (see wisp/core/log.hpp), and this process never
    // tears it down.
    static wisp::port::esp::EspLogSink log_sink;
    wisp::log::add_sink(&log_sink);

    wisp::app::Platform platform;
    platform.board = wisp::board::create_board();
    // Fix round 1, M7: this board has zero devices, so App::start() (see
    // app.cpp) never actually reaches the `if (display)` branch that is
    // the only place it dereferences Platform::settings -- the previous
    // comment here claiming it does so "unconditionally" was wrong.
    // Still wired up (per the Task 15 controller ruling) because
    // Platform requires *some* non-null settings backend regardless,
    // and to not leave a null-pointer trap for the next board that adds
    // a display. The NVS-backed replacement is Task 18's job -- see
    // wisp/port/esp/ram_settings_backend.hpp.
    platform.settings = std::make_unique<wisp::port::esp::RamSettingsBackend>();
    platform.emit_line = [](const std::string& line) {
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    };
    platform.request_power_off = [] { esp_restart(); };
    platform.heap_info = [] {
        wisp::Map info;
        info["internal_free"] = static_cast<int64_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        info["large_free"] = static_cast<int64_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return wisp::Value(std::move(info));
    };

    // Static storage duration (per this task's brief): app_main() returns
    // by deleting its own task below rather than by a normal function
    // return, so nothing would actually run `app`'s destructor either
    // way -- FreeRTOS's vTaskDelete() frees the task's stack without C++
    // unwinding it -- but making the lifetime explicit here documents
    // the intent rather than relying on that FreeRTOS implementation
    // detail.
    static wisp::app::App app(std::move(platform));
    auto started = app.start();
    if (!started) {
        WLOG_E("main", "App::start() failed: %s", started.error().message.c_str());
    }

    vTaskDelete(nullptr);
}
