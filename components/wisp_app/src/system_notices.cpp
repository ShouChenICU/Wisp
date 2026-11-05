#include "wisp/app/system_notices.hpp"

#include <mutex>
#include <string>
#include <utility>

#include "wisp/core/log.hpp"

namespace wisp::app {

void post_wakelock_reminders(core::WakeLockRegistry& locks, core::NotificationService& notes,
                             uint64_t now_ms) {
    for (const core::WakeLockInfo& lock : locks.due_reminders(now_ms)) {
        if (lock.holder.rfind("scene.", 0) == 0) {
            notes.post_system(core::notice::kWakeLockLong, lock.holder,
                              core::notice::kActionReleaseLock, std::to_string(lock.id));
        } else {
            notes.post_system(core::notice::kWakeLockLong, lock.holder);
        }
    }
}

OneShotTimers::~OneShotTimers() { stop(); }

void OneShotTimers::schedule(uint32_t ms, std::function<void()> cb) {
    auto timer = os::Timer::create("wisp.oneshot", ms, /*periodic=*/false, std::move(cb));
    if (!timer) {
        WLOG_W("app", "no timer for a delayed job; dropped");
        return;
    }
    std::vector<std::unique_ptr<os::Timer>> fired;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (stopped_) return;
        // A timer whose callback is running (this call may come from it)
        // is still active and stays.
        for (auto it = timers_.begin(); it != timers_.end();) {
            if (!(*it)->active()) {
                fired.push_back(std::move(*it));
                it = timers_.erase(it);
            } else {
                ++it;
            }
        }
        timer->start();
        timers_.push_back(std::move(timer));
    }
    // Destroyed outside the lock: a destructor waits for its callback.
}

void OneShotTimers::stop() {
    std::vector<std::unique_ptr<os::Timer>> all;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        stopped_ = true;
        all = std::move(timers_);
        timers_.clear();
    }
    for (auto& t : all) t->stop();
}

}  // namespace wisp::app
