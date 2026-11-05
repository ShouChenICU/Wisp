#include "wisp/port/esp/esp_imu.hpp"

#include <string>
#include <utility>

#include "esp_err.h"
#include "wisp/base/error.hpp"
#include "wisp/os/os.hpp"

namespace wisp::port::esp {

Result<std::shared_ptr<EspImu>> EspImu::create(i2c_master_bus_handle_t bus, std::string model) {
    using R = Result<std::shared_ptr<EspImu>>;
    if (bus == nullptr) {
        return R::err(ErrorCode::InvalidArgument, "null i2c bus handle");
    }

    qmi8658_handle_t handle = nullptr;
    esp_err_t err = qmi8658_init(bus, &handle);
    if (err != ESP_OK) {
        return R::err(ErrorCode::DeviceFailure,
                      std::string("qmi8658_init: ") + esp_err_to_name(err));
    }
    // Not make_shared: the constructor is private.
    return R::ok(std::shared_ptr<EspImu>(new EspImu(handle, std::move(model))));
}

EspImu::EspImu(qmi8658_handle_t handle, std::string model) : handle_(handle) {
    info_.id = "imu.main";
    info_.cls = kClass;
    info_.model = std::move(model);
    info_.caps = wisp::Properties{
        {"accel", true},
        {"gyro", true},
        {"magnetometer", false},
        {"max_rate_hz", 1000},
        {"standby", wisp::Value("off")},
    };
}

// No deinit: this device, like every other ESP HAL device Wisp
// registers, lives for the process's lifetime (drivers/qmi8658 provides
// no qmi8658_deinit()).
EspImu::~EspImu() = default;

Result<wisp::hal::ImuSample> EspImu::read() {
    using R = Result<wisp::hal::ImuSample>;
    if (!powered_.load(std::memory_order_acquire)) {
        return R::err(ErrorCode::DeviceFailure, "imu is powered off");
    }
    qmi8658_sample_t raw = {};
    esp_err_t err = qmi8658_read(handle_, &raw);
    if (err != ESP_OK) {
        return R::err(ErrorCode::DeviceFailure,
                      std::string("qmi8658_read: ") + esp_err_to_name(err));
    }

    wisp::hal::ImuSample out;
    out.ax = raw.ax_g;
    out.ay = raw.ay_g;
    out.az = raw.az_g;
    out.gx = raw.gx_dps;
    out.gy = raw.gy_dps;
    out.gz = raw.gz_dps;
    out.ts_ms = wisp::os::now_ms();
    return R::ok(out);
}

Result<void> EspImu::set_rate(uint32_t hz) {
    // Clamp to the "max_rate_hz" cap advertised in info_.caps above
    // (qmi8658_set_odr() clamps the same way internally, but this keeps
    // the HAL-level contract self-evident without reading the driver).
    constexpr uint32_t kMaxRateHz = 1000;
    if (hz > kMaxRateHz) {
        hz = kMaxRateHz;
    }
    esp_err_t err = qmi8658_set_odr(handle_, hz);
    if (err != ESP_OK) {
        return Result<void>::err(ErrorCode::DeviceFailure,
                                 std::string("qmi8658_set_odr: ") + esp_err_to_name(err));
    }
    return Result<void>::ok();
}

Result<void> EspImu::set_power(wisp::hal::PowerLevel level) {
    const bool on = wisp::hal::powered_at(wisp::hal::StandbyPolicy::Off, level);
    if (on == powered_.load(std::memory_order_acquire)) {
        return Result<void>::ok();
    }
    esp_err_t err = qmi8658_set_power(handle_, on);
    if (err != ESP_OK) {
        return Result<void>::err(ErrorCode::DeviceFailure,
                                 std::string("qmi8658_set_power: ") + esp_err_to_name(err));
    }
    // Only flip the cached flag read() checks once the chip call itself
    // actually succeeded, same as EspDisplay::set_power()'s powered_.
    powered_.store(on, std::memory_order_release);
    return Result<void>::ok();
}

}  // namespace wisp::port::esp
