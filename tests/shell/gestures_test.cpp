// The shell's gestures (wisp/shell/gestures.hpp): swipe classification, the
// navigation table and a press on the wisp.

#include "wisp/shell/gestures.hpp"

#include <gtest/gtest.h>

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
