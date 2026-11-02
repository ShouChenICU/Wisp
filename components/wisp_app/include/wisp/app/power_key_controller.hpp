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
//
// Task 18b review round 1 (C1): this class used to track "screen on"
// itself (a private screen_on_ bool), separately from PowerManager's own
// standby_ flag -- the two could disagree (e.g. after an automatic
// idle-timeout standby, or after some *other* button woke the device),
// producing a stale decision: a short press right after an automatic
// screen-off could be read by this class as "screen is still on" and so
// turn it right back off again. There is now exactly one source of
// truth for "is the screen on" -- PowerManager::standby() -- and this
// class no longer stores its own copy: every call site passes the
// *current* `!PowerManager::standby()` in explicitly (see
// self_check_screen.cpp's on_button_event()).
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

    // Starts not confirming.
    PowerKeyController() = default;

    // Call once per "button.power.short" event, with the screen's
    // *current* on/off state (e.g. `!PowerManager::standby()`) -- read
    // right before this call, on the same thread that will act on the
    // returned Action (see self_check_screen.cpp).
    //   - confirming: cancels the confirm dialog (CancelConfirm),
    //     regardless of `screen_on`.
    //   - screen_on: turns the screen off (ScreenOff).
    //   - !screen_on: turns the screen on (ScreenOn).
    Action on_short(bool screen_on);

    // Call once per "button.power.long" event, with the screen's current
    // on/off state (see on_short()).
    //   - confirming already: does nothing (None) -- a long press while
    //     the confirm dialog is already up must not stack a second one.
    //   - screen_on: opens the confirm dialog (ShowConfirm).
    //   - !screen_on: wakes the screen *and* opens the confirm dialog
    //     (ScreenOnAndConfirm) -- per global-constraints.md Review Focus
    //     #4, "息屏时长按先亮屏再弹框".
    Action on_long(bool screen_on);

    // Call with the confirm dialog's result (true = "是"/yes, false =
    // "否"/no). Always clears `confirming()`.
    Action on_confirm(bool yes);

    bool confirming() const { return confirming_; }

private:
    bool confirming_ = false;
};

}  // namespace wisp::app
