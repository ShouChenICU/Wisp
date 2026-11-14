#include "wisp/net/wifi_snapshot.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "wisp/base/sha256.hpp"
#include "wisp/core/topics.hpp"

namespace wisp::net {

namespace {

void publish(core::EventBus& bus, const char* topic, Properties payload) {
    core::Event e;
    e.topic = topic;
    e.source = kWifiServiceSource;
    e.payload = std::move(payload);
    bus.publish(std::move(e));
}

}  // namespace

// The value types are the ones WifiService published before the cells:
// strings, rssi and clients as int, the flags as bool.
Properties wifi_state_payload(const WifiSnapshot& s) {
    const auto* ssid_bytes = reinterpret_cast<const uint8_t*>(s.ssid.view().data());
    return Properties{{"state", to_string(s.state)},
                      {"ssid", s.ssid.str()},
                      {"ssid_hex", to_hex({ssid_bytes, s.ssid.size()})},
                      {"ip", s.ip.str()},
                      {"rssi", s.rssi},
                      {"reason", s.reason.str()}};
}

Properties wifi_enabled_payload(const WifiSnapshot& s) {
    return Properties{{"enabled", s.enabled}};
}

Properties ap_state_payload(const ApSnapshot& s) {
    return Properties{{"active", s.active}, {"ssid", s.ssid.str()}, {"clients", s.clients}};
}

// Reproduces WifiService's two station events: set_state_locked() published
// net.wifi.state only when state, ssid, ip or reason moved (an rssi change
// alone was silent), and set_enabled() published net.wifi.enabled after the
// state change its radio switch caused.
void wifi_mirror(core::EventBus& bus, const WifiSnapshot& before, const WifiSnapshot& after) {
    if (after.state != before.state || after.ssid != before.ssid || after.ip != before.ip ||
        after.reason != before.reason) {
        publish(bus, core::topics::kNetWifiState, wifi_state_payload(after));
    }
    if (after.enabled != before.enabled) {
        publish(bus, core::topics::kNetWifiEnabled, wifi_enabled_payload(after));
    }
}

// Reproduces WifiService's net.ap.state: every open, close and change of
// the client count.
void ap_mirror(core::EventBus& bus, const ApSnapshot& before, const ApSnapshot& after) {
    if (after == before) return;
    publish(bus, core::topics::kNetApState, ap_state_payload(after));
}

}  // namespace wisp::net
