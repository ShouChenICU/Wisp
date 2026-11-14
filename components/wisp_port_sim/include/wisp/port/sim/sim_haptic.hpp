#pragma once

// Simulator haptic motor ("haptic.main", model "sim-haptic"): the worked
// example of a self-describing device (device model spec 3), and the one a
// driver author copies. It has no typed interface (interface None, class
// Other); everything it offers is in its schema (prop `active`, commands
// `pulse` and `effect`), which core::DeviceService, the agent's device
// tools and wisp.device reach through get()/invoke().
//
// No motor is involved. A command starts a bounded action by recording when
// it ends, and `active` compares that with the clock, so the simulator needs
// no timer; a real driver finishes the action with an os::Timer (see
// Device::invoke()). `now_ms` is injectable so tests move the clock by hand.
//
// Caps "standby": "off": set_power(Standby/DeepStandby) stops a running
// pulse and the device answers Busy until Active/Dim. Thread-safe: get(),
// invoke() and set_power() may run on different threads.

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "wisp/hal/device.hpp"
#include "wisp/hal/device_schema.hpp"
#include "wisp/os/os.hpp"

namespace wisp::port::sim {

class SimHaptic : public wisp::hal::Device {
public:
    // `role`, when not empty, becomes caps "role" (wisp/hal/roles.hpp).
    SimHaptic(std::string id, std::string role = {},
              std::function<uint64_t()> now_ms = wisp::os::now_ms);

    const wisp::hal::DeviceInfo& info() const override { return info_; }
    const wisp::hal::DeviceSchema* describe() const override;
    wisp::Result<wisp::Value> get(std::string_view prop) override;
    wisp::Result<wisp::Value> invoke(std::string_view cmd, const wisp::Map& args) override;
    wisp::Result<void> set_power(wisp::hal::PowerLevel level) override;

private:
    wisp::Result<wisp::Value> run(uint64_t duration_ms);

    wisp::hal::DeviceInfo info_;
    const std::function<uint64_t()> now_ms_;

    // One mutex over both fields: a set_power(Standby) that lands between
    // invoke()'s "am I on" check and its write of `until_ms_` must not leave
    // a pulse running on a powered-down motor. The critical sections are a
    // few loads and stores, never a log call or a clock read.
    std::mutex mutex_;
    bool powered_ = true;
    uint64_t until_ms_ = 0;
};

}  // namespace wisp::port::sim
