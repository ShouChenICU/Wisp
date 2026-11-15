#include "wisp/shell/talk_key.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

#include "wisp/core/log.hpp"
#include "wisp/voice/limits.hpp"

namespace wisp::shell {

TalkKey::TalkKey(core::EventBus& bus, core::ButtonService& buttons, std::string button_id,
                 ConversationController& conv, std::function<bool()> screen_on,
                 std::function<uint64_t()> now_ms)
    : bus_(bus),
      buttons_(buttons),
      id_(std::move(button_id)),
      conv_(conv),
      screen_on_(std::move(screen_on)),
      now_ms_(now_ms ? std::move(now_ms) : std::function<uint64_t()>(os::now_ms)) {}

TalkKey::~TalkKey() { stop(); }

void TalkKey::start() {
    if (listener_ != 0) return;
    if (!timer_) {
        timer_ = os::Timer::create("wisp.talk", kPollMs, /*periodic=*/true, [this] { tick(); });
        if (!timer_) WLOG_W("shell", "no timer for the talk key; holding it does nothing");
    }
    inbox_ = os::make_large<core::EdgeInbox<core::ButtonEdge, 8>>(
        "talk_key", bus_, [this](const core::ButtonEdge& e) { on_edge(e); });
    if (!inbox_) {
        WLOG_E("shell", "no memory for the talk key's inbox; the key does nothing");
        return;
    }
    // On wisp.button under ButtonService's lock: a compare, an atomic and
    // a push (which, on an overflow, also logs the inbox's rate-limited
    // warning there). Short and long edges would only take room in the
    // ring.
    listener_ = buttons_.add_listener([this](const core::ButtonEdge& e) {
        if (e.kind != core::ButtonEdge::Kind::Down && e.kind != core::ButtonEdge::Kind::Up) {
            return;
        }
        if (e.id.view() != id_) return;
        if (e.kind == core::ButtonEdge::Kind::Up) {
            last_up_us_.store(e.at_us, std::memory_order_release);
        }
        inbox_->push(e);
    });
}

void TalkKey::stop() {
    if (listener_ != 0) {
        buttons_.remove_listener(listener_);
        listener_ = 0;
    }
    // With no producer left: waits for a run in progress, so no edge is
    // handled once this returns, and drops the edges still waiting (the
    // press state is reset below).
    inbox_.reset();
    if (timer_) timer_->stop();
    bool was_pressed = false;
    bool was_holding = false;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        was_pressed = pressed_;
        was_holding = holding_;
        pressed_ = holding_ = false;
    }
    if (was_holding) {
        conv_.hold_cancel();
    } else if (was_pressed) {
        conv_.press_end();
    }
}

void TalkKey::on_edge(const core::ButtonEdge& e) {
    if (e.kind == core::ButtonEdge::Kind::Down) {
        on_down(e.at_us);
    } else {
        on_up(e.at_us);
    }
}

void TalkKey::on_down(uint64_t at_us) {
    // From a dark screen the press counts too, but it does not prime: the
    // wake the press asked for is still on its way (PowerManager's
    // follower on the UI task), and the mic is powered up only with it.
    const bool lit = !screen_on_ || screen_on_();
    {
        std::lock_guard<os::Mutex> lock(mu_);
        pressed_ = true;
        holding_ = false;
        down_us_ = at_us;
        // An edge stamped ahead of the clock would make tick()'s unsigned
        // now - down wrap around.
        down_ms_ = std::min<uint64_t>(at_us / 1000, now_ms_());
    }
    if (lit) conv_.press_begin();
    // Not under mu_: start() may wait for a tick that takes it.
    if (timer_) timer_->start();
}

void TalkKey::on_up(uint64_t at_us) {
    bool was_holding = false;
    bool short_press = false;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (!pressed_) return;
        was_holding = holding_;
        // Released sooner than a hold takes: a press, even if a tick began
        // a hold because this up was handled late (an up stamped before
        // its down counts as short too).
        short_press = at_us < down_us_ || at_us - down_us_ < uint64_t{voice::kHoldStartMs} * 1000;
        pressed_ = holding_ = false;
    }
    // A tick that saw the press set holding_ before the read above; this
    // waits for it to return, so its hold_begin() comes first.
    if (timer_) timer_->stop();
    if (was_holding && short_press) {
        conv_.hold_cancel();
    } else if (was_holding) {
        conv_.hold_end();
    } else {
        conv_.press_end();
    }
}

void TalkKey::tick() {
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (!pressed_ || holding_) return;
        if (now_ms_() - down_ms_ < voice::kHoldStartMs) return;
        // Held long enough by the clock, but the listener has already seen
        // this press's up, which wisp.bus has not handled yet: a press
        // delivered late, not a hold.
        const uint64_t up_us = last_up_us_.load(std::memory_order_acquire);
        if (up_us != 0 && up_us >= down_us_) return;
        holding_ = true;
    }
    if (timer_) timer_->stop();
    conv_.hold_begin();
    // The press is a hold now (or, refused, never will be): the controller
    // unprimes unless the hold's capture took the mic.
    conv_.press_end();
}

}  // namespace wisp::shell
