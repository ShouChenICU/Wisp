// The self-check screen's LVGL-free bookkeeping: the device summary chip's
// count and colour level, the device list's order and the interactive
// tests' pass state.

#include "self_check_model.hpp"

#include <gtest/gtest.h>

#include <vector>

using wisp::app::device_display_order;
using wisp::app::InteractiveTest;
using wisp::app::InteractiveTests;
using wisp::app::LoopbackStatus;
using wisp::app::summarize_devices;
using wisp::app::SummaryLevel;
using wisp::core::DeviceState;
using wisp::ui::StrId;

TEST(SelfCheckModel, AllOkIsGood) {
    std::vector<DeviceState> states{DeviceState::Ok, DeviceState::Ok, DeviceState::Ok};
    auto s = summarize_devices(states);
    EXPECT_EQ(s.ok, 3);
    EXPECT_EQ(s.total, 3);
    EXPECT_EQ(s.level, SummaryLevel::Good);
}

TEST(SelfCheckModel, NoDevicesIsGood) {
    auto s = summarize_devices({});
    EXPECT_EQ(s.ok, 0);
    EXPECT_EQ(s.total, 0);
    EXPECT_EQ(s.level, SummaryLevel::Good);
}

TEST(SelfCheckModel, DegradedWithoutFailureIsWarning) {
    std::vector<DeviceState> states{DeviceState::Ok, DeviceState::Degraded, DeviceState::Missing,
                                    DeviceState::Ok};
    auto s = summarize_devices(states);
    EXPECT_EQ(s.ok, 2);
    EXPECT_EQ(s.total, 4);
    EXPECT_EQ(s.level, SummaryLevel::Warning);
}

TEST(SelfCheckModel, AnyFailureIsError) {
    std::vector<DeviceState> states{DeviceState::Degraded, DeviceState::Ok, DeviceState::Failed,
                                    DeviceState::Missing};
    auto s = summarize_devices(states);
    EXPECT_EQ(s.ok, 1);
    EXPECT_EQ(s.total, 4);
    EXPECT_EQ(s.level, SummaryLevel::Error);
}

// Missing is not OK, so the chip cannot be green, but nothing is broken
// either: it is neutral, like the gray dot of a missing device.
TEST(SelfCheckModel, OnlyMissingIsNeutral) {
    std::vector<DeviceState> states{DeviceState::Ok, DeviceState::Missing};
    auto s = summarize_devices(states);
    EXPECT_EQ(s.ok, 1);
    EXPECT_EQ(s.total, 2);
    EXPECT_EQ(s.level, SummaryLevel::Neutral);
}

TEST(SelfCheckModel, ProblemsComeFirstInStableOrder) {
    std::vector<DeviceState> states{
        DeviceState::Ok,        // 0
        DeviceState::Missing,   // 1
        DeviceState::Failed,    // 2
        DeviceState::Ok,        // 3
        DeviceState::Degraded,  // 4
        DeviceState::Failed,    // 5
    };
    EXPECT_EQ(device_display_order(states), (std::vector<size_t>{2, 5, 4, 1, 0, 3}));
}

TEST(SelfCheckModel, AllOkKeepsTheReportOrder) {
    std::vector<DeviceState> states{DeviceState::Ok, DeviceState::Ok, DeviceState::Ok};
    EXPECT_EQ(device_display_order(states), (std::vector<size_t>{0, 1, 2}));
}

TEST(SelfCheckModel, InteractiveTestsPassOnceEach) {
    InteractiveTests tests;
    EXPECT_EQ(tests.passed_count(), 0);
    EXPECT_FALSE(tests.passed(InteractiveTest::Touch));

    EXPECT_TRUE(tests.pass(InteractiveTest::Touch));
    EXPECT_TRUE(tests.passed(InteractiveTest::Touch));
    EXPECT_EQ(tests.passed_count(), 1);
    EXPECT_FALSE(tests.pass(InteractiveTest::Touch)) << "a second press changes nothing";
    EXPECT_EQ(tests.passed_count(), 1);

    EXPECT_TRUE(tests.pass(InteractiveTest::Button));
    EXPECT_EQ(tests.passed_count(), 2);
    EXPECT_FALSE(tests.passed(InteractiveTest::Loopback));
}

TEST(SelfCheckModel, LoopbackPassesOnlyWhenARunCompletes) {
    InteractiveTests tests;
    EXPECT_FALSE(tests.on_loopback(LoopbackStatus{StrId::LoopRecording}));
    EXPECT_FALSE(tests.on_loopback(LoopbackStatus{StrId::LoopPlaying}));
    EXPECT_FALSE(tests.on_loopback(LoopbackStatus{StrId::LoopFailed, wisp::ErrorCode::NotFound}));
    EXPECT_FALSE(tests.passed(InteractiveTest::Loopback));
    EXPECT_EQ(tests.passed_count(), 0);

    EXPECT_TRUE(tests.on_loopback(LoopbackStatus{StrId::LoopDone}));
    EXPECT_TRUE(tests.passed(InteractiveTest::Loopback));
    EXPECT_EQ(tests.passed_count(), 1);

    // A later failed run does not take the pass back.
    EXPECT_FALSE(tests.on_loopback(LoopbackStatus{StrId::LoopFailed, wisp::ErrorCode::IoError}));
    EXPECT_TRUE(tests.passed(InteractiveTest::Loopback));
    EXPECT_FALSE(tests.on_loopback(LoopbackStatus{StrId::LoopDone}));
}
