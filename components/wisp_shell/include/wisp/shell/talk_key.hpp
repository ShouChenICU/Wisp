#pragma once

// TalkKey (MVP spec 4.1, 6.3): the button with role talk
// (find_talk_button()) as hold-to-talk. Held for voice::kHoldStartMs from
// its down edge it calls hold_begin(); its up edge then calls hold_end().
// Released sooner, the press is no hold.
//
// The press primes the mic (the controller's press_begin() at the down)
// and every press ends with press_end(): right after hold_begin(), at a
// release before the hold, or at stop() (see conversation.hpp).
//
// From a dark screen the key talks too: the press itself wakes the screen
// (PowerManager's own ButtonService listener), and held kHoldStartMs it
// starts the capture like any other press; released sooner it only woke
// the screen. Such a press does not prime the mic, which standby powered
// down and the wake is still powering up. This needs the key's down/up
// edges in standby: an EdgeEvent button delivers them, while a PollOnly
// button is not read in standby (docs/architecture.md "Button input and
// standby"), so its press wakes nothing and is not seen here at all.
//
// Edges. A ButtonService listener pushes the key's down and up edges (only
// those) into an EdgeInbox ("talk_key", 8 edges) drained on wisp.bus,
// where the key's logic and its calls into the controller run. No edge
// needs the bus's event queue, so a flooded bus loses none; while the bus
// is busy they wait in the inbox, in order. An overflow (more than 8 edges
// waiting) drops the oldest, is counted (overflows()) and logged.
//
// Timing. The hold is timed from the down edge's own time, not from when
// it was handled, with now_ms() on a kPollMs timer that runs only while
// the key is down: a down handled late starts the hold at the next tick
// if it is already kHoldStartMs old, so a late delivery does not turn a
// short press into a hold or delay the start of a hold. The end is not
// back-dated: the capture ends when the up is handled, so a late up ends
// the capture late. Two guards keep a short press a press however late
// it is handled: the listener stores the last up's time, and the timer
// starts no hold for a key already released at or after its down; and an
// up less than kHoldStartMs after its down drops a hold the timer already
// began (hold_cancel()) instead of sending it. A press whose down and up
// both wait in the inbox past kHoldStartMs is therefore no hold: its
// sound was never recorded.
//
// The controller calls (press_begin(), hold_begin() and the rest) run on
// wisp.bus and on the timer; its triggers never block.
//
// A key that holds both the power and the talk role gets both
// behaviours: this does not arbitrate (the device model warns at boot).

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "wisp/core/button_service.hpp"
#include "wisp/core/edge_inbox.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"
#include "wisp/shell/conversation.hpp"

namespace wisp::shell {

class TalkKey {
public:
    static constexpr uint32_t kPollMs = 20;

    // `bus` is the dispatcher the edges are handled on; `buttons` (whose
    // listeners hear the key) must outlive the key.
    TalkKey(core::EventBus& bus, core::ButtonService& buttons, std::string button_id,
            ConversationController& conv, std::function<bool()> screen_on,
            std::function<uint64_t()> now_ms = os::now_ms);
    ~TalkKey();  // stop()

    TalkKey(const TalkKey&) = delete;
    TalkKey& operator=(const TalkKey&) = delete;

    // Builds the inbox and adds the listener. Idempotent.
    void start();
    // Removes the listener, then destroys the inbox (waiting for a run on
    // wisp.bus in progress; edges still waiting are dropped), then stops
    // the timer; a hold in progress is cancelled (hold_cancel()): at
    // shutdown its recording has nowhere to go. Not from wisp.bus.
    // Idempotent.
    void stop();

    // Edges the inbox dropped since start(); 0 while stopped. Any thread
    // between start() and stop() (an atomic).
    uint32_t overflows() const { return inbox_ ? inbox_->overflows() : 0; }

private:
    void on_edge(const core::ButtonEdge& e);
    void on_down(uint64_t at_us);
    void on_up(uint64_t at_us);
    void tick();

    core::EventBus& bus_;
    core::ButtonService& buttons_;
    std::string id_;
    ConversationController& conv_;
    std::function<bool()> screen_on_;
    std::function<uint64_t()> now_ms_;
    std::unique_ptr<os::Timer> timer_;
    // Between start() and stop(); its only producer is the listener.
    os::LargeUnique<core::EdgeInbox<core::ButtonEdge, 8>> inbox_;
    uint32_t listener_ = 0;  // 0 while stopped
    // The last up edge's time, stored by the listener as it happens (on
    // wisp.button), before wisp.bus handles it; 0: none yet.
    std::atomic<uint64_t> last_up_us_{0};

    os::Mutex mu_;
    bool pressed_ = false;  // the key is down
    bool holding_ = false;  // ... and has called hold_begin()
    uint64_t down_us_ = 0;  // the down edge's time
    // The down edge's time on now_ms()'s scale, never ahead of it.
    uint64_t down_ms_ = 0;
};

}  // namespace wisp::shell
