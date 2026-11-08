#pragma once

#include <cstdint>

#include "wisp/base/result.hpp"
#include "wisp/hal/device.hpp"

namespace wisp::hal {

// Acceleration in g, angular rate in degrees per second, in the screen's
// frame: x toward the screen's right edge, y toward its top edge, z out of
// the screen (right-handed). An axis pointing up reads +1 g, so a device
// lying face up reads az = +1 and one held upright reads ay = +1.
struct ImuSample {
    float ax = 0;
    float ay = 0;
    float az = 0;
    float gx = 0;
    float gy = 0;
    float gz = 0;
    uint64_t ts_ms = 0;
};

// How a sensor chip is mounted relative to the screen: screen axis i
// (x, y, z) is the chip's axis `from[i]` (0 = x, 1 = y, 2 = z), negated
// when `negate[i]`. The same map applies to acceleration and angular rate,
// so it must be a rotation (determinant +1) for the rates to keep their
// sense. The default is a chip whose axes already match the screen's.
struct ImuAxisMap {
    uint8_t from[3] = {0, 1, 2};
    bool negate[3] = {false, false, false};
};

// `raw` (in the chip's axes) in the screen's frame; ts_ms is kept.
inline ImuSample to_screen_frame(const ImuSample& raw, const ImuAxisMap& map) {
    const float a[3] = {raw.ax, raw.ay, raw.az};
    const float g[3] = {raw.gx, raw.gy, raw.gz};
    float oa[3], og[3];
    for (int i = 0; i < 3; ++i) {
        const uint8_t f = map.from[i] < 3 ? map.from[i] : static_cast<uint8_t>(i);
        oa[i] = map.negate[i] ? -a[f] : a[f];
        og[i] = map.negate[i] ? -g[f] : g[f];
    }
    ImuSample out = raw;
    out.ax = oa[0];
    out.ay = oa[1];
    out.az = oa[2];
    out.gx = og[0];
    out.gy = og[1];
    out.gz = og[2];
    return out;
}

// caps keys: "accel" (bool), "gyro" (bool), "magnetometer" (bool),
// "max_rate_hz" (int), "standby" (string, optional: "off" when the
// sensors can power down, in which case set_power() is overridden and
// read() fails while they are off rather than returning a stale sample;
// see StandbyPolicy in wisp/hal/device.hpp).
class Imu : public Device {
public:
    static constexpr DeviceClass kClass = DeviceClass::Imu;

    virtual Result<ImuSample> read() = 0;
    virtual Result<void> set_rate(uint32_t hz) = 0;
};

}  // namespace wisp::hal
