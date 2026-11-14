#include "wisp/net/wifi_snapshot.hpp"

#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "wisp/base/value.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/topics.hpp"

using wisp::Properties;
using wisp::Value;
using wisp::core::Event;
using wisp::core::EventBus;
using wisp::net::ApSnapshot;
using wisp::net::WifiSnapshot;
using wisp::net::WifiState;

namespace {

static_assert(std::is_trivially_copyable_v<WifiSnapshot>);
static_assert(std::is_trivially_copyable_v<ApSnapshot>);

// A started bus recording every "net." event, in publish order.
struct BusRecorder {
    BusRecorder() {
        bus.start();
        bus.subscribe("net.", [this](const Event& e) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(e);
        });
    }
    ~BusRecorder() { bus.stop(); }

    std::vector<Event> take() {
        bus.wait_idle(2000);
        std::lock_guard<std::mutex> lock(mutex);
        return std::exchange(events, {});
    }

    EventBus bus;
    std::mutex mutex;
    std::vector<Event> events;
};

WifiSnapshot connected_home() {
    WifiSnapshot s;
    s.enabled = true;
    s.state = WifiState::Connected;
    s.ssid.assign("Home");
    s.ip.assign("192.168.1.50");
    s.rssi = -50;
    return s;
}

}  // namespace

// The keys and value types net.wifi.state, net.wifi.enabled and
// net.ap.state carry on the wire (see tests/net/wifi_forwarded_test.cpp).
TEST(WifiSnapshot, PayloadsMatchTheDocumentedShape) {
    WifiSnapshot s;
    s.enabled = true;
    s.state = WifiState::Backoff;
    s.ssid.assign(std::string("Caf\xff\x00x", 6));
    s.reason.assign("auth_failed");
    EXPECT_EQ(wisp::net::wifi_state_payload(s),
              (Properties{{"state", Value("backoff")},
                          {"ssid", Value(std::string("Caf\xff\x00x", 6))},
                          {"ssid_hex", Value("436166ff0078")},
                          {"ip", Value("")},
                          {"rssi", Value(0)},
                          {"reason", Value("auth_failed")}}));

    const WifiSnapshot c = connected_home();
    const Properties p = wisp::net::wifi_state_payload(c);
    EXPECT_EQ(p, (Properties{{"state", Value("connected")},
                             {"ssid", Value("Home")},
                             {"ssid_hex", Value("486f6d65")},
                             {"ip", Value("192.168.1.50")},
                             {"rssi", Value(-50)},
                             {"reason", Value("")}}));
    EXPECT_EQ(p.at("rssi").type(), Value::Type::Int);
    EXPECT_EQ(p.at("reason").type(), Value::Type::String);

    EXPECT_EQ(wisp::net::wifi_enabled_payload(c), (Properties{{"enabled", Value(true)}}));
    EXPECT_EQ(wisp::net::wifi_enabled_payload(WifiSnapshot{}),
              (Properties{{"enabled", Value(false)}}));

    ApSnapshot ap;
    ap.active = true;
    ap.ssid.assign("Wisp-3F2A");
    ap.clients = 2;
    EXPECT_EQ(wisp::net::ap_state_payload(ap),
              (Properties{{"active", Value(true)}, {"ssid", Value("Wisp-3F2A")}, {"clients", 2}}));
    EXPECT_EQ(wisp::net::ap_state_payload(ApSnapshot{}),
              (Properties{{"active", Value(false)}, {"ssid", Value("")}, {"clients", 0}}));
}

// A 32-byte SSID (the most a network may have) is kept whole.
TEST(WifiSnapshot, KeepsAThirtyTwoByteSsid) {
    const std::string longest(32, 'x');
    WifiSnapshot s;
    EXPECT_TRUE(s.ssid.assign(longest));
    EXPECT_EQ(wisp::net::wifi_state_payload(s).at("ssid").as_string(), longest);
    std::string hex;
    for (int i = 0; i < 32; ++i) hex += "78";
    EXPECT_EQ(wisp::net::wifi_state_payload(s).at("ssid_hex").as_string(), hex);
}

// State first, then the flag, from one change; rssi alone publishes nothing.
TEST(WifiSnapshot, WifiMirrorPublishesStateThenEnabled) {
    BusRecorder r;
    const WifiSnapshot before = connected_home();
    WifiSnapshot after;  // off, disabled
    wisp::net::wifi_mirror(r.bus, before, after);
    auto events = r.take();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].topic, wisp::core::topics::kNetWifiState);
    EXPECT_EQ(events[0].source, wisp::net::kWifiServiceSource);
    EXPECT_EQ(events[0].payload, wisp::net::wifi_state_payload(after));
    EXPECT_EQ(events[1].topic, wisp::core::topics::kNetWifiEnabled);
    EXPECT_EQ(events[1].source, wisp::net::kWifiServiceSource);
    EXPECT_EQ(events[1].payload, (Properties{{"enabled", Value(false)}}));

    WifiSnapshot rssi_only = before;
    rssi_only.rssi = -70;
    wisp::net::wifi_mirror(r.bus, before, rssi_only);
    EXPECT_TRUE(r.take().empty());

    WifiSnapshot flag_only = WifiSnapshot{};
    flag_only.enabled = true;
    wisp::net::wifi_mirror(r.bus, WifiSnapshot{}, flag_only);
    events = r.take();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].topic, wisp::core::topics::kNetWifiEnabled);

    // Each of the four fields alone is a state change.
    for (int field = 0; field < 4; ++field) {
        WifiSnapshot moved = before;
        if (field == 0) moved.state = WifiState::Connecting;
        if (field == 1) moved.ssid.assign("Other");
        if (field == 2) moved.ip.assign("10.0.0.2");
        if (field == 3) moved.reason.assign("other");
        wisp::net::wifi_mirror(r.bus, before, moved);
        events = r.take();
        ASSERT_EQ(events.size(), 1u) << field;
        EXPECT_EQ(events[0].topic, wisp::core::topics::kNetWifiState) << field;
    }
}

TEST(WifiSnapshot, ApMirrorPublishesAnyChange) {
    BusRecorder r;
    ApSnapshot open;
    open.active = true;
    open.ssid.assign("Wisp-3F2A");
    wisp::net::ap_mirror(r.bus, ApSnapshot{}, open);
    ApSnapshot joined = open;
    joined.clients = 1;
    wisp::net::ap_mirror(r.bus, open, joined);
    auto events = r.take();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].topic, wisp::core::topics::kNetApState);
    EXPECT_EQ(events[0].source, wisp::net::kWifiServiceSource);
    EXPECT_EQ(events[0].payload, wisp::net::ap_state_payload(open));
    EXPECT_EQ(events[1].payload, wisp::net::ap_state_payload(joined));
}
