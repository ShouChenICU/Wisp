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

}  // namespace wisp::test
