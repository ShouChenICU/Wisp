#include "wisp/net/request_watch.hpp"

#include <mutex>
#include <utility>

namespace wisp::net {

RequestWatch::RequestWatch() = default;

RequestWatch::~RequestWatch() { disarm(); }

void RequestWatch::arm(uint32_t timeout_ms) {
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (disarmed_ || reason_ != Reason::None || timer_) return;
    }
    if (timeout_ms == 0) {
        fire(Reason::Deadline);
        return;
    }
    timer_ = std::make_unique<os::Timer>("wisp.http.watch", timeout_ms, false,
                                         [this] { fire(Reason::Deadline); });
    timer_->start();
}

void RequestWatch::attach(std::function<void()> stop) {
    std::lock_guard<os::Mutex> lock(mu_);
    if (disarmed_) return;
    if (reason_ != Reason::None) {
        if (stop) stop();
        return;
    }
    stop_ = std::move(stop);
}

void RequestWatch::abort() { fire(Reason::Aborted); }

void RequestWatch::disarm() {
    std::function<void()> dropped;
    {
        // Taking the lock waits out a stop() that is running.
        std::lock_guard<os::Mutex> lock(mu_);
        disarmed_ = true;
        dropped = std::move(stop_);
        stop_ = nullptr;
    }
    // Outside the lock: a timer callback in flight blocks on it in fire(),
    // and stop() waits for that callback to return.
    if (timer_) timer_->stop();
}

RequestWatch::Reason RequestWatch::reason() const {
    std::lock_guard<os::Mutex> lock(mu_);
    return reason_;
}

void RequestWatch::fire(Reason why) {
    std::lock_guard<os::Mutex> lock(mu_);
    if (disarmed_ || reason_ != Reason::None) return;
    reason_ = why;
    if (stop_) {
        stop_();
        stop_ = nullptr;
    }
}

}  // namespace wisp::net
