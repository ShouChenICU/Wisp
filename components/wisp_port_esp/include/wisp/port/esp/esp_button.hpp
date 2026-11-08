#pragma once

// ESP-IDF wisp::hal::Button backed directly by a GPIO level -- on
// boards/waveshare_esp32_s3_touch_amoled_2_16 both the volume buttons and
// the AXP2101 PWRON key's detect pin are plain GPIOs, so one class covers
// all three. A board whose PWRON key reaches no GPIO uses EspPmuPowerKey
// below instead. Pin, polarity, id and model (e.g.
// "gpio-button" vs. the power key's "axp2101-pwrkey") are all supplied
// by the board (AGENTS.md "Board independence" rule).
//
// This class does not itself call gpio_config(): polarity and
// pull-up/pull-down needs differ per pin (e.g. this board's power-key
// pin reads high while pressed -- see the hardware doc), which is
// exactly the kind of per-pin fact
// AGENTS.md keeps out of the portable/port layers. The board configures
// each pin as an input (with whatever pull resistor it needs) before
// constructing the EspGpioButton that reads it.

#include <memory>
#include <string>

#include "axp2101_conv.h"
#include "driver/gpio.h"
#include "wisp/base/result.hpp"
#include "wisp/hal/button.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/esp/esp_power.hpp"

namespace wisp::port::esp {

class EspGpioButton : public wisp::hal::Button {
public:
    // `role`, when not empty, becomes caps "role" (wisp/hal/button.hpp).
    EspGpioButton(gpio_num_t pin, bool active_low, std::string id, std::string model,
                  std::string role = {});

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    // gpio_get_level(pin), inverted when active_low.
    bool pressed() override;
    // caps "standby" is "wake_source": a button keeps working at every
    // level, so this is a no-op.
    Result<void> set_power(wisp::hal::PowerLevel level) override;

private:
    wisp::hal::DeviceInfo info_;
    gpio_num_t pin_;
    bool active_low_;
};

// The AXP2101's PWRON key on a board where no GPIO follows it: pressed()
// polls the PMU's latched PWRON edge IRQ bits over I2C
// (EspPower::take_power_key_irqs()) and rebuilds the level with
// axp2101_pwrkey_update() (drivers/axp2101/include/axp2101_conv.h), so
// ButtonService times short and long presses exactly as for a GPIO key.
// One I2C read per poll (20 ms active, 50 ms in standby); an extra write
// only when a bit was set. A failed read keeps the last level.
class EspPmuPowerKey : public wisp::hal::Button {
public:
    // Calls power->enable_power_key_irqs(); on failure the key still
    // registers but logs a warning (pressed() then sees no edges).
    EspPmuPowerKey(std::shared_ptr<EspPower> power, std::string id, std::string model);

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    bool pressed() override;
    // caps "standby" is "wake_source": ButtonService keeps polling in
    // standby, and the PMU stays on, so this is a no-op.
    Result<void> set_power(wisp::hal::PowerLevel level) override;

private:
    wisp::hal::DeviceInfo info_;
    std::shared_ptr<EspPower> power_;
    wisp::os::Mutex mutex_;  // guards tracker_
    axp2101_pwrkey_tracker_t tracker_ = {};
};

}  // namespace wisp::port::esp
