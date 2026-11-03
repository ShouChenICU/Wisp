#pragma once

// Pure, platform-free mapping from an ESP-IDF station disconnect reason
// (wifi_err_reason_t, as carried by WIFI_EVENT_STA_DISCONNECTED) to the
// HAL's WifiFailReason, used by wisp::port::esp::EspWifi
// (net/esp_wifi.cpp). Header-only and free of IDF includes so it is
// unit-tested on the host (tests/port_esp/wifi_reason_test.cpp); the
// numeric codes below are static_asserted against IDF's enum in
// esp_wifi.cpp.
//
// AuthFailed has a lasting effect: WifiService takes a network that
// failed that way three times in a row out of automatic selection until
// the user re-enters its password (connectivity spec 4.2). So only
// failures that really mean "wrong credentials" map to it. The driver cannot tell a rejected
// WPA2/WPA3 password from frames lost on a weak link -- both surface as
// an authentication failure or a handshake timeout -- so those count as
// AuthFailed only while the access point was heard clearly; on a weak
// signal they are Other and the network keeps being retried. When the
// disconnect event carries no reading, a recent scan of the same network
// stands in for it (scan_rssi_for()); with neither, the answer is Other.

#include <cstdint>
#include <string_view>
#include <vector>

#include "wisp/hal/wifi.hpp"

namespace wisp::port::esp {

inline constexpr uint8_t kReasonAssocLeave = 8;
inline constexpr uint8_t kReason4wayHandshakeTimeout = 15;
inline constexpr uint8_t kReason8021xAuthFailed = 23;
inline constexpr uint8_t kReasonNoApFound = 201;
inline constexpr uint8_t kReasonAuthFail = 202;
inline constexpr uint8_t kReasonHandshakeTimeout = 204;
inline constexpr uint8_t kReasonNoApFoundCompatibleSecurity = 210;
inline constexpr uint8_t kReasonNoApFoundInAuthmodeThreshold = 211;
inline constexpr uint8_t kReasonNoApFoundInRssiThreshold = 212;

// Weakest RSSI (dBm) at which a failed handshake is still taken to mean a
// wrong password. 0 is what the driver reports when it has no reading.
inline constexpr int kAuthTrustRssiDbm = -75;

// How old a scan may be and still stand in for a missing reading. Long
// enough to cover picking a network from the list and typing its password.
// WifiService scans right before its automatic attempts, so in practice
// almost every attempt has a fresh scan: whenever the driver leaves the
// event's reading at 0, this fallback is what decides AuthFailed.
inline constexpr uint64_t kScanRssiMaxAgeMs = 120000;

// The strongest reading for `ssid` in a scan taken `age_ms` ago (the
// driver joins the strongest access point of a network), or 0 when the
// scan is too old or does not list it. SSIDs compare as bytes.
inline int scan_rssi_for(const std::vector<hal::WifiScanEntry>& scan, std::string_view ssid,
                         uint64_t age_ms) {
    if (age_ms > kScanRssiMaxAgeMs) return 0;
    int best = 0;
    for (const hal::WifiScanEntry& e : scan) {
        if (e.ssid != ssid || e.rssi >= 0) continue;
        if (best == 0 || e.rssi > best) best = e.rssi;
    }
    return best;
}

// `rssi` is the disconnect event's reading; `fallback_rssi` (e.g. from
// scan_rssi_for()) is used only when the event has none.
inline hal::WifiFailReason fail_reason_for(uint8_t reason, int rssi, int fallback_rssi = 0) {
    if (rssi == 0) rssi = fallback_rssi;
    switch (reason) {
        case kReasonNoApFound:
        case kReasonNoApFoundCompatibleSecurity:
        case kReasonNoApFoundInAuthmodeThreshold:
        case kReasonNoApFoundInRssiThreshold:
            return hal::WifiFailReason::NotFound;
        case kReason8021xAuthFailed:
            // The authentication server itself said no.
            return hal::WifiFailReason::AuthFailed;
        case kReasonAuthFail:
        case kReason4wayHandshakeTimeout:
        case kReasonHandshakeTimeout:
            return rssi < 0 && rssi >= kAuthTrustRssiDbm ? hal::WifiFailReason::AuthFailed
                                                         : hal::WifiFailReason::Other;
        default:
            return hal::WifiFailReason::Other;
    }
}

// esp_wifi_disconnect() reports its own disconnect with this reason. One
// that arrives while a newer attempt is already under way belongs to the
// attempt being replaced and must not be taken as the new one's outcome.
inline bool is_own_leave(uint8_t reason) { return reason == kReasonAssocLeave; }

}  // namespace wisp::port::esp
