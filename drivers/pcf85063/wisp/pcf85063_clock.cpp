#include "wisp/drivers/pcf85063_clock.hpp"

#include <string>
#include <utility>

#include "esp_err.h"
#include "wisp/base/error.hpp"

namespace wisp::drivers {

Result<std::shared_ptr<Pcf85063Clock>> Pcf85063Clock::create(i2c_master_bus_handle_t bus,
                                                             Pcf85063ClockConfig config) {
    using R = Result<std::shared_ptr<Pcf85063Clock>>;
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
    return R::ok(std::shared_ptr<Pcf85063Clock>(new Pcf85063Clock(handle, std::move(config))));
}

Pcf85063Clock::Pcf85063Clock(pcf85063_handle_t handle, Pcf85063ClockConfig config)
    : handle_(handle) {
    info_.id = std::move(config.id);
    info_.cls = kClass;
    info_.model = std::move(config.model);
    if (!config.role.empty()) info_.caps["role"] = wisp::Value(std::move(config.role));
}

// No deinit: this device, like every other ESP HAL device Wisp
// registers, lives for the process's lifetime (pcf85063.h provides no
// pcf85063_deinit()).
Pcf85063Clock::~Pcf85063Clock() = default;

Result<int64_t> Pcf85063Clock::get_unix() {
    int64_t seconds = 0;
    esp_err_t err = pcf85063_get_unix(handle_, &seconds);
    if (err != ESP_OK) {
        return Result<int64_t>::err(ErrorCode::DeviceFailure,
                                    std::string("pcf85063_get_unix: ") + esp_err_to_name(err));
    }
    return Result<int64_t>::ok(seconds);
}

Result<void> Pcf85063Clock::set_unix(int64_t seconds) {
    esp_err_t err = pcf85063_set_unix(handle_, seconds);
    if (err != ESP_OK) {
        return Result<void>::err(ErrorCode::DeviceFailure,
                                 std::string("pcf85063_set_unix: ") + esp_err_to_name(err));
    }
    return Result<void>::ok();
}

}  // namespace wisp::drivers
