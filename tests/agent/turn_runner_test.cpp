// The agent's turn internals (components/wisp_agent/src): Calibration, the
// token estimate's multiplier kept per (profile id, model) pair (agent-core
// spec 3.3).

#include <gtest/gtest.h>

#include "calibration.hpp"

using wisp::agent::Calibration;

TEST(Calibration, StartsAt100) {
    Calibration cal;
    EXPECT_EQ(cal.pct(), 100u);
    EXPECT_EQ(cal.scaled(300), 300u);
}

TEST(Calibration, ObserveSetsTheClampedRatio) {
    Calibration cal;
    cal.observe("p", "m", 2002, 1001);
    EXPECT_EQ(cal.pct(), 200u);
    EXPECT_EQ(cal.scaled(300), 600u);
    cal.observe("p", "m", 10, 1000);
    EXPECT_EQ(cal.pct(), Calibration::kMinPct);
    cal.observe("p", "m", 10000, 1000);
    EXPECT_EQ(cal.pct(), Calibration::kMaxPct);
    // An estimate of 0 counts as 1.
    cal.observe("p", "m", 5, 0);
    EXPECT_EQ(cal.pct(), Calibration::kMaxPct);
}

TEST(Calibration, OverflowRaisesToAtLeast150) {
    Calibration cal;
    cal.overflow("p", "m");
    EXPECT_EQ(cal.pct(), Calibration::kOverflowPct);
    cal.observe("p", "m", 2002, 1001);
    cal.overflow("p", "m");
    EXPECT_EQ(cal.pct(), 200u);
}

TEST(Calibration, SamePairKeepsItAcrossTurns) {
    Calibration cal;
    cal.observe("p", "m", 2002, 1001);
    cal.begin_turn("p", "m");
    EXPECT_EQ(cal.pct(), 200u);
}

TEST(Calibration, AnotherModelStartsAgain) {
    Calibration cal;
    cal.observe("p", "m", 2002, 1001);
    cal.begin_turn("p", "m2");
    EXPECT_EQ(cal.pct(), 100u);
}

TEST(Calibration, AnotherProfileWithTheSameModelStartsAgain) {
    Calibration cal;
    cal.observe("p", "m", 2002, 1001);
    cal.begin_turn("q", "m");
    EXPECT_EQ(cal.pct(), 100u);
}

// A turn with no active model has the key ("", ""), and it holds like any
// other pair.
TEST(Calibration, NoModelIsAPairToo) {
    Calibration cal;
    cal.observe("", "", 2002, 1001);
    cal.begin_turn("", "");
    cal.begin_turn("", "");
    EXPECT_EQ(cal.pct(), 200u);
}

TEST(Calibration, UnscaledWindow) {
    Calibration cal;
    EXPECT_EQ(cal.unscaled_window(4096), 4096u);
    cal.observe("p", "m", 2002, 1001);
    EXPECT_EQ(cal.unscaled_window(4096), 2048u);
    // The intermediate does not overflow 32 bits.
    cal.observe("p", "m", 10000, 1000);
    EXPECT_EQ(cal.unscaled_window(4000000000u), 1333333333u);
}
