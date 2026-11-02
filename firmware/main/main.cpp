// ESP32-S3 firmware entry point (Task 15 -- see
// .superpowers/sdd/2026-11-01-foundation/task-15-brief.md): wires up the
// ESP-only pieces of wisp::app::Platform (EspLogSink,
// wisp::port::esp::NvsSettingsBackend -- Task 18, replacing Task 15's RAM
// -only placeholder -- emit_line/request_power_off/heap_info) around
// whichever board firmware/CMakeLists.txt's WISP_BOARD selected, then
// runs the same wisp::app::App boot sequence the PC simulator runs (see
// simulator/main.cpp and wisp/app/app.hpp). The board registers
// display.main/touch.main (Task 16 -- see
// boards/waveshare_amoled_216/board.cpp), so App::start() brings up the
// UI and the self-check screen; if the panel fails to come up it falls
// back to the headless path instead.

#include <cstdio>
#include <memory>
#include <string>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wisp/app/app.hpp"
#include "wisp/app/board.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"
#include "wisp/port/esp/log_esp.hpp"
#include "wisp/port/esp/settings_nvs.hpp"

namespace {

// Task 18b/spec 6.7: DFS (dynamic frequency scaling) is always on; only
// automatic tickless light sleep is toggled, by PowerManager via
// Platform::set_standby -- entering standby turns it on, exiting turns
// it back off. Requires CONFIG_PM_ENABLE and
// CONFIG_FREERTOS_USE_TICKLESS_IDLE (firmware/sdkconfig.defaults);
// CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION there additionally keeps light
// sleep from firing while a USB-Serial-JTAG debug session is actually
// connected, without this code needing to know or care (see
// docs/hardware/waveshare_amoled_216.md).
void apply_pm_config(bool light_sleep_enable) {
    esp_pm_config_t cfg = {};
    // Task 18b review round 1 (M3): this file is shared by every ESP
    // board firmware/CMakeLists.txt's WISP_BOARD can select, not just
    // this board's S3 (max 240MHz) -- a different chip (e.g. an C3,
    // whose max is 160MHz) would fail esp_pm_configure() outright if
    // this stayed hard-coded to 240. CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ is
    // each target's own Kconfig-validated default max frequency.
    cfg.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    cfg.min_freq_mhz = 80;
    cfg.light_sleep_enable = light_sleep_enable;
    esp_err_t err = esp_pm_configure(&cfg);
    if (err != ESP_OK) {
        WLOG_W("main", "esp_pm_configure(light_sleep_enable=%d) failed: %s", light_sleep_enable,
               esp_err_to_name(err));
    }
}

}  // namespace

extern "C" void app_main() {
    // Static storage duration: add_sink() requires `sink` to outlive the
    // logging subsystem (see wisp/core/log.hpp), and this process never
    // tears it down.
    static wisp::port::esp::EspLogSink log_sink;
    wisp::log::add_sink(&log_sink);

    wisp::app::Platform platform;
    platform.board = wisp::board::create_board();
    // App::start() (see app.cpp) reads Platform::settings in its
    // `if (display)` branch (e.g. the stored display brightness), which
    // this board now reaches once display.main is registered. Backed by
    // NVS -- see wisp/port/esp/settings_nvs.hpp; create() never returns
    // null (even if NVS itself fails to initialize) so Platform::settings
    // is always safe to dereference.
    platform.settings = wisp::port::esp::NvsSettingsBackend::create();
    platform.emit_line = [](const std::string& line) {
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    };
    // Fallback for the self-check UI's power-off confirm dialog (see
    // self_check_screen.cpp's do_power_off()): reached only when the
    // board's own Power::power_off() either doesn't exist or just
    // failed. The user already confirmed shutdown at that point, so
    // restarting the device (the old behavior) is the opposite of what
    // they asked for; enter deep sleep with no wakeup source configured
    // instead -- the closest approximation of "off" this chip has, and
    // generic across every ESP32 variant (no per-chip wakeup config).
    platform.request_power_off = [] { esp_deep_sleep_start(); };
    platform.heap_info = [] {
        wisp::Map info;
        info["internal_free"] = static_cast<int64_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        info["large_free"] = static_cast<int64_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return wisp::Value(std::move(info));
    };
    platform.set_standby = [](bool standby) { apply_pm_config(/*light_sleep_enable=*/standby); };
    // The single source for this (rather than a literal duplicated here
    // and in the build config): esp_app_desc_t is filled in at build
    // time from PROJECT_VER (itself git-describe by default -- see the
    // IDF build system docs), so this always reports exactly what was
    // actually flashed.
    platform.firmware_version = esp_app_get_description()->version;

    // DFS always on; light sleep off until the first enter_standby()
    // (see apply_pm_config()'s comment above).
    apply_pm_config(/*light_sleep_enable=*/false);

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
