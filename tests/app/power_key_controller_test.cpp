#include "wisp/app/power_key_controller.hpp"

#include <gtest/gtest.h>

using A = wisp::app::PowerKeyController::Action;

TEST(PowerKey, ShortTogglesScreen) {
    wisp::app::PowerKeyController c;
    EXPECT_EQ(c.on_short(), A::ScreenOff);
    EXPECT_FALSE(c.screen_on());
    EXPECT_EQ(c.on_short(), A::ScreenOn);
    EXPECT_TRUE(c.screen_on());
}

TEST(PowerKey, LongShowsConfirmOnlyOnce) {
    wisp::app::PowerKeyController c;
    EXPECT_EQ(c.on_long(), A::ShowConfirm);
    EXPECT_EQ(c.on_long(), A::None);
    EXPECT_TRUE(c.confirming());
    EXPECT_EQ(c.on_confirm(false), A::CancelConfirm);
    EXPECT_FALSE(c.confirming());
}

TEST(PowerKey, LongWhileScreenOffWakesAndConfirms) {
    wisp::app::PowerKeyController c;
    c.on_short();
    EXPECT_EQ(c.on_long(), A::ScreenOnAndConfirm);
    EXPECT_TRUE(c.screen_on());
    EXPECT_EQ(c.on_confirm(true), A::PowerOff);
}

TEST(PowerKey, ShortCancelsConfirm) {
    wisp::app::PowerKeyController c;
    c.on_long();
    EXPECT_EQ(c.on_short(), A::CancelConfirm);
    EXPECT_TRUE(c.screen_on());
}
