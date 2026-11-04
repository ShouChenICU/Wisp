#pragma once

// The self-check screen's bookkeeping that needs no LVGL: the device
// summary behind the "设备 n/m 正常" chip, the order the device list shows
// its rows in, and which interactive tests passed during this boot. Kept
// apart from the widgets so tests/app/self_check_model_test.cpp can check
// it directly.

#include <cstddef>
#include <span>
#include <vector>

#include "loopback.hpp"
#include "wisp/core/boot_report.hpp"

namespace wisp::app {

// How a summary chip is coloured.
enum class SummaryLevel {
    Good,     // everything it counts is fine
    Warning,  // something works with limits (Degraded), nothing failed
    Error,    // something failed
    Neutral,  // nothing is wrong, but not everything is there (yet)
};

struct DeviceSummary {
    int ok = 0;
    int total = 0;
    SummaryLevel level = SummaryLevel::Good;
};

// Counts the Ok states. The level is Error if any state is Failed, else
// Warning if any is Degraded, else Neutral if any is Missing, else Good.
DeviceSummary summarize_devices(std::span<const core::DeviceState> states);

// Indices into `states` in display order: Failed, Degraded, Missing, then
// Ok, keeping the given order within each state.
std::vector<size_t> device_display_order(std::span<const core::DeviceState> states);

// Where a row in `state` goes relative to the others (lower comes first),
// as used by device_display_order().
int device_display_rank(core::DeviceState state);

// The three interactive checks. Their results last for this boot only.
enum class InteractiveTest { Touch, Button, Loopback };
inline constexpr int kInteractiveTestCount = 3;

class InteractiveTests {
public:
    // Marks `test` passed. Returns true if it had not passed before.
    bool pass(InteractiveTest test);
    // Passes the loopback test once a run reports LoopDone (recorded and
    // played back). Returns true if that changed anything.
    bool on_loopback(const LoopbackStatus& status);

    bool passed(InteractiveTest test) const;
    int passed_count() const;

private:
    bool passed_[kInteractiveTestCount] = {};
};

}  // namespace wisp::app
