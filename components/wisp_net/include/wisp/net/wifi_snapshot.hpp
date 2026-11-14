#pragma once

// What WifiService's two state cells hold (spec 3.4 of the state-cells
// design), and the mirrors that turn a change of them into the events
// net.wifi.state, net.wifi.enabled and net.ap.state, with the payloads
// those events have always carried (docs/architecture.md, event topics).

#include "wisp/base/fixed_string.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/event_bus.hpp"

namespace wisp::net {

enum class WifiState { Off, Connecting, Backoff, Connected };

// "off" / "connecting" / "backoff" / "connected".
const char* to_string(WifiState s);

// Event::source of the three events. Not a device id: boards choose those,
// and a mirror is a plain function that cannot carry one.
inline constexpr char kWifiServiceSource[] = "wifi_service";

// The station as last announced. Fields as WifiStatus's.
struct WifiSnapshot {
    bool enabled = false;  // the user's setting, not whether the radio is on
    WifiState state = WifiState::Off;
    FixedString<32> ssid;    // raw bytes, as WifiStatus::ssid
    FixedString<15> ip;      // dotted IPv4; empty unless Connected
    int rssi = 0;            // rssi at the last cell change, not live
    FixedString<11> reason;  // "", "not_found", "auth_failed", "other"
    bool operator==(const WifiSnapshot&) const = default;
};

// The provisioning hotspot as last announced. A closed hotspot keeps the
// name it had, as net.ap.state's "ssid" does.
struct ApSnapshot {
    bool active = false;
    FixedString<32> ssid;
    int clients = 0;  // 0 while closed
    bool operator==(const ApSnapshot&) const = default;
};

// {state, ssid, ssid_hex, ip, rssi, reason}: ssid_hex is the lower-case hex
// of the raw SSID bytes, for clients that cannot carry non-UTF-8 in JSON.
Properties wifi_state_payload(const WifiSnapshot& s);
// {enabled}
Properties wifi_enabled_payload(const WifiSnapshot& s);
// {active, ssid, clients}
Properties ap_state_payload(const ApSnapshot& s);

// The Wi-Fi cell's mirror: net.wifi.state when state, ssid, ip or reason
// differ (not rssi alone), then net.wifi.enabled when enabled differs.
void wifi_mirror(core::EventBus& bus, const WifiSnapshot& before, const WifiSnapshot& after);
// The hotspot cell's mirror: net.ap.state on any change.
void ap_mirror(core::EventBus& bus, const ApSnapshot& before, const ApSnapshot& after);

}  // namespace wisp::net
