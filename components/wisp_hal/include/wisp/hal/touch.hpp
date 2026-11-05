#pragma once

#include <cstdint>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/hal/device.hpp"

namespace wisp::hal {

struct TouchPoint {
    int16_t x = 0;
    int16_t y = 0;
    uint8_t id = 0;
};

// caps keys: "max_points" (int), "standby" (string, optional: "off" when
// the controller can sleep, in which case set_power() is overridden; see
// StandbyPolicy in wisp/hal/device.hpp).
class Touch : public Device {
public:
    static constexpr DeviceClass kClass = DeviceClass::Touch;

    // Returns the currently active touch points (empty if none).
    virtual Result<std::vector<TouchPoint>> read() = 0;
};

}  // namespace wisp::hal
