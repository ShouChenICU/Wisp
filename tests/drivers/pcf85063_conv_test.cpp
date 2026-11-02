// Unit tests for the pure BCD/register conversion helpers ported from
// MyOS's hal_rtc.cpp into drivers/pcf85063/include/pcf85063_conv.h (see
// that header's file comment and docs/references.md). These compile on
// the host without driver/i2c_master.h -- see tests/CMakeLists.txt's
// pcf85063_conv_test target, which adds drivers/pcf85063/include to this
// translation unit's include path.

#include "pcf85063_conv.h"

#include <gtest/gtest.h>

#include <cstring>

TEST(Pcf85063Conv, BcdRoundTrip) {
    for (uint8_t v = 0; v <= 59; v++) {
        EXPECT_EQ(pcf85063_bcd_to_dec(pcf85063_dec_to_bcd(v)), v) << "v=" << (int)v;
    }
}

TEST(Pcf85063Conv, DecToBcdKnownValues) {
    EXPECT_EQ(pcf85063_dec_to_bcd(0), 0x00);
    EXPECT_EQ(pcf85063_dec_to_bcd(9), 0x09);
    EXPECT_EQ(pcf85063_dec_to_bcd(10), 0x10);
    EXPECT_EQ(pcf85063_dec_to_bcd(59), 0x59);
}

TEST(Pcf85063Conv, EncodeDecodeRoundTrip) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_sec = 45;
    t.tm_min = 30;
    t.tm_hour = 12;
    t.tm_mday = 2;
    t.tm_wday = 1;    // Monday
    t.tm_mon = 10;    // November (0-based)
    t.tm_year = 126;  // 2026

    uint8_t regs[7] = {0};
    pcf85063_encode(&t, regs);

    struct tm decoded;
    memset(&decoded, 0, sizeof(decoded));
    ASSERT_TRUE(pcf85063_decode(regs, &decoded));
    EXPECT_EQ(decoded.tm_sec, 45);
    EXPECT_EQ(decoded.tm_min, 30);
    EXPECT_EQ(decoded.tm_hour, 12);
    EXPECT_EQ(decoded.tm_mday, 2);
    EXPECT_EQ(decoded.tm_wday, 1);
    EXPECT_EQ(decoded.tm_mon, 10);
    EXPECT_EQ(decoded.tm_year, 126);
}

TEST(Pcf85063Conv, DecodeFailsWhenOscillatorStopFlagSet) {
    uint8_t regs[7] = {0};
    regs[0] = PCF85063_SECONDS_OS;  // OS flag set, rest zeroed.

    struct tm decoded;
    memset(&decoded, 0, sizeof(decoded));
    EXPECT_FALSE(pcf85063_decode(regs, &decoded));
}

TEST(Pcf85063Conv, YearPlausibility) {
    EXPECT_FALSE(pcf85063_year_plausible(2000));
    EXPECT_FALSE(pcf85063_year_plausible(2023));
    EXPECT_TRUE(pcf85063_year_plausible(2024));
    EXPECT_TRUE(pcf85063_year_plausible(2099));
}
