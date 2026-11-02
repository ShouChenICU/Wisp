#include "wisp/app/power_key_controller.hpp"

#include <gtest/gtest.h>

using A = wisp::app::PowerKeyController::Action;

// screen_on is passed in by the caller on every call (the single source
// of truth is PowerManager::standby(), not this class) -- these tests
// drive it explicitly to cover the same scenarios a self-tracking
// version would need, plus the case of the caller's state disagreeing
// with whatever this class would otherwise have guessed on its own.

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

TEST(PowerKey, ShortCancelsConfirmOnlyWhileScreenIsOn) {
    wisp::app::PowerKeyController c;
    c.on_long(/*screen_on=*/true);
    ASSERT_TRUE(c.confirming());
    // Cancelling a confirm dialog only wins once the screen is actually
    // on; while it's off, waking takes priority (see
    // ShortWhileScreenOffWakesEvenWithAConfirmDialogAlreadyOpen below --
    // a correction to this test's own previous
    // assumption that cancelling always won).
    EXPECT_EQ(c.on_short(/*screen_on=*/true), A::CancelConfirm);
}

// A confirm dialog left open when automatic standby
// turns the screen off must not swallow the next power-key press as a
// dialog decision -- spec 6.7's "待机中按任意键都会唤醒" requires that
// press to wake the screen first. The dialog itself is left open (not
// cancelled) so a UI can leave it exactly as the user last saw it.
TEST(PowerKey, ShortWhileScreenOffWakesEvenWithAConfirmDialogAlreadyOpen) {
    wisp::app::PowerKeyController c;
    c.on_long(/*screen_on=*/true);  // opens the confirm dialog.
    ASSERT_TRUE(c.confirming());

    EXPECT_EQ(c.on_short(/*screen_on=*/false), A::ScreenOn);
    EXPECT_TRUE(c.confirming());  // dialog is left open, not cancelled.

    // Once the screen really is back on, the next short press cancels
    // the dialog exactly as before.
    EXPECT_EQ(c.on_short(/*screen_on=*/true), A::CancelConfirm);
}

// Same wake-first priority for a long press -- it
// must not stack a second confirm dialog on top of one left open by
// automatic standby.
TEST(PowerKey, LongWhileScreenOffWithConfirmAlreadyOpenJustWakes) {
    wisp::app::PowerKeyController c;
    c.on_long(/*screen_on=*/true);  // opens the confirm dialog.
    ASSERT_TRUE(c.confirming());

    EXPECT_EQ(c.on_long(/*screen_on=*/false), A::ScreenOn);
    EXPECT_TRUE(c.confirming());
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
