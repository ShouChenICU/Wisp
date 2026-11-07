// The shell's gestures (wisp/shell/gestures.hpp): swipe classification, the
// navigation table and a press on the wisp.

#include "wisp/shell/gestures.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>

#include "wisp/shell/limits.hpp"
#include "wisp/voice/limits.hpp"

using wisp::shell::classify_swipe;
using wisp::shell::navigate;
using wisp::shell::Page;
using wisp::shell::Swipe;
using wisp::shell::WispPress;
using Action = WispPress::Action;

namespace {

lv_point_t pt(int32_t x, int32_t y) { return lv_point_t{x, y}; }

}  // namespace

TEST(Gestures, FourDirectionsAndThreshold) {
    const lv_point_t o = pt(100, 100);
    EXPECT_EQ(classify_swipe(o, pt(141, 100)), Swipe::Right);
    EXPECT_EQ(classify_swipe(o, pt(59, 100)), Swipe::Left);
    EXPECT_EQ(classify_swipe(o, pt(100, 141)), Swipe::Down);
    EXPECT_EQ(classify_swipe(o, pt(100, 59)), Swipe::Up);
    // Not past kSwipeMinPx.
    EXPECT_EQ(classify_swipe(o, pt(140, 100)), Swipe::None);
    // Too diagonal: less than kSwipeDominance times across.
    EXPECT_EQ(classify_swipe(o, pt(150, 130)), Swipe::None);
    EXPECT_EQ(classify_swipe(o, pt(150, 125)), Swipe::Right);
    EXPECT_EQ(classify_swipe(o, o), Swipe::None);
}

TEST(Gestures, NavigationTable) {
    EXPECT_EQ(navigate(Page::Home, Swipe::Right, false, true), Page::Settings);
    EXPECT_EQ(navigate(Page::Home, Swipe::Left, false, true), Page::Scenes);
    EXPECT_EQ(navigate(Page::Home, Swipe::Down, false, true), Page::History);
    EXPECT_EQ(navigate(Page::Home, Swipe::Up, false, true), std::nullopt);
    EXPECT_EQ(navigate(Page::Home, Swipe::None, false, true), std::nullopt);
    EXPECT_EQ(navigate(Page::Settings, Swipe::Left, false, true), Page::Home);
    EXPECT_EQ(navigate(Page::Settings, Swipe::Right, false, true), std::nullopt);
    EXPECT_EQ(navigate(Page::Scenes, Swipe::Right, false, true), Page::Home);
    EXPECT_EQ(navigate(Page::Scenes, Swipe::Left, false, true), std::nullopt);
    // The scene list scrolls up and down; that is not a way back.
    EXPECT_EQ(navigate(Page::Scenes, Swipe::Up, false, true), std::nullopt);
    EXPECT_EQ(navigate(Page::Scenes, Swipe::Down, false, true), std::nullopt);
}

TEST(Gestures, TopZoneDownIsThePanels) {
    EXPECT_EQ(navigate(Page::Home, Swipe::Down, true, true), std::nullopt);
    // Only the down swipe belongs to the panel.
    EXPECT_EQ(navigate(Page::Home, Swipe::Right, true, true), Page::Settings);
}

TEST(Gestures, HistoryUpOnlyAtBottomEnd) {
    EXPECT_EQ(navigate(Page::History, Swipe::Up, false, true), Page::Home);
    EXPECT_EQ(navigate(Page::History, Swipe::Up, false, false), std::nullopt);
    EXPECT_EQ(navigate(Page::History, Swipe::Down, false, true), std::nullopt);
    EXPECT_EQ(navigate(Page::History, Swipe::Left, false, true), std::nullopt);
}

TEST(WispPressTest, TapHoldCancel) {
    constexpr uint64_t kHold = wisp::voice::kHoldStartMs;
    {
        // Let go after 200 ms: a tap.
        WispPress p;
        EXPECT_EQ(p.pressed(pt(50, 50), 1000), Action::None);
        EXPECT_EQ(p.tick(1200), Action::None);
        EXPECT_EQ(p.released(pt(52, 51), 1200), Action::Tap);
        EXPECT_FALSE(p.held());
    }
    {
        // Held 300 ms: the hold begins, and the release ends it.
        WispPress p;
        p.pressed(pt(50, 50), 1000);
        EXPECT_EQ(p.tick(1000 + kHold - 1), Action::None);
        EXPECT_EQ(p.moved(pt(55, 52), 1000 + kHold), Action::HoldBegin);
        EXPECT_TRUE(p.holding());
        EXPECT_EQ(p.tick(1000 + kHold + 50), Action::None);
        EXPECT_EQ(p.released(pt(55, 52), 2000), Action::HoldEnd);
        EXPECT_FALSE(p.holding());
        EXPECT_TRUE(p.held());
    }
    {
        // Holding, then up 41 px: cancelled; the release does nothing more.
        WispPress p;
        p.pressed(pt(50, 100), 1000);
        EXPECT_EQ(p.tick(1000 + kHold), Action::HoldBegin);
        EXPECT_EQ(p.moved(pt(50, 60), 1400), Action::None);
        EXPECT_EQ(p.moved(pt(50, 59), 1410), Action::HoldCancel);
        EXPECT_EQ(p.released(pt(50, 59), 1500), Action::None);
    }
    {
        // Moved away before the hold: a swipe, neither tap nor hold.
        WispPress p;
        p.pressed(pt(50, 50), 1000);
        EXPECT_EQ(p.moved(pt(100, 50), 1100), Action::None);
        EXPECT_EQ(p.tick(1000 + kHold), Action::None);
        EXPECT_EQ(p.released(pt(100, 50), 1500), Action::None);
    }
    {
        // The press taken away while holding cancels the hold.
        WispPress p;
        p.pressed(pt(50, 50), 1000);
        p.tick(1000 + kHold);
        EXPECT_EQ(p.lost(), Action::HoldCancel);
        EXPECT_EQ(p.lost(), Action::None);
    }
}

// --- DragTrack -------------------------------------------------------------

using wisp::shell::DragTrack;

// The direction comes once, on the move that passes the slop, along the
// axis the finger moved more on.
TEST(DragTrack, SlopGivesTheDirectionOnce) {
    DragTrack t;
    t.press(pt(100, 100), 0);
    EXPECT_EQ(t.moved(pt(105, 104), 5), Swipe::None);
    EXPECT_EQ(t.moved(pt(100 + wisp::shell::kDragSlopPx, 106), 10), Swipe::Right);
    EXPECT_EQ(t.moved(pt(140, 106), 15), Swipe::None);
    EXPECT_FALSE(t.dragging());

    t.press(pt(100, 100), 0);
    EXPECT_EQ(t.moved(pt(94, 88), 5), Swipe::Up);
    t.press(pt(100, 100), 0);
    // A tie is vertical, as LVGL decides it.
    EXPECT_EQ(t.moved(pt(110, 110), 5), Swipe::Down);
    t.press(pt(100, 100), 0);
    EXPECT_EQ(t.moved(pt(80, 95), 5), Swipe::Left);
}

// From start() on: the distance along the direction, the off-axis part
// ignored, clamped to [0, extent].
TEST(DragTrack, OffsetFollowsAlongTheAxisOnly) {
    DragTrack t;
    t.press(pt(100, 200), 0);
    ASSERT_EQ(t.moved(pt(90, 200), 10), Swipe::Left);
    t.start(Swipe::Left, 480);
    EXPECT_TRUE(t.dragging());
    t.moved(pt(40, 260), 20);
    EXPECT_EQ(t.offset(), 60);
    t.moved(pt(130, 200), 30);  // back past the start
    EXPECT_EQ(t.offset(), 0);
    t.press(pt(10, 10), 0);
    ASSERT_EQ(t.moved(pt(10, 30), 10), Swipe::Down);
    t.start(Swipe::Down, 300);
    t.moved(pt(-50, 900), 20);
    EXPECT_EQ(t.offset(), 300);
}

// Slow (well under kFlingPxPerS): a third of the extent decides.
TEST(DragTrack, AThirdCompletesWhenSlow) {
    DragTrack t;
    auto slow_to = [&](int32_t x_end) {
        t.press(pt(0, 100), 0);
        uint64_t now = 0;
        int32_t x = 0;
        t.moved(pt(10, 100), now += 100);
        t.start(Swipe::Right, 480);
        while (x < x_end) {
            x = std::min(x + 5, x_end);
            t.moved(pt(x, 100), now += 100);  // 0.05 px/ms
        }
        return t.release(pt(x, 100), now + 100);
    };
    EXPECT_FALSE(slow_to(159));
    EXPECT_TRUE(slow_to(160));
    EXPECT_FALSE(t.dragging());
}

// Short but fast completes; fast the wrong way, or after a pause, does not.
TEST(DragTrack, FlingCompletes) {
    DragTrack t;
    t.press(pt(300, 100), 0);
    t.moved(pt(290, 100), 10);
    t.start(Swipe::Left, 480);
    t.moved(pt(280, 100), 20);
    t.moved(pt(260, 100), 60);  // 20 px in 40 ms: 0.5 px/ms
    EXPECT_TRUE(t.release(pt(260, 100), 60));

    t.press(pt(300, 100), 0);
    t.moved(pt(290, 100), 10);
    t.start(Swipe::Left, 480);
    t.moved(pt(280, 100), 20);
    t.moved(pt(270, 100), 60);  // 10 px in 40 ms: 0.25 px/ms
    EXPECT_FALSE(t.release(pt(270, 100), 60));

    // Out to 100 px, then back fast.
    t.press(pt(300, 100), 0);
    t.moved(pt(290, 100), 10);
    t.start(Swipe::Left, 480);
    t.moved(pt(200, 100), 300);
    t.moved(pt(240, 100), 320);
    EXPECT_FALSE(t.release(pt(240, 100), 330));

    // Fast, then held still past the window before the release.
    t.press(pt(300, 100), 0);
    t.moved(pt(290, 100), 10);
    t.start(Swipe::Left, 480);
    t.moved(pt(250, 100), 20);
    t.moved(pt(250, 100), 100);
    EXPECT_FALSE(t.release(pt(250, 100), 120));
}

// Never started: no completion.
TEST(DragTrack, ReleaseWithoutDragDoesNothing) {
    DragTrack t;
    t.press(pt(0, 0), 0);
    t.moved(pt(300, 0), 1);
    EXPECT_FALSE(t.release(pt(300, 0), 2));
}
