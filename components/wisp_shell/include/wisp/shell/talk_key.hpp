#pragma once

// TalkKey (MVP spec 4.1, 6.3): the button whose caps "role" is "talk"
// (find_talk_button()) as hold-to-talk. Held for voice::kHoldStartMs from
// its "<id>.down" it calls hold_begin(); its "<id>.up" then calls
// hold_end(). Released sooner, the press does nothing. A press that
// starts while the screen is off only wakes the device (PowerManager does
// that) and is ignored here, its release included.
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
    // Unsubscribes and stops the timer; a hold in progress is ended
    // (hold_end()). Idempotent.
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
    bool pressed_ = false;  // a press that counts is down
    bool holding_ = false;  // ... and has called hold_begin()
    uint64_t down_ms_ = 0;
};

}  // namespace wisp::shell
