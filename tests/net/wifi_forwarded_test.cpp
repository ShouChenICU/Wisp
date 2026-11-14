// The forwarded frames (support/forwarded_recorder.hpp) of the Wi-Fi and
// pairing paths that depend on time, pinned where time is an argument:
// WifiService and PairingService driven with explicit now_ms values, so no
// retry or expiry can fire on its own. app/forwarded_stream_test.cpp pins
// the paths an App drives; this file pins the failures, the power-level
// round trip, forget_all() and code expiry, which there would race a timer.
//
// Every publish below happens inside the call that the test makes (or the
// tick() it passes a time to), on this thread, so each list is in program
// order.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/fake_wifi.hpp"
#include "support/forwarded_recorder.hpp"
#include "support/temp_dir.hpp"
#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/hal/device.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/net/wifi_service.hpp"

using wisp::core::DeviceRegistry;
using wisp::core::EventBus;
using wisp::core::StorageManager;
using wisp::hal::PowerLevel;
using wisp::net::PairingConfig;
using wisp::net::PairingService;
using wisp::net::WifiService;
using wisp::test::FakeStorage;
using wisp::test::FakeWifi;
using wisp::test::ForwardedRecorder;

namespace {

using Frames = std::vector<std::string>;

constexpr char kConnectingHome[] =
    R"({"payload":{"ip":"","reason":"","rssi":0,"ssid":"Home","ssid_hex":"486f6d65",)"
    R"("state":"connecting"},"t":"event","topic":"net.wifi.state"})";
constexpr char kConnectingAny[] =
    R"({"payload":{"ip":"","reason":"","rssi":0,"ssid":"","ssid_hex":"",)"
    R"("state":"connecting"},"t":"event","topic":"net.wifi.state"})";
constexpr char kConnectedHome[] =
    R"({"payload":{"ip":"192.168.1.50","reason":"","rssi":-50,"ssid":"Home",)"
    R"("ssid_hex":"486f6d65","state":"connected"},"t":"event","topic":"net.wifi.state"})";
constexpr char kOff[] = R"({"payload":{"ip":"","reason":"","rssi":0,"ssid":"","ssid_hex":"",)"
                        R"("state":"off"},"t":"event","topic":"net.wifi.state"})";
constexpr char kEnabledFalse[] =
    R"({"payload":{"enabled":false},"t":"event","topic":"net.wifi.enabled"})";
constexpr char kEnabledTrue[] =
    R"({"payload":{"enabled":true},"t":"event","topic":"net.wifi.enabled"})";

// A StorageManager over a temp dir, a FakeWifi with "Home" (WPA, rssi -50)
// in range, a started bus and a recorder on it (the pattern of
// net/wifi_service_test.cpp's fixture).
struct WifiFixture {
    WifiFixture() : dir(wisp::test::temp_dir().string()) {
        registry.add(std::make_shared<FakeStorage>("storage.int", dir, /*removable=*/false));
        storage = std::make_unique<StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        wifi = std::make_shared<FakeWifi>("net.wifi");
        wifi->networks = {{"Home", -50, true}};
        wifi->passwords = {{"Home", "password1"}};
        EXPECT_TRUE(bus.start());
        svc = std::make_unique<WifiService>(wifi, *storage, bus, [](uint8_t* out, size_t len) {
            for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(i * 13 + 5);
        });
    }

    ~WifiFixture() {
        svc.reset();
        rec.reset();
        bus.stop();
    }

    // Writes net/wifi.json as a previous boot would have left it.
    void seed(const std::vector<std::pair<std::string, std::string>>& nets) {
        wisp::List list;
        for (const auto& [ssid, pw] : nets) {
            list.push_back(wisp::Map{{"ssid", ssid}, {"password", pw}});
        }
        const std::string json = wisp::to_json(wisp::Map{{"networks", list}});
        ASSERT_TRUE(storage->write_atomic(dir + "/system/net/wifi.json",
                                          std::vector<uint8_t>(json.begin(), json.end())));
    }

    // What was published since the last call.
    Frames frames() {
        EXPECT_TRUE(bus.wait_idle(2000));
        return rec->take();
    }

    // Boots with "Home" saved and runs the first attempt: scan and connect
    // on one tick, the link reported on the next.
    void connect_home() {
        seed({{"Home", "password1"}});
        ASSERT_TRUE(svc->init(1000));
        svc->tick(1000);
        svc->tick(1001);
        ASSERT_EQ(frames(), (Frames{kConnectingAny, kConnectedHome}));
    }

    std::string dir;
    DeviceRegistry registry;
    EventBus bus;
    std::unique_ptr<ForwardedRecorder> rec = std::make_unique<ForwardedRecorder>(bus);
    std::unique_ptr<StorageManager> storage;
    std::shared_ptr<FakeWifi> wifi;
    std::unique_ptr<WifiService> svc;
};

}  // namespace

// add_network() announces the direct attempt (wifi_service.cpp,
// try_directly_locked()); the next tick connects, the one after reads the
// rejection and backs off (fail_attempt_locked()).
TEST(ForwardedStream, WifiWrongPasswordBacksOff) {
    WifiFixture f;
    ASSERT_TRUE(f.svc->init(1000));
    ASSERT_TRUE(f.svc->add_network("Home", "wrong-password", 1000));
    f.svc->tick(1000);
    f.svc->tick(1001);
    EXPECT_EQ(f.frames(),
              (Frames{kConnectingHome,
                      R"({"payload":{"ip":"","reason":"auth_failed","rssi":0,"ssid":"Home",)"
                      R"("ssid_hex":"486f6d65","state":"backoff"},"t":"event",)"
                      R"("topic":"net.wifi.state"})"}));
}

// A saved network that the scan does not find: init() announces the
// attempt with no SSID, the scan's empty match backs off naming none
// (finish_attempt_locked(): in_flight_ssid_ is the plan's empty SSID).
TEST(ForwardedStream, WifiNetworkNotInRangeBacksOff) {
    WifiFixture f;
    f.seed({{"Away", "password1"}});
    ASSERT_TRUE(f.svc->init(1000));
    f.svc->tick(1000);
    EXPECT_EQ(f.frames(), (Frames{kConnectingAny,
                                  R"({"payload":{"ip":"","reason":"not_found","rssi":0,"ssid":"",)"
                                  R"("ssid_hex":"","state":"backoff"},"t":"event",)"
                                  R"("topic":"net.wifi.state"})"}));
}

// The driver refuses connect() (start_connect_locked()): a backoff with
// reason "other", naming the network tried.
TEST(ForwardedStream, WifiConnectErrorBacksOff) {
    WifiFixture f;
    ASSERT_TRUE(f.svc->init(1000));
    ASSERT_TRUE(f.svc->add_network("Home", "password1", 1000));
    f.wifi->radio_on.store(false);  // the fake's connect() now fails
    f.svc->tick(1000);
    EXPECT_EQ(f.frames(), (Frames{kConnectingHome,
                                  R"({"payload":{"ip":"","reason":"other","rssi":0,"ssid":"Home",)"
                                  R"("ssid_hex":"486f6d65","state":"backoff"},"t":"event",)"
                                  R"("topic":"net.wifi.state"})"}));
}

// Deep standby turns the radio off and back on (apply_radio_locked()): the
// state moves, the user's enable flag does not, so no net.wifi.enabled.
TEST(ForwardedStream, WifiPowerLevelRoundTripKeepsTheFlag) {
    WifiFixture f;
    f.connect_home();
    f.svc->set_power_level(PowerLevel::DeepStandby, 2000);
    EXPECT_EQ(f.frames(), (Frames{kOff}));
    f.svc->set_power_level(PowerLevel::Active, 3000);
    f.svc->tick(3000);
    f.svc->tick(3001);
    EXPECT_EQ(f.frames(), (Frames{kConnectingAny, kConnectedHome}));
}

// With nothing saved the radio has nothing to do, so set_enabled() publishes
// the flag alone: the state stays "off" (set_state_locked() publishes only
// a change).
TEST(ForwardedStream, WifiEnableWithNothingSavedPublishesOnlyTheFlag) {
    WifiFixture f;
    ASSERT_TRUE(f.svc->init(1000));
    ASSERT_TRUE(f.svc->set_enabled(false, 1000));
    EXPECT_EQ(f.frames(), (Frames{kEnabledFalse}));
    ASSERT_TRUE(f.svc->set_enabled(true, 2000));
    EXPECT_EQ(f.frames(), (Frames{kEnabledTrue}));
}

// forget_all() (factory reset only) turns Wi-Fi back on without announcing
// the flag, and with nothing left to join the state stays "off": nothing.
TEST(ForwardedStream, WifiForgetAllWhileDisabledPublishesNothing) {
    WifiFixture f;
    f.seed({{"Home", "password1"}});
    ASSERT_TRUE(f.svc->init(1000));
    ASSERT_TRUE(f.svc->set_enabled(false, 1000));
    EXPECT_EQ(f.frames(), (Frames{kConnectingAny, kOff, kEnabledFalse}));
    f.svc->forget_all();
    EXPECT_EQ(f.frames(), Frames{});
}

// forget_all() while connected with the hotspot open: the hotspot closes
// (close_ap_locked()), then the state goes to "off"; still no flag.
TEST(ForwardedStream, WifiForgetAllWhileConnectedClosesTheHotspot) {
    WifiFixture f;
    f.connect_home();
    ASSERT_TRUE(f.svc->start_provisioning(2000));
    EXPECT_EQ(f.frames(), (Frames{R"({"payload":{"active":true,"clients":0,"ssid":"Wisp-3F2A"},)"
                                  R"("t":"event","topic":"net.ap.state"})"}));
    f.svc->forget_all();
    EXPECT_EQ(f.frames(), (Frames{R"({"payload":{"active":false,"clients":0,"ssid":"Wisp-3F2A"},)"
                                  R"("t":"event","topic":"net.ap.state"})",
                                  kOff}));
}

// A code that lapses is announced when the service next sees the time
// (pairing_service.cpp, observe_locked() -> clear_code_locked()), here from
// the request that replaces it.
TEST(ForwardedStream, PairingCodeExpiryIsAnnounced) {
    std::string dir = wisp::test::temp_dir().string();
    DeviceRegistry registry;
    EventBus bus;
    ForwardedRecorder rec(bus);
    registry.add(std::make_shared<FakeStorage>("storage.int", dir, /*removable=*/false));
    StorageManager storage(registry, bus);
    ASSERT_TRUE(storage.init());
    ASSERT_TRUE(bus.start());
    const PairingConfig config;
    PairingService pairing(
        storage, bus,
        [](uint8_t* out, size_t len) {
            for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(i * 29 + 1);
        },
        [] { return int64_t{1767225600}; }, config);
    ASSERT_TRUE(pairing.init());
    ASSERT_TRUE(pairing.request_code(1000));
    ASSERT_TRUE(pairing.request_code(1000 + config.code_ttl_ms + 1));
    ASSERT_TRUE(bus.wait_idle(2000));
    EXPECT_EQ(
        rec.take(),
        (Frames{
            R"({"payload":{"active":true,"expires_ms":"<now+ttl>"},"t":"event","topic":"net.pairing.code"})",
            R"({"payload":{"active":false,"expires_ms":0},"t":"event","topic":"net.pairing.code"})",
            R"({"payload":{"active":true,"expires_ms":"<now+ttl>"},"t":"event","topic":"net.pairing.code"})"}));
    bus.stop();
}
