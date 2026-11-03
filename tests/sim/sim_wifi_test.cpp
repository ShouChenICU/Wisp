#include "wisp/port/sim/sim_wifi.hpp"

#include <gtest/gtest.h>

#include <string>

#include "wisp/hal/wifi.hpp"

using wisp::hal::WifiFailReason;
using wisp::hal::WifiLinkState;
using wisp::port::sim::SimWifi;

TEST(SimWifi, DescribesItself) {
    SimWifi wifi("net.wifi");
    EXPECT_EQ(wifi.info().id, "net.wifi");
    EXPECT_EQ(wifi.info().cls, wisp::hal::DeviceClass::Network);
    EXPECT_EQ(wifi.info().model, "sim-wifi");
    EXPECT_TRUE(wifi.info().caps.at("ap").as_bool());
}

TEST(SimWifi, ScanListsTheBuiltInNetworks) {
    SimWifi wifi("net.wifi");
    auto scan = wifi.scan();
    ASSERT_TRUE(scan);
    ASSERT_EQ(scan.value().size(), 3u);
    EXPECT_EQ(scan.value()[0].ssid, "WispLab");
    EXPECT_EQ(scan.value()[0].rssi, -45);
    EXPECT_TRUE(scan.value()[0].secured);
    EXPECT_EQ(scan.value()[1].ssid, "Neighbor");
    EXPECT_EQ(scan.value()[1].rssi, -70);
    EXPECT_EQ(scan.value()[2].ssid, "OpenCafe");
    EXPECT_EQ(scan.value()[2].rssi, -80);
    EXPECT_FALSE(scan.value()[2].secured);
}

TEST(SimWifi, ConnectsWithRightPasswordOnly) {
    SimWifi wifi("net.wifi");
    ASSERT_TRUE(wifi.connect("WispLab", "wrong-pass"));
    auto link = wifi.link();
    EXPECT_EQ(link.state, WifiLinkState::Failed);
    EXPECT_EQ(link.reason, WifiFailReason::AuthFailed);

    ASSERT_TRUE(wifi.connect("WispLab", "wisp1234"));
    link = wifi.link();
    EXPECT_EQ(link.state, WifiLinkState::Connected);
    EXPECT_EQ(link.ssid, "WispLab");
    EXPECT_EQ(link.rssi, -45);
    EXPECT_FALSE(link.ip.empty());

    ASSERT_TRUE(wifi.connect("Neighbor", "neighbor99"));
    EXPECT_EQ(wifi.link().state, WifiLinkState::Connected);

    ASSERT_TRUE(wifi.connect("OpenCafe", ""));
    EXPECT_EQ(wifi.link().state, WifiLinkState::Connected);

    ASSERT_TRUE(wifi.connect("Elsewhere", "whatever1"));
    link = wifi.link();
    EXPECT_EQ(link.state, WifiLinkState::Failed);
    EXPECT_EQ(link.reason, WifiFailReason::NotFound);

    ASSERT_TRUE(wifi.disconnect());
    EXPECT_EQ(wifi.link().state, WifiLinkState::Idle);
}

TEST(SimWifi, WifiFailMakesEveryConnectFail) {
    SimWifi wifi("net.wifi", /*fail_all=*/true);
    ASSERT_TRUE(wifi.connect("WispLab", "wisp1234"));
    auto link = wifi.link();
    EXPECT_EQ(link.state, WifiLinkState::Failed);
    EXPECT_EQ(link.reason, WifiFailReason::NotFound);
}

TEST(SimWifi, HotspotOnlyChangesState) {
    SimWifi wifi("net.wifi");
    EXPECT_EQ(wifi.ap_clients(), 0);
    auto ip = wifi.start_ap("Wisp-3F2A", "abcd1234");
    ASSERT_TRUE(ip);
    EXPECT_EQ(ip.value(), "192.168.4.1");
    // A phone is pretended to have joined, so the idle timeout never fires.
    EXPECT_EQ(wifi.ap_clients(), 1);
    ASSERT_TRUE(wifi.stop_ap());
    EXPECT_EQ(wifi.ap_clients(), 0);
}
