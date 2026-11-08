#include "wisp/port/esp/esp_button.hpp"

#include <utility>

#include "esp_err.h"
#include "wisp/core/log.hpp"

namespace wisp::port::esp {

EspGpioButton::EspGpioButton(gpio_num_t pin, bool active_low, std::string id, std::string model,
                             std::string role)
    : pin_(pin), active_low_(active_low) {
    info_.id = std::move(id);
    info_.cls = kClass;
    info_.model = std::move(model);
    info_.caps = wisp::Properties{{"standby", wisp::Value("wake_source")}};
    if (!role.empty()) info_.caps.emplace("role", wisp::Value(std::move(role)));
}

Result<void> EspGpioButton::set_power(wisp::hal::PowerLevel /*level*/) {
    return Result<void>::ok();
}

bool EspGpioButton::pressed() {
    int level = gpio_get_level(pin_);
    return active_low_ ? level == 0 : level != 0;
}

namespace {
constexpr const char* kTag = "button";
}  // namespace

EspPmuPowerKey::EspPmuPowerKey(std::shared_ptr<EspPower> power, std::string id, std::string model)
    : power_(std::move(power)) {
    info_.id = std::move(id);
    info_.cls = kClass;
    info_.model = std::move(model);
    info_.caps = wisp::Properties{{"standby", wisp::Value("wake_source")}};
    esp_err_t err = power_->enable_power_key_irqs();
    if (err != ESP_OK) {
        WLOG_W(kTag, "%s: enabling the PWRON IRQs failed: %s", info_.id.c_str(),
               esp_err_to_name(err));
    }
}

Result<void> EspPmuPowerKey::set_power(wisp::hal::PowerLevel /*level*/) {
    return Result<void>::ok();
}

bool EspPmuPowerKey::pressed() {
    // The read and the update stay under one lock: bits taken by one
    // caller must reach the tracker before another caller's later bits.
    mutex_.lock();
    uint8_t bits = 0;
    const bool read_ok = power_->take_power_key_irqs(&bits) == ESP_OK;
    const bool down = read_ok ? axp2101_pwrkey_update(&tracker_, bits) : tracker_.down;
    mutex_.unlock();
    return down;
}

}  // namespace wisp::port::esp
