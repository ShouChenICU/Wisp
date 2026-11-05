#pragma once

// ESP-IDF wisp::hal::Touch backed by an esp_lcd_touch handle (e.g. the one
// esp_board_manager's lcd_touch device hands out -- see
// boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp). The chip model and the
// coordinate range are passed in by the board (AGENTS.md's "板子无关"
// rule); orientation (swap/mirror) is the esp_lcd_touch driver's job,
// configured when the handle was created.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_lcd_touch.h"
#include "wisp/base/result.hpp"
#include "wisp/hal/touch.hpp"

namespace wisp::port::esp {

struct EspTouchConfig {
    std::string model;  // DeviceInfo::model, e.g. the touch controller chip
    // Reported x/y are clamped to [0, width - 1] x [0, height - 1]; a
    // non-positive width/height means "range unknown" (no clamping).
    int width = 0;
    int height = 0;
};

class EspTouch : public wisp::hal::Touch {
public:
    EspTouch(esp_lcd_touch_handle_t handle, EspTouchConfig config);

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    // esp_lcd_touch_read_data() (polls the controller), then
    // esp_lcd_touch_get_data() for at most one point (caps "max_points"
    // is 1).
    Result<std::vector<wisp::hal::TouchPoint>> read() override;

    // esp_lcd_touch_enter_sleep() at Standby/DeepStandby and
    // exit_sleep() at Active/Dim, if the underlying driver implements
    // them -- only then do the caps declare "standby": "off". Returns
    // ErrorCode::Unsupported otherwise (true for every touch driver this
    // codebase has ported so far). A level that maps to the current
    // state sends nothing.
    Result<void> set_power(wisp::hal::PowerLevel level) override;

private:
    // Whether the driver implements both sleep hooks.
    bool can_sleep() const;

    wisp::hal::DeviceInfo info_;
    esp_lcd_touch_handle_t handle_;
    EspTouchConfig config_;
    std::atomic<bool> powered_{true};
};

}  // namespace wisp::port::esp
