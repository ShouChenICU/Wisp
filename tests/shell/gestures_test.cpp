// The shell's gestures (wisp/shell/gestures.hpp): swipe classification, the
// navigation table and a press on the wisp.

#include "wisp/shell/gestures.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>

#include "lvgl_private.h"
#include "support/fake_devices.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/shell/limits.hpp"
#include "wisp/ui/drag.hpp"
#include "wisp/ui/ui_runtime.hpp"
#include "wisp/voice/limits.hpp"

using wisp::shell::classify_swipe;
using wisp::shell::navigate;
using wisp::shell::Page;
using wisp::shell::Swipe;
using wisp::shell::WispPress;
using Action = WispPress::Action;

namespace {

lv_point_t pt(int32_t x, int32_t y) { return lv_point_t{x, y}; }

using wisp::hal::ScreenShape;
using wisp::hal::ScreenShapeKind;

const ScreenShape k480{ScreenShapeKind::Rect, 480, 480, 0};
// The distances on the 480 px screen the coordinates below are for.
const int32_t kSwipe480 = wisp::shell::swipe_min_px(k480);
const int32_t kSlop480 = wisp::shell::drag_slop_px(k480);

}  // namespace

TEST(Gestures, FourDirectionsAndThreshold) {
    const lv_point_t o = pt(100, 100);
    const int32_t m = kSwipe480;
    ASSERT_EQ(m, 38);
    EXPECT_EQ(classify_swipe(o, pt(100 + m + 1, 100), m), Swipe::Right);
    EXPECT_EQ(classify_swipe(o, pt(100 - m - 1, 100), m), Swipe::Left);
    EXPECT_EQ(classify_swipe(o, pt(100, 100 + m + 1), m), Swipe::Down);
    EXPECT_EQ(classify_swipe(o, pt(100, 100 - m - 1), m), Swipe::Up);
    // Not past the least distance.
    EXPECT_EQ(classify_swipe(o, pt(100 + m, 100), m), Swipe::None);
    // Too diagonal: less than kSwipeDominance times across.
    EXPECT_EQ(classify_swipe(o, pt(150, 130), m), Swipe::None);
    EXPECT_EQ(classify_swipe(o, pt(150, 125), m), Swipe::Right);
    EXPECT_EQ(classify_swipe(o, o, m), Swipe::None);
}

// The swipe and the drag slop are shares of the screen's shorter side, and
// LVGL's own scroll limit (UiRuntime sets it) equals the drag slop, so a
// page drag and a scroll decide on the same touch read on every size.
TEST(Gestures, DistancesScaleWithTheScreen) {
    struct Case {
        int32_t w, h, swipe, slop;
    };
    for (const Case c : {Case{480, 480, 38, 10}, Case{466, 466, 37, 9}, Case{360, 480, 29, 7}}) {
        SCOPED_TRACE(std::to_string(c.w) + "x" + std::to_string(c.h));
        const ScreenShape s{ScreenShapeKind::Rect, c.w, c.h, 0};
        EXPECT_EQ(wisp::shell::swipe_min_px(s), c.swipe);
        EXPECT_EQ(wisp::shell::drag_slop_px(s), c.slop);
        EXPECT_EQ(classify_swipe(pt(0, 0), pt(c.swipe + 1, 0), c.swipe), Swipe::Right);
        EXPECT_EQ(classify_swipe(pt(0, 0), pt(c.swipe, 0), c.swipe), Swipe::None);

        auto disp = std::make_shared<wisp::test::FakeDisplay>("display.main", c.w, c.h, 2);
        auto touch = std::make_shared<wisp::test::FakeTouch>("touch.main");
        wisp::ui::UiRuntime ui(disp, touch);
        ASSERT_TRUE(ui.start());
        int32_t limit = -1;
        ui.run_blocking([&] {
            lv_indev_t* indev = lv_indev_get_next(nullptr);
            ASSERT_NE(indev, nullptr);
            limit = indev->scroll_limit;
        });
        EXPECT_EQ(limit, wisp::shell::drag_slop_px(s));
        ui.stop();
    }
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
        WispPress p(kSwipe480);
        EXPECT_EQ(p.pressed(pt(50, 50), 1000), Action::None);
        EXPECT_EQ(p.tick(1200), Action::None);
        EXPECT_EQ(p.released(pt(52, 51), 1200), Action::Tap);
        EXPECT_FALSE(p.held());
    }
    {
        // Held 300 ms: the hold begins, and the release ends it.
        WispPress p(kSwipe480);
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
        // Holding, then up past the swipe distance: cancelled; the
        // release does nothing more.
        WispPress p(kSwipe480);
        p.pressed(pt(50, 100), 1000);
        EXPECT_EQ(p.tick(1000 + kHold), Action::HoldBegin);
        EXPECT_EQ(p.moved(pt(50, 100 - kSwipe480), 1400), Action::None);
        EXPECT_EQ(p.moved(pt(50, 99 - kSwipe480), 1410), Action::HoldCancel);
        EXPECT_EQ(p.released(pt(50, 99 - kSwipe480), 1500), Action::None);
    }
    {
        // Moved away before the hold: a swipe, neither tap nor hold.
        WispPress p(kSwipe480);
        p.pressed(pt(50, 50), 1000);
        EXPECT_EQ(p.moved(pt(100, 50), 1100), Action::None);
        EXPECT_EQ(p.tick(1000 + kHold), Action::None);
        EXPECT_EQ(p.released(pt(100, 50), 1500), Action::None);
    }
    {
        // The press taken away while holding cancels the hold.
        WispPress p(kSwipe480);
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
    DragTrack t(kSlop480);
    t.press(pt(100, 100), 0);
    EXPECT_EQ(t.moved(pt(105, 104), 5), Swipe::None);
    EXPECT_EQ(t.moved(pt(100 + kSlop480, 106), 10), Swipe::Right);
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
    DragTrack t(kSlop480);
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
    DragTrack t(kSlop480);
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
    DragTrack t(kSlop480);
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
    DragTrack t(kSlop480);
    t.press(pt(0, 0), 0);
    t.moved(pt(300, 0), 1);
    EXPECT_FALSE(t.release(pt(300, 0), 2));
}
