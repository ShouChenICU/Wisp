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
