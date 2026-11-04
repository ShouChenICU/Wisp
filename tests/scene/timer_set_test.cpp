// TimerSet: a scene's wisp.timer deadlines (scene-runtime-core spec 3.4,
// 5.1).

#include "wisp/scene/timer_set.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using wisp::ErrorCode;
using wisp::scene::TimerSet;

namespace {

using Ids = std::vector<uint32_t>;

TEST(TimerSet, AfterEveryCancel) {
    TimerSet t;
    EXPECT_EQ(t.next_due(), std::nullopt);
    auto once = t.after(100, 1000);
    auto every = t.every(250, 1000);
    auto gone = t.after(60, 1000);
    ASSERT_TRUE(once && every && gone);
    EXPECT_NE(once.value(), every.value());
    EXPECT_EQ(t.size(), 3u);
    EXPECT_TRUE(t.cancel(gone.value()));
    EXPECT_FALSE(t.cancel(gone.value()));
    EXPECT_EQ(t.size(), 2u);
    EXPECT_EQ(t.next_due(), std::optional<uint64_t>(1100));

    EXPECT_EQ(t.due(1099), Ids{});
    EXPECT_EQ(t.due(1100), Ids{once.value()});
    EXPECT_EQ(t.size(), 1u);  // an after timer goes once it fired
    EXPECT_EQ(t.next_due(), std::optional<uint64_t>(1250));
    EXPECT_EQ(t.due(1250), Ids{every.value()});
    EXPECT_EQ(t.next_due(), std::optional<uint64_t>(1500));
    // Late by less than a period: the phase is kept.
    EXPECT_EQ(t.due(1520), Ids{every.value()});
    EXPECT_EQ(t.next_due(), std::optional<uint64_t>(1750));
    EXPECT_TRUE(t.cancel(every.value()));
    EXPECT_EQ(t.due(5000), Ids{});
    EXPECT_EQ(t.next_due(), std::nullopt);
    EXPECT_EQ(t.size(), 0u);
}

TEST(TimerSet, DueInDeadlineOrder) {
    TimerSet t;
    auto a = t.after(300, 0).value();
    auto b = t.after(100, 0).value();
    auto c = t.every(200, 0).value();
    EXPECT_EQ(t.due(300), (Ids{b, c, a}));
}

TEST(TimerSet, MinIntervalAndCount) {
    TimerSet t;
    auto small = t.after(49, 0);
    ASSERT_FALSE(small);
    EXPECT_EQ(small.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(t.every(49, 0).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(t.every(0, 0).error().code, ErrorCode::InvalidArgument);
    EXPECT_TRUE(t.after(50, 0));

    std::vector<uint32_t> ids;
    for (size_t i = 1; i < TimerSet::kMaxTimers; ++i) {
        auto r = (i % 2) ? t.every(50, 0) : t.after(1000, 0);
        ASSERT_TRUE(r) << i;
        ids.push_back(r.value());
    }
    EXPECT_EQ(t.size(), 32u);
    auto over = t.after(100, 0);
    ASSERT_FALSE(over);
    EXPECT_EQ(over.error().code, ErrorCode::QuotaExceeded);
    EXPECT_EQ(t.every(100, 0).error().code, ErrorCode::QuotaExceeded);

    ASSERT_TRUE(t.cancel(ids.front()));
    EXPECT_TRUE(t.after(100, 0));
    EXPECT_EQ(t.size(), 32u);
}

TEST(TimerSet, ResumeFiresMissedEveryOnce) {
    TimerSet t;
    auto every = t.every(100, 0).value();
    t.pause(50);
    EXPECT_EQ(t.next_due(), std::nullopt);  // nothing to wake up for
    EXPECT_EQ(t.due(1050), Ids{});          // paused: nothing fires
    t.resume(1050);
    EXPECT_EQ(t.next_due(), std::optional<uint64_t>(100));
    EXPECT_EQ(t.due(1050), Ids{every});  // ten periods missed, one fire
    EXPECT_EQ(t.due(1050), Ids{});
    EXPECT_EQ(t.next_due(), std::optional<uint64_t>(1150));
    EXPECT_EQ(t.due(1150), Ids{every});

    // Pause and resume are idempotent; a timer not yet due keeps its time.
    auto later = t.after(5000, 1150).value();
    t.pause(1200);
    t.pause(1300);
    t.resume(1400);
    t.resume(1400);
    EXPECT_EQ(t.due(1400), Ids{every});
    EXPECT_EQ(t.due(6150), (Ids{every, later}));
}

TEST(TimerSet, AddedWhilePausedWaitsForResume) {
    TimerSet t;
    t.pause(0);
    auto id = t.after(100, 0).value();
    EXPECT_EQ(t.due(500), Ids{});
    EXPECT_EQ(t.next_due(), std::nullopt);
    t.resume(500);
    EXPECT_EQ(t.due(500), Ids{id});
}

TEST(TimerSet, ClearDropsEverything) {
    TimerSet t;
    t.every(100, 0);
    t.after(100, 0);
    t.clear();
    EXPECT_EQ(t.size(), 0u);
    EXPECT_EQ(t.due(1000), Ids{});
}

}  // namespace
