#pragma once

#include <cstdint>

#include "wisp/base/result.hpp"
#include "wisp/hal/device.hpp"

namespace wisp::hal {

// A closed pixel rectangle: [x1, x2] x [y1, y2], both ends inclusive.
struct Area {
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = 0;
    int32_t y2 = 0;
};

// caps keys: "width" (int), "height" (int), "shape" (string), "corner_radius"
// (int), "panel" (string), "brightness" (bool, true if set_brightness() is
// supported), "area_align" (int, flush()'s required alignment -- see
// align_area() in a later task).
class Display : public Device {
public:
    static constexpr DeviceClass kClass = DeviceClass::Display;

    // Writes `rgb565` (area.x2 - area.x1 + 1) * (area.y2 - area.y1 + 1)
    // pixels, row-major, into `area`. Blocks until the transfer has
    // completed (the caller may reuse/free the buffer as soon as this
    // returns).
    virtual Result<void> flush(const Area& area, const uint16_t* rgb565) = 0;
    virtual Result<void> set_brightness(uint8_t level) = 0;
    virtual Result<void> set_power(bool on) = 0;
    virtual bool powered() const = 0;
};

}  // namespace wisp::hal
