#pragma once

// ESP-IDF wisp::hal::Button backed directly by a GPIO level -- both the
// volume buttons and the AXP2101 PWRON key's detect pin are plain GPIOs
// on this board (see boards/waveshare_amoled_216/board.cpp), so one
// class covers all three. Pin, polarity, id and model (e.g.
// "gpio-button" vs. the power key's "axp2101-pwrkey") are all supplied
// by the board (AGENTS.md "板子无关" rule).
//
// This class does not itself call gpio_config(): pull-up/pull-down needs
// differ per pin (e.g. the power key's GPIO already has an external
// pull-up on this board and must not also enable the internal one --
// see the hardware doc), which is exactly the kind of per-pin fact
// AGENTS.md keeps out of the portable/port layers. The board configures
// each pin as an input (with whatever pull resistor it needs) before
// constructing the EspGpioButton that reads it.

#include <string>

#include "driver/gpio.h"
#include "wisp/hal/button.hpp"

namespace wisp::port::esp {

class EspGpioButton : public wisp::hal::Button {
public:
    EspGpioButton(gpio_num_t pin, bool active_low, std::string id, std::string model);

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    // gpio_get_level(pin), inverted when active_low.
    bool pressed() override;

private:
    wisp::hal::DeviceInfo info_;
    gpio_num_t pin_;
    bool active_low_;
};

}  // namespace wisp::port::esp
