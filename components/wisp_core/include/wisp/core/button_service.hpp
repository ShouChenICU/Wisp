#pragma once

// Portable short-press/long-press gesture detection on top of
// wisp::hal::Button's raw level. A periodic os::Timer polls every tracked
// button's pressed() state and publishes events to the EventBus on topic
// "<id>.<down|up|short|long>" (id is the device id, e.g. "button.power",
// so the topic is e.g. "button.power.short"); `Event::source` is the same
// device id.
//
// Rules, per button, independently of every other button:
//   - a transition to pressed publishes `down`;
//   - once held continuously for `long_press_ms`, publishes `long` exactly
//     once for that press (not repeated while still held -- see
//     global-constraints.md Review Focus #4);
//   - on release, publishes `up`, followed by `short` only if `long` was
//     not already sent for this press.
//
// The set of buttons polled is read from the DeviceRegistry once -- on
// start(), or lazily on the first poll() call if start() was never called
// (e.g. a test driving poll() directly with synthetic timestamps).
// Buttons added to the registry after that point are not picked up; this
// keeps poll() itself allocation-free and free of registry locking on the
// hot path. See device_registry.hpp and task-7-brief.md.
//
// Detection is poll-driven: a press that goes down and back up again
// between two polls (i.e. shorter than poll_ms) is never observed and so
// produces no events at all. This is intentional -- poll_ms is the
// service's deliberate debounce/resolution floor, not a bug.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/hal/button.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

class ButtonService {
public:
    ButtonService(DeviceRegistry& registry, EventBus& bus, uint32_t long_press_ms = 1000,
                  uint32_t poll_ms = 20);

    ButtonService(const ButtonService&) = delete;
    ButtonService& operator=(const ButtonService&) = delete;

    // Refreshes the tracked button list from the registry (see file
    // header) and starts the periodic poll Timer, which calls
    // poll(os::now_ms()) on its own dedicated thread every poll_ms.
    //
    // Truly idempotent: if the Timer is already active (i.e. a previous
    // start() wasn't followed by stop()), this returns immediately
    // without touching the button list at all. This matters because the
    // Timer thread may be concurrently inside poll() at the exact moment
    // a second start() is called from another thread -- re-running
    // refresh_buttons() in that window would both race poll()'s reads of
    // buttons_ (see mutex_ below) and silently discard in-progress
    // press/hold state for every tracked button, as if they'd just been
    // released and re-pressed.
    void start();

    // Stops the poll Timer. Per os::Timer::stop(), once this returns
    // (when called from outside the Timer's own callback thread, i.e.
    // outside of poll()), no poll() call is in flight and none will run
    // again until the next start().
    void stop();

    // Reads every tracked button's current pressed() level and publishes
    // the down/long/up/short transitions described above, using `now_ms`
    // as the current time. Safe to call directly -- with synthetic
    // timestamps, and without start() ever having been called -- which is
    // how this is unit tested. On the very first call, if the button list
    // hasn't been populated yet (start() was never called first), it is
    // refreshed from the registry before polling.
    //
    // Thread-safe: holds mutex_ for the whole call (both the button-state
    // scan and the publish() calls it makes -- safe to hold a lock across
    // those since EventBus::publish() never blocks on subscriber
    // handlers, per its own doc comment), so a poll() driven by the
    // Timer thread and one called directly/concurrently from elsewhere
    // never interleave their reads/writes of per-button state.
    void poll(uint64_t now_ms);

    // Changes the poll period, taking effect immediately: if the Timer
    // is currently active, it is stopped and replaced with a new one at
    // the new period (then restarted); if not (start() hasn't been
    // called, or stop() has), only poll_ms_ itself is updated, and the
    // new period takes effect the next time start() creates/starts the
    // timer. A no-op if `poll_ms` already equals the current period.
    // Thread-safe, including against a concurrent start()/stop() (see
    // timer_mutex_ below); does not touch buttons_/buttons_loaded_, so
    // it never races poll() the way a second start() would (see
    // start()'s doc comment) -- only the Timer itself is swapped.
    //
    // Task 18b: the default 20ms poll period is widened to 50ms while
    // the device is in standby (see wisp::app::PowerManager) and
    // restored on wake.
    void set_poll_interval_ms(uint32_t poll_ms);

    // The current poll period (see set_poll_interval_ms()). Mainly for
    // tests; thread-safe.
    uint32_t poll_interval_ms() const;

private:
    struct ButtonState {
        std::shared_ptr<hal::Button> button;
        std::string id;  // device id, e.g. "button.power"; also the topic prefix and source
        bool pressed = false;
        uint64_t down_at = 0;
        bool long_sent = false;
    };

    void refresh_buttons();
    void publish_event(const ButtonState& state, const char* suffix);

    DeviceRegistry& registry_;
    EventBus& bus_;
    uint32_t long_press_ms_;

    // Guards buttons_/buttons_loaded_ against concurrent poll()/start()
    // calls (see their doc comments above). refresh_buttons() is always
    // called with this already held by its caller.
    os::Mutex mutex_;
    std::vector<ButtonState> buttons_;
    bool buttons_loaded_ = false;

    // Guards poll_ms_ and timer_ itself (the pointer, not what it does
    // internally) against concurrent start()/stop()/set_poll_interval_ms()
    // calls. Never held across anything that takes mutex_ above (poll(),
    // the Timer's own callback, never touches timer_mutex_), so holding
    // it across a (possibly blocking) Timer::stop()/start() call below
    // cannot deadlock against poll().
    mutable os::Mutex timer_mutex_;
    uint32_t poll_ms_;
    std::unique_ptr<os::Timer> timer_;
};

}  // namespace wisp::core
