#include "wisp/port/esp/esp_button.hpp"

#include <utility>

namespace wisp::port::esp {

EspGpioButton::EspGpioButton(gpio_num_t pin, bool active_low, std::string id, std::string model)
    : pin_(pin), active_low_(active_low) {
    info_.id = std::move(id);
    info_.cls = kClass;
    info_.model = std::move(model);
    info_.caps = wisp::Properties{{"standby", wisp::Value("wake_source")}};
}

Result<void> EspGpioButton::set_power(wisp::hal::PowerLevel /*level*/) {
    return Result<void>::ok();
}

bool EspGpioButton::pressed() {
    int level = gpio_get_level(pin_);
    return active_low_ ? level == 0 : level != 0;
}

}  // namespace wisp::port::esp
