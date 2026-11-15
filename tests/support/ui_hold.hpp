#pragma once

// Holds the UI task inside a posted job until release() (or destruction):
// posts queue up behind it and no slot runs meanwhile. With fill(), every
// further post fails. The job shares the semaphores: after release() the
// UI task may still be inside take() when this object goes.

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>

#include "wisp/os/os.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::test {

class UiHold {
public:
    explicit UiHold(ui::UiRuntime* ui) : ui_(ui) {
        held_ = ui_->post([gates = gates_] {
            gates->entered.give();
            gates->gate.take(os::kForever);
        });
        EXPECT_TRUE(held_);
        if (held_) EXPECT_TRUE(gates_->entered.take(3000));
    }
    ~UiHold() { release(); }
    UiHold(const UiHold&) = delete;
    UiHold& operator=(const UiHold&) = delete;

    // Fills the post queue; how many posts it took.
    size_t fill() {
        size_t n = 0;
        while (ui_->post([] {})) ++n;
        return n;
    }
    void release() {
        if (held_) {
            held_ = false;
            gates_->gate.give();
        }
    }

private:
    struct Gates {
        os::Semaphore entered{1, 0};
        os::Semaphore gate{1, 0};
    };
    ui::UiRuntime* ui_;
    std::shared_ptr<Gates> gates_ = std::make_shared<Gates>();
    bool held_ = false;
};

}  // namespace wisp::test
