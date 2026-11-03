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
// failed that way out of automatic selection until the user re-enters
// its password (connectivity spec 4.2). So only failures that really
// mean "wrong credentials" map to it. The driver cannot tell a rejected
// WPA2/WPA3 password from frames lost on a weak link -- both surface as
// an authentication failure or a handshake timeout -- so those count as
// AuthFailed only while the access point was heard clearly; on a weak
// signal they are Other and the network keeps being retried.

#include <cstdint>

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

inline hal::WifiFailReason fail_reason_for(uint8_t reason, int rssi) {
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
