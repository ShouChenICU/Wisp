#pragma once

// PowerManager (spec 6.7): the single owner of Wisp's "screen on" <->
// "standby" transition. Sub-project 1's scope is exactly these two
// states -- no background scenes, no wake locks; see spec 1.6 for the
// direction later sub-projects take this in.
//
// enter_standby() puts the display panel and touch controller to sleep,
// pauses UiRuntime's rendering, widens ButtonService's poll interval,
// and calls the platform's set_standby(true) hook (real hardware: keeps
// DFS always on, additionally enables tickless light sleep; simulator:
// logs). exit_standby() reverses all four, in the opposite order, and
// (via UiRuntime::set_paused(false)) forces a full redraw. Both are
// idempotent -- calling either while already in that state does nothing
// (no device calls, no event). Every transition that actually happens
// publishes "power/state" ({"state": "active"|"standby"}) on `bus`.
//
// PowerManager also owns the idle-timeout policy that triggers an
// automatic enter_standby(): note_activity() resets an idle clock to
// "now"; tick(now_ms), called periodically by the owner (see
// App::start()), compares the elapsed idle time against Settings
// "power"/"screen_off_s" (default 60, 0 disables) and calls
// enter_standby() once it's exceeded. The constructor subscribes to
// every "button."-prefixed event on `bus` (any button, any event type)
// to call note_activity() -- and, while in standby, exit_standby() --
// implementing "待机中按任意键都会唤醒" (spec 6.7/8) independently of
// whatever UI-specific handling (e.g. the self-check screen's
// PowerKeyController) a particular button's events also drive. The
// owner additionally wires UiRuntime's touch-activity hook (see
// UiRuntime::set_touch_activity_cb()) to note_activity() when both `ui`
// and `touch` are non-null, so touch input resets the idle clock too
// (but never wakes from standby itself -- the touch controller is
// asleep and UiRuntime is paused by then, so no touch read ever
// happens while in standby in the first place).
//
// Thread-safety: every public method may be called from any thread, any
// number of times concurrently, including from this PowerManager's own
// bus subscription handler (on the EventBus dispatcher thread) and from
// UI-thread code (e.g. self_check_screen.cpp's PowerKeyController
// handling, already running inside a ui()->post()'d callback).

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "wisp/core/button_service.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/touch.hpp"
#include "wisp/os/os.hpp"

namespace wisp::ui {
class UiRuntime;
}

namespace wisp::app {

class PowerManager {
public:
    // `display`/`touch` may be null (headless, or no touch panel).
    // `ui`/`buttons` may be null in headless mode (no UiRuntime); every
    // call below tolerates any of the four being null. `set_standby`
    // may be an empty std::function (no platform hook). `settings`
    // should already be bound to the "power" namespace (see
    // wisp::core::Settings); screen_off_s is read fresh on every tick(),
    // so a later Settings change (e.g. from a future settings UI) takes
    // effect without reconstructing this object.
    PowerManager(core::EventBus& bus, std::shared_ptr<hal::Display> display,
                 std::shared_ptr<hal::Touch> touch, ui::UiRuntime* ui, core::ButtonService* buttons,
                 std::function<void(bool)> set_standby, core::Settings settings);

    // Unsubscribes from `bus` and clears the UiRuntime touch-activity
    // hook this installed (if any). Does not call exit_standby() --
    // callers tear this down only as part of a larger shutdown (see
    // App::stop()) where the display/touch/ui/buttons it would
    // otherwise restore are about to go away anyway.
    ~PowerManager();

    PowerManager(const PowerManager&) = delete;
    PowerManager& operator=(const PowerManager&) = delete;

    // Idempotent: a no-op if already in standby. Otherwise, in order:
    //   1. Display::set_power(false) (panel sleep).
    //   2. Touch::set_power(false) (ErrorCode::Unsupported is expected,
    //      not an error -- most touch drivers have no sleep mode).
    //   3. UiRuntime::set_paused(true).
    //   4. ButtonService::set_poll_interval_ms(50) (the poll interval in
    //      effect right before this call is remembered and restored by
    //      exit_standby(), so this works correctly even if the caller
    //      didn't construct ButtonService with the spec's 20ms default).
    //   5. The platform set_standby(true) hook, if any.
    // Then publishes "power/state":"standby" on the bus.
    void enter_standby();

    // Idempotent: a no-op if already active. Reverses enter_standby()'s
    // five steps in the opposite order (so the platform hook runs
    // first, the panel wake last), forcing a full redraw along the way
    // (UiRuntime::set_paused(false)'s own doc comment). Then publishes
    // "power/state":"active" and calls note_activity() (waking resets
    // the idle clock, so tick() doesn't immediately re-enter standby).
    void exit_standby();

    bool standby() const;

    // Resets the idle clock to "now" (wisp::os::now_ms()).
    void note_activity();

    // Compares the idle time (now_ms minus the last note_activity()) to
    // Settings "screen_off_s" (0 disables; default 60) and calls
    // enter_standby() if exceeded. A no-op if already in standby, or if
    // auto screen-off is disabled. Meant to be called periodically
    // (e.g. every ~1s) by the owner -- see App::start()'s "wisp.power"
    // Timer.
    void tick(uint64_t now_ms);

private:
    void publish_state();

    core::EventBus& bus_;
    std::shared_ptr<hal::Display> display_;
    std::shared_ptr<hal::Touch> touch_;
    ui::UiRuntime* ui_;
    core::ButtonService* buttons_;
    std::function<void(bool)> set_standby_;
    core::Settings settings_;

    // Serializes the whole bodies of enter_standby()/exit_standby()
    // against each other and against themselves (e.g. a tick()-driven
    // auto enter_standby() on one thread racing a button-driven
    // exit_standby() on another) -- without this, two opposite
    // transitions arriving at the same moment could interleave their
    // steps (e.g. one's set_paused(true) landing after the other's
    // set_paused(false)), leaving UiRuntime/ButtonService out of sync
    // with the hardware and/or standby_ itself. `standby_` stays a
    // plain atomic (rather than something requiring this lock to read)
    // purely so standby() const stays cheap and lock-free for callers
    // that just want to know the current state.
    mutable os::Mutex transition_mutex_;
    std::atomic<bool> standby_{false};
    std::atomic<uint64_t> last_activity_ms_;

    // The ButtonService poll interval in effect right before
    // enter_standby() last widened it, restored by exit_standby().
    // Guarded by transition_mutex_ (see above).
    uint32_t saved_poll_ms_ = 20;

    core::SubscriptionId button_sub_ = 0;
};

}  // namespace wisp::app
