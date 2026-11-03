// Unit tests for the pure disconnect-reason mapping used by
// wisp::port::esp::EspWifi (see wisp/port/esp/wifi_reason.hpp). Compiles
// on the host without any ESP-IDF include; the numeric reason codes are
// checked against IDF's wifi_err_reason_t by static_asserts in
// net/esp_wifi.cpp.

#include "wisp/port/esp/wifi_reason.hpp"

#include <gtest/gtest.h>

#include <vector>

using wisp::hal::WifiFailReason;
using wisp::hal::WifiScanEntry;
using wisp::port::esp::fail_reason_for;
using wisp::port::esp::is_own_leave;
using wisp::port::esp::kScanRssiMaxAgeMs;
using wisp::port::esp::scan_rssi_for;

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

// With no reading in the event, a recent scan of the same network stands
// in for it: a wrong password at a strong signal must still be reported.
TEST(WifiReason, RecentScanStandsInForAMissingReading) {
    EXPECT_EQ(fail_reason_for(15, 0, kStrong), WifiFailReason::AuthFailed);
    EXPECT_EQ(fail_reason_for(202, 0, -75), WifiFailReason::AuthFailed);
    EXPECT_EQ(fail_reason_for(204, 0, kWeak), WifiFailReason::Other);
    // Nothing known either way keeps the safe answer.
    EXPECT_EQ(fail_reason_for(15, 0, 0), WifiFailReason::Other);
}

// The event's own reading wins over an older scan.
TEST(WifiReason, EventReadingBeatsTheScan) {
    EXPECT_EQ(fail_reason_for(15, kWeak, kStrong), WifiFailReason::Other);
    EXPECT_EQ(fail_reason_for(15, kStrong, kWeak), WifiFailReason::AuthFailed);
}

TEST(WifiReason, ScanRssiIsTheStrongestMatchingEntry) {
    const std::vector<WifiScanEntry> scan = {
        {"home", -80, true}, {"cafe", -40, true}, {"home", -55, true}, {"Home", -30, true}};
    EXPECT_EQ(scan_rssi_for(scan, "home", 1000), -55);
    EXPECT_EQ(scan_rssi_for(scan, "cafe", 0), -40);
    // SSIDs are bytes: no case folding.
    EXPECT_EQ(scan_rssi_for(scan, "HOME", 0), 0);
    EXPECT_EQ(scan_rssi_for(scan, "office", 0), 0);
    EXPECT_EQ(scan_rssi_for({}, "home", 0), 0);
}

// A reading from long ago says nothing about the link now.
TEST(WifiReason, StaleScanIsIgnored) {
    const std::vector<WifiScanEntry> scan = {{"home", -50, true}};
    EXPECT_EQ(scan_rssi_for(scan, "home", kScanRssiMaxAgeMs), -50);
    EXPECT_EQ(scan_rssi_for(scan, "home", kScanRssiMaxAgeMs + 1), 0);
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
