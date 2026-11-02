#include "wisp/app/power_key_controller.hpp"

namespace wisp::app {

PowerKeyController::Action PowerKeyController::on_short(bool screen_on) {
    if (confirming_) {
        confirming_ = false;
        return Action::CancelConfirm;
    }
    if (screen_on) {
        return Action::ScreenOff;
    }
    return Action::ScreenOn;
}

PowerKeyController::Action PowerKeyController::on_long(bool screen_on) {
    if (confirming_) {
        return Action::None;
    }
    confirming_ = true;
    if (screen_on) {
        return Action::ShowConfirm;
    }
    return Action::ScreenOnAndConfirm;
}

PowerKeyController::Action PowerKeyController::on_confirm(bool yes) {
    confirming_ = false;
    return yes ? Action::PowerOff : Action::CancelConfirm;
}

}  // namespace wisp::app
