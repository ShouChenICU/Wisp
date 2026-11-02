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
    };
}

// No deinit: this device, like every other ESP HAL device Wisp
// registers, lives for the process's lifetime (drivers/qmi8658 provides
// no qmi8658_deinit()).
EspImu::~EspImu() = default;

Result<wisp::hal::ImuSample> EspImu::read() {
    using R = Result<wisp::hal::ImuSample>;
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
    esp_err_t err = qmi8658_set_odr(handle_, hz);
    if (err != ESP_OK) {
        return Result<void>::err(ErrorCode::DeviceFailure,
                                 std::string("qmi8658_set_odr: ") + esp_err_to_name(err));
    }
    return Result<void>::ok();
}

}  // namespace wisp::port::esp
