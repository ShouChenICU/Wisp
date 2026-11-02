#include "wisp/app/power_key_controller.hpp"

#include <gtest/gtest.h>

using A = wisp::app::PowerKeyController::Action;

// Task 18b review round 1 (C1): screen_on is now passed in by the caller
// on every call (the single source of truth is PowerManager::standby(),
// not this class) -- these tests drive it explicitly to cover the same
// scenarios the old self-tracking version covered, plus the
// previously-impossible-to-express case of the caller's state
// disagreeing with whatever this class would have guessed on its own.

TEST(PowerKey, ShortTogglesScreen) {
    wisp::app::PowerKeyController c;
    EXPECT_EQ(c.on_short(/*screen_on=*/true), A::ScreenOff);
    EXPECT_EQ(c.on_short(/*screen_on=*/false), A::ScreenOn);
}

TEST(PowerKey, LongShowsConfirmOnlyOnce) {
    wisp::app::PowerKeyController c;
    EXPECT_EQ(c.on_long(/*screen_on=*/true), A::ShowConfirm);
    EXPECT_EQ(c.on_long(/*screen_on=*/true), A::None);
    EXPECT_TRUE(c.confirming());
    EXPECT_EQ(c.on_confirm(false), A::CancelConfirm);
    EXPECT_FALSE(c.confirming());
}

TEST(PowerKey, LongWhileScreenOffWakesAndConfirms) {
    wisp::app::PowerKeyController c;
    EXPECT_EQ(c.on_long(/*screen_on=*/false), A::ScreenOnAndConfirm);
    EXPECT_EQ(c.on_confirm(true), A::PowerOff);
}

TEST(PowerKey, ShortCancelsConfirm) {
    wisp::app::PowerKeyController c;
    c.on_long(/*screen_on=*/true);
    // Even if the caller's live state already disagrees with what this
    // class last assumed (e.g. some *other* path turned the screen off
    // while the confirm dialog was up), cancelling a confirm dialog
    // always wins over the screen_on-dependent branches.
    EXPECT_EQ(c.on_short(/*screen_on=*/false), A::CancelConfirm);
}

// The scenario C1's fix makes possible to express at all: the caller's
// live screen state can differ from whatever this class would have
// tracked internally (e.g. woken by a different button, or by an
// automatic idle-timeout standby) -- on_short()/on_long() must always
// follow the state they're handed, not any state of their own.
TEST(PowerKey, FollowsWhateverScreenOnStateItIsHandedEachCall) {
    wisp::app::PowerKeyController c;
    EXPECT_EQ(c.on_short(/*screen_on=*/false), A::ScreenOn);
    // A second call handed the *same* screen_on=false (e.g. the wake
    // this class just "caused" hasn't actually taken effect yet from
    // the caller's point of view) still returns ScreenOn, not some
    // internally-toggled ScreenOff -- there is no hidden state to get
    // out of sync.
    EXPECT_EQ(c.on_short(/*screen_on=*/false), A::ScreenOn);
    EXPECT_EQ(c.on_short(/*screen_on=*/true), A::ScreenOff);
}
