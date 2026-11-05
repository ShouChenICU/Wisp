#pragma once

// The system notifications App posts from other services' signals
// (scene-device spec 5.2), and the timers NotificationService coalesces its
// writes with.

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "wisp/core/notification_service.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/os/os.hpp"

namespace wisp::app {

// Posts a notice::kWakeLockLong for every lock due a reminder at `now_ms`
// (WakeLockRegistry::due_reminders()), with the holder as its argument. A
// scene's lock carries the release_lock action with the lock id; a system
// lock has no action, the user cannot release it.
void post_wakelock_reminders(core::WakeLockRegistry& locks, core::NotificationService& notes,
                             uint64_t now_ms);

// One-shot callbacks after a delay, each on its own os::Timer: a single
// re-armed one-shot timer would drop a schedule() made while its callback
// is still returning (start() is a no-op on an active timer). Timers are
// created on demand and the fired ones are freed on the next schedule().
class OneShotTimers {
public:
    OneShotTimers() = default;
    ~OneShotTimers();  // stop()

    OneShotTimers(const OneShotTimers&) = delete;
    OneShotTimers& operator=(const OneShotTimers&) = delete;

    // Runs `cb` once after `ms` on the timer task. Dropped (logged) after
    // stop() or if the port cannot make a timer.
    void schedule(uint32_t ms, std::function<void()> cb);

    // Cancels what has not fired, waits for any callback running, and drops
    // every later schedule(). Not from inside a callback.
    void stop();

private:
    os::Mutex mutex_;
    std::vector<std::unique_ptr<os::Timer>> timers_;
    bool stopped_ = false;
};

}  // namespace wisp::app
