#include "wisp/port/esp/esp_power.hpp"

#include <string>
#include <utility>

#include "esp_err.h"
#include "wisp/base/error.hpp"

namespace wisp::port::esp {

Result<std::shared_ptr<EspPower>> EspPower::create(i2c_master_bus_handle_t bus, std::string model) {
    using R = Result<std::shared_ptr<EspPower>>;
    if (bus == nullptr) {
        return R::err(ErrorCode::InvalidArgument, "null i2c bus handle");
    }

    axp2101_handle_t handle = nullptr;
    esp_err_t err = axp2101_init(bus, &handle);
    if (err != ESP_OK) {
        return R::err(ErrorCode::DeviceFailure,
                      std::string("axp2101_init: ") + esp_err_to_name(err));
    }
    // Not make_shared: the constructor is private.
    return R::ok(std::shared_ptr<EspPower>(new EspPower(handle, std::move(model))));
}

EspPower::EspPower(axp2101_handle_t handle, std::string model) : handle_(handle) {
    info_.id = "power.main";
    info_.cls = kClass;
    info_.model = std::move(model);
    info_.caps = wisp::Properties{
        {"battery", true},
        {"charging_status", true},
    };
}

// No deinit: this device, like every other ESP HAL device Wisp
// registers, lives for the process's lifetime (drivers/axp2101 provides
// no axp2101_deinit() -- see its file header).
EspPower::~EspPower() = default;

Result<wisp::hal::PowerStatus> EspPower::status() {
    using R = Result<wisp::hal::PowerStatus>;
    axp2101_status_t raw = {};
    esp_err_t err = axp2101_read_status(handle_, &raw);
    if (err != ESP_OK) {
        return R::err(ErrorCode::DeviceFailure,
                      std::string("axp2101_read_status: ") + esp_err_to_name(err));
    }

    wisp::hal::PowerStatus out;
    // wisp/hal/power.hpp: battery_percent is -1 when unknown, e.g. no
    // battery present. battery_mv/charging/usb_present are still
    // meaningful (or at least harmlessly zero) without a battery.
    out.battery_percent = raw.battery_present ? static_cast<int>(raw.battery_percent) : -1;
    out.battery_mv = raw.battery_mv;
    out.charging = raw.charging;
    out.usb_present = raw.vbus_present;
    return R::ok(out);
}

Result<void> EspPower::power_off() {
    esp_err_t err = axp2101_power_off(handle_);
    if (err != ESP_OK) {
        return Result<void>::err(ErrorCode::DeviceFailure,
                                 std::string("axp2101_power_off: ") + esp_err_to_name(err));
    }
    return Result<void>::ok();
}

esp_err_t EspPower::charging_config_status() const {
    return axp2101_charging_config_status(handle_);
}

}  // namespace wisp::port::esp
