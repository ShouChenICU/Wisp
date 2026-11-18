#include "wisp/drivers/axp2101_power.hpp"

#include <string>
#include <utility>

#include "esp_err.h"
#include "wisp/base/error.hpp"
#include "wisp/core/log.hpp"
#include "wisp/port/esp/button_input.hpp"

namespace wisp::drivers {
namespace {
constexpr const char* kTag = "button";
}  // namespace

Result<std::shared_ptr<Axp2101Power>> Axp2101Power::create(i2c_master_bus_handle_t bus,
                                                           Axp2101PowerConfig config) {
    using R = Result<std::shared_ptr<Axp2101Power>>;
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
    return R::ok(std::shared_ptr<Axp2101Power>(new Axp2101Power(handle, std::move(config))));
}

Axp2101Power::Axp2101Power(axp2101_handle_t handle, Axp2101PowerConfig config) : handle_(handle) {
    info_.id = std::move(config.id);
    info_.cls = kClass;
    info_.model = std::move(config.model);
    info_.caps = wisp::Properties{
        {"battery", true},
        {"charging_status", true},
    };
    if (!config.role.empty()) info_.caps["role"] = wisp::Value(std::move(config.role));
}

// No deinit: this device, like every other ESP HAL device Wisp
// registers, lives for the process's lifetime (axp2101.h provides no
// axp2101_deinit()).
Axp2101Power::~Axp2101Power() = default;

Result<wisp::hal::PowerStatus> Axp2101Power::status() {
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

Result<void> Axp2101Power::power_off() {
    esp_err_t err = axp2101_power_off(handle_);
    if (err != ESP_OK) {
        return Result<void>::err(ErrorCode::DeviceFailure,
                                 std::string("axp2101_power_off: ") + esp_err_to_name(err));
    }
    return Result<void>::ok();
}

esp_err_t Axp2101Power::charging_config_status() const {
    return axp2101_charging_config_status(handle_);
}

esp_err_t Axp2101Power::enable_power_key_irqs() { return axp2101_pwrkey_irq_enable(handle_); }

esp_err_t Axp2101Power::take_power_key_irqs(uint8_t* bits) {
    return axp2101_pwrkey_irq_take(handle_, bits);
}

Axp2101PowerKey::Axp2101PowerKey(std::shared_ptr<Axp2101Power> power, Axp2101PowerKeyConfig config)
    : power_(std::move(power)) {
    info_.id = std::move(config.id);
    info_.cls = kClass;
    info_.model = std::move(config.model);
    info_.caps = wisp::Properties{{"standby", wisp::Value("wake_source")}};
    if (!config.role.empty()) info_.caps.emplace("role", wisp::Value(std::move(config.role)));
    esp_err_t err = power_->enable_power_key_irqs();
    if (err != ESP_OK) {
        WLOG_W(kTag, "%s: enabling the PWRON IRQs failed: %s", info_.id.c_str(),
               esp_err_to_name(err));
    }
}

// The standby read interval is the one EspGpioButton uses, so every
// power key polls at the same rate while the screen is off.
uint32_t Axp2101PowerKey::standby_poll_ms() const { return wisp::port::esp::kButtonStandbyPollMs; }

Result<void> Axp2101PowerKey::set_power(wisp::hal::PowerLevel /*level*/) {
    return Result<void>::ok();
}

bool Axp2101PowerKey::pressed() {
    // The read and the update stay under one lock: bits taken by one
    // caller must reach the tracker before another caller's later bits.
    mutex_.lock();
    uint8_t bits = 0;
    const bool read_ok = power_->take_power_key_irqs(&bits) == ESP_OK;
    const bool down = read_ok ? axp2101_pwrkey_update(&tracker_, bits) : tracker_.down;
    mutex_.unlock();
    return down;
}

}  // namespace wisp::drivers
