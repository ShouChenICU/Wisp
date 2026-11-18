#include "held_pins.hpp"

#include <gtest/gtest.h>

using wisp::board_kit::HeldPins;
using wisp::board_kit::kMaxHeldPins;

TEST(HeldPins, KeepsOrderAndSkipsNegativesAndDuplicates) {
    HeldPins held;
    EXPECT_TRUE(held.add(39));
    EXPECT_TRUE(held.add(40));
    EXPECT_FALSE(held.add(-1));
    EXPECT_TRUE(held.add(46));
    EXPECT_TRUE(held.add(12));
    EXPECT_FALSE(held.add(39));
    ASSERT_EQ(held.count, 4u);
    EXPECT_EQ(held.pins[0], 39);
    EXPECT_EQ(held.pins[1], 40);
    EXPECT_EQ(held.pins[2], 46);
    EXPECT_EQ(held.pins[3], 12);
}

TEST(HeldPins, TheOneSeventyFiveCList) {
    HeldPins held;
    for (int pin : {1, 2, 46, 12}) {
        EXPECT_TRUE(held.add(pin));
    }
    ASSERT_EQ(held.count, 4u);
    EXPECT_EQ(held.pins[0], 1);
    EXPECT_EQ(held.pins[1], 2);
    EXPECT_EQ(held.pins[2], 46);
    EXPECT_EQ(held.pins[3], 12);
}

TEST(HeldPins, IsBounded) {
    HeldPins held;
    for (int pin = 0; pin < static_cast<int>(kMaxHeldPins); ++pin) {
        EXPECT_TRUE(held.add(pin));
    }
    EXPECT_FALSE(held.add(static_cast<int>(kMaxHeldPins)));
    ASSERT_EQ(held.count, kMaxHeldPins);
    EXPECT_EQ(held.pins[kMaxHeldPins - 1], static_cast<int>(kMaxHeldPins) - 1);
}
