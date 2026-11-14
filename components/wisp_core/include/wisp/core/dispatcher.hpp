#pragma once

// Dispatch slots and the dispatchers that run them (spec 3.2 of the
// state-cells design). A slot is a unit of work its owner keeps (a cell
// subscription, an inbox); scheduling one links it into the dispatcher's
// ready list, so making work ready never allocates and never fails -- the
// property that lets a state cell lose no change however busy its
// dispatcher is. A slot queued several times before it runs runs once
// (coalescing); its run reads whatever is current at that moment.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "wisp/os/os.hpp"

namespace wisp::core {

// A unit of work owned by whoever scheduled it (a cell subscription, an
// inbox). Intrusive: scheduling never allocates. `flags` and `next` belong
// to the dispatcher holding it and are guarded by its ready-list lock.
struct DispatchSlot {
    static constexpr uint8_t kQueued = 1, kRunning = 2, kCancelled = 4;
    void (*run)(DispatchSlot&) = nullptr;
    // Called by the dispatcher after a run during which cancel() returned
    // false, once it no longer touches the slot; the owner frees it here.
    void (*finish)(DispatchSlot&) = nullptr;
    DispatchSlot* next = nullptr;
    uint8_t flags = 0;
};

class Dispatcher {
public:
    virtual ~Dispatcher() = default;
    // Any thread. Queues `slot` to run once on this dispatcher's thread;
    // a slot already queued stays queued once (coalescing). A slot that is
    // running is queued again and runs once more after the current run.
    // Never blocks on a run, never allocates, never fails.
    virtual void schedule(DispatchSlot& slot) = 0;
    // Unqueues `slot`. Returns true when the owner may free it now: off
    // this dispatcher's thread, after waiting for a run of `slot` in
    // progress (only that one) to return. Returns false only when called
    // on this dispatcher's thread from inside `slot`'s own run; the
    // dispatcher then calls slot.finish after the run. Never waits after
    // the dispatcher has stopped. Must not be called while holding a lock
    // the slot's own run takes.
    [[nodiscard]] virtual bool cancel(DispatchSlot& slot) = 0;
    virtual bool on_dispatch_thread() const = 0;

    // Owners of a slot on this dispatcher attach once when they create it
    // and detach once when they free it; a dispatcher destroyed with
    // attached() != 0 reports misuse.
    void attach();
    void detach();
    uint32_t attached() const;

protected:
    // For implementations' destructors: misuse("<who> destroyed with N
    // attached slots"). Called from the most derived destructor, after the
    // dispatcher has stopped running slots, so a report names the
    // implementation whose owners outlived it.
    void check_detached(const char* who) const;

private:
    std::atomic<uint32_t> attached_{0};
};

// The ready list EventBus, UiRuntime and the tests' ManualDispatcher share:
// a FIFO of slots under one leaf lock, at most one slot running at a time.
// Nothing is called while the lock is held, except finish() which done()
// calls after releasing it.
class SlotQueue {
public:
    // Queues `slot` unless it is already queued or was cancelled inside
    // its own run. True when the list was empty before (a running slot
    // does not count): the caller then wakes its dispatcher, and any later
    // push finds the list non-empty until the dispatcher has drained it.
    bool push(DispatchSlot& slot);
    DispatchSlot* take();           // pops the head, marks it running; nullptr when empty
    void done(DispatchSlot& slot);  // ends take()'s run; calls finish when cancelled inside it
    // Dispatcher::cancel()'s rule; off the thread polls every 1 ms.
    bool cancel(DispatchSlot& slot, bool on_dispatch_thread);
    size_t pending() const;  // queued slots plus the one running

private:
    void unlink_locked(DispatchSlot& slot);

    mutable os::Mutex mu_;
    DispatchSlot* head_ = nullptr;
    DispatchSlot* tail_ = nullptr;
    DispatchSlot* running_ = nullptr;
    size_t queued_ = 0;
};

namespace detail {
// Misuse that would corrupt memory later (a dispatcher destroyed with
// slots attached, a cell re-entered from its own callback).
// Host builds: prints "wisp: misuse: <what>" to stderr and aborts, so the
// test that causes it fails with the call site named.
// ESP_PLATFORM: WLOG_E("core", "misuse: %s", what) and returns.
void misuse(const char* what);
}  // namespace detail

}  // namespace wisp::core
