// Unit tests for the pure interface classification used by
// wisp::port::esp::EspHttpServer (see wisp/port/esp/net_iface.hpp).
// Compiles on the host without any ESP-IDF or lwIP include.

#include "wisp/port/esp/net_iface.hpp"

#include <gtest/gtest.h>

using wisp::net::Iface;
using wisp::port::esp::iface_for;
using wisp::port::esp::ipv4;
using wisp::port::esp::LocalAddresses;

namespace {

const uint32_t kApIp = ipv4(192, 168, 4, 1);
const uint32_t kApMask = ipv4(255, 255, 255, 0);
const uint32_t kStaIp = ipv4(192, 168, 1, 88);
const uint32_t kPhone = ipv4(192, 168, 4, 2);
const uint32_t kLaptop = ipv4(192, 168, 1, 20);

const LocalAddresses kBoth{kApIp, kApMask, kStaIp};

}  // namespace

TEST(NetIface, Ipv4PacksHostOrder) { EXPECT_EQ(ipv4(192, 168, 4, 1), 0xC0A80401u); }

TEST(NetIface, HotspotClientIsAp) {
    EXPECT_EQ(iface_for(kApIp, kPhone, kBoth), Iface::Ap);
    EXPECT_EQ(iface_for(kApIp, kPhone, {kApIp, kApMask, 0}), Iface::Ap);
}

TEST(NetIface, StationClientIsSta) { EXPECT_EQ(iface_for(kStaIp, kLaptop, kBoth), Iface::Sta); }

// lwIP accepts a packet for any of its addresses on any interface, so a
// LAN host could address the hotspot's IP through the station link; its
// own address gives it away.
TEST(NetIface, HotspotAddressFromOutsideTheHotspotSubnetIsSta) {
    EXPECT_EQ(iface_for(kApIp, kLaptop, kBoth), Iface::Sta);
}

// A home network that happens to use the hotspot's subnet: only requests
// addressed to the hotspot's own address count.
TEST(NetIface, StationInTheHotspotSubnetIsSta) {
    const uint32_t sta = ipv4(192, 168, 4, 23);
    const uint32_t lan_peer = ipv4(192, 168, 4, 50);
    EXPECT_EQ(iface_for(sta, lan_peer, {kApIp, kApMask, sta}), Iface::Sta);
}

// Should the station ever hold the hotspot's address, nothing can tell
// the two apart; the stricter answer wins.
TEST(NetIface, ClashWithStationAddressIsSta) {
    EXPECT_EQ(iface_for(kApIp, kPhone, {kApIp, kApMask, kApIp}), Iface::Sta);
}

TEST(NetIface, UnknownAddressesAreSta) {
    EXPECT_EQ(iface_for(0, kPhone, kBoth), Iface::Sta);
    EXPECT_EQ(iface_for(kApIp, kPhone, {0, 0, 0}), Iface::Sta);
    EXPECT_EQ(iface_for(kApIp, kPhone, {kApIp, 0, kStaIp}), Iface::Sta);
    EXPECT_EQ(iface_for(ipv4(10, 0, 0, 2), kPhone, {0, 0, kStaIp}), Iface::Sta);
}

TEST(NetIface, LoopbackIsLoopback) {
    EXPECT_EQ(iface_for(ipv4(127, 0, 0, 1), ipv4(127, 0, 0, 1), kBoth), Iface::Loopback);
    EXPECT_EQ(iface_for(ipv4(127, 1, 2, 3), ipv4(127, 0, 0, 1), kBoth), Iface::Loopback);
}
