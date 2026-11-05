#include <utility>

#include "sim_devices.hpp"

namespace wisp::port::sim {

SimTouch::SimTouch(std::string id, std::shared_ptr<SimInput> input) : input_(std::move(input)) {
    info_.id = std::move(id);
    info_.cls = kClass;
    info_.model = "sdl-mouse";
    info_.caps = wisp::Properties{{"max_points", 1}, {"standby", "off"}};
}

wisp::Result<std::vector<wisp::hal::TouchPoint>> SimTouch::read() {
    std::vector<wisp::hal::TouchPoint> points;
    SimTouchState state = input_->touch();
    if (state.down && powered_.load()) {
        points.push_back({state.x, state.y, 0});
    }
    return wisp::Result<std::vector<wisp::hal::TouchPoint>>::ok(std::move(points));
}

wisp::Result<void> SimTouch::set_power(wisp::hal::PowerLevel level) {
    powered_.store(wisp::hal::powered_at(wisp::hal::StandbyPolicy::Off, level));
    return wisp::Result<void>::ok();
}

}  // namespace wisp::port::sim
