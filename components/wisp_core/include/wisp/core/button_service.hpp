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
    // Idempotent the same way os::Timer::start() is.
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
    void poll(uint64_t now_ms);

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
    uint32_t poll_ms_;

    std::vector<ButtonState> buttons_;
    bool buttons_loaded_ = false;

    os::Timer timer_;
};

}  // namespace wisp::core
