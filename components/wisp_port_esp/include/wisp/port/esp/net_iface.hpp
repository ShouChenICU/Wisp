#pragma once

// Pure, platform-free classification of the interface an incoming HTTP
// request arrived on, used by wisp::port::esp::EspHttpServer
// (net/esp_http_server.cpp). Header-only and free of IDF/lwIP includes so
// it is unit-tested on the host (tests/port_esp/net_iface_test.cpp).
//
// Decided from the connection's addresses, never from anything the
// client sends. A request is from the hotspot only when both hold:
//   - it was addressed to the hotspot's own address (the local address).
//     Comparing subnets alone would misfile a home network that happens
//     to use the hotspot's 192.168.4.0/24, opening the hotspot-only
//     provisioning routes to the LAN;
//   - the client's address is inside the hotspot's subnet. lwIP accepts a
//     packet for any of the device's addresses on any interface, so a LAN
//     host could otherwise reach the hotspot address over the station
//     link.

#include <cstdint>

#include "wisp/net/http.hpp"

namespace wisp::port::esp {

// IPv4 address in host byte order: ipv4(192, 168, 4, 1) == 0xC0A80401.
constexpr uint32_t ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(c) << 8) | static_cast<uint32_t>(d);
}

// The device's own addresses, host byte order; 0 means none.
struct LocalAddresses {
    uint32_t ap_ip = 0;
    uint32_t ap_mask = 0;
    uint32_t sta_ip = 0;
};

// `local` is the address the client connected to, `peer` the client's.
inline net::Iface iface_for(uint32_t local, uint32_t peer, const LocalAddresses& own) {
    if ((local >> 24) == 127) return net::Iface::Loopback;
    if (local == 0 || own.ap_ip == 0 || own.ap_mask == 0) return net::Iface::Sta;
    // Should the station ever hold the hotspot's address too, the two
    // cannot be told apart; the stricter answer wins.
    if (local != own.ap_ip || local == own.sta_ip) return net::Iface::Sta;
    if ((peer & own.ap_mask) != (own.ap_ip & own.ap_mask)) return net::Iface::Sta;
    return net::Iface::Ap;
}

}  // namespace wisp::port::esp
