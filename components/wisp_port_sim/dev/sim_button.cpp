#include <utility>

#include "sim_devices.hpp"

namespace wisp::port::sim {

SimButton::SimButton(std::string id, Kind kind, std::shared_ptr<SimInput> input, std::string role)
    : kind_(kind), input_(std::move(input)) {
    info_.id = std::move(id);
    info_.cls = kClass;
    info_.model = "sdl-key";
    info_.caps = wisp::Properties{{"standby", "wake_source"}};
    if (!role.empty()) info_.caps.emplace("role", wisp::Value(std::move(role)));
}

SimButton::~SimButton() { (void)set_edge_sink({}); }

wisp::Result<void> SimButton::set_edge_sink(wisp::hal::ButtonEdgeSink sink) {
    return input_->set_button_sink(kind_, this, sink);
}

bool SimButton::pressed() {
    switch (kind_) {
        case Kind::Power:
            return input_->power_pressed();
        case Kind::VolUp:
            return input_->vol_up_pressed();
        case Kind::VolDown:
            return input_->vol_down_pressed();
    }
    return false;
}

wisp::Result<void> SimButton::set_power(wisp::hal::PowerLevel /*level*/) {
    return wisp::Result<void>::ok();
}

}  // namespace wisp::port::sim
