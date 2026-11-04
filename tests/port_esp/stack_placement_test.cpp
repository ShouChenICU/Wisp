// Unit tests for the ESP port's task-stack placement policy (see
// wisp/port/esp/stack_placement.hpp). Compiles on the host without any
// ESP-IDF include; the PSRAM availability check is not called here.

#include "wisp/port/esp/stack_placement.hpp"

#include <gtest/gtest.h>

#include "wisp/os/os.hpp"

using wisp::port::esp::stack_may_use_psram;

// A task that may touch internal flash always keeps an internal stack:
// on the targets this port builds for, a flash operation can stop the
// cache or map flash, and both assert on a PSRAM stack.
TEST(StackPlacement, FlashAccessKeepsTheStackInternal) {
    EXPECT_FALSE(stack_may_use_psram(/*flash_access=*/true));
}

TEST(StackPlacement, NoFlashAccessMayUsePsram) {
    EXPECT_TRUE(stack_may_use_psram(/*flash_access=*/false));
}

// The declaration's default is the safe one.
TEST(StackPlacement, DefaultDeclarationIsFlashAccess) {
    EXPECT_FALSE(stack_may_use_psram(wisp::os::TaskConfig{}.flash_access));
}
