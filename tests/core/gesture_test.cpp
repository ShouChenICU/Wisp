#include "wisp/core/gesture.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using wisp::core::FlipDetector;
using wisp::core::ShakeDetector;

namespace {

constexpr float kG = 9.80665f;

// Feeds a 50 Hz timeline from `from` to `to` (exclusive): 3 g on the
// peaks' 40 ms, 1 g between them. Returns how many times it fired.
int run_peaks(ShakeDetector& d, uint64_t from, uint64_t to, uint64_t first_peak, uint64_t spacing,
              int peaks) {
    int fired = 0;
    for (uint64_t t = from; t < to; t += 20) {
        bool peak = false;
        for (int i = 0; i < peaks; ++i) {
            const uint64_t at = first_peak + static_cast<uint64_t>(i) * spacing;
            if (t >= at && t < at + 40) peak = true;
        }
        if (d.feed(0, 0, peak ? 3 * kG : kG, t)) ++fired;
    }
    return fired;
}

}  // namespace

TEST(ShakeDetector, ThreePeaksWithin500ms) {
    ShakeDetector d;
    // Rising edges at 100, 300, 500: within 400 ms.
    EXPECT_EQ(run_peaks(d, 0, 1000, 100, 200, 3), 1);
    // The magnitude counts, not one axis.
    ShakeDetector m;
    int fired = 0;
    for (uint64_t t = 0; t < 600; t += 20) {
        const bool peak = t % 200 == 0;
        if (m.feed(peak ? 15.0f : 0, peak ? 15.0f : 0, kG, t)) ++fired;
    }
    EXPECT_EQ(fired, 1);
}

TEST(ShakeDetector, SlowPeaksIgnored) {
    ShakeDetector d;
    // 300 ms apart: the three span 600 ms.
    EXPECT_EQ(run_peaks(d, 0, 1200, 100, 300, 3), 0);
    // A peak held high is one edge, not many.
    ShakeDetector held;
    int fired = 0;
    for (uint64_t t = 0; t < 1000; t += 20)
        if (held.feed(0, 0, 3 * kG, t)) ++fired;
    EXPECT_EQ(fired, 0);
}

TEST(ShakeDetector, RefractoryOneSecond) {
    ShakeDetector d;
    // Fires on the third edge at 200.
    EXPECT_EQ(run_peaks(d, 0, 300, 0, 100, 3), 1);
    // Three more right away: within the second after it, nothing.
    EXPECT_EQ(run_peaks(d, 300, 1100, 400, 100, 3), 0);
    // After the second: fires again.
    EXPECT_EQ(run_peaks(d, 1100, 1600, 1200, 100, 3), 1);
}

TEST(FlipDetector, FlipAfterStable300ms) {
    FlipDetector d;
    for (uint64_t t = 0; t < 100; t += 20) EXPECT_FALSE(d.feed(kG, t));
    // Face down from 100 on.
    EXPECT_FALSE(d.feed(-kG, 100));
    EXPECT_FALSE(d.feed(-kG, 300));
    EXPECT_FALSE(d.feed(-kG, 399));
    EXPECT_TRUE(d.feed(-kG, 400));
    // Once.
    EXPECT_FALSE(d.feed(-kG, 420));
    EXPECT_FALSE(d.feed(-kG, 2000));
    // And back over.
    EXPECT_FALSE(d.feed(kG, 2100));
    EXPECT_TRUE(d.feed(kG, 2400));
}

TEST(FlipDetector, BriefFlipIgnored) {
    FlipDetector d;
    EXPECT_FALSE(d.feed(kG, 0));
    int fired = 0;
    for (uint64_t t = 20; t < 300; t += 20)
        if (d.feed(-kG, t)) ++fired;
    for (uint64_t t = 300; t < 1000; t += 20)
        if (d.feed(kG, t)) ++fired;
    EXPECT_EQ(fired, 0);
    // A reading on edge restarts the hold.
    for (uint64_t t = 1000; t < 1200; t += 20)
        if (d.feed(-kG, t)) ++fired;
    EXPECT_FALSE(d.feed(0, 1200));
    for (uint64_t t = 1220; t < 1500; t += 20)
        if (d.feed(-kG, t)) ++fired;
    EXPECT_EQ(fired, 0);
    // The first face it sees is not a flip.
    FlipDetector fresh;
    EXPECT_FALSE(fresh.feed(-kG, 0));
    EXPECT_FALSE(fresh.feed(-kG, 1000));
}
