#pragma once

// Portable power-key state machine (spec 8, global-constraints.md Review
// Focus #4: one hold -> one `long`; no stacked confirm dialogs; a long
// press while the screen is off wakes it, then still confirms). This
// class has no device/UI dependencies at all -- it just turns the two
// gestures ButtonService already produces for "button.power"
// (`on_short()`/`on_long()`, called once per `short`/`long` event) plus
// the self-check screen's confirm dialog result (`on_confirm()`) into the
// Action the caller (the self-check screen) should perform. See
// self_check_screen.cpp for how each Action is carried out (Display::
// set_power, lv_msgbox, Power::power_off()/Platform::request_power_off).

namespace wisp::app {

class PowerKeyController {
public:
    enum class Action {
        None,
        ScreenOff,
        ScreenOn,
        ShowConfirm,
        ScreenOnAndConfirm,
        PowerOff,
        CancelConfirm,
    };

    // Starts screen-on, not confirming.
    PowerKeyController() = default;

    // Call once per "button.power.short" event.
    //   - confirming: cancels the confirm dialog (CancelConfirm).
    //   - screen on (not confirming): turns the screen off (ScreenOff).
    //   - screen off: turns the screen on (ScreenOn).
    Action on_short();

    // Call once per "button.power.long" event.
    //   - confirming already: does nothing (None) -- a long press while
    //     the confirm dialog is already up must not stack a second one.
    //   - screen on: opens the confirm dialog (ShowConfirm).
    //   - screen off: wakes the screen *and* opens the confirm dialog
    //     (ScreenOnAndConfirm) -- per global-constraints.md Review Focus
    //     #4, "息屏时长按先亮屏再弹框".
    Action on_long();

    // Call with the confirm dialog's result (true = "是"/yes, false =
    // "否"/no). Always clears `confirming()`.
    Action on_confirm(bool yes);

    bool screen_on() const { return screen_on_; }
    bool confirming() const { return confirming_; }

private:
    bool screen_on_ = true;
    bool confirming_ = false;
};

}  // namespace wisp::app
