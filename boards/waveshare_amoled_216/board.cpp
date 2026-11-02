// Board definition for the Waveshare ESP32-S3-Touch-AMOLED-2.16 (see
// docs/hardware/waveshare_amoled_216.md).
//
// Buses and the esp_lcd-level display/touch devices are brought up by
// esp_board_manager (bmgr) from this directory's board_*.yaml +
// setup_device.c (generated into firmware/components/gen_bmgr_codes/ by
// `idf.py bmgr -c ../boards -b waveshare_amoled_216`). This file then
// wraps bmgr's handles in Wisp HAL devices and registers them:
//   - display_lcd -> display.main (wisp::port::esp::EspDisplay, CO5300)
//   - lcd_touch   -> touch.main   (wisp::port::esp::EspTouch, CST9217)
// Power/IMU/RTC/buttons/audio/SD are added by later tasks.
//
// Every failure is recorded in the boot report and bring-up continues
// with the next device -- a missing panel or touch controller degrades
// the device (App::start() falls back to headless without display.main),
// it never blocks boot (AGENTS.md's device model rule).
//
// wisp::board::create_board() (declared in
// components/wisp_app/include/wisp/app/board.hpp) is defined here, once,
// for whichever single board a given firmware binary is built for --
// selected at CMake configure time by firmware/CMakeLists.txt's
// WISP_BOARD cache variable, which picks this very file as one of
// firmware/main's sources.

#include "wisp/app/board.hpp"

#include <memory>
#include <string>
#include <utility>

#include "dev_display_lcd.h"
#include "dev_lcd_touch.h"
#include "esp_board_manager.h"
#include "esp_board_manager_err.h"
#include "esp_err.h"
#include "wisp/core/log.hpp"
#include "wisp/port/esp/esp_display.hpp"
#include "wisp/port/esp/esp_touch.hpp"

namespace {

constexpr const char* kTag = "board";

// Device names as declared in board_devices.yaml.
constexpr const char* kBmgrDisplay = "display_lcd";
constexpr const char* kBmgrTouch = "lcd_touch";

// Panel facts not carried by bmgr's config (see the hardware doc).
constexpr int kCornerRadius = 40;
// CO5300 partial updates must start/end on even coordinates.
constexpr int kAreaAlign = 2;

std::string err_name(esp_err_t err) {
    if (err >= ESP_BOARD_ERR_BASE) {
        return esp_board_manager_err_to_name(err);
    }
    return esp_err_to_name(err);
}

void record_failed(wisp::core::BootReport& report, const char* id, wisp::hal::DeviceClass cls,
                   std::string reason) {
    WLOG_E(kTag, "%s failed: %s", id, reason.c_str());
    report.record({id, cls, wisp::core::DeviceState::Failed, std::move(reason)});
}

void add_device(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report,
                std::shared_ptr<wisp::hal::Device> device) {
    const wisp::hal::DeviceInfo& info = device->info();
    auto added = registry.add(device);
    if (!added) {
        record_failed(report, info.id.c_str(), info.cls, "registry.add: " + added.error().message);
    }
}

void init_display(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) {
    constexpr const char* id = "display.main";
    constexpr auto cls = wisp::hal::DeviceClass::Display;

    void* handle = nullptr;
    esp_err_t err = esp_board_manager_get_device_handle(kBmgrDisplay, &handle);
    if (err != ESP_OK || handle == nullptr) {
        record_failed(report, id, cls, "bmgr display_lcd handle: " + err_name(err));
        return;
    }
    void* config = nullptr;
    err = esp_board_manager_get_device_config(kBmgrDisplay, &config);
    if (err != ESP_OK || config == nullptr) {
        record_failed(report, id, cls, "bmgr display_lcd config: " + err_name(err));
        return;
    }
    auto* handles = static_cast<dev_display_lcd_handles_t*>(handle);
    auto* cfg = static_cast<const dev_display_lcd_config_t*>(config);

    wisp::port::esp::EspDisplayConfig dc;
    dc.model = "CO5300";
    dc.width = cfg->lcd_width;
    dc.height = cfg->lcd_height;
    dc.shape = "rounded_rect";
    dc.corner_radius = kCornerRadius;
    dc.panel = "amoled";
    dc.area_align = kAreaAlign;
    dc.qspi_cmd_framing = true;  // io_spi_config.flags.quad_mode
    dc.swap_rgb565_bytes = cfg->frame_format == DEV_DISPLAY_LCD_FRAME_FORMAT_RGB565_BE;

    auto display =
        wisp::port::esp::EspDisplay::create(handles->panel_handle, handles->io_handle, dc);
    if (!display) {
        record_failed(report, id, cls, display.error().message);
        return;
    }
    add_device(registry, report, display.value());
}

void init_touch(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) {
    constexpr const char* id = "touch.main";
    constexpr auto cls = wisp::hal::DeviceClass::Touch;

    void* handle = nullptr;
    esp_err_t err = esp_board_manager_get_device_handle(kBmgrTouch, &handle);
    if (err != ESP_OK || handle == nullptr) {
        record_failed(report, id, cls, "bmgr lcd_touch handle: " + err_name(err));
        return;
    }
    auto* handles = static_cast<dev_lcd_touch_handles_t*>(handle);
    if (handles->touch_handle == nullptr) {
        record_failed(report, id, cls, "bmgr lcd_touch: null touch handle");
        return;
    }

    // Coordinate range: the panel's (480x480; the touch driver applies
    // the swap_xy/mirror_y that setup_device.c configures).
    int width = 0;
    int height = 0;
    void* config = nullptr;
    if (esp_board_manager_get_device_config(kBmgrDisplay, &config) == ESP_OK && config) {
        auto* cfg = static_cast<const dev_display_lcd_config_t*>(config);
        width = cfg->lcd_width;
        height = cfg->lcd_height;
    }

    wisp::port::esp::EspTouchConfig tc;
    tc.model = "CST9217";
    tc.width = width;
    tc.height = height;
    add_device(registry, report,
               std::make_shared<wisp::port::esp::EspTouch>(handles->touch_handle, tc));
}

class WaveshareAmoled216Board : public wisp::app::Board {
public:
    std::string name() const override { return "waveshare_amoled_216"; }

    void init(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) override {
        // esp_board_manager_init() only fails if a *peripheral* (bus) or
        // its own setup fails -- an individual device that fails to come
        // up is logged by bmgr and simply has no handle afterwards, which
        // init_display()/init_touch() below report per device.
        esp_err_t err = esp_board_manager_init();
        if (err != ESP_OK) {
            std::string reason = "esp_board_manager_init: " + err_name(err);
            record_failed(report, "display.main", wisp::hal::DeviceClass::Display, reason);
            record_failed(report, "touch.main", wisp::hal::DeviceClass::Touch, reason);
            return;
        }

        init_display(registry, report);
        init_touch(registry, report);
    }
};

}  // namespace

namespace wisp::board {

std::unique_ptr<wisp::app::Board> create_board() {
    return std::make_unique<WaveshareAmoled216Board>();
}

}  // namespace wisp::board
