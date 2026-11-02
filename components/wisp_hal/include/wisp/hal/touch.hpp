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

// caps keys: "max_points" (int).
class Touch : public Device {
public:
    static constexpr DeviceClass kClass = DeviceClass::Touch;

    // Returns the currently active touch points (empty if none).
    virtual Result<std::vector<TouchPoint>> read() = 0;

    // Puts the touch controller to sleep (false) or wakes it (true) --
    // spec 6.7's standby. Not pure virtual: most touch drivers (and
    // every one this codebase has ported so far -- see
    // wisp::port::esp::EspTouch) don't expose a sleep mode at all, so
    // the default just reports that rather than forcing every existing
    // Touch implementation to add a no-op override. A driver that does
    // support it overrides this; wisp::app::PowerManager treats
    // ErrorCode::Unsupported here as expected, not an error.
    virtual Result<void> set_power(bool /*on*/) {
        return Result<void>::err(ErrorCode::Unsupported, "touch sleep unsupported");
    }
};

}  // namespace wisp::hal
