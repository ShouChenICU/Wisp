#pragma once

// Test-only polling helper shared across the test suite (not built into
// any production component -- see AGENTS.md's layering rule, which does
// not apply to tests/).

#include <cstdint>
#include <functional>

#include "wisp/os/os.hpp"

namespace wisp::test {

// Polls `pred` (sleeping 1ms between attempts) until it returns true or
// `timeout_ms` elapses. Returns the final value of `pred()` (so true means
// it succeeded before the timeout, false means it never did).
inline bool wait_until(const std::function<bool()>& pred, uint32_t timeout_ms) {
    uint64_t deadline = wisp::os::now_ms() + timeout_ms;
    while (true) {
        if (pred()) {
            return true;
        }
        if (wisp::os::now_ms() >= deadline) {
            return pred();
        }
        wisp::os::sleep_ms(1);
    }
}

}  // namespace wisp::test
