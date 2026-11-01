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

    // Equivalent to calling stop(). Not safe to call from within one of
    // this bus's own handlers (the same way os::Timer documents that
    // destroying a Timer from inside its own callback is not supported):
    // destroying the object out from under a callback currently running
    // on one of its member threads is a caller lifetime bug no amount of
    // internal locking can paper over.
    ~EventBus();

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // Starts the dispatcher task (named "wisp.bus"). Idempotent: calling
    // start() again while a dispatcher from a previous start() is still
    // live (i.e. nothing has called stop() since) is a safe no-op.
    //
    // If a previous dispatcher is still shutting down instead (e.g.
    // stop() was called from within a handler and hasn't finished
    // exiting yet), this blocks until it has fully exited before
    // starting the new one -- so it's never possible for two dispatcher
    // tasks to be delivering events at the same time.
    //
    // Returns an error (ErrorCode::Busy) without doing anything if called
    // from within a handler on the dispatcher task's own thread -- in
    // that case "the previous dispatcher" is this very call stack, and
    // waiting for it to exit would mean waiting for itself.
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
    // stop; a later stop() call (including the one made by ~EventBus()),
    // or the next start(), from any other thread then performs the real
    // join before doing anything else.
    void stop();

    // Registers `handler` to be invoked, on the dispatcher task and in
    // subscription order relative to other matching subscribers, for
    // every subsequently dispatched event whose topic starts with
    // `prefix`. An empty prefix matches every topic. Safe to call from
    // within a handler (including the handler being subscribed, for
    // events dispatched after that call returns).
    SubscriptionId subscribe(std::string prefix, EventHandler handler);

    // Removes a subscription; a no-op if `id` is unknown (e.g. already
    // removed).
    //
    // When called from any thread other than the dispatcher task's own,
    // this blocks until whichever dispatch is currently in flight (i.e.
    // the handlers for whatever event, if any, is currently being
    // delivered) has finished running, before removing the subscription.
    // This means that once unsubscribe() returns, `handler` is guaranteed
    // never to run again, so it's safe to immediately free/destroy
    // anything it captured (e.g. "unsubscribe in a destructor, then
    // finish destroying the rest of the object").
    //
    // Note that the wait is for *whichever* dispatch is currently in
    // flight, not specifically for a dispatch that includes `handler` --
    // unsubscribe() has no cheap way to know in advance which handlers a
    // yet-to-be-copied matched list will contain. Callers must not call
    // unsubscribe() (from a non-dispatcher thread) while holding a lock
    // that *some other* currently-running handler might itself try to
    // acquire, and must not call it from a thread that a currently-
    // running handler is, in turn, waiting on -- either would deadlock
    // against that unrelated handler, not just against the one being
    // removed.
    //
    // When called from within a handler on the dispatcher task's own
    // thread -- including from the very handler being unsubscribed --
    // this does NOT wait (waiting for "the current dispatch" would mean
    // waiting for itself, which would deadlock). In that case, if the
    // subscription being removed was already included in the current
    // event's already-copied handler list (e.g. it comes later, in
    // subscription order, than the handler that's calling unsubscribe()),
    // it may still be invoked once more for this same event after
    // unsubscribe() returns; it is guaranteed not to run for any event
    // dispatched after that.
    void unsubscribe(SubscriptionId id);

    // Enqueues `e` for dispatch and returns immediately without blocking.
    // If e.ts_ms is 0, it is filled in with os::now_ms() first. If the
    // queue is full, the oldest queued event is dropped to make room for
    // this one; dropped() is incremented and (throttled; see dropped())
    // a warning is logged. Safe to call from any thread -- including
    // from within a handler -- and safe to call before start() (events
    // simply accumulate in the queue until the dispatcher starts
    // consuming them).
    void publish(Event e);

    // Polls (does not busy-spin indefinitely) for up to timeout_ms for
    // the queue to drain and every handler invocation in flight to
    // finish. Returns true if that idle state was reached, false if
    // timeout_ms elapsed first. Never returns true while a handler is
    // still running, even under concurrent publish() calls from multiple
    // threads.
    bool wait_idle(uint32_t timeout_ms);

    // Thread-safe. Total number of events dropped (due to queue overflow)
    // by publish() so far. Each drop is counted here regardless of
    // whether it was also logged -- the WLOG_W warning itself is
    // throttled (logged for the first drop, then again only every 64th)
    // to avoid flooding the log under sustained overflow.
    uint64_t dropped() const;

private:
    struct Subscription {
        SubscriptionId id;
        std::string prefix;
        // Shared (not stored by value) so dispatch() can copy the small
        // handle cheaply out of subs_ while holding subs_mutex_, instead
        // of copying the full std::function (which can itself be an
        // arbitrarily expensive/allocating copy, and would needlessly
        // duplicate -- rather than share -- any state a mutable lambda
        // captured).
        std::shared_ptr<EventHandler> handler;
    };

    void run_dispatcher(uint64_t my_generation);
    void dispatch(const Event& e);

    os::Queue<Event> queue_;
    os::Mutex publish_mutex_;  // serializes publish() callers; see file header note

    // Signals the dispatcher task that it has work to do: given exactly
    // once per publish() call (whether or not that call also dropped an
    // older event -- the new event is always a genuinely new deliverable
    // item), and once by stop()/start() to wake a dispatcher that might
    // be idly blocked waiting for work so it can notice it should exit.
    // This -- rather than a polling pop() timeout -- is what lets the
    // dispatcher block indefinitely (no periodic wake-ups to burn power)
    // while still being promptly interruptible.
    os::Semaphore ready_;

    os::Mutex subs_mutex_;  // guards subs_ and next_id_
    std::vector<Subscription> subs_;
    SubscriptionId next_id_ = 1;

    // Held for the full duration of a dispatch() call (copying the
    // matched handler list *and* invoking all of them), so that
    // unsubscribe() from a non-dispatcher thread can block on it to wait
    // out whichever dispatch is currently in flight. See unsubscribe()'s
    // doc comment.
    os::Mutex dispatch_mutex_;

    // Guards dispatcher_/running_/generation_ transitions in start()/
    // stop(). The (possibly blocking) os::Task::join() call itself is
    // always done *outside* this lock -- never while holding it -- so
    // that a handler running on the very dispatcher task being joined
    // can still call start()/stop()/unsubscribe() (which each take this
    // lock only briefly) without the joining thread and the joined
    // thread deadlocking on each other.
    os::Mutex lifecycle_mutex_;
    std::unique_ptr<os::Task> dispatcher_;
    std::atomic<bool> running_{false};
    // Bumped by every start() that actually spawns a new dispatcher task.
    // Each dispatcher task's run_dispatcher() loop captures the
    // generation value in effect when it was spawned and re-checks it
    // (alongside running_) after every wake, so a dispatcher that start()
    // is in the process of replacing is guaranteed to notice and exit on
    // its own -- even if, for a brief window, two Task handles for "the
    // dispatcher" exist (one being joined, one freshly spawned) -- rather
    // than relying solely on timing to prevent two dispatchers from ever
    // delivering events at the same time.
    std::atomic<uint64_t> generation_{0};

    std::atomic<uint64_t> dropped_{0};
    // Number of events that have been successfully enqueued but not yet
    // fully dispatched (i.e. still sitting in the queue, or currently
    // being handed to subscriber handlers). Incremented in publish()
    // *before* the event becomes visible to the dispatcher (i.e. before
    // push_drop_oldest() returns) rather than after, and decremented only
    // once every matching handler for a popped event has returned.
    // Reaches 0 exactly when the bus is idle; see wait_idle(). Getting
    // the increment's placement right matters under concurrent
    // publishers: incrementing *after* push_drop_oldest() returns would
    // let the dispatcher pop and start dispatching that same event before
    // its own increment ran, making wait_idle() briefly observe
    // inflight_ == 0 while a handler is actually still executing.
    std::atomic<int64_t> inflight_{0};
};

}  // namespace wisp::core
