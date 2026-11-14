#pragma once

// An edge inbox: every edge, in order, on one consumer's dispatcher (spec
// 3.5 of the state-cells design). For input and lifecycle edges whose
// every occurrence matters (a key going down, a scene being deleted),
// which a state cell would coalesce and the bus may drop.
//
// Delivery. push() copies the edge into a fixed ring and schedules the
// inbox's slot; the slot's run calls the handler once per edge, in push
// order, on the dispatcher the inbox was given -- never on the pusher's
// thread unless that thread is the dispatcher, and never inside push().
// A run delivers only the edges that were waiting when it started; edges
// pushed meanwhile (the handler's own included) go to the next run, which
// the inbox queues behind whatever else is ready on the dispatcher, so a
// handler that always pushes another edge cannot hold its dispatcher.
// Edges pushed by one thread arrive in that thread's order.
//
// Overflow. The ring holds N edges. A push into a full ring drops the
// oldest waiting edge, counts it in overflows() and logs a warning for the
// first drop and every 64th after it. Overflow needs the consumer's
// dispatcher to stall for N edges; it is never silent.
//
// Threads and locks. push() may be called from any thread (not from an
// ISR: it takes an os::Mutex), including hot paths and code holding its
// own locks: it never allocates and blocks only
// on the inbox's own lock and inside the dispatcher's schedule(). The
// order is inbox -> the dispatcher's schedule() (its ready list and its
// wake: the bus's ready semaphore, the UI runtime's no-op post); both are
// leaf and non-blocking, and a dispatcher runs the slot with its
// ready-list lock released, so nothing takes them the other way. The
// warning is logged after the inbox's lock is released. The handler runs
// with no inbox lock held, so it may push into any inbox, its own
// included.
//
// Lifetime. The inbox has one owner, who destroys it once. The destructor
// cancels the slot: off the dispatcher's thread it waits for a run in
// progress to return, so once it returns the handler never runs again;
// edges still waiting are discarded. Being the only canceller of its slot
// by construction, it meets the dispatcher's one-cancel-at-a-time rule.
// The owner must stop every producer before destroying the inbox: a push()
// racing the destructor touches freed memory. Destroying the inbox from
// inside its own handler aborts on every build (the run would return into
// it). The dispatcher must outlive the inbox.
//
// Handlers run on wisp.bus or the UI task, neither of which may touch
// flash (AGENTS.md section 5): hand flash work to StorageWorker.
//
// Memory. The ring of N edges and the handler's callable live in
// Mem::Large, allocated once in the constructor. The inbox object itself
// lives inside its owner and is a few tens of bytes whatever E and N are;
// its lock also allocates, in internal RAM: os::Mutex's small Impl and the
// RTOS mutex that holds.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#include "wisp/core/dispatcher.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

// Not `detail`: a wisp::core::detail would hide wisp::detail for every
// file that includes this header.
namespace edge_detail {

// The part of an inbox that does not depend on E: edges are copied in and
// out as bytes, which is exact for the trivially copyable E it is used
// with.
class InboxCore {
public:
    InboxCore(const InboxCore&) = delete;
    InboxCore& operator=(const InboxCore&) = delete;

    uint32_t overflows() const { return overflows_.load(std::memory_order_relaxed); }
    const char* name() const { return name_; }

protected:
    // Allocates the ring (aborts when Mem::Large is exhausted) and attaches
    // to `on`. `run` is the typed run of the slot.
    InboxCore(const char* name, Dispatcher& on, size_t capacity, size_t edge_size,
              void (*run)(DispatchSlot&));
    // Frees the ring; close() must have run.
    ~InboxCore();

    // From the derived destructor, before the handler is destroyed: a run
    // in progress off this thread still calls it. Aborts when called from
    // the inbox's own handler; otherwise cancels the slot (waiting for a
    // run in progress) and detaches.
    void close();

    void push_bytes(const void* edge);

    // For the typed run, on the dispatcher's thread. begin_run() returns
    // how many edges this run delivers (an overflow during the run drops
    // the oldest and adds one, so the count never falls below that before
    // the run pops); pop() copies out the oldest, false when the ring is
    // empty; end_run() queues the next run if edges remain.
    size_t begin_run();
    bool pop(void* out);
    void end_run();

    static InboxCore& of(DispatchSlot& slot) { return *static_cast<Slot&>(slot).owner; }

private:
    struct Slot : DispatchSlot {
        InboxCore* owner = nullptr;
    };

    const char* const name_;
    Dispatcher& on_;
    Slot slot_;
    unsigned char* ring_ = nullptr;
    const size_t edge_size_;
    const size_t capacity_;
    size_t head_ = 0;   // index of the oldest waiting edge; guarded by mu_
    size_t count_ = 0;  // waiting edges; guarded by mu_
    std::atomic<uint32_t> overflows_{0};
    os::Mutex mu_;
    // Set for the duration of a run. Written and read only on the
    // dispatcher's thread: the destructor reads it only there.
    bool in_run_ = false;
};

}  // namespace edge_detail

template <class E, size_t N>
class EdgeInbox : private edge_detail::InboxCore {
    static_assert(std::is_trivially_copyable_v<E>, "edges are copied as bytes");
    static_assert(alignof(E) <= alignof(std::max_align_t),
                  "the ring has os::alloc's (malloc's) alignment");
    static_assert(N > 0);

public:
    using Handler = os::LargeFunction<void(const E&)>;

    // `name` (a string literal) labels the overflow warning and mem's
    // counters. Allocates the ring of N edges in Mem::Large (aborts when
    // PSRAM is exhausted) and attaches to `on`, which must outlive the
    // inbox.
    EdgeInbox(const char* name, Dispatcher& on, Handler handler)
        : InboxCore(name, on, N, sizeof(E), &EdgeInbox::run), handler_(std::move(handler)) {}
    // See "Lifetime" above.
    ~EdgeInbox() { close(); }

    EdgeInbox(const EdgeInbox&) = delete;
    EdgeInbox& operator=(const EdgeInbox&) = delete;

    // Any thread, but not from an ISR: it takes the inbox's os::Mutex. See
    // the file comment.
    void push(const E& e) { push_bytes(&e); }

    using InboxCore::name;
    using InboxCore::overflows;

private:
    static void run(DispatchSlot& slot) {
        auto& self = static_cast<EdgeInbox&>(of(slot));
        const size_t n = self.begin_run();
        for (size_t i = 0; i < n; ++i) {
            // Raw storage: E need not be default-constructible. Copying the
            // bytes of a trivially copyable E into it creates the E.
            alignas(E) unsigned char storage[sizeof(E)];
            if (!self.pop(storage)) {
                break;
            }
            // Destroying the inbox from here aborts (see close()), so
            // `self` is still alive when the handler returns.
            self.handler_(*std::launder(reinterpret_cast<const E*>(storage)));
        }
        self.end_run();
    }

    Handler handler_;
};

}  // namespace wisp::core
