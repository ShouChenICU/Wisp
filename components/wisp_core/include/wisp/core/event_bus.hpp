#pragma once

// Async, prefix-matching publish/subscribe event bus. Any thread may call
// publish(); a single dedicated dispatcher task calls matching subscriber
// handlers (in subscription order) outside of publish()'s own call stack,
// so publishers never run handler code themselves -- except, of course,
// when publish() is itself called from within a handler (on the
// dispatcher task), which is explicitly supported.
//
// Concurrency note on the underlying queue: wisp::os::Queue::push_drop_oldest()
// documents that it supports any number of concurrent pop() callers, but
// NOT concurrent callers of push_drop_oldest() itself -- it is designed
// for a single logical producer. EventBus's publish() must support any
// number of concurrent publisher threads, which does not fit that
// contract directly, so publish() serializes its own callers with
// `publish_mutex_` below before touching the queue, turning the many
// publisher threads into the single producer the queue expects. The
// dispatcher task is the queue's only consumer (pop()), which the queue
// supports concurrently with push_drop_oldest() without any extra
// locking on our part.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/os/os.hpp"
#include "wisp/os/queue.hpp"

namespace wisp::core {

struct Event {
    std::string topic;
    std::string source;
    uint64_t ts_ms = 0;  // filled in with os::now_ms() by publish() if left 0
    Properties payload;
};

using EventHandler = std::function<void(const Event&)>;
using SubscriptionId = uint32_t;

class EventBus {
public:
    explicit EventBus(size_t capacity = 64);

    // Equivalent to calling stop().
    ~EventBus();

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // Starts the dispatcher task (named "wisp.bus"). Idempotent: calling
    // start() again while already started is a safe no-op.
    Result<void> start();

    // Stops dispatching and joins the dispatcher task. Idempotent and
    // safe to call even if start() was never called.
    //
    // Safe to call from within a subscriber handler (i.e. from the
    // dispatcher task's own thread): in that case it marks the bus as
    // stopping and returns immediately *without* joining -- joining would
    // mean the dispatcher task joining itself, which would deadlock. The
    // dispatcher task actually exits shortly afterwards, once the
    // in-progress handler call returns and the dispatch loop observes the
    // stop; a later stop() call (including the one made by ~EventBus())
    // from any other thread then performs the real join.
    void stop();

    // Registers `handler` to be invoked, on the dispatcher task and in
    // subscription order relative to other matching subscribers, for
    // every subsequently dispatched event whose topic starts with
    // `prefix`. An empty prefix matches every topic. Safe to call from
    // within a handler (including the handler being subscribed, for
    // events dispatched after that call returns).
    SubscriptionId subscribe(std::string prefix, EventHandler handler);

    // Removes a subscription; a no-op if `id` is unknown (e.g. already
    // removed). Safe to call from within a handler, including the
    // handler being unsubscribed itself.
    void unsubscribe(SubscriptionId id);

    // Enqueues `e` for dispatch and returns immediately without blocking.
    // If e.ts_ms is 0, it is filled in with os::now_ms() first. If the
    // queue is full, the oldest queued event is dropped to make room for
    // this one; dropped() is incremented and a warning is logged. Safe to
    // call from any thread -- including from within a handler -- and
    // safe to call before start() (events simply accumulate in the queue
    // until the dispatcher starts consuming them).
    void publish(Event e);

    // Polls (does not busy-spin indefinitely) for up to timeout_ms for
    // the queue to drain and every handler invocation in flight to
    // finish. Returns true if that idle state was reached, false if
    // timeout_ms elapsed first. Never returns true while a handler is
    // still running.
    bool wait_idle(uint32_t timeout_ms);

    // Thread-safe. Total number of events dropped (due to queue overflow)
    // by publish() so far.
    uint64_t dropped() const;

private:
    struct Subscription {
        SubscriptionId id;
        std::string prefix;
        EventHandler handler;
    };

    void run_dispatcher();
    void dispatch(const Event& e);

    os::Queue<Event> queue_;
    os::Mutex publish_mutex_;  // serializes publish() callers; see file header note
    os::Mutex subs_mutex_;     // guards subs_ and next_id_

    std::vector<Subscription> subs_;
    SubscriptionId next_id_ = 1;

    std::atomic<uint64_t> dropped_{0};
    // Number of events that have been successfully enqueued but not yet
    // fully dispatched (i.e. still sitting in the queue, or currently
    // being handed to subscriber handlers). Reaches 0 exactly when the
    // bus is idle; see wait_idle().
    std::atomic<int64_t> inflight_{0};

    std::atomic<bool> running_{false};
    std::unique_ptr<os::Task> dispatcher_;
};

}  // namespace wisp::core
