#include "wisp/core/state_cell.hpp"

#include <cstdio>
#include <cstdlib>

#include "wisp/core/log.hpp"

namespace wisp::core::cell_detail {

namespace {

#if !defined(ESP_PLATFORM)
// The innermost cell whose lock this thread holds. A thread holds several
// only when a read() body or a mirror reads another cell.
thread_local const CellCore* tls_held = nullptr;
#endif

// How often a cell destroyed while a release() is still in flight looks
// again, and for how long. This wait happens only after the destruction
// was reported as misuse, so only where dispatch_misuse() returns: on the
// device, or on the host while a test has installed a misuse handler.
// The bound is not a promise about how long a run takes (a callback may
// legally run longer); it exists to turn the one wait that can never end
// -- the cell destroyed from the very run that release() waits for --
// into a logged abort instead of a hang. A long callback racing a misused
// teardown can trip it as well.
constexpr uint32_t kReleasePollMs = 1;
constexpr uint32_t kReleaseWaitLimitMs = 3000;

void free_sub(CellSub& sub) { sub.finish(sub); }

}  // namespace

CellCore::Hold::Hold(const CellCore& core) : core_(&core) {
#if !defined(ESP_PLATFORM)
    if (tls_held == &core) {
        dispatch_misuse("StateCell re-entered from inside its own read(), update() or mirror");
    }
    outer_ = tls_held;
#endif
    core.mu_.lock();
#if !defined(ESP_PLATFORM)
    tls_held = &core;
#endif
}

void CellCore::Hold::unlock() {
    if (core_ == nullptr) {
        return;
    }
#if !defined(ESP_PLATFORM)
    tls_held = outer_;
#endif
    core_->mu_.unlock();
    core_ = nullptr;
}

void CellCore::link(CellSub& sub, Dispatcher& on) const {
    sub.core = const_cast<CellCore*>(this);
    sub.on = &on;
    Hold hold(*this);
    // Appended, so subscriptions are scheduled in the order they were made.
    sub.prev_sub = tail_;
    sub.next_sub = nullptr;
    if (tail_ != nullptr) {
        tail_->next_sub = &sub;
    } else {
        head_ = &sub;
    }
    tail_ = &sub;
    on.attach();
    bus_.note_cell_subscription(+1);
    // `delivered` is 0 and the revision at least 1: the first run delivers.
    on.schedule(sub);
}

void CellCore::changed_locked() {
    // Only writers move it, all under this lock, so load-then-store is safe.
    revision_.store(next_revision(revision_.load(std::memory_order_relaxed)),
                    std::memory_order_release);
    for (CellSub* s = head_; s != nullptr; s = s->next_sub) {
        s->on->schedule(*s);
    }
}

void CellCore::unlink_locked(CellSub& sub) const {
    if (sub.prev_sub != nullptr) {
        sub.prev_sub->next_sub = sub.next_sub;
    } else {
        head_ = sub.next_sub;
    }
    if (sub.next_sub != nullptr) {
        sub.next_sub->prev_sub = sub.prev_sub;
    } else {
        tail_ = sub.prev_sub;
    }
    sub.prev_sub = nullptr;
    sub.next_sub = nullptr;
}

void CellCore::release(CellSub* sub) {
    uint8_t prior = sub->claim.fetch_or(CellSub::kHandleGone, std::memory_order_acq_rel);
    if ((prior & CellSub::kCellOwned) != 0) {
        // The cell was destroyed with this subscription live and took it
        // over; it frees it when done unless it already is.
        if ((prior & CellSub::kCellDone) != 0) {
            free_sub(*sub);
        }
        return;
    }
    // This handle owns the teardown. The cell cannot be destroyed under
    // us: its destructor waits while the subscription is linked or counted
    // in releasing_.
    CellCore& core = *sub->core;
    Dispatcher* on = sub->on;
    EventBus* bus = &core.bus_;
    {
        Hold hold(core);
        core.unlink_locked(*sub);
        core.releasing_.fetch_add(1, std::memory_order_relaxed);
    }
    // Outside the cell's lock: a run waiting for that lock is what cancel()
    // may have to wait for. Unlinked first, so no set() can schedule the
    // slot again once cancel() has unqueued it.
    bool free_now = on->cancel(*sub);
    on->detach();
    bus->note_cell_subscription(-1);
    // Last touch of the cell, after the dispatcher and the bus: once a run
    // has returned (or, inside our own run, is past reading the cell)
    // nothing here needs it, and a destructor waiting for this count may
    // complete, after which the owner may free the bus or the dispatcher
    // too. From here on only the subscription itself is touched.
    core.releasing_.fetch_sub(1, std::memory_order_release);
    if (free_now) {
        free_sub(*sub);
    }
    // Otherwise we are inside this subscription's own run: the dispatcher
    // calls finish (free) once the run returns.
}

void CellCore::release_all() {
    uint32_t live = 0;
    {
        Hold hold(*this);
        for (CellSub* s = head_; s != nullptr; s = s->next_sub) {
            ++live;
        }
        live += releasing_.load(std::memory_order_acquire);
    }
    if (live == 0) {
        return;
    }
    char what[80];
    std::snprintf(what, sizeof(what), "StateCell destroyed with %u live subscription%s",
                  static_cast<unsigned>(live), live == 1 ? "" : "s");
    dispatch_misuse(what);

    // The report returned (the device, or a test's misuse handler).
    // Detach every subscription so that no run, set() or later reset()
    // touches this cell again.
    const uint64_t start = os::now_ms();
    while (true) {
        CellSub* claimed = nullptr;
        bool in_flight = false;
        {
            Hold hold(*this);
            for (CellSub* s = head_; s != nullptr; s = s->next_sub) {
                uint8_t expected = 0;
                if (s->claim.compare_exchange_strong(expected, CellSub::kCellOwned,
                                                     std::memory_order_acq_rel)) {
                    claimed = s;
                    break;
                }
            }
            if (claimed != nullptr) {
                unlink_locked(*claimed);
            } else {
                // Linked ones left here are being released by their handle,
                // which still has to take this lock to unlink them.
                in_flight = head_ != nullptr || releasing_.load(std::memory_order_acquire) != 0;
            }
        }
        if (claimed != nullptr) {
            orphan(*claimed);
        } else if (in_flight) {
            if (os::now_ms() - start >= kReleaseWaitLimitMs) {
                // As orphan() below: crash with a reason rather than hang.
                WLOG_E("core",
                       "StateCell destroyed while a reset() waits for a run that "
                       "cannot end (destroyed from that run?)");
                std::abort();
            }
            os::sleep_ms(kReleasePollMs);
        } else {
            return;
        }
    }
}

void CellCore::orphan(CellSub& sub) {
    Dispatcher* on = sub.on;
    if (!on->cancel(sub)) {
        // Destroyed from this subscription's own callback: the run returns
        // into a cell that no longer exists. There is no safe way on.
        std::abort();
    }
    on->detach();
    bus_.note_cell_subscription(-1);
    sub.core = nullptr;
    sub.on = nullptr;
    uint8_t prior = sub.claim.fetch_or(CellSub::kCellDone, std::memory_order_acq_rel);
    if ((prior & CellSub::kHandleGone) != 0) {
        free_sub(sub);
    }
}

}  // namespace wisp::core::cell_detail
