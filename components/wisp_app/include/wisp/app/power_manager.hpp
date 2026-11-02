#pragma once

// PowerManager (spec 6.7): the single owner of Wisp's "screen on" <->
// "standby" transition. Sub-project 1's scope is exactly these two
// states -- no background scenes, no wake locks; see spec 1.6 for the
// direction later sub-projects take this in.
//
// enter_standby() pauses UiRuntime's rendering, puts the display panel
// and touch controller to sleep, widens ButtonService's poll interval,
// and calls the platform's set_standby(true) hook (real hardware: keeps
// DFS always on, additionally enables tickless light sleep; simulator:
// logs). exit_standby() reverses all four, in the opposite order (so
// the panel is already awake again before UiRuntime resumes and -- via
// UiRuntime::set_paused(false)) -- forces a full redraw against it).
// Both are idempotent -- calling either while already in that state does
// nothing (no device calls, no event). Every transition that actually
// happens publishes "power/state" ({"state": "active"|"standby"}) on
// `bus`.
//
// PowerManager also owns the idle-timeout policy that triggers an
// automatic enter_standby(): note_activity() resets an idle clock to
// "now"; tick(now_ms), called periodically by the owner (see
// App::start()), compares the elapsed idle time against Settings
// "power"/"screen_off_s" (default 60, 0 disables) and triggers
// enter_standby() once it's exceeded -- see tick()'s own doc comment for
// *how* it triggers it (posted onto the UI task when there is one,
// never run directly on tick()'s own caller's thread). The constructor
// subscribes to every "button."-prefixed event on `bus` (any button,
// any event type) to call note_activity() -- and, while in standby,
// wake it -- implementing "待机中按任意键都会唤醒" (spec 6.7/8)
// independently of whatever UI-specific handling (e.g. the self-check
// screen's PowerKeyController) a particular button's events also drive.
// set_power_key_owned(true) excludes "button.power" specifically from
// that generic wake path once some other piece of UI (so far, only the
// self-check screen) has taken over deciding what button.power does
// with its own, more specific state machine -- see that method's doc
// comment for why both of these reacting to the same button.power press
// is a bug (task-18b-review.md C1), not a belt-and-suspenders safety
// net. The owner additionally wires UiRuntime's touch-activity hook (see
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
// handling, already running inside a ui()->post()'d callback). In
// practice, whenever a UiRuntime exists, every call this class makes to
// enter_standby()/exit_standby() (tick()'s own, the generic "any button
// wakes it" subscriber's, and self_check_screen.cpp's) is posted onto
// that one UI task rather than invoked directly from whichever thread
// noticed the trigger -- so in the only configuration sub-project 1
// actually ships (a board with a display), these two methods'
// device-touching bodies never truly run concurrently with each other
// at all; the mutex below exists mainly to keep the equally-supported
// headless configuration (no UiRuntime) correct too.

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
    //   1. UiRuntime::set_paused(true) -- stop rendering *before*
    //      touching the panel (task-18b-review.md I1): since this always
    //      runs on the UI task itself in practice (see the class-level
    //      comment), there is never a concurrent lv_timer_handler()/
    //      flush() straddling the panel-sleep call below regardless of
    //      ordering, but pausing first keeps the two in the order the
    //      exit path mirrors (see exit_standby()).
    //   2. Display::set_power(false) (panel sleep).
    //   3. Touch::set_power(false) (ErrorCode::Unsupported is expected,
    //      not an error -- most touch drivers have no sleep mode).
    //   4. ButtonService::set_poll_interval_ms(50) (the poll interval in
    //      effect right before this call is remembered and restored by
    //      exit_standby(), so this works correctly even if the caller
    //      didn't construct ButtonService with the spec's 20ms default).
    //   5. The platform set_standby(true) hook, if any.
    // Then publishes "power/state":"standby" on the bus.
    void enter_standby();

    // Idempotent: a no-op if already active. Reverses enter_standby()'s
    // five steps in the opposite order -- the platform hook runs first,
    // ButtonService's poll interval is restored, then Touch::set_power
    // (true), then Display::set_power(true) (panel wake), and only once
    // the panel is actually awake again does UiRuntime::set_paused
    // (false) run last, forcing a full redraw against an already-awake
    // panel (UiRuntime::set_paused()'s own doc comment). Then publishes
    // "power/state":"active" and calls note_activity() (waking resets
    // the idle clock, so tick() doesn't immediately re-enter standby).
    void exit_standby();

    bool standby() const;

    // Resets the idle clock to "now" (wisp::os::now_ms()).
    void note_activity();

    // Compares the idle time (now_ms minus the last note_activity()) to
    // Settings "screen_off_s" (0 disables; default 60) and, if exceeded,
    // triggers enter_standby() -- posted onto the UI task via ui->post()
    // when one exists, or called directly in headless mode. A no-op if
    // already in standby, or if auto screen-off is disabled. Meant to be
    // called periodically (e.g. every ~1s) by the owner -- see
    // App::start()'s "wisp.power" Timer.
    //
    // task-18b-review.md I1: tick() itself never calls enter_standby()
    // directly when a UiRuntime exists. On real hardware, every
    // wisp::os::Timer (including the one driving this call) shares a
    // single esp_timer task; enter_standby() can block for a while (the
    // panel's sleep-settle delay) and, if invoked directly here, would
    // do so *on that shared task*, delaying every other Timer in the
    // system (ButtonService's poll included) for no good reason -- and
    // risks a genuine deadlock if enter_standby() ever needs that same
    // task to finish something else first (see
    // ButtonService::set_poll_interval_ms()'s use inside
    // enter_standby()). Posting it instead returns from this call (and
    // so from the Timer callback) almost immediately.
    void tick(uint64_t now_ms);

    // Excludes "button.power"-sourced events from this class's own
    // generic "any button wakes it from standby" subscriber (note_
    // activity() is unaffected -- every button event still resets the
    // idle clock regardless of this flag). Call with `true` once some
    // other piece of code (so far, only self_check_screen.cpp's
    // PowerKeyController) has taken over deciding what button.power
    // presses do to the screen on its own -- see
    // self_check_screen.cpp's subscribe_buttons().
    //
    // Why this exists (task-18b-review.md C1): without it, a single
    // physical short press on the power key while in standby produces,
    // in order, "button.power.down" then "button.power.short". If this
    // class's generic handler reacted to the `down` half by waking the
    // device (as it does for every other button), and the owner's own
    // button.power-specific logic reacted to the `short` half using
    // *its own* idea of "is the screen on" -- which, if it is not also
    // reading PowerManager::standby() live at that exact moment, would
    // still believe the screen was off when `short` arrived, reasoning
    // "this press' job is to turn it on" -- the two decisions can
    // disagree and visibly flash the panel on and back off for what the
    // user experienced as one single press. Turning button.power over
    // entirely to the owner (down included, since note_activity() still
    // runs for it regardless) means exactly one piece of code ever
    // decides what a button.power press does, so there is nothing left
    // to disagree with itself.
    void set_power_key_owned(bool owned);

private:
    void publish_state();

    // Re-review 1 (N5): the body of what tick() posts onto the UI task
    // once it decides idle_ms has crossed the threshold. tick()'s own
    // decision is based on `now_ms` and last_activity_ms_ as of the
    // *call* to tick() -- but, when posted (see tick()'s doc comment),
    // actually entering standby only happens once that job runs, which
    // can lag tick() by up to one UI loop iteration. A button press
    // arriving in that window already calls note_activity() (the
    // constructor's "button." subscriber), but standby_ is still false
    // at that point, so nothing posts a counteracting exit_standby() --
    // without this recheck, the screen would still turn off right after
    // that press. Re-reads the *current* last_activity_ms_ (which that
    // press would have updated) against the same `now_ms` tick() was
    // told about, and only then calls enter_standby().
    void enter_standby_if_still_idle(uint64_t now_ms);

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
    //
    // task-18b-review.md I1: this is released for the one step
    // (ButtonService::set_poll_interval_ms()) that can block waiting on
    // the shared esp_timer task's own fence -- never hold this lock
    // across that wait, since (on real hardware) the esp_timer task is
    // also where every wisp::os::Timer callback -- including tick()'s
    // own -- runs; holding this lock while blocked waiting on that same
    // task would deadlock if that task ever needed this lock itself to
    // make progress. See enter_standby()/exit_standby()'s definitions.
    mutable os::Mutex transition_mutex_;
    std::atomic<bool> standby_{false};
    std::atomic<uint64_t> last_activity_ms_;
    std::atomic<bool> power_key_owned_{false};

    // The ButtonService poll interval in effect right before
    // enter_standby() last widened it, restored by exit_standby().
    // Guarded by transition_mutex_ (see above).
    uint32_t saved_poll_ms_ = 20;

    core::SubscriptionId button_sub_ = 0;
};

}  // namespace wisp::app
