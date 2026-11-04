#include "self_check_model.hpp"

#include <algorithm>
#include <numeric>

namespace wisp::app {

namespace {

size_t index_of(InteractiveTest test) { return static_cast<size_t>(test); }

}  // namespace

DeviceSummary summarize_devices(std::span<const core::DeviceState> states) {
    DeviceSummary summary;
    bool degraded = false;
    bool failed = false;
    bool missing = false;
    for (core::DeviceState state : states) {
        ++summary.total;
        switch (state) {
            case core::DeviceState::Ok:
                ++summary.ok;
                break;
            case core::DeviceState::Degraded:
                degraded = true;
                break;
            case core::DeviceState::Missing:
                missing = true;
                break;
            case core::DeviceState::Failed:
                failed = true;
                break;
        }
    }
    summary.level = failed     ? SummaryLevel::Error
                    : degraded ? SummaryLevel::Warning
                    : missing  ? SummaryLevel::Neutral
                               : SummaryLevel::Good;
    return summary;
}

int device_display_rank(core::DeviceState state) {
    switch (state) {
        case core::DeviceState::Failed:
            return 0;
        case core::DeviceState::Degraded:
            return 1;
        case core::DeviceState::Missing:
            return 2;
        case core::DeviceState::Ok:
            return 3;
    }
    return 0;
}

std::vector<size_t> device_display_order(std::span<const core::DeviceState> states) {
    std::vector<size_t> order(states.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return device_display_rank(states[a]) < device_display_rank(states[b]);
    });
    return order;
}

bool InteractiveTests::pass(InteractiveTest test) {
    bool& passed = passed_[index_of(test)];
    if (passed) {
        return false;
    }
    passed = true;
    return true;
}

bool InteractiveTests::on_loopback(const LoopbackStatus& status) {
    return status.id == ui::StrId::LoopDone && pass(InteractiveTest::Loopback);
}

bool InteractiveTests::passed(InteractiveTest test) const { return passed_[index_of(test)]; }

int InteractiveTests::passed_count() const {
    return static_cast<int>(std::count(std::begin(passed_), std::end(passed_), true));
}

}  // namespace wisp::app
