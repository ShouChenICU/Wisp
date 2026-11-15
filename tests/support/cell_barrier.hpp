#pragma once

// Waits until every subscription of `cell` on dispatcher `on` has had its
// call for the cell's value as it is now. A long-lived probe cannot tell:
// a slot already queued keeps its earlier place in the dispatcher's queue,
// so a probe still queued for an older revision can run after a later set()
// that queued another subscriber behind it, and see the new value first. A
// fresh subscription is pushed now, behind every slot queued for the
// current value or earlier (a slot running when the value changed is queued
// again then), so its first call comes after theirs. Calls already
// delivered need nothing; a UI-thread post such a call made still needs a
// round trip after this (the UI drains posts before it runs slots).

#include <atomic>
#include <cstdint>

#include "support/wait.hpp"
#include "wisp/core/dispatcher.hpp"
#include "wisp/core/state_cell.hpp"

namespace wisp::test {

template <class T>
[[nodiscard]] bool cell_barrier(const core::StateCell<T>& cell, core::Dispatcher* on,
                                uint32_t ms = 2000) {
    std::atomic<bool> called{false};
    core::CellSubscription sub = cell.subscribe([&called](const T&) { called = true; }, on);
    const bool ok = wait_until([&] { return called.load(); }, ms);
    // Released before `called` goes: reset() returns once no call runs.
    sub.reset();
    return ok;
}

// Waits until every slot already queued on `on` (an inbox's run, a cell
// subscription's call) has run: a throwaway cell's fresh subscription goes
// behind them. The same caveat applies to posts those runs made.
//
// An edge pushed into an EdgeInbox that already holds edges (its run
// queued or running) schedules nothing itself: the run's end_run()
// schedules the next one later, so a barrier made in that window can run
// before that edge is delivered. The barrier covers an edge only when its
// push found the inbox idle, or when no push followed the start of the
// run that delivers it.
[[nodiscard]] inline bool slot_barrier(core::EventBus& bus, core::Dispatcher* on,
                                       uint32_t ms = 2000) {
    core::StateCell<int> cell(bus, 0);
    return cell_barrier(cell, on, ms);
}

}  // namespace wisp::test
