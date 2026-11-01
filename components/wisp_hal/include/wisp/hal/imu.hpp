#pragma once

#include <cstdint>

#include "wisp/base/result.hpp"
#include "wisp/hal/device.hpp"

namespace wisp::hal {

// Acceleration in g, angular rate in degrees per second.
struct ImuSample {
    float ax = 0;
    float ay = 0;
    float az = 0;
    float gx = 0;
    float gy = 0;
    float gz = 0;
    uint64_t ts_ms = 0;
};

// caps keys: "accel" (bool), "gyro" (bool), "magnetometer" (bool),
// "max_rate_hz" (int).
class Imu : public Device {
public:
    static constexpr DeviceClass kClass = DeviceClass::Imu;

    virtual Result<ImuSample> read() = 0;
    virtual Result<void> set_rate(uint32_t hz) = 0;
};

}  // namespace wisp::hal
