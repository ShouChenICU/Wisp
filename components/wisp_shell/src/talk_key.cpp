#include "wisp/shell/talk_key.hpp"

#include <mutex>
#include <utility>

#include "wisp/core/log.hpp"
#include "wisp/voice/limits.hpp"

namespace wisp::shell {

TalkKey::TalkKey(core::EventBus& bus, std::string button_id, ConversationController& conv,
                 std::function<bool()> screen_on, std::function<uint64_t()> now_ms)
    : bus_(bus),
      id_(std::move(button_id)),
      conv_(conv),
      screen_on_(std::move(screen_on)),
      now_ms_(now_ms ? std::move(now_ms) : std::function<uint64_t()>(os::now_ms)) {}

TalkKey::~TalkKey() { stop(); }

void TalkKey::start() {
    if (sub_ != 0) return;
    if (!timer_) {
        timer_ = os::Timer::create("wisp.talk", kPollMs, /*periodic=*/true, [this] { tick(); });
        if (!timer_) WLOG_W("shell", "no timer for the talk key; holding it does nothing");
    }
    sub_ = bus_.subscribe(id_ + ".", [this](const core::Event& e) { on_event(e); });
}

void TalkKey::stop() {
    if (sub_ != 0) {
        bus_.unsubscribe(sub_);
        sub_ = 0;
    }
    if (timer_) timer_->stop();
    bool was_holding = false;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        was_holding = holding_;
        pressed_ = holding_ = false;
    }
    if (was_holding) conv_.hold_cancel();
}

void TalkKey::on_event(const core::Event& e) {
    if (e.topic == id_ + ".down") {
        on_down();
    } else if (e.topic == id_ + ".up") {
        on_up();
    }
}

void TalkKey::on_down() {
    // The press that wakes the screen does only that.
    if (screen_on_ && !screen_on_()) return;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        pressed_ = true;
        holding_ = false;
        down_ms_ = now_ms_();
    }
    // Not under mu_: start() may wait for a tick that takes it.
    if (timer_) timer_->start();
}

void TalkKey::on_up() {
    bool was_holding = false;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (!pressed_) return;
        was_holding = holding_;
        pressed_ = holding_ = false;
    }
    // A tick that saw the press set holding_ before the read above; this
    // waits for it to return, so its hold_begin() comes first.
    if (timer_) timer_->stop();
    if (was_holding) conv_.hold_end();
}

void TalkKey::tick() {
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (!pressed_ || holding_) return;
        if (now_ms_() - down_ms_ < voice::kHoldStartMs) return;
        holding_ = true;
    }
    if (timer_) timer_->stop();
    conv_.hold_begin();
}

}  // namespace wisp::shell
