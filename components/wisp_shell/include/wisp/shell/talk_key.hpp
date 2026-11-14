#pragma once

// TalkKey (MVP spec 4.1, 6.3): the button whose caps "role" is "talk"
// (find_talk_button()) as hold-to-talk. Held for voice::kHoldStartMs from
// its "<id>.down" it calls hold_begin(); its "<id>.up" then calls
// hold_end(). Released sooner, the press is no hold.
//
// The press primes the mic (the controller's press_begin() at the down)
// and every press ends with press_end(): right after hold_begin(), at a
// release before the hold, or at stop() (see conversation.hpp).
//
// From a dark screen the key talks too: the button event itself wakes the
// screen (PowerManager's activity hook), and held kHoldStartMs it starts
// the capture like any other press; released sooner it only woke the
// screen. Such a press does not prime the mic, which standby powered down
// and the wake is still powering up. This needs the key's down/up events
// in standby: an EdgeEvent button delivers them, while a PollOnly button
// is not read in standby (docs/architecture.md "Button input and
// standby"), so its press wakes nothing and is not seen here at all.
//
// The hold is timed with now_ms() on a kPollMs timer that runs only while
// the key is down. The bus callbacks and the timer may call into the
// controller; its triggers never block.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "wisp/core/event_bus.hpp"
#include "wisp/os/os.hpp"
#include "wisp/shell/conversation.hpp"

namespace wisp::shell {

class TalkKey {
public:
    static constexpr uint32_t kPollMs = 20;

    TalkKey(core::EventBus& bus, std::string button_id, ConversationController& conv,
            std::function<bool()> screen_on, std::function<uint64_t()> now_ms = os::now_ms);
    ~TalkKey();  // stop()

    TalkKey(const TalkKey&) = delete;
    TalkKey& operator=(const TalkKey&) = delete;

    // Subscribes to the button's events. Idempotent.
    void start();
    // Unsubscribes and stops the timer; a hold in progress is cancelled
    // (hold_cancel()): at shutdown its recording has nowhere to go.
    // Idempotent.
    void stop();

private:
    void on_event(const core::Event& e);
    void on_down();
    void on_up();
    void tick();

    core::EventBus& bus_;
    std::string id_;
    ConversationController& conv_;
    std::function<bool()> screen_on_;
    std::function<uint64_t()> now_ms_;
    std::unique_ptr<os::Timer> timer_;
    core::SubscriptionId sub_ = 0;

    os::Mutex mu_;
    bool pressed_ = false;  // the key is down
    bool holding_ = false;  // ... and has called hold_begin()
    uint64_t down_ms_ = 0;
};

}  // namespace wisp::shell
