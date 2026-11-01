#pragma once

#include <cstdint>

#include "wisp/base/result.hpp"
#include "wisp/hal/device.hpp"

namespace wisp::hal {

struct PowerStatus {
    int battery_percent = -1;  // -1 if unknown (e.g. no battery present)
    uint32_t battery_mv = 0;
    bool charging = false;
    bool usb_present = false;
};

// caps keys: "battery" (bool, true if a battery is present), "charging_status"
// (bool, true if charging/usb_present are meaningful).
class Power : public Device {
public:
    static constexpr DeviceClass kClass = DeviceClass::Power;

    virtual Result<PowerStatus> status() = 0;
    virtual Result<void> power_off() = 0;
};

}  // namespace wisp::hal
