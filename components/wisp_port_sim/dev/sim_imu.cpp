#include <utility>

#include "sim_devices.hpp"
#include "wisp/os/os.hpp"

namespace wisp::port::sim {

SimImu::SimImu(std::string id, std::shared_ptr<SimInput> input) : input_(std::move(input)) {
    info_.id = std::move(id);
    info_.cls = kClass;
    info_.model = "sim-imu";
    info_.caps = wisp::Properties{
        {"accel", true},
        {"gyro", true},
        {"magnetometer", false},
        {"max_rate_hz", 1000},
    };
}

wisp::Result<wisp::hal::ImuSample> SimImu::read() {
    SimTiltState tilt = input_->tilt_state();
    wisp::hal::ImuSample sample;
    sample.ax = tilt.ax;
    sample.ay = tilt.ay;
    sample.az = tilt.az;
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

}  // namespace wisp::port::sim
