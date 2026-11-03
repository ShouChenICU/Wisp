// Unit tests for the pure interface classification used by
// wisp::port::esp::EspHttpServer (see wisp/port/esp/net_iface.hpp).
// Compiles on the host without any ESP-IDF or lwIP include.

#include "wisp/port/esp/net_iface.hpp"

#include <gtest/gtest.h>

using wisp::net::Iface;
using wisp::port::esp::iface_for_local_address;
using wisp::port::esp::ipv4;

namespace {

const uint32_t kApIp = ipv4(192, 168, 4, 1);
const uint32_t kStaIp = ipv4(192, 168, 1, 88);

}  // namespace

TEST(NetIface, Ipv4PacksHostOrder) { EXPECT_EQ(ipv4(192, 168, 4, 1), 0xC0A80401u); }

TEST(NetIface, HotspotAddressIsAp) {
    EXPECT_EQ(iface_for_local_address(kApIp, kApIp, kStaIp), Iface::Ap);
    EXPECT_EQ(iface_for_local_address(kApIp, kApIp, 0), Iface::Ap);
}

TEST(NetIface, StationAddressIsSta) {
    EXPECT_EQ(iface_for_local_address(kStaIp, kApIp, kStaIp), Iface::Sta);
}

// Only the hotspot's own address counts, not its whole subnet: a home
// network that happens to use 192.168.4.0/24 must never reach the
// hotspot-only routes.
TEST(NetIface, SameSubnetOnTheStationIsSta) {
    const uint32_t sta_in_ap_subnet = ipv4(192, 168, 4, 23);
    EXPECT_EQ(iface_for_local_address(sta_in_ap_subnet, kApIp, sta_in_ap_subnet), Iface::Sta);
}

// Should the station ever be handed the hotspot's address, nothing can
// tell the two apart; the stricter answer wins.
TEST(NetIface, ClashWithStationAddressIsSta) {
    EXPECT_EQ(iface_for_local_address(kApIp, kApIp, kApIp), Iface::Sta);
}

TEST(NetIface, UnknownAddressesAreSta) {
    EXPECT_EQ(iface_for_local_address(0, kApIp, kStaIp), Iface::Sta);
    EXPECT_EQ(iface_for_local_address(0, 0, 0), Iface::Sta);
    EXPECT_EQ(iface_for_local_address(ipv4(10, 0, 0, 2), 0, kStaIp), Iface::Sta);
}

TEST(NetIface, LoopbackIsLoopback) {
    EXPECT_EQ(iface_for_local_address(ipv4(127, 0, 0, 1), kApIp, kStaIp), Iface::Loopback);
    EXPECT_EQ(iface_for_local_address(ipv4(127, 1, 2, 3), kApIp, kStaIp), Iface::Loopback);
}
