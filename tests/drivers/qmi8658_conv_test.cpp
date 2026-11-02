// Unit tests for drivers/qmi8658/include/qmi8658_conv.h. The raw-to-g
// accelerometer conversion is ported from MyOS's hal_imu.cpp; the
// raw-to-dps gyroscope conversion and the ODR lookup table are new (see
// that header's file comment and docs/references.md). These compile on
// the host without driver/i2c_master.h -- see tests/CMakeLists.txt's
// qmi8658_conv_test target, which adds drivers/qmi8658/include to this
// translation unit's include path.

#include "qmi8658_conv.h"

#include <gtest/gtest.h>

TEST(Qmi8658Conv, LeI16DecodesLowByteFirst) {
    uint8_t bytes[2] = {0x34, 0x12};
    EXPECT_EQ(qmi8658_le_i16(bytes), 0x1234);
}

TEST(Qmi8658Conv, LeI16DecodesNegativeValues) {
    uint8_t bytes[2] = {0xFF, 0xFF};
    EXPECT_EQ(qmi8658_le_i16(bytes), -1);
}

TEST(Qmi8658Conv, RawToGFullScale) {
    // +/-4g full scale: raw 32767 (max positive) is just under +4g.
    EXPECT_NEAR(qmi8658_raw_to_g(0, 4), 0.0f, 1e-6f);
    EXPECT_NEAR(qmi8658_raw_to_g(8192, 4), 1.0f, 1e-4f);  // 1g at +/-4g FS.
    EXPECT_NEAR(qmi8658_raw_to_g(-8192, 4), -1.0f, 1e-4f);
}

TEST(Qmi8658Conv, RawToDpsFullScale) {
    // +/-512dps full scale: 64 LSB/dps.
    EXPECT_NEAR(qmi8658_raw_to_dps(0, 512), 0.0f, 1e-6f);
    EXPECT_NEAR(qmi8658_raw_to_dps(64, 512), 1.0f, 1e-4f);
    EXPECT_NEAR(qmi8658_raw_to_dps(-6400, 512), -100.0f, 1e-3f);
}

TEST(Qmi8658Conv, OdrCodeExactMatches) {
    EXPECT_EQ(qmi8658_odr_code_for_hz(8000), 0);
    EXPECT_EQ(qmi8658_odr_code_for_hz(125), 6);
    EXPECT_EQ(qmi8658_odr_code_for_hz(31), 8);
}

TEST(Qmi8658Conv, OdrCodePicksNearestRate) {
    // 100Hz is closer to the 125Hz entry than to the 63Hz one.
    EXPECT_EQ(qmi8658_odr_code_for_hz(100), 6);
    // 1050Hz is closer to the 1000Hz entry than to the 2000Hz one.
    EXPECT_EQ(qmi8658_odr_code_for_hz(1050), 3);
}
