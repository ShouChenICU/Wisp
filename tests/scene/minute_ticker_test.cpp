// MinuteTicker: when wisp.device's "minute" event fires (scene-runtime-core
// spec 3.4, 5.2).

#include "wisp/scene/minute_ticker.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

using wisp::scene::MinuteTicker;

namespace {

// 2026-11-04 12:00:00 UTC.
constexpr int64_t kNoon = 1793793600;
static_assert(kNoon % 60 == 0);

TEST(MinuteTicker, FiresOncePerWallMinute) {
    MinuteTicker t;
    EXPECT_FALSE(t.poll(kNoon + 30, 0));  // the first look only syncs
    EXPECT_EQ(t.next_poll_ms(kNoon + 30, 0), 30000u);
    EXPECT_FALSE(t.poll(kNoon + 59, 29000));
    EXPECT_EQ(t.next_poll_ms(kNoon + 59, 29000), 1000u);
    EXPECT_TRUE(t.poll(kNoon + 60, 30000));
    EXPECT_FALSE(t.poll(kNoon + 60, 30000));
    EXPECT_FALSE(t.poll(kNoon + 61, 31000));
    EXPECT_FALSE(t.poll(kNoon + 119, 89000));
    EXPECT_TRUE(t.poll(kNoon + 120, 90000));
    EXPECT_EQ(t.next_poll_ms(kNoon + 120, 90000), 60000u);
}

TEST(MinuteTicker, ForwardJumpFiresOnceNotPerSkippedMinute) {
    MinuteTicker t;
    EXPECT_FALSE(t.poll(kNoon + 10, 0));
    EXPECT_TRUE(t.poll(kNoon + 3 * 3600 + 10, 1000));
    EXPECT_FALSE(t.poll(kNoon + 3 * 3600 + 11, 2000));
    EXPECT_EQ(t.next_poll_ms(kNoon + 3 * 3600 + 11, 2000), 49000u);
}

TEST(MinuteTicker, BackwardJumpFiresOnceAndDoesNotStall) {
    MinuteTicker t;
    EXPECT_FALSE(t.poll(kNoon + 20, 0));
    EXPECT_TRUE(t.poll(kNoon - 3600 + 25, 1000));
    EXPECT_FALSE(t.poll(kNoon - 3600 + 26, 2000));
    // The next one is at the next whole minute of the new time, not an
    // hour away.
    EXPECT_EQ(t.next_poll_ms(kNoon - 3600 + 26, 2000), 34000u);
    EXPECT_TRUE(t.poll(kNoon - 3600 + 60, 36000));
}

TEST(MinuteTicker, UnsetClockFallsBackToMonotonic) {
    MinuteTicker t;
    const int64_t unset = MinuteTicker::kValidSince - 1;
    EXPECT_FALSE(t.poll(unset, 1000));
    EXPECT_EQ(t.next_poll_ms(unset, 1000), 60000u);
    EXPECT_FALSE(t.poll(std::nullopt, 60999));
    EXPECT_EQ(t.next_poll_ms(std::nullopt, 60999), 1u);
    EXPECT_TRUE(t.poll(std::nullopt, 61000));
    EXPECT_FALSE(t.poll(std::nullopt, 61000));
    EXPECT_EQ(t.next_poll_ms(std::nullopt, 61000), 60000u);
    EXPECT_TRUE(t.poll(0, 121000));
    // Polled late: one fire, then a minute from that poll, not a burst.
    EXPECT_TRUE(t.poll(std::nullopt, 400000));
    EXPECT_FALSE(t.poll(std::nullopt, 400001));
    EXPECT_EQ(t.next_poll_ms(std::nullopt, 400001), 59999u);
}

TEST(MinuteTicker, ClockBecomingValidResyncs) {
    MinuteTicker t;
    EXPECT_FALSE(t.poll(std::nullopt, 0));
    EXPECT_FALSE(t.poll(std::nullopt, 30000));
    // The clock got set: the time shown is now known, so it fires at once
    // and from then on on whole minutes.
    EXPECT_TRUE(t.poll(kNoon + 45, 31000));
    EXPECT_EQ(t.next_poll_ms(kNoon + 45, 31000), 15000u);
    EXPECT_FALSE(t.poll(kNoon + 59, 45000));
    EXPECT_TRUE(t.poll(kNoon + 60, 46000));
    // And back to unset: monotonic minutes from there.
    EXPECT_FALSE(t.poll(std::nullopt, 50000));
    EXPECT_EQ(t.next_poll_ms(std::nullopt, 50000), 60000u);
    EXPECT_TRUE(t.poll(std::nullopt, 110000));
}

TEST(MinuteTicker, ForceFiresOnceAtNextPoll) {
    MinuteTicker t;
    EXPECT_FALSE(t.poll(kNoon + 10, 0));
    t.force();
    EXPECT_EQ(t.next_poll_ms(kNoon + 10, 0), 0u);
    EXPECT_TRUE(t.poll(kNoon + 10, 0));
    EXPECT_FALSE(t.poll(kNoon + 10, 0));
    // Forced in a minute that also changed: still one fire.
    t.force();
    EXPECT_TRUE(t.poll(kNoon + 70, 60000));
    EXPECT_FALSE(t.poll(kNoon + 71, 61000));
    // Forced before the first poll: fires, and syncs.
    MinuteTicker fresh;
    fresh.force();
    EXPECT_TRUE(fresh.poll(kNoon + 5, 0));
    EXPECT_FALSE(fresh.poll(kNoon + 6, 1000));
}

// A time sync (clock_set()) fires the minute it brings, unless a poll
// already fired for that minute: the scene task may poll between the
// clock moving and the sync's message reaching it.
TEST(MinuteTicker, SyncFiresOnlyAMinuteNotYetFired) {
    // The poll saw the jump first: the sync adds nothing.
    MinuteTicker polled;
    EXPECT_FALSE(polled.poll(std::nullopt, 0));
    EXPECT_TRUE(polled.poll(kNoon + 5, 100));
    EXPECT_FALSE(polled.sync(kNoon + 5, 200));
    EXPECT_FALSE(polled.poll(kNoon + 6, 300));

    // The sync came first: it fires, and the poll after it does not.
    MinuteTicker synced;
    EXPECT_FALSE(synced.poll(std::nullopt, 0));
    EXPECT_TRUE(synced.sync(kNoon + 5, 100));
    EXPECT_FALSE(synced.poll(kNoon + 6, 200));

    // Only bearings taken (nothing fired yet): the sync fires.
    MinuteTicker fresh;
    EXPECT_FALSE(fresh.poll(kNoon + 5, 0));
    EXPECT_TRUE(fresh.sync(kNoon + 5, 100));

    // A sync that moves the clock to a minute not yet fired fires, once.
    MinuteTicker moved;
    EXPECT_FALSE(moved.poll(kNoon + 5, 0));
    EXPECT_TRUE(moved.poll(kNoon + 60, 1000));
    EXPECT_TRUE(moved.sync(kNoon + 3600, 2000));
    EXPECT_FALSE(moved.poll(kNoon + 3601, 3000));
    EXPECT_FALSE(moved.sync(kNoon + 3602, 4000));

    // The minute that fired on the monotonic clock is not a wall minute.
    MinuteTicker mono;
    EXPECT_FALSE(mono.poll(std::nullopt, 0));
    EXPECT_TRUE(mono.poll(std::nullopt, 60000));
    EXPECT_TRUE(mono.sync(kNoon, 60100));
}

}  // namespace
