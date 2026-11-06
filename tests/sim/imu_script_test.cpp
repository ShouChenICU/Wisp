// The simulator's --imu-script: parsing, and the accelerations it plays.

#include "wisp/port/sim/imu_script.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "wisp/core/gesture.hpp"

using wisp::port::sim::imu_script_accel;
using wisp::port::sim::ImuScriptStep;
using wisp::port::sim::parse_imu_script;

namespace {

constexpr float kG = 9.80665f;

}  // namespace

TEST(ImuScript, ParsesStepsAndComments) {
    auto r = parse_imu_script(
        "# a comment\n"
        "\n"
        "100 tilt 0.5 -0.25 0.8\n"
        "  2000   shake  \n"
        "3000 flip\r\n"
        "# trailing\n");
    ASSERT_TRUE(r) << r.error().message;
    const auto& s = r.value();
    ASSERT_EQ(s.size(), 3u);
    EXPECT_EQ(s[0].at_ms, 100u);
    EXPECT_EQ(s[0].kind, ImuScriptStep::Kind::Tilt);
    EXPECT_FLOAT_EQ(s[0].x, 0.5f);
    EXPECT_FLOAT_EQ(s[0].y, -0.25f);
    EXPECT_FLOAT_EQ(s[0].z, 0.8f);
    EXPECT_EQ(s[1].at_ms, 2000u);
    EXPECT_EQ(s[1].kind, ImuScriptStep::Kind::Shake);
    EXPECT_EQ(s[2].at_ms, 3000u);
    EXPECT_EQ(s[2].kind, ImuScriptStep::Kind::Flip);

    for (const char* bad : {"100 tilt 1 2\n", "x shake\n", "100 wobble\n", "100 shake now\n",
                            "-5 flip\n", "100 tilt a b c\n", "100 tilt 0 0 1 2\n"}) {
        auto e = parse_imu_script(std::string("# ok\n") + bad);
        ASSERT_FALSE(e) << bad;
        EXPECT_EQ(e.error().code, wisp::ErrorCode::InvalidArgument);
        EXPECT_NE(e.error().message.find("line 2"), std::string::npos) << e.error().message;
    }
    // Out of order.
    auto late = parse_imu_script("200 flip\n100 shake\n");
    ASSERT_FALSE(late);
    EXPECT_NE(late.error().message.find("line 2"), std::string::npos);
    EXPECT_TRUE(parse_imu_script(""));
}

TEST(ImuScript, PlaysTiltShakeAndFlip) {
    auto steps = parse_imu_script("100 tilt 0 0.6 0.8\n1000 shake\n2000 flip\n").value();
    // Before the first step: nothing (the keyboard's tilt applies).
    EXPECT_FALSE(imu_script_accel(steps, 50).has_value());
    auto a = imu_script_accel(steps, 500).value();
    EXPECT_FLOAT_EQ(a[1], 0.6f);
    EXPECT_FLOAT_EQ(a[2], 0.8f);

    // Sampled at 50 Hz, the shake is one shake and the flip one flip.
    wisp::core::ShakeDetector shake;
    wisp::core::FlipDetector flip;
    int shakes = 0, flips = 0;
    float z_end = 0;
    for (uint64_t t = 100; t < 3000; t += 20) {
        auto v = imu_script_accel(steps, t).value();
        if (shake.feed(v[0] * kG, v[1] * kG, v[2] * kG, t)) ++shakes;
        if (flip.feed(v[2] * kG, t)) ++flips;
        z_end = v[2];
    }
    EXPECT_EQ(shakes, 1);
    EXPECT_EQ(flips, 1);
    EXPECT_LT(z_end, -0.7f);
    // The peaks are 3 g.
    auto peak = imu_script_accel(steps, 1000).value();
    EXPECT_NEAR(std::sqrt(peak[0] * peak[0] + peak[1] * peak[1] + peak[2] * peak[2]), 3.0f, 0.01f);
}
