#pragma once

#include <memory>
#include <string>

#include "driver/gpio.h"
#include "wisp/base/result.hpp"
#include "wisp/hal/button.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/esp/button_input.hpp"

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

}  // namespace wisp::port::esp
