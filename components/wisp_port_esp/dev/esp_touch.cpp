#include "wisp/port/esp/esp_touch.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "esp_err.h"
#include "wisp/base/error.hpp"

namespace wisp::port::esp {

EspTouch::EspTouch(esp_lcd_touch_handle_t handle, EspTouchConfig config)
    : handle_(handle), config_(std::move(config)) {
    info_.id = "touch.main";
    info_.cls = kClass;
    info_.model = config_.model;
    info_.caps = wisp::Properties{{"max_points", 1}};
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

}  // namespace wisp::port::esp
