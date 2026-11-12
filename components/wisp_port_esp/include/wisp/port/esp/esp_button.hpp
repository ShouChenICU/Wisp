#pragma once

#include <memory>
#include <string>

#include "axp2101_conv.h"
#include "driver/gpio.h"
#include "wisp/base/result.hpp"
#include "wisp/hal/button.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/esp/button_input.hpp"
#include "wisp/port/esp/esp_power.hpp"

namespace wisp::port::esp {

// The board configures the input and its pulls before construction. Pin,
// polarity and opt-in edge mode are board facts, not port-layer policy.
class EspGpioButton : public wisp::hal::Button {
public:
    EspGpioButton(gpio_num_t pin, bool active_low, std::string id, std::string model,
                  std::string role = {},
                  hal::ButtonInputMode mode = hal::ButtonInputMode::PollOnly);
    ~EspGpioButton() override;

    const wisp::hal::DeviceInfo& info() const override { return info_; }
    bool pressed() override;
    hal::ButtonInputMode input_mode() const override { return mode_; }
    Result<void> set_edge_sink(hal::ButtonEdgeSink sink) override;
    uint32_t standby_poll_ms() const override { return kButtonStandbyPollMs; }
    Result<void> set_power(wisp::hal::PowerLevel level) override;
    void service_input() override;

    // Installs the shared IRAM GPIO service once. An unknown pre-existing
    // service is rejected rather than assuming its interrupt flags are safe.
    static Result<void> prepare_edge_input();

private:
    struct Input;
    void detach();
    wisp::hal::DeviceInfo info_;
    gpio_num_t pin_;
    bool active_low_;
    hal::ButtonInputMode mode_;
    os::Mutex mutex_;
    std::unique_ptr<Input> input_;
};

// No direct GPIO: reconstruct the PWRON level from AXP2101 REG 0x49.
// One I2C read per poll (20 ms active, 100 ms power standby), plus a
// write when latched IRQ bits need clearing. Failed reads retain state.
class EspPmuPowerKey : public wisp::hal::Button {
public:
    EspPmuPowerKey(std::shared_ptr<EspPower> power, std::string id, std::string model);
    const wisp::hal::DeviceInfo& info() const override { return info_; }
    bool pressed() override;
    hal::ButtonInputMode input_mode() const override { return hal::ButtonInputMode::PollOnly; }
    uint32_t standby_poll_ms() const override { return kButtonStandbyPollMs; }
    Result<void> set_power(wisp::hal::PowerLevel level) override;

private:
    wisp::hal::DeviceInfo info_;
    std::shared_ptr<EspPower> power_;
    wisp::os::Mutex mutex_;
    axp2101_pwrkey_tracker_t tracker_ = {};
};

}  // namespace wisp::port::esp
