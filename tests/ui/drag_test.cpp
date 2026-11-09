// How a finger-following drag settles (wisp/ui/drag.hpp), shared by the
// shell's page drags and the pull-down panel.

#include "wisp/ui/drag.hpp"

#include <gtest/gtest.h>

namespace {

using wisp::ui::drag_completes;
using wisp::ui::FlingTrack;
using wisp::ui::kFlingWindowMs;
using wisp::ui::kSlideMs;
using wisp::ui::slide_ms;

TEST(FlingTrack, FastOverTheLastWindowIsAFling) {
    FlingTrack t;
    EXPECT_FALSE(t.fling());  // no points
    t.add(0, 0);
    t.add(20, 40);  // 0.5 px/ms
    EXPECT_TRUE(t.fling());
    t.reset();
    t.add(0, 0);
    t.add(10, 40);  // 0.25 px/ms
    EXPECT_FALSE(t.fling());
    // Every point in one millisecond counts as one millisecond.
    t.reset();
    t.add(0, 5);
    t.add(3, 5);
    EXPECT_TRUE(t.fling());
}

TEST(FlingTrack, OnlyTheLastWindowAndForwardCount) {
    FlingTrack t;
    // Fast long ago, then still.
    t.add(0, 0);
    t.add(100, 20);
    t.add(100, 20 + kFlingWindowMs + 100);
    EXPECT_FALSE(t.fling());
    // Fast backwards.
    t.reset();
    t.add(100, 0);
    t.add(40, 30);
    EXPECT_FALSE(t.fling());
}

TEST(DragSettle, AThirdOrAFlingCompletes) {
    EXPECT_FALSE(drag_completes(159, 480, false));
    EXPECT_TRUE(drag_completes(160, 480, false));
    EXPECT_TRUE(drag_completes(10, 480, true));
}

TEST(DragSettle, SlideTimeIsInProportionAndNeverZero) {
    EXPECT_EQ(slide_ms(480, 480), kSlideMs);
    EXPECT_EQ(slide_ms(240, 480), kSlideMs / 2);
    EXPECT_EQ(slide_ms(-240, 480), kSlideMs / 2);
    EXPECT_EQ(slide_ms(0, 480), 1u);
    EXPECT_EQ(slide_ms(900, 480), kSlideMs);
}

}  // namespace
