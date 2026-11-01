#include <utility>

#include "sim_devices.hpp"

namespace wisp::port::sim {

SimButton::SimButton(std::string id, Kind kind, std::shared_ptr<SimInput> input)
    : kind_(kind), input_(std::move(input)) {
    info_.id = std::move(id);
    info_.cls = kClass;
    info_.model = "sdl-key";
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

}  // namespace wisp::port::sim
