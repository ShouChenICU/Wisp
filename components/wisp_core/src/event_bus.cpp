#include "wisp/core/event_bus.hpp"

#include <mutex>

#include "wisp/core/log.hpp"

namespace wisp::core {

namespace {
constexpr const char* kTag = "event_bus";
// How long the dispatcher task blocks in queue_.pop() before re-checking
// running_. This is what lets stop()/~EventBus() join a dispatcher that is
// currently blocked waiting for the next event, instead of hanging.
constexpr uint32_t kDispatcherPollMs = 50;
constexpr uint32_t kWaitIdlePollMs = 2;
}  // namespace

EventBus::EventBus(size_t capacity) : queue_(capacity) {}

EventBus::~EventBus() { stop(); }

Result<void> EventBus::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return Result<void>::ok();  // already running: idempotent no-op
    }
    dispatcher_ = os::Task::spawn({.name = "wisp.bus", .stack_bytes = 6144, .priority = 5},
                                  [this] { run_dispatcher(); });
    return Result<void>::ok();
}

void EventBus::stop() {
    running_.store(false, std::memory_order_release);
    if (!dispatcher_) {
        return;
    }
    if (dispatcher_->is_current()) {
        // Called from within a handler running on the dispatcher task
        // itself. See the doc comment on stop() for why we must not join
        // here, and who ends up joining instead.
        return;
    }
    dispatcher_->join();
}

SubscriptionId EventBus::subscribe(std::string prefix, EventHandler handler) {
    std::lock_guard<os::Mutex> lock(subs_mutex_);
    SubscriptionId id = next_id_++;
    subs_.push_back(Subscription{id, std::move(prefix), std::move(handler)});
    return id;
}

void EventBus::unsubscribe(SubscriptionId id) {
    std::lock_guard<os::Mutex> lock(subs_mutex_);
    for (auto it = subs_.begin(); it != subs_.end(); ++it) {
        if (it->id == id) {
            subs_.erase(it);
            return;
        }
    }
}

void EventBus::publish(Event e) {
    if (e.ts_ms == 0) {
        e.ts_ms = os::now_ms();
    }

    bool dropped_one;
    {
        // See the class's file-header note: push_drop_oldest() itself
        // does not support concurrent callers, so we serialize them here
        // to present the queue with a single logical producer.
        std::lock_guard<os::Mutex> lock(publish_mutex_);
        dropped_one = queue_.push_drop_oldest(std::move(e));
    }

    if (dropped_one) {
        // A drop means one previously-enqueued event was evicted (net
        // queue occupancy unchanged), so inflight_ does not change: the
        // new event's slot replaces the dropped one's.
        dropped_.fetch_add(1, std::memory_order_relaxed);
        WLOG_W(kTag, "event queue full, dropped oldest event");
    } else {
        inflight_.fetch_add(1, std::memory_order_acq_rel);
    }
}

bool EventBus::wait_idle(uint32_t timeout_ms) {
    uint64_t start = os::now_ms();
    while (inflight_.load(std::memory_order_acquire) != 0) {
        if (os::now_ms() - start >= timeout_ms) {
            return inflight_.load(std::memory_order_acquire) == 0;
        }
        os::sleep_ms(kWaitIdlePollMs);
    }
    return true;
}

uint64_t EventBus::dropped() const { return dropped_.load(std::memory_order_relaxed); }

void EventBus::dispatch(const Event& e) {
    // Copy the matching handlers out while holding subs_mutex_, then
    // invoke them outside the lock -- so a handler that itself calls
    // subscribe()/unsubscribe() (on this or another subscription) never
    // re-enters subs_mutex_ while it is held, and so a slow handler never
    // blocks a concurrent subscribe()/unsubscribe() call from another
    // thread.
    std::vector<EventHandler> matched;
    {
        std::lock_guard<os::Mutex> lock(subs_mutex_);
        matched.reserve(subs_.size());
        for (const auto& sub : subs_) {
            if (e.topic.starts_with(sub.prefix)) {
                matched.push_back(sub.handler);
            }
        }
    }
    for (auto& handler : matched) {
        handler(e);
    }
}

void EventBus::run_dispatcher() {
    while (running_.load(std::memory_order_acquire)) {
        Event e;
        if (!queue_.pop(e, kDispatcherPollMs)) {
            continue;  // timed out; re-check running_
        }
        dispatch(e);
        inflight_.fetch_sub(1, std::memory_order_acq_rel);
    }
}

}  // namespace wisp::core
