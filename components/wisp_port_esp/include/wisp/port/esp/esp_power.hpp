#pragma once

// ESP-IDF wisp::hal::Power backed by drivers/axp2101 (the board's own
// bmgr i2c_master bus handle -- see boards/waveshare_amoled_216/
// board.cpp). The chip model string is passed in by the board (AGENTS.md
// "板子无关" rule); caps ("battery", "charging_status") describe what
// an AXP2101-class PMU always reports, so they're fixed here rather than
// configured per board, the same way EspTouch fixes "max_points".

#include <cstdint>
#include <memory>
#include <string>

#include "axp2101.h"
#include "driver/i2c_master.h"
#include "wisp/base/result.hpp"
#include "wisp/hal/power.hpp"

namespace wisp::port::esp {

class EspPower : public wisp::hal::Power {
public:
    // Calls axp2101_init(bus) and wraps the resulting handle. Fails with
    // DeviceFailure if that returns anything other than ESP_OK.
    static Result<std::shared_ptr<EspPower>> create(i2c_master_bus_handle_t bus, std::string model);

    ~EspPower() override;

    EspPower(const EspPower&) = delete;
    EspPower& operator=(const EspPower&) = delete;

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    // axp2101_read_status().
    Result<wisp::hal::PowerStatus> status() override;
    // axp2101_power_off(); never returns ok (the chip cuts power).
    Result<void> power_off() override;

private:
    EspPower(axp2101_handle_t handle, std::string model);

    wisp::hal::DeviceInfo info_;
    axp2101_handle_t handle_;
};

}  // namespace wisp::port::esp
