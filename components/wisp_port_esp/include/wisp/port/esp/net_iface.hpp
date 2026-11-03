#pragma once

// Pure, platform-free classification of the interface an incoming HTTP
// request arrived on, used by wisp::port::esp::EspHttpServer
// (net/esp_http_server.cpp). Header-only and free of IDF/lwIP includes so
// it is unit-tested on the host (tests/port_esp/net_iface_test.cpp).
//
// The decision uses the connection's LOCAL address (the address the
// client connected to), never anything the client sends: a request is
// from the hotspot exactly when it was addressed to the hotspot's own
// address. Comparing whole subnets instead would misfile a home network
// that happens to use the hotspot's 192.168.4.0/24 as the hotspot, and
// with it open the hotspot-only provisioning routes to the LAN.

#include <cstdint>

#include "wisp/net/http.hpp"

namespace wisp::port::esp {

// IPv4 address in host byte order: ipv4(192, 168, 4, 1) == 0xC0A80401.
constexpr uint32_t ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(c) << 8) | static_cast<uint32_t>(d);
}

// All addresses in host byte order; 0 means unknown/none. `ap_ip` and
// `sta_ip` are the hotspot's and the station's current addresses.
inline net::Iface iface_for_local_address(uint32_t local, uint32_t ap_ip, uint32_t sta_ip) {
    if ((local >> 24) == 127) return net::Iface::Loopback;
    // Should the station ever hold the hotspot's address too, the two
    // cannot be told apart; the stricter answer wins.
    if (local != 0 && local == ap_ip && local != sta_ip) return net::Iface::Ap;
    return net::Iface::Sta;
}

}  // namespace wisp::port::esp
