#include "wisp/media/play_queue.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

using namespace wisp;
using media::PlayMode;
using media::PlayQueue;

namespace {

std::vector<std::string> names(size_t n, const std::string& prefix = "music/t") {
    std::vector<std::string> v;
    for (size_t i = 0; i < n; ++i) {
        v.push_back(prefix + std::to_string(i) + ".mp3");
    }
    return v;
}

// Deterministic LCG so shuffle tests are repeatable.
std::function<uint32_t()> lcg(uint32_t seed = 12345) {
    return [s = seed]() mutable {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    };
}

}  // namespace

TEST(PlayQueue, ModeNamesRoundTrip) {
    for (auto m :
         {PlayMode::Sequence, PlayMode::RepeatAll, PlayMode::RepeatOne, PlayMode::Shuffle}) {
        auto back = media::play_mode_from(media::to_string(m));
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(*back, m);
    }
    EXPECT_FALSE(media::play_mode_from("loop").has_value());
}

TEST(PlayQueue, SetReplacesAndTruncatesAt1000) {
    PlayQueue q;
    EXPECT_FALSE(q.set(names(3), 1).truncated);
    EXPECT_EQ(q.size(), 3u);
    EXPECT_EQ(q.current(), 1u);
    EXPECT_EQ(q.item(1), "music/t1.mp3");

    EXPECT_TRUE(q.set(names(1005), 0).truncated);
    EXPECT_EQ(q.size(), 1000u);
    EXPECT_FALSE(q.set(names(1000), 999).truncated);

    q.set(names(3), 7);  // start past the end falls back to the first
    EXPECT_EQ(q.current(), 0u);
    q.set({}, 0);
    EXPECT_FALSE(q.current().has_value());
    EXPECT_FALSE(q.next().has_value());
}

TEST(PlayQueue, SequenceStopsAtTheEnd) {
    PlayQueue q;
    q.set(names(3), 0);
    EXPECT_EQ(q.advance_auto(), 1u);
    EXPECT_EQ(q.next(), 2u);
    EXPECT_FALSE(q.advance_auto().has_value());
    EXPECT_FALSE(q.next().has_value());
}

TEST(PlayQueue, RepeatAllWraps) {
    PlayQueue q;
    q.set(names(3), 2);
    q.set_mode(PlayMode::RepeatAll, nullptr);
    EXPECT_EQ(q.advance_auto(), 0u);
    EXPECT_EQ(q.prev(0), 2u);
    EXPECT_EQ(q.next(), 0u);
}

TEST(PlayQueue, RepeatOneRepeatsOnAutoButNextMoves) {
    PlayQueue q;
    q.set(names(3), 1);
    q.set_mode(PlayMode::RepeatOne, nullptr);
    EXPECT_EQ(q.advance_auto(), 1u);
    EXPECT_EQ(q.advance_auto(), 1u);
    EXPECT_EQ(q.next(), 2u);
    EXPECT_EQ(q.next(), 0u);  // queue order with wrap
    EXPECT_EQ(q.prev(0), 2u);
}

TEST(PlayQueue, ShufflePlaysEachOncePerCycle) {
    PlayQueue q;
    q.set(names(10), 4);
    q.set_mode(PlayMode::Shuffle, lcg());
    // The starting item counts as the first play: 30 plays, 3 per item.
    std::map<size_t, int> count;
    std::vector<size_t> seq{*q.current()};
    for (int i = 0; i < 29; ++i) {
        auto n = q.advance_auto();
        ASSERT_TRUE(n.has_value());
        seq.push_back(*n);
    }
    for (size_t v : seq) {
        ++count[v];
    }
    ASSERT_EQ(count.size(), 10u);
    for (auto& [idx, c] : count) {
        EXPECT_EQ(c, 3) << idx;
    }
    // Every window of one cycle holds each item once; no repeat at wraps.
    for (size_t i = 1; i < seq.size(); ++i) {
        EXPECT_NE(seq[i], seq[i - 1]);
    }
    std::vector<int> first(10, 0);
    for (size_t i = 0; i < 10; ++i) {
        ++first[seq[i]];
    }
    for (int c : first) {
        EXPECT_EQ(c, 1);
    }
}

TEST(PlayQueue, ShuffleOfTwoAlternates) {
    PlayQueue q;
    q.set(names(2), 0);
    q.set_mode(PlayMode::Shuffle, lcg(7));
    size_t last = *q.current();
    for (int i = 0; i < 20; ++i) {
        auto n = q.advance_auto();
        ASSERT_TRUE(n.has_value());
        EXPECT_NE(*n, last);
        last = *n;
    }
}

TEST(PlayQueue, ShufflePrevGoesBackThroughThePermutation) {
    PlayQueue q;
    q.set(names(6), 0);
    q.set_mode(PlayMode::Shuffle, lcg());
    size_t a = *q.current();
    size_t b = *q.advance_auto();
    size_t c = *q.advance_auto();
    EXPECT_EQ(q.prev(0), b);
    EXPECT_EQ(q.prev(0), a);
    EXPECT_EQ(q.next(), b);
    EXPECT_EQ(q.next(), c);
}

TEST(PlayQueue, LeavingShuffleContinuesInOrderFromCurrent) {
    PlayQueue q;
    q.set(names(6), 0);
    q.set_mode(PlayMode::Shuffle, lcg());
    q.advance_auto();
    q.advance_auto();
    size_t cur = *q.current();
    q.set_mode(PlayMode::RepeatAll, nullptr);
    EXPECT_EQ(q.current(), cur);
    EXPECT_EQ(q.next(), (cur + 1) % 6);
}

TEST(PlayQueue, PrevRestartsAfter3s) {
    PlayQueue q;
    q.set(names(3), 1);
    EXPECT_EQ(q.prev(4), 1u);
    EXPECT_EQ(q.prev(3), 0u);  // exactly 3 s still goes back
    EXPECT_EQ(q.current(), 0u);
}

TEST(PlayQueue, PrevAtFirstInSequenceStays) {
    PlayQueue q;
    q.set(names(3), 0);
    EXPECT_EQ(q.prev(0), 0u);
    EXPECT_EQ(q.current(), 0u);
}

TEST(PlayQueue, RemovePrefixMovesCurrent) {
    PlayQueue q;
    auto items = names(2);
    auto scene = names(2, "scene:a/");
    items.insert(items.begin() + 1, scene.begin(), scene.end());  // m0 a0 a1 m1
    q.set(items, 1);
    EXPECT_EQ(q.remove_prefix("scene:a/"), 2u);
    ASSERT_EQ(q.size(), 2u);
    EXPECT_EQ(q.current(), 1u);
    EXPECT_EQ(q.item(1), "music/t1.mp3");

    // Current before the removed items keeps its index.
    q.set(items, 0);
    q.remove_prefix("scene:a/");
    EXPECT_EQ(q.current(), 0u);

    // Nothing follows in Sequence: no current, items remain.
    auto tail = names(1);
    auto sc = names(1, "scene:a/");
    tail.insert(tail.end(), sc.begin(), sc.end());
    q.set(tail, 1);
    EXPECT_EQ(q.remove_prefix("scene:a/"), 1u);
    EXPECT_FALSE(q.current().has_value());

    // RepeatAll wraps to the first.
    q.set(tail, 1);
    q.set_mode(PlayMode::RepeatAll, nullptr);
    q.remove_prefix("scene:a/");
    EXPECT_EQ(q.current(), 0u);
}

TEST(PlayQueue, RemovePrefixKeepsShuffleCycle) {
    PlayQueue q;
    auto items = names(4);
    auto sc = names(2, "scene:a/");
    items.insert(items.end(), sc.begin(), sc.end());
    q.set(items, 0);
    q.set_mode(PlayMode::Shuffle, lcg());
    q.remove_prefix("scene:a/");
    std::map<size_t, int> count;
    ++count[*q.current()];
    for (int i = 0; i < 3; ++i) {
        auto n = q.advance_auto();
        ASSERT_TRUE(n.has_value());
        ASSERT_LT(*n, 4u);
        ++count[*n];
    }
    EXPECT_EQ(count.size(), 4u);
}

TEST(PlayQueue, RemovePrefixEmptiesTheQueue) {
    PlayQueue q;
    q.set(names(3, "scene:a/"), 1);
    q.set_mode(PlayMode::Shuffle, lcg());
    EXPECT_EQ(q.remove_prefix("scene:a/"), 3u);
    EXPECT_EQ(q.size(), 0u);
    EXPECT_FALSE(q.current().has_value());
    EXPECT_FALSE(q.next().has_value());
    EXPECT_EQ(q.remove_prefix("scene:a/"), 0u);
}

// A queue whose current item ran out in Sequence mode stays without one
// when a later removal happens in Shuffle mode.
TEST(PlayQueue, RemovePrefixKeepsAnEndedQueueEnded) {
    PlayQueue q;
    q.set({"music/a.mp3", "music/b.mp3", "scene:x/1.mp3", "scene:y/1.mp3"}, 3);
    EXPECT_EQ(q.remove_prefix("scene:y/"), 1u);
    EXPECT_FALSE(q.current().has_value());
    q.set_mode(PlayMode::Shuffle, lcg());
    EXPECT_EQ(q.remove_prefix("scene:x/"), 1u);
    EXPECT_EQ(q.size(), 2u);
    EXPECT_FALSE(q.current().has_value());
    EXPECT_FALSE(q.next().has_value());
}
