// Unit tests for the ESP port's task-stack placement policy (see
// wisp/port/esp/stack_placement.hpp). Compiles on the host without any
// ESP-IDF include; the PSRAM availability check is not called here.

#include "wisp/port/esp/stack_placement.hpp"

#include <gtest/gtest.h>

#include "wisp/os/os.hpp"

using wisp::port::esp::stack_may_use_psram;

// A task that may touch internal flash always keeps an internal stack:
// on the targets this port builds for, a flash operation can stop the
// cache or map flash, and both assert on a PSRAM stack. A task declaring
// hot_stack keeps its stack internal too, for speed rather than flash
// safety; either reason alone is enough to refuse PSRAM.
TEST(StackPlacement, HotOrFlashStacksStayInternal) {
    EXPECT_TRUE(stack_may_use_psram(/*flash_access=*/false, /*hot_stack=*/false));
    EXPECT_FALSE(stack_may_use_psram(/*flash_access=*/true, /*hot_stack=*/false));
    EXPECT_FALSE(stack_may_use_psram(/*flash_access=*/true, /*hot_stack=*/true));
    EXPECT_FALSE(stack_may_use_psram(/*flash_access=*/false, /*hot_stack=*/true));
}

// The default declaration is the common case: a task that never touches
// flash, so its stack may go to PSRAM. wisp.fs is the one task that
// opts out, explicitly (AGENTS.md section 5).
TEST(StackPlacement, DefaultDeclarationMayUsePsram) {
    EXPECT_TRUE(
        stack_may_use_psram(wisp::os::TaskConfig{}.flash_access, wisp::os::TaskConfig{}.hot_stack));
}
