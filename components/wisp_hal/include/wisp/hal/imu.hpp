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

    // Puts the IMU's sensors to sleep (false) or wakes them (true) --
    // spec 6.7's standby. Mirrors wisp::hal::Touch::set_power()'s
    // contract: not pure virtual, since not every IMU driver can power
    // its sensors down independently of the rest of the chip, and the
    // default just reports that via ErrorCode::Unsupported rather than
    // forcing every existing Imu implementation to add a no-op override.
    // wisp::app::PowerManager treats Unsupported here as expected, not
    // an error. A driver that does support it (e.g.
    // wisp::port::esp::EspImu, backed by the QMI8658's CTRL7 sensor
    // -enable bits) overrides this, and must make read() fail instead of
    // returning a stale/frozen sample while powered off.
    virtual Result<void> set_power(bool /*on*/) {
        return Result<void>::err(ErrorCode::Unsupported, "imu sleep unsupported");
    }
};

}  // namespace wisp::hal
