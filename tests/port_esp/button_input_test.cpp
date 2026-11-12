#include "wisp/port/esp/button_input.hpp"

#include <gtest/gtest.h>

#include <vector>

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

// Models a callback already executing on the driver-selected core. Disabling
// a pin clears hardware pending state, but cannot finish that old callback.
struct DetachOps {
    uint32_t irq_core = 0;
    uint32_t caller_core = 0;
    int failed_core = -1;
    bool disable_ok = true;
    bool remove_ok = true;
    bool enabled = true;
    bool pending = true;
    bool in_flight = true;
    bool removed = false;
    bool invalid_order = false;
    std::vector<uint32_t> synchronized;

    bool disable_input() {
        if (!disable_ok) return false;
        enabled = false;
        pending = false;
        return true;
    }
    bool synchronize_core(uint32_t core) {
        if (enabled || pending) invalid_order = true;
        synchronized.push_back(core);
        if (static_cast<int>(core) == failed_core) return false;
        if (core == irq_core) in_flight = false;
        return true;
    }
    bool remove_handler() {
        if (enabled || pending || in_flight) {
            invalid_order = true;
            return false;
        }
        removed = remove_ok;
        return remove_ok;
    }
};
}  // namespace

TEST(ButtonDetach, MigratedInstallerDoesNotDetermineIsrCore) {
    DetachOps ops;
    ops.irq_core = 0;
    ops.caller_core = 1;
    ASSERT_TRUE(quiesce_button_input(ops, 2));
    EXPECT_FALSE(ops.in_flight);
    EXPECT_TRUE(ops.removed);
    EXPECT_FALSE(ops.invalid_order);
    EXPECT_EQ(ops.synchronized, (std::vector<uint32_t>{0, 1}));
}
TEST(ButtonDetach, PreselectedOtherCoreIsDrainedBeforeRemoval) {
    DetachOps ops;
    ops.irq_core = 1;
    ops.caller_core = 0;
    ASSERT_TRUE(quiesce_button_input(ops, 2));
    EXPECT_FALSE(ops.in_flight);
    EXPECT_TRUE(ops.removed);
    EXPECT_FALSE(ops.invalid_order);
}
TEST(ButtonDetach, SingleCoreAlsoDisablesAndDrainsBeforeRemoval) {
    DetachOps ops;
    ASSERT_TRUE(quiesce_button_input(ops, 1));
    EXPECT_EQ(ops.synchronized, (std::vector<uint32_t>{0}));
    EXPECT_FALSE(ops.invalid_order);
    EXPECT_TRUE(ops.removed);
}
TEST(ButtonDetach, FailedDisableDoesNotRemoveLiveCallback) {
    DetachOps ops;
    ops.disable_ok = false;
    EXPECT_FALSE(quiesce_button_input(ops, 2));
    EXPECT_TRUE(ops.synchronized.empty());
    EXPECT_FALSE(ops.removed);
    EXPECT_TRUE(ops.in_flight);
}
TEST(ButtonDetach, FailedBarrierDoesNotRemoveCallback) {
    DetachOps ops;
    ops.irq_core = 1;
    ops.failed_core = 1;
    EXPECT_FALSE(quiesce_button_input(ops, 2));
    EXPECT_FALSE(ops.removed);
    EXPECT_TRUE(ops.in_flight);
}
TEST(ButtonDetach, RemoveFailureIsNotReportedAsQuiescentDetach) {
    DetachOps ops;
    ops.remove_ok = false;
    EXPECT_FALSE(quiesce_button_input(ops, 2));
    EXPECT_FALSE(ops.removed);
    EXPECT_FALSE(ops.in_flight);
}
TEST(ButtonDetach, EmptyCoreSetCannotProveQuiescence) {
    DetachOps ops;
    EXPECT_FALSE(quiesce_button_input(ops, 0));
    EXPECT_FALSE(ops.removed);
}

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
