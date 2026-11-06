#include "wisp/core/occupancy.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>

using wisp::ErrorCode;
using wisp::core::OccupancyMode;
using wisp::core::OccupancyRegistry;

TEST(Occupancy, SharedAndExclusive) {
    std::atomic<uint64_t> now{1000};
    OccupancyRegistry reg([&] { return now.load(); });
    ASSERT_TRUE(reg.acquire("imu.main", "scene.a", OccupancyMode::Shared));
    now = 2000;
    ASSERT_TRUE(reg.acquire("imu.main", "system.selfcheck", OccupancyMode::Shared));
    // The same holder again: no second entry.
    ASSERT_TRUE(reg.acquire("imu.main", "scene.a", OccupancyMode::Shared));

    auto list = reg.list();
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].device, "imu.main");
    EXPECT_EQ(list[0].holder, "scene.a");
    EXPECT_EQ(list[0].mode, OccupancyMode::Shared);
    EXPECT_EQ(list[0].since_ms, 1000u);
    EXPECT_EQ(list[1].holder, "system.selfcheck");
    EXPECT_EQ(list[1].since_ms, 2000u);

    // Exclusive over shared holders is refused.
    auto r = reg.acquire("imu.main", "scene.b", OccupancyMode::Exclusive);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::Busy);
    // Another device is free.
    EXPECT_TRUE(reg.acquire("audio_in.main", "scene.b", OccupancyMode::Exclusive));
    // Nothing joins an exclusive holder.
    r = reg.acquire("audio_in.main", "scene.c", OccupancyMode::Shared);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::Busy);

    reg.release("imu.main", "scene.a");
    reg.release("imu.main", "system.selfcheck");
    reg.release("imu.main", "nobody");
    EXPECT_TRUE(reg.acquire("imu.main", "scene.b", OccupancyMode::Exclusive));
    list = reg.list();
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[1].mode, OccupancyMode::Exclusive);
    EXPECT_STREQ(wisp::core::to_string(OccupancyMode::Shared), "shared");
    EXPECT_STREQ(wisp::core::to_string(OccupancyMode::Exclusive), "exclusive");
}
