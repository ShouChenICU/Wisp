// pick_evictions(): which background scenes close to keep within the
// background count and the shared VM budget (scene-device spec 3.5).

#include "wisp/scene/scene_schedule.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using wisp::scene::Eviction;
using wisp::scene::pick_evictions;
using wisp::scene::SlotInfo;

namespace {

constexpr size_t kMiB = 1024 * 1024;

SlotInfo bg(const char* id, uint64_t last_front_ms, size_t bytes = 100 * 1024) {
    return SlotInfo{id, false, last_front_ms, bytes};
}

SlotInfo fg(const char* id, size_t bytes = 100 * 1024) { return SlotInfo{id, true, 0, bytes}; }

std::vector<std::string> ids(const std::vector<Eviction>& e) {
    std::vector<std::string> out;
    for (const auto& x : e) out.push_back(x.id);
    return out;
}

}  // namespace

TEST(SceneSchedule, CountEvictsLeastRecentlyFront) {
    const std::vector<SlotInfo> slots = {bg("a", 300), bg("b", 100), bg("c", 400), bg("d", 200)};
    auto out = pick_evictions(slots, false, 3, 3 * kMiB, 2 * kMiB);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].id, "b");
    EXPECT_FALSE(out[0].memory);

    // Two past the limit: the two oldest, oldest first.
    out = pick_evictions(slots, false, 2, 3 * kMiB, 2 * kMiB);
    EXPECT_EQ(ids(out), (std::vector<std::string>{"b", "d"}));
    // At the limit: nothing; the scene in front never counts.
    std::vector<SlotInfo> three = {bg("a", 1), bg("b", 2), bg("c", 3), fg("f")};
    EXPECT_TRUE(pick_evictions(three, false, 3, 3 * kMiB, 2 * kMiB).empty());
    // Equal times keep the order given.
    std::vector<SlotInfo> ties = {bg("x", 0), bg("y", 0), bg("z", 0), bg("w", 0)};
    EXPECT_EQ(ids(pick_evictions(ties, false, 3, 3 * kMiB, 2 * kMiB)),
              (std::vector<std::string>{"x"}));
}

TEST(SceneSchedule, MemoryEvictsUntilReserveFits) {
    const size_t big = 6 * kMiB / 10;  // 0.6 MiB
    std::vector<SlotInfo> slots = {bg("new", 200, big), bg("old", 100, big), fg("f", kMiB)};
    auto out = pick_evictions(slots, false, 3, 3 * kMiB, 2 * kMiB);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].id, "old");
    EXPECT_TRUE(out[0].memory);

    // Already within budget - reserve: nothing.
    slots = {bg("a", 1, kMiB / 2), bg("b", 2, kMiB / 2), fg("f", 2 * kMiB)};
    EXPECT_TRUE(pick_evictions(slots, false, 3, 3 * kMiB, 2 * kMiB).empty());

    // A reserve past the budget leaves no room behind the front at all.
    slots = {bg("a", 1, 10), bg("b", 2, 10)};
    EXPECT_EQ(ids(pick_evictions(slots, true, 3, kMiB, 2 * kMiB)),
              (std::vector<std::string>{"a", "b"}));

    // The count goes first; a scene it closed is not counted again.
    slots = {bg("a", 1, big), bg("b", 2, big), bg("c", 3, big), bg("d", 4, 10), fg("f")};
    out = pick_evictions(slots, false, 3, 3 * kMiB, 2 * kMiB);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].id, "a");
    EXPECT_FALSE(out[0].memory);
    EXPECT_EQ(out[1].id, "b");
    EXPECT_TRUE(out[1].memory);
}

TEST(SceneSchedule, NoMemoryEvictionWithoutForeground) {
    const size_t big = 6 * kMiB / 10;
    const std::vector<SlotInfo> slots = {bg("a", 1, big), bg("b", 2, big)};
    EXPECT_TRUE(pick_evictions(slots, false, 3, 3 * kMiB, 2 * kMiB).empty());
    // Opening one counts as having one in front.
    EXPECT_EQ(ids(pick_evictions(slots, true, 3, 3 * kMiB, 2 * kMiB)),
              (std::vector<std::string>{"a"}));
}
