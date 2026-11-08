#include <gtest/gtest.h>

#include "wisp/hal/imu.hpp"

using wisp::hal::ImuAxisMap;
using wisp::hal::ImuSample;
using wisp::hal::to_screen_frame;

namespace {

ImuSample sample() {
    ImuSample s;
    s.ax = 1;
    s.ay = 2;
    s.az = 3;
    s.gx = 10;
    s.gy = 20;
    s.gz = 30;
    s.ts_ms = 42;
    return s;
}

}  // namespace

TEST(ImuAxisMap, DefaultKeepsTheChipAxes) {
    const ImuSample out = to_screen_frame(sample(), ImuAxisMap{});
    EXPECT_FLOAT_EQ(out.ax, 1);
    EXPECT_FLOAT_EQ(out.ay, 2);
    EXPECT_FLOAT_EQ(out.az, 3);
    EXPECT_FLOAT_EQ(out.gx, 10);
    EXPECT_FLOAT_EQ(out.gy, 20);
    EXPECT_FLOAT_EQ(out.gz, 30);
    EXPECT_EQ(out.ts_ms, 42u);
}

// A chip turned a quarter turn about z: screen x = -chip y, screen y =
// chip x. Acceleration and angular rate move together.
TEST(ImuAxisMap, QuarterTurnAboutZ) {
    const ImuAxisMap map{.from = {1, 0, 2}, .negate = {true, false, false}};
    const ImuSample out = to_screen_frame(sample(), map);
    EXPECT_FLOAT_EQ(out.ax, -2);
    EXPECT_FLOAT_EQ(out.ay, 1);
    EXPECT_FLOAT_EQ(out.az, 3);
    EXPECT_FLOAT_EQ(out.gx, -20);
    EXPECT_FLOAT_EQ(out.gy, 10);
    EXPECT_FLOAT_EQ(out.gz, 30);
    EXPECT_EQ(out.ts_ms, 42u);
}

TEST(ImuAxisMap, OutOfRangeSourceKeepsTheSameAxis) {
    const ImuAxisMap map{.from = {7, 1, 2}, .negate = {false, false, false}};
    EXPECT_FLOAT_EQ(to_screen_frame(sample(), map).ax, 1);
}
