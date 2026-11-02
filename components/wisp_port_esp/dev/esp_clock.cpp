#include "wisp/port/esp/esp_clock.hpp"

#include <string>
#include <utility>

#include "esp_err.h"
#include "wisp/base/error.hpp"

namespace wisp::port::esp {

Result<std::shared_ptr<EspClock>> EspClock::create(i2c_master_bus_handle_t bus, std::string model) {
    using R = Result<std::shared_ptr<EspClock>>;
    if (bus == nullptr) {
        return R::err(ErrorCode::InvalidArgument, "null i2c bus handle");
    }

    pcf85063_handle_t handle = nullptr;
    esp_err_t err = pcf85063_init(bus, &handle);
    if (err != ESP_OK) {
        return R::err(ErrorCode::DeviceFailure,
                      std::string("pcf85063_init: ") + esp_err_to_name(err));
    }
    // Not make_shared: the constructor is private.
    return R::ok(std::shared_ptr<EspClock>(new EspClock(handle, std::move(model))));
}

EspClock::EspClock(pcf85063_handle_t handle, std::string model) : handle_(handle) {
    info_.id = "clock.main";
    info_.cls = kClass;
    info_.model = std::move(model);
}

// No deinit: this device, like every other ESP HAL device Wisp
// registers, lives for the process's lifetime (drivers/pcf85063 provides
// no pcf85063_deinit()).
EspClock::~EspClock() = default;

Result<int64_t> EspClock::get_unix() {
    int64_t seconds = 0;
    esp_err_t err = pcf85063_get_unix(handle_, &seconds);
    if (err != ESP_OK) {
        return Result<int64_t>::err(ErrorCode::DeviceFailure,
                                    std::string("pcf85063_get_unix: ") + esp_err_to_name(err));
    }
    return Result<int64_t>::ok(seconds);
}

Result<void> EspClock::set_unix(int64_t seconds) {
    esp_err_t err = pcf85063_set_unix(handle_, seconds);
    if (err != ESP_OK) {
        return Result<void>::err(ErrorCode::DeviceFailure,
                                 std::string("pcf85063_set_unix: ") + esp_err_to_name(err));
    }
    return Result<void>::ok();
}

}  // namespace wisp::port::esp
