#include "wisp/app/ram_budget.hpp"

#include <gtest/gtest.h>

#include "wisp/base/value.hpp"

using wisp::Map;
using wisp::Value;
using wisp::app::internal_ram;
using wisp::app::internal_ram_low;
using wisp::app::InternalRam;

TEST(RamBudget, ReadsTheInternalFiguresFromHeapInfo) {
    InternalRam ram = internal_ram(
        Value(Map{{"internal_free", int64_t{70000}}, {"internal_largest", int64_t{31744}}}));
    EXPECT_EQ(ram.free, 70000);
    EXPECT_EQ(ram.largest, 31744);
}

TEST(RamBudget, UnknownWithoutFigures) {
    InternalRam ram = internal_ram(Value(Map{}));
    EXPECT_EQ(ram.free, -1);
    EXPECT_EQ(ram.largest, -1);
    EXPECT_FALSE(internal_ram_low(ram));
    EXPECT_FALSE(internal_ram_low(internal_ram(Value())));
}

// The idle floor is the worst-case floor plus the largest dip measured on
// the board (a TLS handshake, about 20 KB): see AGENTS.md section 5 and
// docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md.
TEST(RamBudget, IdleFloorLeavesRoomForTheWorstCaseDip) {
    EXPECT_GE(wisp::app::kIdleInternalFreeMin, wisp::app::kWorstInternalFreeMin + 20 * 1024);
    EXPECT_GE(wisp::app::kIdleInternalLargestMin, wisp::app::kWorstInternalLargestMin);
    // Room for the admin httpd's 10 KB stack.
    EXPECT_GT(wisp::app::kWorstInternalLargestMin, 10240);
}

TEST(RamBudget, LowWhenEitherFigureIsUnderTheIdleFloor) {
    const int64_t free_ok = wisp::app::kIdleInternalFreeMin;
    const int64_t largest_ok = wisp::app::kIdleInternalLargestMin;
    EXPECT_FALSE(internal_ram_low({free_ok, largest_ok}));
    EXPECT_TRUE(internal_ram_low({free_ok - 1, largest_ok}));
    EXPECT_TRUE(internal_ram_low({free_ok, largest_ok - 1}));
    // Figures at which the admin httpd's 10 KB stack could not be
    // allocated (docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md).
    EXPECT_TRUE(internal_ram_low({13547, 7680}));
    // A platform that reports free but not the largest block.
    EXPECT_FALSE(internal_ram_low({free_ok, -1}));
    EXPECT_TRUE(internal_ram_low({free_ok - 1, -1}));
}
