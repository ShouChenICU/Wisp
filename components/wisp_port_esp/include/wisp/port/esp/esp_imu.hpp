#pragma once

// ESP-IDF wisp::hal::Imu backed by drivers/qmi8658 (the board's own bmgr
// i2c_master bus handle -- see boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp).
// The chip model string is passed in by the board (AGENTS.md "Board independence"
// rule); caps ("accel", "gyro", "magnetometer", "max_rate_hz") describe
// facts about the QMI8658 chip this class wraps (no magnetometer on this
// part) and spec 5.5's fixed max_rate_hz, so they're fixed here rather
// than configured per board, the same way EspTouch fixes "max_points".

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "driver/i2c_master.h"
#include "qmi8658.h"
#include "wisp/base/result.hpp"
#include "wisp/hal/imu.hpp"

namespace wisp::port::esp {

class EspImu : public wisp::hal::Imu {
public:
    // Calls qmi8658_init(bus, addr) and wraps the resulting handle. Fails
    // with DeviceFailure if that returns anything other than ESP_OK. The
    // address is the board's SA0 strap (qmi8658_conv.h).
    static Result<std::shared_ptr<EspImu>> create(i2c_master_bus_handle_t bus, std::string model,
                                                  uint8_t addr = QMI8658_I2C_ADDR_HIGH);

    ~EspImu() override;

    EspImu(const EspImu&) = delete;
    EspImu& operator=(const EspImu&) = delete;

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    // qmi8658_read(); ImuSample::ts_ms is wisp::os::now_ms() at the time
    // of the call (the chip has no timestamp register Wisp reads). Fails
    // with DeviceFailure (without touching the I2C bus) while powered
    // off (IMU power-down stopgap, spec 6.7) -- a register read while
    // the sensors are disabled would return a stale/frozen value, not a
    // fresh sample, and silently returning that would be worse than
    // just failing.
    Result<wisp::hal::ImuSample> read() override;
    // qmi8658_set_odr().
    Result<void> set_rate(uint32_t hz) override;
    // caps "standby" is "off". qmi8658_set_power(): Standby/DeepStandby
    // disable the accelerometer/gyroscope (CTRL7); Active/Dim restore
    // them at the currently-selected output data rate (not necessarily
    // the default -- see qmi8658_set_power()'s own doc comment). A level
    // that maps to the current state sends nothing.
    Result<void> set_power(wisp::hal::PowerLevel level) override;

private:
    EspImu(qmi8658_handle_t handle, std::string model);

    wisp::hal::DeviceInfo info_;
    qmi8658_handle_t handle_;
    // Mirrors the chip's actual CTRL7 enable state, written only by a
    // successful set_power() call; read() consults this rather than the
    // chip itself (cheaper, and avoids ambiguity about what a read
    // while nominally "off" would even mean). Starts true: qmi8658_init()
    // leaves the sensors enabled.
    std::atomic<bool> powered_{true};
};

}  // namespace wisp::port::esp
