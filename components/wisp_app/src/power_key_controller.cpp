#include "wisp/app/power_key_controller.hpp"

namespace wisp::app {

PowerKeyController::Action PowerKeyController::on_short() {
    if (confirming_) {
        confirming_ = false;
        return Action::CancelConfirm;
    }
    if (screen_on_) {
        screen_on_ = false;
        return Action::ScreenOff;
    }
    screen_on_ = true;
    return Action::ScreenOn;
}

PowerKeyController::Action PowerKeyController::on_long() {
    if (confirming_) {
        return Action::None;
    }
    confirming_ = true;
    if (screen_on_) {
        return Action::ShowConfirm;
    }
    screen_on_ = true;
    return Action::ScreenOnAndConfirm;
}

PowerKeyController::Action PowerKeyController::on_confirm(bool yes) {
    confirming_ = false;
    return yes ? Action::PowerOff : Action::CancelConfirm;
}

}  // namespace wisp::app
