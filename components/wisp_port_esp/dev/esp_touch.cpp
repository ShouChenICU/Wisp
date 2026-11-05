#include "wisp/port/esp/esp_touch.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "wisp/base/error.hpp"

namespace wisp::port::esp {

EspTouch::EspTouch(esp_lcd_touch_handle_t handle, EspTouchConfig config)
    : handle_(handle), config_(std::move(config)) {
    info_.id = "touch.main";
    info_.cls = kClass;
    info_.model = config_.model;
    info_.caps = wisp::Properties{{"max_points", 1}};
    if (can_sleep()) {
        info_.caps["standby"] = wisp::Value("off");
    }
}

// esp_lcd_touch_enter_sleep()/exit_sleep() themselves ESP_LOGE and
// return ESP_FAIL when the driver's vtable has no implementation (see
// components/espressif__esp_lcd_touch/esp_lcd_touch.c) -- every standby
// transition would otherwise spam an error log on a controller (e.g.
// this board's CST9217 -- see
// docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md) that simply
// never wired one up. `enter_sleep`/`exit_sleep` are plain public struct
// fields (see esp_lcd_touch.h's esp_lcd_touch_s), so checking them
// directly avoids ever making that call at all when unsupported.
bool EspTouch::can_sleep() const {
    return handle_ != nullptr && handle_->enter_sleep != nullptr && handle_->exit_sleep != nullptr;
}

Result<std::vector<wisp::hal::TouchPoint>> EspTouch::read() {
    using R = Result<std::vector<wisp::hal::TouchPoint>>;
    if (handle_ == nullptr) {
        return R::err(ErrorCode::DeviceFailure, "null touch handle");
    }
    esp_err_t err = esp_lcd_touch_read_data(handle_);
    if (err != ESP_OK) {
        return R::err(ErrorCode::DeviceFailure, std::string("read_data: ") + esp_err_to_name(err));
    }

    esp_lcd_touch_point_data_t point = {};
    uint8_t count = 0;
    err = esp_lcd_touch_get_data(handle_, &point, &count, 1);
    if (err != ESP_OK) {
        return R::err(ErrorCode::DeviceFailure, std::string("get_data: ") + esp_err_to_name(err));
    }

    std::vector<wisp::hal::TouchPoint> points;
    if (count > 0) {
        // A non-positive width/height means the range is unknown: pass
        // the driver's coordinates through unclamped.
        int x = point.x;
        int y = point.y;
        if (config_.width > 0) {
            x = std::clamp(x, 0, config_.width - 1);
        }
        if (config_.height > 0) {
            y = std::clamp(y, 0, config_.height - 1);
        }
        wisp::hal::TouchPoint pt;
        pt.x = static_cast<int16_t>(x);
        pt.y = static_cast<int16_t>(y);
        pt.id = point.track_id;
        points.push_back(pt);
    }
    return R::ok(std::move(points));
}

Result<void> EspTouch::set_power(wisp::hal::PowerLevel level) {
    using R = Result<void>;
    if (!can_sleep()) {
        return R::err(ErrorCode::Unsupported, "touch controller has no sleep mode");
    }
    const bool on = wisp::hal::powered_at(wisp::hal::StandbyPolicy::Off, level);
    if (on == powered_.load()) {
        return R::ok();
    }
    esp_err_t err = on ? esp_lcd_touch_exit_sleep(handle_) : esp_lcd_touch_enter_sleep(handle_);
    if (err != ESP_OK) {
        return R::err(ErrorCode::DeviceFailure,
                      std::string(on ? "exit_sleep: " : "enter_sleep: ") + esp_err_to_name(err));
    }
    powered_.store(on);
    return R::ok();
}

}  // namespace wisp::port::esp
