#include "wisp/app/power_key_controller.hpp"

namespace wisp::app {

PowerKeyController::Action PowerKeyController::on_short(bool screen_on) {
    // Waking takes priority over a dormant confirm-dialog decision --
    // spec 6.7's "待机中按任意键都会唤醒" applies even
    // if a confirm dialog was already open when automatic standby kicked
    // in (e.g. the user opened it, then left the device idle). The
    // dialog itself (if the caller's UI still has it up) is left alone
    // here -- just left open, to be dealt with by whatever press comes
    // next once the screen is actually back on -- rather than cancelled,
    // since cancelling is a *decision*, not a wake, and this call's only
    // job while the screen is off is to wake it.
    if (!screen_on) {
        return Action::ScreenOn;
    }
    if (confirming_) {
        confirming_ = false;
        return Action::CancelConfirm;
    }
    return Action::ScreenOff;
}

PowerKeyController::Action PowerKeyController::on_long(bool screen_on) {
    // Same wake-first priority as on_short() above.
    if (!screen_on) {
        if (confirming_) {
            // A dialog was already open before standby -- just wake,
            // don't stack a second one (confirming_ stays true).
            return Action::ScreenOn;
        }
        confirming_ = true;
        return Action::ScreenOnAndConfirm;
    }
    if (confirming_) {
        return Action::None;
    }
    confirming_ = true;
    return Action::ShowConfirm;
}

PowerKeyController::Action PowerKeyController::on_confirm(bool yes) {
    confirming_ = false;
    return yes ? Action::PowerOff : Action::CancelConfirm;
}

}  // namespace wisp::app
