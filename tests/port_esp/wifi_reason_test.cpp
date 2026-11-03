// Unit tests for the pure disconnect-reason mapping used by
// wisp::port::esp::EspWifi (see wisp/port/esp/wifi_reason.hpp). Compiles
// on the host without any ESP-IDF include; the numeric reason codes are
// checked against IDF's wifi_err_reason_t by static_asserts in
// net/esp_wifi.cpp.

#include "wisp/port/esp/wifi_reason.hpp"

#include <gtest/gtest.h>

using wisp::hal::WifiFailReason;
using wisp::port::esp::fail_reason_for;
using wisp::port::esp::is_own_leave;

namespace {

constexpr int kStrong = -50;
constexpr int kWeak = -85;

}  // namespace

TEST(WifiReason, NoApFoundVariantsAreNotFound) {
    for (uint8_t reason : {201, 210, 211, 212}) {
        EXPECT_EQ(fail_reason_for(reason, kStrong), WifiFailReason::NotFound) << int(reason);
        EXPECT_EQ(fail_reason_for(reason, kWeak), WifiFailReason::NotFound) << int(reason);
    }
}

TEST(WifiReason, EnterpriseRejectionIsAuthFailedAtAnySignal) {
    EXPECT_EQ(fail_reason_for(23, kStrong), WifiFailReason::AuthFailed);
    EXPECT_EQ(fail_reason_for(23, kWeak), WifiFailReason::AuthFailed);
}

// A wrong WPA2 password shows up as a handshake timeout; so does a frame
// lost on a weak link, which must not park the network.
TEST(WifiReason, HandshakeTimeoutsAreAuthFailedOnlyWithGoodSignal) {
    for (uint8_t reason : {15, 202, 204}) {
        EXPECT_EQ(fail_reason_for(reason, kStrong), WifiFailReason::AuthFailed) << int(reason);
        EXPECT_EQ(fail_reason_for(reason, -75), WifiFailReason::AuthFailed) << int(reason);
        EXPECT_EQ(fail_reason_for(reason, -76), WifiFailReason::Other) << int(reason);
        EXPECT_EQ(fail_reason_for(reason, kWeak), WifiFailReason::Other) << int(reason);
    }
}

// The driver reports 0 when it never heard the access point.
TEST(WifiReason, UnknownSignalIsNotGood) {
    EXPECT_EQ(fail_reason_for(15, 0), WifiFailReason::Other);
    EXPECT_EQ(fail_reason_for(204, 0), WifiFailReason::Other);
}

TEST(WifiReason, EverythingElseIsOther) {
    for (uint8_t reason : {1, 2, 8, 14, 200, 203, 205, 39}) {
        EXPECT_EQ(fail_reason_for(reason, kStrong), WifiFailReason::Other) << int(reason);
    }
}

TEST(WifiReason, OwnLeaveIsAssocLeaveOnly) {
    EXPECT_TRUE(is_own_leave(8));
    EXPECT_FALSE(is_own_leave(3));
    EXPECT_FALSE(is_own_leave(202));
}
