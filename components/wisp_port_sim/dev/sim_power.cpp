#include <utility>

#include "sim_devices.hpp"

namespace wisp::port::sim {

SimPower::SimPower(std::string id, std::function<void()> on_power_off)
    : on_power_off_(std::move(on_power_off)) {
    info_.id = std::move(id);
    info_.cls = kClass;
    info_.model = "sim-battery";
    info_.caps = wisp::Properties{{"battery", true}, {"charging_status", true}};
}

wisp::Result<wisp::hal::PowerStatus> SimPower::status() {
    wisp::hal::PowerStatus s;
    s.battery_percent = 80;
    s.battery_mv = 3900;
    s.charging = false;
    s.usb_present = true;
    return wisp::Result<wisp::hal::PowerStatus>::ok(s);
}

wisp::Result<void> SimPower::power_off() {
    if (on_power_off_) {
        on_power_off_();
    }
    return wisp::Result<void>::ok();
}

}  // namespace wisp::port::sim
