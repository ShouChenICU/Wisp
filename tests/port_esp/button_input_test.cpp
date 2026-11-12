#include "wisp/port/esp/button_input.hpp"

#include <gtest/gtest.h>

using namespace wisp::port::esp;

namespace {
struct WakeOps {
    bool fail_lock = false;
    bool fail_enable = false;
    bool fail_disable = false;
    int locks = 0;
    int enables = 0;
    int disables = 0;
    bool lock_sleep() {
        if (fail_lock) return false;
        ++locks;
        return true;
    }
    void unlock_sleep() { --locks; }
    bool enable_wake() {
        ++enables;
        return !fail_enable;
    }
    bool disable_wake() {
        ++disables;
        return !fail_disable;
    }
};
}  // namespace

TEST(ButtonInput, ActiveHighMapsBothEdges) {
    EXPECT_FALSE(button_pressed(false, false));
    EXPECT_TRUE(button_pressed(true, false));
}
TEST(ButtonInput, ActiveLowMapsBothEdges) {
    EXPECT_TRUE(button_pressed(false, true));
    EXPECT_FALSE(button_pressed(true, true));
}
TEST(ButtonInput, PmuFallbackIs100Milliseconds) { EXPECT_EQ(kButtonStandbyPollMs, 100u); }
TEST(ButtonInput, UnvalidatedPowerPinUsesPmu) {
    EXPECT_FALSE(power_button_use_gpio(false, true, true));
}
TEST(ButtonInput, GpioSetupFailureUsesPmu) {
    EXPECT_FALSE(power_button_use_gpio(true, false, true));
    EXPECT_FALSE(power_button_use_gpio(true, true, false));
    EXPECT_TRUE(power_button_use_gpio(true, true, true));
}

TEST(ButtonInput, HeldWakeDisarmsUntilRelease) {
    ButtonWakeState state;
    WakeOps ops;
    ASSERT_TRUE(state.update(false, ops));
    EXPECT_TRUE(state.armed());
    ASSERT_TRUE(state.update(true, ops));
    EXPECT_FALSE(state.armed());
    EXPECT_EQ(ops.locks, 1);
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(state.update(true, ops));
    EXPECT_EQ(ops.enables, 1);
    EXPECT_EQ(ops.locks, 1);
    ASSERT_TRUE(state.update(false, ops));
    EXPECT_TRUE(state.armed());
    EXPECT_EQ(ops.enables, 2);
    EXPECT_EQ(ops.locks, 0);
}
TEST(ButtonInput, InitiallyHeldPreventsSleepWithoutArming) {
    ButtonWakeState state;
    WakeOps ops;
    ASSERT_TRUE(state.update(true, ops));
    EXPECT_FALSE(state.armed());
    EXPECT_EQ(ops.enables, 0);
    EXPECT_EQ(ops.locks, 1);
    ASSERT_TRUE(state.update(false, ops));
    EXPECT_TRUE(state.armed());
    EXPECT_EQ(ops.locks, 0);
}
TEST(ButtonInput, WakeEnableFailureRetainsSleepInhibition) {
    ButtonWakeState state;
    WakeOps ops;
    ASSERT_TRUE(state.update(true, ops));
    ops.fail_enable = true;
    EXPECT_FALSE(state.update(false, ops));
    EXPECT_FALSE(state.armed());
    EXPECT_EQ(ops.locks, 1);
    ops.fail_enable = false;
    ASSERT_TRUE(state.update(false, ops));
    EXPECT_EQ(ops.locks, 0);
}
TEST(ButtonInput, SleepLockFailureDoesNotDisarmWake) {
    ButtonWakeState state;
    WakeOps ops;
    ASSERT_TRUE(state.update(false, ops));
    ops.fail_lock = true;
    EXPECT_FALSE(state.update(true, ops));
    EXPECT_TRUE(state.armed());
    EXPECT_EQ(ops.disables, 0);
}
TEST(ButtonInput, DisableFailureKeepsSleepInhibited) {
    ButtonWakeState state;
    WakeOps ops;
    ASSERT_TRUE(state.update(false, ops));
    ops.fail_disable = true;
    EXPECT_FALSE(state.update(true, ops));
    EXPECT_EQ(ops.locks, 1);
    EXPECT_TRUE(state.armed());
    ops.fail_disable = false;
    EXPECT_TRUE(state.update(true, ops));
    EXPECT_FALSE(state.armed());
}
TEST(ButtonInput, DetachDisarmsAndReleasesHeldLock) {
    ButtonWakeState state;
    WakeOps ops;
    ASSERT_TRUE(state.update(false, ops));
    ASSERT_TRUE(state.reset(ops));
    EXPECT_FALSE(state.armed());
    ASSERT_TRUE(state.update(true, ops));
    ASSERT_TRUE(state.reset(ops));
    EXPECT_EQ(ops.locks, 0);
}
