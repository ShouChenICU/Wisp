#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/hal/device.hpp"

namespace wisp::hal {

struct WifiScanEntry {
    std::string ssid;
    int rssi = 0;  // dBm
    bool secured = true;
};

enum class WifiLinkState { Idle, Connecting, Connected, Failed };

enum class WifiFailReason { None, NotFound, AuthFailed, Other };

// Snapshot of the station link. `ip` is dotted-quad and only meaningful
// in Connected; `reason` is only meaningful in Failed.
struct WifiLink {
    WifiLinkState state = WifiLinkState::Idle;
    std::string ssid;
    std::string ip;
    int rssi = 0;
    WifiFailReason reason = WifiFailReason::None;
};

// Station + provisioning-AP radio. Deliberately poll-based (no callbacks):
// connect() only starts an attempt, and the owning service observes the
// outcome through link() from its own periodic tick, so no driver thread
// ever calls back into service code.
//
// caps keys: "ap" (bool, true if start_ap() is supported), "bands" (list of
// strings, e.g. ["2.4GHz"]).
class Wifi : public Device {
public:
    static constexpr DeviceClass kClass = DeviceClass::Network;

    // Blocking; may take up to about 3 s.
    virtual Result<std::vector<WifiScanEntry>> scan() = 0;
    // Asynchronous: returns once the attempt has started; poll link() for
    // the outcome.
    virtual Result<void> connect(const std::string& ssid, const std::string& password) = 0;
    virtual Result<void> disconnect() = 0;
    virtual WifiLink link() = 0;
    // Starts the provisioning access point and returns its IP address.
    virtual Result<std::string> start_ap(const std::string& ssid, const std::string& password) = 0;
    virtual Result<void> stop_ap() = 0;
    // Number of stations currently associated with the access point.
    virtual int ap_clients() = 0;
    virtual std::array<uint8_t, 6> mac() = 0;
    virtual Result<void> set_power_save(bool on) = 0;
    // Turns the radio off (deep standby, scene-device spec 4.1) or back
    // on. Idempotent. While it is off the link is Idle and scan()/connect()
    // fail; the caller stops the hotspot and disconnects first. A radio
    // that cannot be switched answers Unsupported.
    virtual Result<void> set_radio(bool /*on*/) {
        return Result<void>::err(ErrorCode::Unsupported, "radio cannot be switched off");
    }
};

}  // namespace wisp::hal
