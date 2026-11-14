#pragma once

// Saturates a started EventBus on demand, the way a stalled wisp.bus
// overflows on the device: hold() parks the bus's dispatcher inside a
// handler, flood() then publishes into a queue nobody drains, so it drops
// its oldest events, and release() lets the dispatcher go on. Every step
// is gated, none timed: hold() returns only once the dispatcher is inside
// the handler, so nothing published after it can be dispatched before
// release().

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "wisp/core/event_bus.hpp"
#include "wisp/os/os.hpp"

namespace wisp::test {

class BusFlood {
public:
    // Bounds both waits, so a broken bus fails the test rather than
    // hanging it; a correct one never comes near.
    static constexpr uint32_t kWaitMs = 10000;

    explicit BusFlood(core::EventBus& bus) : bus_(bus) {
        id_ = bus_.subscribe("test.flood.hold", [this](const core::Event&) {
            entered_.give();
            if (!gate_.take(kWaitMs)) {
                timed_out_ = true;
            }
        });
    }
    ~BusFlood() {
        release();
        bus_.unsubscribe(id_);
    }

    BusFlood(const BusFlood&) = delete;
    BusFlood& operator=(const BusFlood&) = delete;

    // Publishes the hold event and returns once the dispatcher is handling
    // it. Events queued before it are dispatched first.
    void hold() {
        bus_.publish(core::Event{"test.flood.hold", "test", 0, {}});
        if (entered_.take(kWaitMs)) {
            held_ = true;
        } else {
            ADD_FAILURE() << "BusFlood: the dispatcher never reached the hold handler";
        }
    }

    // Publishes `events` events on "test.flood", which nothing handles.
    void flood(size_t events) {
        for (size_t i = 0; i < events; ++i) {
            bus_.publish(core::Event{"test.flood", "test", 0, {}});
        }
    }

    // Lets the hold handler return. A no-op while not held.
    void release() {
        if (held_) {
            held_ = false;
            gate_.give();
        }
        EXPECT_FALSE(timed_out_.load()) << "BusFlood: held past " << kWaitMs << " ms";
    }

private:
    core::EventBus& bus_;
    core::SubscriptionId id_ = 0;
    os::Semaphore entered_{1, 0};
    os::Semaphore gate_{1, 0};
    bool held_ = false;  // touched only by the test thread
    std::atomic<bool> timed_out_{false};
};

}  // namespace wisp::test
