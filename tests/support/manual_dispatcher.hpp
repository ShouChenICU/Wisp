#pragma once

// A core::Dispatcher a test steps by hand: scheduled slots run only inside
// run_all(), on the thread that calls it, so a test decides exactly when
// callbacks happen instead of waiting for a dispatcher task.

#include <cstddef>

#include "wisp/core/dispatcher.hpp"

namespace wisp::test {

class ManualDispatcher : public core::Dispatcher {
public:
    ManualDispatcher() = default;
    ~ManualDispatcher() override { check_detached("ManualDispatcher"); }

    ManualDispatcher(const ManualDispatcher&) = delete;
    ManualDispatcher& operator=(const ManualDispatcher&) = delete;

    void schedule(core::DispatchSlot& slot) override { slots_.push(slot); }
    bool cancel(core::DispatchSlot& slot) override {
        return slots_.cancel(slot, on_dispatch_thread());
    }
    bool on_dispatch_thread() const override { return running_ == this; }

    // Runs queued slots on the calling thread until none is queued,
    // including slots queued by the runs themselves; returns how many ran.
    size_t run_all() {
        const ManualDispatcher* outer = running_;
        running_ = this;
        size_t ran = 0;
        while (core::DispatchSlot* slot = slots_.take()) {
            slot->run(*slot);
            // done() may hand the slot to its owner's finish(), which frees
            // it: nothing touches `slot` after this line.
            slots_.done(*slot);
            ++ran;
        }
        running_ = outer;
        return ran;
    }

    // Slots waiting to run (plus the one running, when asked from inside
    // a run).
    size_t queued() const { return slots_.pending(); }

private:
    // Per thread, so on_dispatch_thread() is true only on the thread
    // inside run_all(), as it is only on wisp.bus for the bus.
    static inline thread_local const ManualDispatcher* running_ = nullptr;
    core::SlotQueue slots_;
};

}  // namespace wisp::test
