// Unit tests for the pure conversion helpers ported from MyOS's
// hal_power.cpp into drivers/axp2101/include/axp2101_conv.h (see that
// header's file comment and docs/references.md). These compile on the
// host without driver/i2c_master.h -- see tests/CMakeLists.txt's
// axp2101_conv_test target, which adds drivers/axp2101/include to this
// translation unit's include path.

#include "axp2101_conv.h"

#include <gtest/gtest.h>

TEST(Axp2101Conv, NormalizeBatteryPctPassesThroughValidValues) {
    EXPECT_EQ(axp2101_normalize_battery_pct(0), 0);
    EXPECT_EQ(axp2101_normalize_battery_pct(57), 57);
    EXPECT_EQ(axp2101_normalize_battery_pct(100), 100);
}

TEST(Axp2101Conv, NormalizeBatteryPctMasksExtraStatusBit) {
    // raw > 100 with the low 7 bits forming a valid percentage: the extra
    // bit (e.g. a "charging"/status flag some firmware revisions pack
    // into bit 7) is masked off.
    EXPECT_EQ(axp2101_normalize_battery_pct(0x80 | 42), 42);
    EXPECT_EQ(axp2101_normalize_battery_pct(0x80 | 0), 0);
}

TEST(Axp2101Conv, NormalizeBatteryPctClampsImplausibleMaskedValue) {
    // Masking still leaves > 100: clamp rather than report nonsense.
    EXPECT_EQ(axp2101_normalize_battery_pct(0xFF), 100);
}

TEST(Axp2101Conv, CombineH5L8UsesLow5BitsOfHigh) {
    // high=0x1F (all 5 valid bits set), low=0xFF -> 0x1FFF.
    EXPECT_EQ(axp2101_combine_h5l8(0x1F, 0xFF), 0x1FFF);
    // High bits above the 5 valid ones are ignored.
    EXPECT_EQ(axp2101_combine_h5l8(0xFF, 0x00), 0x1F00);
    EXPECT_EQ(axp2101_combine_h5l8(0x00, 0x00), 0);
}

// These four encoders are written against XPowersLib, not MyOS's
// original REG 0x62/0x63/0x64 encodings, which were found to be wrong
// (see axp2101_conv.h's file comment). Expected values
// below are taken directly from XPowersLib's xpowers_axp2101_chg_curr_t/
// chg_iterm_t/chg_vol_t enums.

TEST(Axp2101Conv, IccCodeStepsBy25maBelow200) {
    EXPECT_EQ(axp2101_icc_code(0), 0);
    EXPECT_EQ(axp2101_icc_code(100), 4);
    EXPECT_EQ(axp2101_icc_code(200), 8);
}

TEST(Axp2101Conv, IccCodeStepsBy100maAbove200) {
    EXPECT_EQ(axp2101_icc_code(300), 9);
    EXPECT_EQ(axp2101_icc_code(400), 0x0A);
    EXPECT_EQ(axp2101_icc_code(1000), 0x10);
}

TEST(Axp2101Conv, IccCodeClampsAboveMax) { EXPECT_EQ(axp2101_icc_code(5000), 0x10); }

TEST(Axp2101Conv, ItermCodeStepsBy25ma) {
    EXPECT_EQ(axp2101_iterm_code(0), 0);
    EXPECT_EQ(axp2101_iterm_code(25), 1);
    EXPECT_EQ(axp2101_iterm_code(200), 8);
}

TEST(Axp2101Conv, ItermCodeClampsAboveMax) { EXPECT_EQ(axp2101_iterm_code(1000), 8); }

TEST(Axp2101Conv, CvCodeMatchesXPowersLibThresholds) {
    EXPECT_EQ(axp2101_cv_code(4000), 1);
    EXPECT_EQ(axp2101_cv_code(4100), 2);
    EXPECT_EQ(axp2101_cv_code(4200), 3);
    EXPECT_EQ(axp2101_cv_code(4350), 4);
    EXPECT_EQ(axp2101_cv_code(4400), 5);
}

TEST(Axp2101Conv, CvCodeClampsAboveMax) { EXPECT_EQ(axp2101_cv_code(5000), 5); }

TEST(Axp2101Conv, IprechgCodeStepsBy25ma) {
    EXPECT_EQ(axp2101_iprechg_code(0), 0);
    EXPECT_EQ(axp2101_iprechg_code(50), 2);
    EXPECT_EQ(axp2101_iprechg_code(375), 15);
}

TEST(Axp2101Conv, IprechgCodeClampsAboveMax) { EXPECT_EQ(axp2101_iprechg_code(1000), 15); }

// A failed REG_BAT_PCT (0xA4) read must never be reported as a present
// battery with battery_percent == 0 -- that looks exactly like a real
// empty battery and would spuriously fire wisp::core::PowerMonitor's
// power.battery.low. Downgrading axp2101_status_t::battery_present to
// false on that failure routes it through the same "no battery" path
// wisp::port::esp::EspPower::status() already maps to
// PowerStatus::battery_percent == -1 (unknown), rather than leaving
// battery_percent at whatever axp2101_read_status() memset it to (0).
TEST(Axp2101Conv, BatteryPresentAfterPctReadIsTrueOnlyWhenBothHold) {
    EXPECT_TRUE(axp2101_battery_present_after_pct_read(/*preliminary_present=*/true,
                                                       /*pct_read_ok=*/true));
}

TEST(Axp2101Conv, BatteryPresentAfterPctReadIsFalseWhenPctReadFails) {
    EXPECT_FALSE(axp2101_battery_present_after_pct_read(/*preliminary_present=*/true,
                                                        /*pct_read_ok=*/false));
}

TEST(Axp2101Conv, BatteryPresentAfterPctReadStaysFalseWhenNoBatteryWasWired) {
    // Whatever pct_read_ok is -- axp2101_read_status() never actually
    // attempts the PCT read in this case (returns early), but the
    // function's own logic should still be false || anything == false.
    EXPECT_FALSE(axp2101_battery_present_after_pct_read(/*preliminary_present=*/false,
                                                        /*pct_read_ok=*/true));
    EXPECT_FALSE(axp2101_battery_present_after_pct_read(/*preliminary_present=*/false,
                                                        /*pct_read_ok=*/false));
}

// axp2101_pwrkey_update(): the PWRON key's level rebuilt from the edge
// IRQ bits of REG 0x49, one call per poll.

TEST(Axp2101PwrkeyTracker, StartsReleased) {
    axp2101_pwrkey_tracker_t t = {};
    EXPECT_FALSE(axp2101_pwrkey_update(&t, 0));
    EXPECT_FALSE(axp2101_pwrkey_update(&t, 0));
}

TEST(Axp2101PwrkeyTracker, PressEdgeHoldsUntilReleaseEdge) {
    axp2101_pwrkey_tracker_t t = {};
    EXPECT_TRUE(axp2101_pwrkey_update(&t, AXP2101_IRQ2_PKEY_NEGATIVE));
    EXPECT_TRUE(axp2101_pwrkey_update(&t, 0));
    EXPECT_TRUE(axp2101_pwrkey_update(&t, AXP2101_IRQ2_PKEY_LONG));
    EXPECT_FALSE(axp2101_pwrkey_update(&t, AXP2101_IRQ2_PKEY_POSITIVE));
    EXPECT_FALSE(axp2101_pwrkey_update(&t, 0));
}

TEST(Axp2101PwrkeyTracker, PressAndReleaseInOneReadReportsOnePollDown) {
    axp2101_pwrkey_tracker_t t = {};
    EXPECT_TRUE(axp2101_pwrkey_update(
        &t, AXP2101_IRQ2_PKEY_NEGATIVE | AXP2101_IRQ2_PKEY_POSITIVE | AXP2101_IRQ2_PKEY_SHORT));
    EXPECT_FALSE(axp2101_pwrkey_update(&t, 0));
}

TEST(Axp2101PwrkeyTracker, ReleaseAndPressInOneReadWhileHeldStaysDown) {
    axp2101_pwrkey_tracker_t t = {};
    EXPECT_TRUE(axp2101_pwrkey_update(&t, AXP2101_IRQ2_PKEY_NEGATIVE));
    EXPECT_TRUE(axp2101_pwrkey_update(&t, AXP2101_IRQ2_PKEY_NEGATIVE | AXP2101_IRQ2_PKEY_POSITIVE));
    EXPECT_TRUE(axp2101_pwrkey_update(&t, 0));
    EXPECT_FALSE(axp2101_pwrkey_update(&t, AXP2101_IRQ2_PKEY_POSITIVE));
}

TEST(Axp2101PwrkeyTracker, StrayReleaseIsNotAPress) {
    // The power-on press is still held when the edges are cleared at
    // init, so its release arrives with no press before it.
    axp2101_pwrkey_tracker_t t = {};
    EXPECT_FALSE(axp2101_pwrkey_update(&t, AXP2101_IRQ2_PKEY_POSITIVE));
    EXPECT_FALSE(axp2101_pwrkey_update(&t, 0));
}

TEST(Axp2101PwrkeyTracker, ShortPressBitAloneIsAPulseUntilAnEdgeIsSeen) {
    axp2101_pwrkey_tracker_t t = {};
    EXPECT_TRUE(axp2101_pwrkey_update(&t, AXP2101_IRQ2_PKEY_SHORT));
    EXPECT_FALSE(axp2101_pwrkey_update(&t, 0));
}

TEST(Axp2101PwrkeyTracker, ShortPressBitIgnoredOnceEdgesWork) {
    axp2101_pwrkey_tracker_t t = {};
    EXPECT_TRUE(axp2101_pwrkey_update(&t, AXP2101_IRQ2_PKEY_NEGATIVE));
    EXPECT_FALSE(axp2101_pwrkey_update(&t, AXP2101_IRQ2_PKEY_POSITIVE));
    // A short-press bit latched a little after the release edge must not
    // become a second press.
    EXPECT_FALSE(axp2101_pwrkey_update(&t, AXP2101_IRQ2_PKEY_SHORT));
}

TEST(Axp2101PwrkeyTracker, LongPressBitAloneIsIgnored) {
    axp2101_pwrkey_tracker_t t = {};
    EXPECT_FALSE(axp2101_pwrkey_update(&t, AXP2101_IRQ2_PKEY_LONG));
}

TEST(Axp2101PwrkeyTracker, OtherBitsAreIgnored) {
    axp2101_pwrkey_tracker_t t = {};
    EXPECT_FALSE(axp2101_pwrkey_update(&t, 0xF0));
}
