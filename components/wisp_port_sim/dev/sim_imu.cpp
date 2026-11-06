#include <utility>

#include "sim_devices.hpp"
#include "wisp/os/os.hpp"

namespace wisp::port::sim {

SimImu::SimImu(std::string id, std::shared_ptr<SimInput> input) : input_(std::move(input)) {
    info_.id = std::move(id);
    info_.cls = kClass;
    info_.model = "sim-imu";
    info_.caps = wisp::Properties{
        {"accel", true},       {"gyro", true},     {"magnetometer", false},
        {"max_rate_hz", 1000}, {"standby", "off"},
    };
}

wisp::Result<wisp::hal::ImuSample> SimImu::read() {
    if (!powered_.load()) {
        return wisp::Result<wisp::hal::ImuSample>::err(wisp::ErrorCode::DeviceFailure,
                                                       "imu is powered off");
    }
    SimTiltState tilt = input_->tilt_state();
    wisp::hal::ImuSample sample;
    sample.ax = tilt.ax;
    sample.ay = tilt.ay;
    sample.az = tilt.az;
    if (auto script = input_->imu_script()) {
        if (auto a = imu_script_accel(*script, wisp::os::now_ms())) {
            sample.ax = (*a)[0];
            sample.ay = (*a)[1];
            sample.az = (*a)[2];
        }
    }
    sample.gx = 0;
    sample.gy = 0;
    sample.gz = 0;
    sample.ts_ms = wisp::os::now_ms();
    return wisp::Result<wisp::hal::ImuSample>::ok(sample);
}

wisp::Result<void> SimImu::set_rate(uint32_t hz) {
    rate_hz_.store(hz);
    return wisp::Result<void>::ok();
}

wisp::Result<void> SimImu::set_power(wisp::hal::PowerLevel level) {
    powered_.store(wisp::hal::powered_at(wisp::hal::StandbyPolicy::Off, level));
    return wisp::Result<void>::ok();
}

}  // namespace wisp::port::sim
