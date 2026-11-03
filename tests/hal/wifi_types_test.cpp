#include <gtest/gtest.h>

#include "support/fake_wifi.hpp"
#include "wisp/hal/device.hpp"
#include "wisp/hal/wifi.hpp"

TEST(WifiTypes, NetworkClassRoundTrips) {
    EXPECT_EQ(wisp::hal::class_from_string("network"), wisp::hal::DeviceClass::Network);
    EXPECT_STREQ(wisp::hal::to_string(wisp::hal::DeviceClass::Network), "network");
    EXPECT_EQ(wisp::hal::Wifi::kClass, wisp::hal::DeviceClass::Network);
}

TEST(WifiTypes, Defaults) {
    wisp::hal::WifiLink link;
    EXPECT_EQ(link.state, wisp::hal::WifiLinkState::Idle);
    EXPECT_EQ(link.reason, wisp::hal::WifiFailReason::None);
    EXPECT_EQ(link.rssi, 0);
    wisp::hal::WifiScanEntry entry;
    EXPECT_TRUE(entry.secured);
}

// Later service tests rely on the fake's documented connect() outcomes.
TEST(FakeWifi, ConnectOutcomes) {
    using wisp::hal::WifiFailReason;
    using wisp::hal::WifiLinkState;
    wisp::test::FakeWifi wifi("net.wifi");
    wifi.networks = {{"home", -50, true}, {"cafe", -70, false}};
    wifi.passwords = {{"home", "secret"}};

    ASSERT_TRUE(wifi.connect("nowhere", "x"));
    EXPECT_EQ(wifi.link().reason, WifiFailReason::NotFound);
    ASSERT_TRUE(wifi.connect("home", "wrong"));
    EXPECT_EQ(wifi.link().state, WifiLinkState::Failed);
    EXPECT_EQ(wifi.link().reason, WifiFailReason::AuthFailed);
    ASSERT_TRUE(wifi.connect("cafe", ""));
    EXPECT_EQ(wifi.link().state, WifiLinkState::Connected);
    ASSERT_TRUE(wifi.connect("home", "secret"));
    auto link = wifi.link();
    EXPECT_EQ(link.state, WifiLinkState::Connected);
    EXPECT_EQ(link.ip, "192.168.1.50");
    EXPECT_EQ(link.rssi, -50);
    wifi.reachable = false;
    ASSERT_TRUE(wifi.connect("home", "secret"));
    EXPECT_EQ(wifi.link().reason, WifiFailReason::Other);
    EXPECT_EQ(wifi.connect_calls.load(), 5);

    auto ip = wifi.start_ap("Wisp-3F2A", "abcd1234");
    ASSERT_TRUE(ip);
    EXPECT_EQ(ip.value(), "192.168.4.1");
    EXPECT_TRUE(wifi.ap_active.load());
    ASSERT_TRUE(wifi.stop_ap());
    EXPECT_FALSE(wifi.ap_active.load());
    EXPECT_EQ(wifi.mac()[5], 0x2A);
}
