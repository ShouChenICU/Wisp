// ESP32-S3 firmware entry point: wires up the ESP-only pieces of
// wisp::app::Platform (EspLogSink, wisp::port::esp::NvsSettingsBackend,
// emit_line/request_power_off/heap_info, and the networking services:
// EspHttpServer/EspHttpClient and the mapped web asset pack) around
// whichever board
// firmware/CMakeLists.txt's WISP_BOARD selected, then runs the same
// wisp::app::App boot sequence the PC simulator runs (see
// simulator/main.cpp and wisp/app/app.hpp). The board registers
// display.main/touch.main (see boards/waveshare_amoled_216/board.cpp),
// so App::start() brings up the UI and the self-check screen; if the
// panel fails to come up it falls back to the headless path instead.

#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_pm.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wisp/app/app.hpp"
#include "wisp/app/board.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"
#include "wisp/port/esp/console_esp.hpp"
#include "wisp/port/esp/esp_http_client.hpp"
#include "wisp/port/esp/esp_http_server.hpp"
#include "wisp/port/esp/log_esp.hpp"
#include "wisp/port/esp/settings_nvs.hpp"

namespace {

// Spec 6.7: DFS (dynamic frequency scaling) is always on; only
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
    // This file is shared by every ESP
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

// The web UI's WispPack (connectivity spec 2.3), mapped read-only from
// the "assets" partition (found by name, spec 3.2) and left mapped for the
// life of the firmware, so the admin server can send files straight from
// flash. Only the pack itself is mapped, sized from its header (bytes
// 12..15: total_bytes, see tools/pack_assets.py); an erased or foreign
// partition yields an empty span and the API is served without a page.
std::span<const uint8_t> map_assets() {
    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (part == nullptr) {
        WLOG_W("main", "no \"assets\" partition; admin page unavailable");
        return {};
    }
    uint8_t header[16] = {};
    if (esp_partition_read(part, 0, header, sizeof(header)) != ESP_OK ||
        std::memcmp(header, "WPAK", 4) != 0) {
        WLOG_W("main", "\"assets\" holds no web pack (flash it with idf.py flash)");
        return {};
    }
    uint32_t total = 0;
    std::memcpy(&total, header + 12, sizeof(total));  // little-endian, like the chip
    if (total < sizeof(header) || total > part->size) {
        WLOG_W("main", "\"assets\" pack size %u is out of range", static_cast<unsigned>(total));
        return {};
    }
    const void* data = nullptr;
    esp_partition_mmap_handle_t handle = 0;
    esp_err_t err = esp_partition_mmap(part, 0, total, ESP_PARTITION_MMAP_DATA, &data, &handle);
    if (err != ESP_OK) {
        WLOG_W("main", "mapping \"assets\" failed: %s", esp_err_to_name(err));
        return {};
    }
    return {static_cast<const uint8_t*>(data), total};
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
    // esp_fill_random() is a true RNG only while the RF subsystem (Wi-Fi/BT)
    // is enabled or between bootloader_random_enable()/_disable(); otherwise
    // its output is merely pseudo-random. Precondition for every caller:
    // the radio is on. All current draws -- pairing codes and tokens, and
    // the hotspot password from WifiService::start_provisioning(), whether
    // asked for over HTTP, from the screen or from the serial console's
    // "wifi ap on" -- only exist when a net.wifi device is registered, and
    // its driver starts the radio during board init. bootloader_random_
    // enable() is deliberately not wrapped around draws: while enabled it
    // disturbs Wi-Fi and the ADC. A draw that can happen with the radio off
    // has to enable that source itself.
    platform.random = [](uint8_t* out, size_t len) { esp_fill_random(out, len); };
    platform.reboot = [] { esp_restart(); };
    // Serial console (connectivity spec 8.2). Created before App::start(),
    // and so before any task logs heavily: the stdio VFS switches to the
    // console driver here.
    static std::unique_ptr<wisp::port::esp::EspConsole> console =
        wisp::port::esp::EspConsole::create();
    if (console) {
        platform.read_line = [](std::string& line, uint32_t timeout_ms) {
            return console->read_line(line, timeout_ms) ? wisp::app::LineRead::Line
                                                        : wisp::app::LineRead::Timeout;
        };
    }
    // Admin backend (connectivity spec 6): App serves it on http_port
    // (8900) while the station has an address or the hotspot is up.
    platform.http_server = std::make_unique<wisp::port::esp::EspHttpServer>();
    platform.http_client = std::make_unique<wisp::port::esp::EspHttpClient>();
    platform.assets = map_assets();
    // The hotspot's port-80 captive redirect (spec 4.3): a second httpd
    // instance, so it needs its own control port; it only ever redirects,
    // so it gets few sockets, a small stack and no WebSocket. Both
    // instances plus the captive DNS socket are counted into
    // CONFIG_LWIP_MAX_SOCKETS (firmware/sdkconfig.defaults).
    wisp::port::esp::EspHttpServerConfig captive;
    captive.ctrl_port = static_cast<uint16_t>(wisp::port::esp::EspHttpServerConfig{}.ctrl_port + 1);
    captive.max_open_sockets = 4;
    captive.stack_bytes = 4096;
    captive.websocket = false;
    platform.captive_server = std::make_unique<wisp::port::esp::EspHttpServer>(captive);

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
