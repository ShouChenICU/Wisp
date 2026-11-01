#include "wisp/core/event_bus.hpp"

#include <mutex>

#include "wisp/core/log.hpp"

namespace wisp::core {

namespace {
constexpr const char* kTag = "event_bus";
constexpr uint32_t kWaitIdlePollMs = 2;
// Log the overflow warning for the first drop, then only every Nth one
// after that, so sustained overflow can't flood the log.
constexpr uint64_t kDropLogInterval = 64;

// Identifies "am I the dispatcher task for this specific EventBus?"
// independent of the dispatcher_ handle. dispatcher_ gets moved out (by
// stop()/start()/~EventBus()) *before* the dispatcher task actually
// finishes running -- see run_dispatcher()'s TlsGuard -- so checking
// `dispatcher_->is_current()` from a thread that just took ownership of
// (or already saw null for) that handle is unreliable: it would see
// "not the dispatcher thread" even when called from a handler that *is*
// still running on it. This thread_local, set only by run_dispatcher()
// itself for the duration of that call, stays correct regardless of what
// any other thread does to dispatcher_ in the meantime.
thread_local const EventBus* tls_dispatching = nullptr;
}  // namespace

EventBus::EventBus(size_t capacity)
    : queue_(capacity), ready_(static_cast<uint32_t>(capacity) + 1, 0) {}

EventBus::~EventBus() { stop(); }

Result<void> EventBus::start() {
    if (tls_dispatching == this) {
        return Result<void>::err(ErrorCode::Busy,
                                 "start() called from within the dispatcher's own handler");
    }

    std::unique_ptr<os::Task> stale;
    uint64_t my_generation;
    {
        std::lock_guard<os::Mutex> lock(lifecycle_mutex_);
        if (dispatcher_ && running_.load(std::memory_order_acquire)) {
            // Already started and still live (nobody has asked it to
            // stop since): idempotent no-op, per start()'s documented
            // contract. Getting this right matters -- treating a live
            // dispatcher as "stale" here would retire a perfectly good
            // dispatcher task for no reason, and if it happened to be
            // idly blocked in ready_.take(kForever) with nothing else
            // around to publish and wake it, joining it below would
            // hang this call forever.
            return Result<void>::ok();
        }
        // Take ownership of any existing (but no longer live) dispatcher
        // -- e.g. one that a previous stop()-from-within-a-handler call
        // marked as stopping but could not join itself -- so it's
        // guaranteed joined before a new one is spawned, never
        // destroying/overwriting an unjoined Task. Bumping generation_
        // here (while still under the lock) ensures that stale
        // dispatcher's loop will see a mismatch and exit on its own if
        // it hasn't already, even though the actual join below happens
        // outside the lock.
        stale = std::move(dispatcher_);
        running_.store(true, std::memory_order_release);
        my_generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    if (stale) {
        // Wake it -- it may be idly blocked in ready_.take(kForever),
        // with nothing else around to publish() and wake it on its own
        // -- *before* joining, or this could block forever. Giving the
        // token *before* the join (not after) also means run_dispatcher()
        // sees running_/generation_ already updated by the time it wakes
        // and gives this same token back (see run_dispatcher()), rather
        // than racing the lock update.
        ready_.give();
        // Outside the lock: if the stale dispatcher is itself blocked
        // trying to acquire lifecycle_mutex_ (e.g. from a handler calling
        // stop()/start()/unsubscribe()), joining it while holding the
        // lock would deadlock.
        stale->join();
    }

    auto task = os::Task::spawn({.name = "wisp.bus", .stack_bytes = 6144, .priority = 5},
                                [this, my_generation] { run_dispatcher(my_generation); });
    std::lock_guard<os::Mutex> lock(lifecycle_mutex_);
    dispatcher_ = std::move(task);
    return Result<void>::ok();
}

void EventBus::stop() {
    bool self = (tls_dispatching == this);

    std::unique_ptr<os::Task> to_join;
    {
        std::lock_guard<os::Mutex> lock(lifecycle_mutex_);
        running_.store(false, std::memory_order_release);
        if (!dispatcher_) {
            return;
        }
        if (!self) {
            to_join = std::move(dispatcher_);
        }
    }

    // Wake a dispatcher that might be idly blocked waiting for work (see
    // ready_'s doc comment), so it notices running_ is now false instead
    // of blocking forever. Done outside the lock and regardless of
    // `self`: even when stop() is called from within a handler, the
    // dispatcher isn't blocked on ready_ *right now* (it's busy running
    // this handler), but it will be the next time its loop comes around,
    // and there may be no new publish() left to wake it naturally.
    ready_.give();

    if (self || !to_join) {
        return;  // self-join would deadlock; see stop()'s doc comment
    }
    to_join->join();
}

SubscriptionId EventBus::subscribe(std::string prefix, EventHandler handler) {
    auto shared_handler = std::make_shared<EventHandler>(std::move(handler));
    std::lock_guard<os::Mutex> lock(subs_mutex_);
    SubscriptionId id = next_id_++;
    subs_.push_back(Subscription{id, std::move(prefix), std::move(shared_handler)});
    return id;
}

void EventBus::unsubscribe(SubscriptionId id) {
    bool on_dispatcher_thread = (tls_dispatching == this);

    // See the doc comment on unsubscribe(): wait out any in-flight
    // dispatch when called from outside the dispatcher task, but never
    // self-wait when called from within a handler on the dispatcher's
    // own thread (that would deadlock against the dispatch we're
    // currently inside of).
    std::unique_lock<os::Mutex> dispatch_lock;
    if (!on_dispatcher_thread) {
        dispatch_lock = std::unique_lock<os::Mutex>(dispatch_mutex_);
    }

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
        // Increment *before* push_drop_oldest() -- see inflight_'s doc
        // comment for why the ordering matters under concurrent
        // publishers.
        inflight_.fetch_add(1, std::memory_order_acq_rel);
        dropped_one = queue_.push_drop_oldest(std::move(e));
        if (dropped_one) {
            // The oldest queued event was evicted without ever being
            // dispatched; undo the increment for *that* slot (the new
            // event keeps its own increment above).
            inflight_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    // Exactly one new deliverable item was added above, whether or not
    // it also evicted an older one, so the dispatcher always gets woken
    // exactly once per publish() call.
    ready_.give();

    if (dropped_one) {
        uint64_t total_dropped = dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (total_dropped == 1 || total_dropped % kDropLogInterval == 0) {
            WLOG_W(kTag, "event queue full, dropped oldest event (dropped=%llu total)",
                   static_cast<unsigned long long>(total_dropped));
        }
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
    // Held for the whole call (copy + invoke), so unsubscribe() from a
    // non-dispatcher thread can block on it to know "no dispatch is
    // currently in flight" -- see dispatch_mutex_'s and unsubscribe()'s
    // doc comments.
    std::lock_guard<os::Mutex> dispatch_lock(dispatch_mutex_);

    // Copy the matching handlers out while holding subs_mutex_, then
    // invoke them outside the lock -- so a handler that itself calls
    // subscribe()/unsubscribe() (on this or another subscription) never
    // re-enters subs_mutex_ while it is held, and so a slow handler never
    // blocks a concurrent subscribe()/unsubscribe() call from another
    // thread.
    std::vector<std::shared_ptr<EventHandler>> matched;
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
        (*handler)(e);
    }
}

void EventBus::run_dispatcher(uint64_t my_generation) {
    // Set only for the duration of this call -- see tls_dispatching's
    // doc comment above for why this (rather than checking the
    // dispatcher_ handle) is what start()/stop()/unsubscribe() use to
    // tell whether they're running on the dispatcher task.
    struct TlsGuard {
        explicit TlsGuard(const EventBus* self) { tls_dispatching = self; }
        ~TlsGuard() { tls_dispatching = nullptr; }
    } tls_guard(this);

    while (true) {
        ready_.take(os::kForever);
        if (!running_.load(std::memory_order_acquire) ||
            generation_.load(std::memory_order_acquire) != my_generation) {
            // Give the token we just took back before exiting: it may
            // have been a synthetic stop()/start() wake (harmless to
            // return -- some dispatcher's next pop(e, 0) will just find
            // nothing and loop again) or it may have belonged to a real,
            // still-undelivered event. Either way, nothing else is going
            // to give a replacement for it, so returning without giving
            // it back would leave the count of available tokens
            // permanently short of the number of real undelivered
            // events -- stranding the last one until some unrelated
            // future publish() happens to cover for it.
            ready_.give();
            return;
        }
        Event e;
        if (!queue_.pop(e, 0)) {
            // A spurious wake with nothing to pop: either a stop()/
            // start() wake token (handled above) or a ready_ token whose
            // event was later evicted by push_drop_oldest() before we
            // got to it. Either way, just wait for the next one.
            continue;
        }
        dispatch(e);
        inflight_.fetch_sub(1, std::memory_order_acq_rel);
    }
}

}  // namespace wisp::core
