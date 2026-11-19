#pragma once

// Wisp adapters over the AXP2101 C driver (axp2101.h), on the board's own
// bmgr i2c_master bus handle: Axp2101Power is the board's wisp::hal::Power,
// Axp2101PowerKey its PWRON key for a board where no GPIO follows the key.
// Both share one chip handle, one typed interface per device object
// (AGENTS.md section 3 "Device model"). The board names the ids and roles;
// the model strings default to this chip. Caps ("battery",
// "charging_status") describe what an AXP2101-class PMU always reports, so
// they're fixed here rather than configured per board.

#include <cstdint>
#include <memory>
#include <string>

#include "axp2101.h"
#include "axp2101_conv.h"
#include "driver/i2c_master.h"
#include "wisp/base/result.hpp"
#include "wisp/hal/button.hpp"
#include "wisp/hal/power.hpp"
#include "wisp/os/os.hpp"

namespace wisp::drivers {

struct Axp2101PowerConfig {
    std::string id;
    // Becomes caps "role" when not empty.
    std::string role;
    std::string model = "AXP2101";
};

class Axp2101Power : public wisp::hal::Power {
public:
    // Calls axp2101_init(bus) and wraps the resulting handle. Fails with
    // DeviceFailure if that returns anything other than ESP_OK.
    static Result<std::shared_ptr<Axp2101Power>> create(i2c_master_bus_handle_t bus,
                                                        Axp2101PowerConfig config);

    ~Axp2101Power() override;

    Axp2101Power(const Axp2101Power&) = delete;
    Axp2101Power& operator=(const Axp2101Power&) = delete;

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    // axp2101_read_status().
    Result<wisp::hal::PowerStatus> status() override;
    // axp2101_power_off(). Returns ok once the shutdown request has been
    // written successfully; the chip then cuts power shortly afterwards,
    // so the caller never observes further code running on success.
    Result<void> power_off() override;

    // axp2101_charging_config_status(): ESP_OK if axp2101_init()'s
    // best-effort charging-parameter configuration succeeded, otherwise
    // the esp_err_t it failed with. Purely diagnostic -- the device is
    // registered either way; the board decides whether/how to log this.
    esp_err_t charging_config_status() const;

    // axp2101_pwrkey_irq_enable()/axp2101_pwrkey_irq_take(), for a board
    // whose PWRON key is read through this PMU rather than a GPIO (see
    // Axp2101PowerKey below).
    esp_err_t enable_power_key_irqs();
    esp_err_t take_power_key_irqs(uint8_t* bits);

private:
    Axp2101Power(axp2101_handle_t handle, Axp2101PowerConfig config);

    wisp::hal::DeviceInfo info_;
    axp2101_handle_t handle_;
};

struct Axp2101PowerKeyConfig {
    std::string id;
    // Becomes caps "role" when not empty (wisp/hal/button.hpp).
    std::string role;
    std::string model = "axp2101-pwrkey";
};

// No direct GPIO: reconstruct the PWRON level from AXP2101 REG 0x49.
// One I2C read per poll (20 ms active, 100 ms power standby), plus a
// write when latched IRQ bits need clearing. Failed reads retain state.
class Axp2101PowerKey : public wisp::hal::Button {
public:
    Axp2101PowerKey(std::shared_ptr<Axp2101Power> power, Axp2101PowerKeyConfig config);
    const wisp::hal::DeviceInfo& info() const override { return info_; }
    bool pressed() override;
    wisp::hal::ButtonInputMode input_mode() const override {
        return wisp::hal::ButtonInputMode::PollOnly;
    }
    uint32_t standby_poll_ms() const override;
    Result<void> set_power(wisp::hal::PowerLevel level) override;

private:
    wisp::hal::DeviceInfo info_;
    std::shared_ptr<Axp2101Power> power_;
    wisp::os::Mutex mutex_;
    axp2101_pwrkey_tracker_t tracker_ = {};
};

}  // namespace wisp::drivers
