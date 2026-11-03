#include "wisp/net/wifi_service.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/fake_wifi.hpp"
#include "support/temp_dir.hpp"
#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/port/sim/settings_json_file.hpp"

using wisp::ErrorCode;
using wisp::core::DeviceRegistry;
using wisp::core::Event;
using wisp::core::EventBus;
using wisp::core::Settings;
using wisp::core::StorageManager;
using wisp::hal::WifiLink;
using wisp::hal::WifiLinkState;
using wisp::net::ApSession;
using wisp::net::WifiService;
using wisp::net::WifiServiceConfig;
using wisp::net::WifiState;
using wisp::port::sim::JsonFileSettingsBackend;
using wisp::test::FakeStorage;
using wisp::test::FakeWifi;

namespace {

// A started bus collecting every "net."-prefixed event, a StorageManager
// over a temp dir, a JSON settings backend and a FakeWifi. Tests advance
// time by passing explicit now_ms values to the service.
struct Fixture {
    Fixture() : dir(wisp::test::temp_dir().string()) {
        registry.add(std::make_shared<FakeStorage>("storage.int", dir, /*removable=*/false));
        storage = std::make_unique<StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        backend = std::make_unique<JsonFileSettingsBackend>(dir + "/settings.json");
        wifi = std::make_shared<FakeWifi>("net.wifi");
        bus.start();
        bus.subscribe("net.", [this](const Event& e) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(e);
        });
    }

    ~Fixture() {
        svc.reset();
        bus.stop();
    }

    WifiService& make(WifiServiceConfig config = {}) {
        svc.reset();
        svc = std::make_unique<WifiService>(
            wifi, *storage, bus, Settings(*backend, "net"),
            [this](uint8_t* out, size_t len) {
                std::lock_guard<std::mutex> lock(mutex);
                for (size_t i = 0; i < len; ++i) {
                    if (!scripted_random.empty()) {
                        out[i] = scripted_random.front();
                        scripted_random.erase(scripted_random.begin());
                    } else {
                        out[i] = static_cast<uint8_t>(rng());
                    }
                }
            },
            config);
        return *svc;
    }

    // Puts a network in the fake radio's range. Empty password = open.
    void in_range(const std::string& ssid, int rssi, const std::string& password = "") {
        wifi->networks.push_back({ssid, rssi, !password.empty()});
        if (!password.empty()) {
            wifi->passwords[ssid] = password;
        }
    }

    std::string wifi_json_path() const { return dir + "/system/net/wifi.json"; }

    // Writes net/wifi.json directly, as a previous boot would have left it.
    void seed(const std::vector<std::pair<std::string, std::string>>& nets) {
        wisp::List list;
        for (const auto& [ssid, pw] : nets) {
            list.push_back(wisp::Map{{"ssid", ssid}, {"password", pw}});
        }
        std::string json = wisp::to_json(wisp::Map{{"networks", list}});
        ASSERT_TRUE(storage->write_atomic(wifi_json_path(),
                                          std::vector<uint8_t>(json.begin(), json.end())));
    }

    std::vector<Event> topic(const std::string& t) {
        bus.wait_idle(2000);
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<Event> out;
        for (const auto& e : events) {
            if (e.topic == t) out.push_back(e);
        }
        return out;
    }

    std::vector<Event> all_events() {
        bus.wait_idle(2000);
        std::lock_guard<std::mutex> lock(mutex);
        return events;
    }

    void clear_events() {
        bus.wait_idle(2000);
        std::lock_guard<std::mutex> lock(mutex);
        events.clear();
    }

    std::string dir;
    DeviceRegistry registry;
    EventBus bus;
    std::unique_ptr<StorageManager> storage;
    std::unique_ptr<JsonFileSettingsBackend> backend;
    std::shared_ptr<FakeWifi> wifi;
    std::unique_ptr<WifiService> svc;

    std::mutex mutex;
    std::vector<Event> events;
    std::mt19937 rng{12345};
    std::vector<uint8_t> scripted_random;
};

std::string str_of(const wisp::Properties& p, const char* key) {
    auto it = p.find(key);
    return it == p.end() ? std::string() : it->second.as_string();
}

// FakeWifi resolves connect() on the next link() poll, so ticking twice at
// the same timestamp runs "start attempt" and "observe outcome" without
// letting time pass in between -- keeps backoff intervals exact.
void tick2(WifiService& svc, uint64_t now) {
    svc.tick(now);
    svc.tick(now);
}

}  // namespace

TEST(WifiService, PicksStrongestSavedNetwork) {
    Fixture f;
    f.in_range("A", -70, "passwordA");
    f.in_range("B", -40, "passwordB");
    f.in_range("Stranger", -20);  // strongest, but not saved
    f.seed({{"A", "passwordA"}, {"B", "passwordB"}});
    auto& svc = f.make();

    ASSERT_TRUE(svc.init(0));
    EXPECT_EQ(svc.status().state, WifiState::Connecting);
    // init() never touches the radio; boot must not wait for a scan.
    EXPECT_EQ(f.wifi->scan_calls.load(), 0);
    EXPECT_EQ(f.wifi->connect_calls.load(), 0);

    svc.tick(0);
    EXPECT_EQ(f.wifi->scan_calls.load(), 1);
    EXPECT_EQ(f.wifi->connect_calls.load(), 1);
    EXPECT_EQ(f.wifi->link().ssid, "B");

    svc.tick(250);
    auto st = svc.status();
    EXPECT_EQ(st.state, WifiState::Connected);
    EXPECT_EQ(st.ssid, "B");
    EXPECT_EQ(st.ip, "192.168.1.50");
    EXPECT_EQ(st.rssi, -40);
    EXPECT_TRUE(f.wifi->power_save.load());
}

TEST(WifiService, BackoffDoublesAndCaps) {
    Fixture f;
    f.in_range("A", -50, "passwordA");
    f.wifi->reachable = false;
    f.seed({{"A", "passwordA"}});
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));

    std::vector<uint64_t> connect_times;
    int seen = 0;
    for (uint64_t now = 0; connect_times.size() < 11; now += 250) {
        tick2(svc, now);
        if (f.wifi->connect_calls.load() != seen) {
            seen = f.wifi->connect_calls.load();
            connect_times.push_back(now);
        }
        if (connect_times.size() == 3) {
            EXPECT_EQ(svc.status().state, WifiState::Backoff);
        }
        ASSERT_LT(now, 5'000'000u);
    }

    std::vector<uint64_t> expected = {5000,   10000,  20000,  40000,  80000,
                                      160000, 300000, 300000, 300000, 300000};
    std::vector<uint64_t> intervals;
    for (size_t i = 1; i < connect_times.size(); ++i) {
        intervals.push_back(connect_times[i] - connect_times[i - 1]);
    }
    EXPECT_EQ(intervals, expected);
    // Each retry announces exactly one connecting and one backoff.
    auto states = f.topic("net.wifi.state");
    ASSERT_GE(states.size(), 4u);
    EXPECT_EQ(str_of(states[0].payload, "state"), "connecting");
    EXPECT_EQ(str_of(states[1].payload, "state"), "backoff");
    EXPECT_EQ(str_of(states[2].payload, "state"), "connecting");
    EXPECT_EQ(str_of(states[3].payload, "state"), "backoff");
}

TEST(WifiService, NoSavedNetworkInRangeCountsAsFailure) {
    Fixture f;
    f.in_range("Other", -50);
    f.seed({{"Away", "passwordX"}});
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));

    tick2(svc, 0);
    EXPECT_EQ(f.wifi->scan_calls.load(), 1);
    EXPECT_EQ(f.wifi->connect_calls.load(), 0);
    EXPECT_EQ(svc.status().state, WifiState::Backoff);

    tick2(svc, 4999);
    EXPECT_EQ(f.wifi->scan_calls.load(), 1);
    tick2(svc, 5000);
    EXPECT_EQ(f.wifi->scan_calls.load(), 2);
    tick2(svc, 14999);
    EXPECT_EQ(f.wifi->scan_calls.load(), 2);
    tick2(svc, 15000);
    EXPECT_EQ(f.wifi->scan_calls.load(), 3);
}

TEST(WifiService, ResetsBackoffAfterConnect) {
    Fixture f;
    f.in_range("A", -50, "passwordA");
    f.wifi->reachable = false;
    f.seed({{"A", "passwordA"}});
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));

    tick2(svc, 0);      // attempt 1 fails -> wait 5 s
    tick2(svc, 5000);   // attempt 2 fails -> wait 10 s
    tick2(svc, 14999);  // still waiting
    EXPECT_EQ(f.wifi->connect_calls.load(), 2);

    f.wifi->reachable = true;
    tick2(svc, 15000);
    EXPECT_EQ(f.wifi->connect_calls.load(), 3);
    EXPECT_EQ(svc.status().state, WifiState::Connected);

    // Drop, and the immediate reconnect fails: the wait is back to 5 s.
    f.wifi->reachable = false;
    f.wifi->set_link(WifiLink{});
    tick2(svc, 16000);
    EXPECT_EQ(f.wifi->connect_calls.load(), 4);
    EXPECT_EQ(svc.status().state, WifiState::Backoff);
    tick2(svc, 20999);
    EXPECT_EQ(f.wifi->connect_calls.load(), 4);
    tick2(svc, 21000);
    EXPECT_EQ(f.wifi->connect_calls.load(), 5);
}

TEST(WifiService, DropFromConnectedReconnectsImmediately) {
    Fixture f;
    f.in_range("A", -50, "passwordA");
    f.seed({{"A", "passwordA"}});
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    svc.tick(0);
    svc.tick(250);
    ASSERT_EQ(svc.status().state, WifiState::Connected);
    f.clear_events();

    f.wifi->set_link(WifiLink{});  // Idle: signal lost
    svc.tick(500);
    EXPECT_EQ(svc.status().state, WifiState::Connecting);
    EXPECT_EQ(f.wifi->connect_calls.load(), 2);
    EXPECT_EQ(f.wifi->scan_calls.load(), 2);
    svc.tick(750);
    EXPECT_EQ(svc.status().state, WifiState::Connected);

    WifiLink failed;
    failed.state = WifiLinkState::Failed;
    failed.ssid = "A";
    f.wifi->set_link(failed);
    svc.tick(1000);
    EXPECT_EQ(f.wifi->connect_calls.load(), 3);
    svc.tick(1250);
    EXPECT_EQ(svc.status().state, WifiState::Connected);

    auto states = f.topic("net.wifi.state");
    ASSERT_EQ(states.size(), 4u);
    EXPECT_EQ(str_of(states[0].payload, "state"), "connecting");
    EXPECT_EQ(str_of(states[1].payload, "state"), "connected");
    EXPECT_EQ(str_of(states[2].payload, "state"), "connecting");
    EXPECT_EQ(str_of(states[3].payload, "state"), "connected");
}

TEST(WifiService, ConnectTimeoutCountsAsFailure) {
    Fixture f;
    f.in_range("A", -50, "passwordA");
    f.seed({{"A", "passwordA"}});
    WifiServiceConfig config;
    config.connect_timeout_ms = 15000;
    auto& svc = f.make(config);
    ASSERT_TRUE(svc.init(0));

    svc.tick(0);
    WifiLink stuck;
    stuck.state = WifiLinkState::Connecting;
    stuck.ssid = "A";
    f.wifi->set_link(stuck);
    svc.tick(14999);
    EXPECT_EQ(svc.status().state, WifiState::Connecting);
    EXPECT_EQ(f.wifi->disconnect_calls.load(), 0);
    svc.tick(15000);
    EXPECT_EQ(svc.status().state, WifiState::Backoff);
    EXPECT_EQ(f.wifi->disconnect_calls.load(), 1);  // abandon the stuck attempt
    svc.tick(19999);
    EXPECT_EQ(f.wifi->connect_calls.load(), 1);
    svc.tick(20000);
    EXPECT_EQ(f.wifi->connect_calls.load(), 2);
}

TEST(WifiService, AddNetworkValidatesAndCapsAtFive) {
    Fixture f;
    f.in_range("N5", -60, "password5");
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    EXPECT_EQ(svc.status().state, WifiState::Off);

    EXPECT_EQ(svc.add_network("", "", 0).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(svc.add_network(std::string(33, 's'), "", 0).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(svc.add_network("N1", "1234567", 0).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(svc.add_network("N1", std::string(64, 'p'), 0).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_TRUE(svc.saved_ssids().empty());

    EXPECT_TRUE(svc.add_network(std::string(32, 's'), "12345678", 0));
    EXPECT_TRUE(svc.add_network("N2", std::string(63, 'p'), 0));
    EXPECT_TRUE(svc.add_network("N3", "", 0));  // open network
    EXPECT_TRUE(svc.add_network("N4", "password4", 0));
    EXPECT_TRUE(svc.add_network("N5", "password5", 0));
    EXPECT_EQ(svc.add_network("N6", "password6", 0).error().code, ErrorCode::QuotaExceeded);
    EXPECT_TRUE(svc.add_network("N3", "newpassword", 0));  // overwrite still allowed
    EXPECT_EQ(svc.saved_ssids(),
              (std::vector<std::string>{std::string(32, 's'), "N2", "N3", "N4", "N5"}));

    // The last successful add is tried directly, without a scan.
    EXPECT_TRUE(svc.add_network("N5", "password5", 0));
    EXPECT_EQ(svc.status().state, WifiState::Connecting);
    svc.tick(0);
    EXPECT_EQ(f.wifi->scan_calls.load(), 0);
    EXPECT_EQ(f.wifi->link().ssid, "N5");
    svc.tick(250);
    EXPECT_EQ(svc.status().state, WifiState::Connected);
    EXPECT_EQ(svc.status().ssid, "N5");
}

TEST(WifiService, PersistsNetworksAcrossInstancesWithoutLoggingPasswords) {
    wisp::log::reset_for_test();
    wisp::log::init(64 * 1024);
    {
        Fixture f;
        f.in_range("Home", -50, "homeSecret42");
        f.wifi->reachable = false;  // exercise the failure log lines too
        auto& svc = f.make();
        ASSERT_TRUE(svc.init(0));
        ASSERT_TRUE(svc.add_network("Home", "homeSecret42", 0));
        ASSERT_TRUE(svc.add_network("Cafe", "", 0));
        ASSERT_TRUE(svc.add_network("Work", "workSecret77", 0));
        ASSERT_TRUE(svc.forget_network("Cafe"));
        EXPECT_EQ(svc.forget_network("Cafe").error().code, ErrorCode::NotFound);
        tick2(svc, 0);
        tick2(svc, 5000);
        ASSERT_TRUE(svc.start_provisioning(6000));
        auto ap_password = svc.ap_session()->password;

        auto bytes = f.storage->read_file(f.wifi_json_path());
        ASSERT_TRUE(bytes);
        auto doc = wisp::from_json(std::string(bytes.value().begin(), bytes.value().end()));
        ASSERT_TRUE(doc);
        const auto& nets = doc.value().find("networks")->as_list();
        ASSERT_EQ(nets.size(), 2u);
        EXPECT_EQ(nets[0].find("ssid")->as_string(), "Home");
        EXPECT_EQ(nets[0].find("password")->as_string(), "homeSecret42");
        EXPECT_EQ(nets[1].find("ssid")->as_string(), "Work");

        // Second instance over the same storage.
        auto& again = f.make();
        ASSERT_TRUE(again.init(10000));
        EXPECT_EQ(again.saved_ssids(), (std::vector<std::string>{"Home", "Work"}));
        EXPECT_EQ(again.status().state, WifiState::Connecting);

        std::string log = wisp::log::ring_snapshot();
        EXPECT_FALSE(log.empty());
        EXPECT_EQ(log.find("homeSecret42"), std::string::npos) << log;
        EXPECT_EQ(log.find("workSecret77"), std::string::npos) << log;
        EXPECT_EQ(log.find(ap_password), std::string::npos) << log;
    }
    wisp::log::reset_for_test();
}

TEST(WifiService, CorruptFileIsReportedButServiceStaysUsable) {
    Fixture f;
    std::string junk = "{not json";
    ASSERT_TRUE(f.storage->write_atomic(f.wifi_json_path(),
                                        std::vector<uint8_t>(junk.begin(), junk.end())));
    auto& svc = f.make();
    EXPECT_FALSE(svc.init(0));
    EXPECT_EQ(svc.status().state, WifiState::Off);
    EXPECT_TRUE(svc.add_network("Home", "password1", 0));
    EXPECT_EQ(svc.saved_ssids(), (std::vector<std::string>{"Home"}));
}

TEST(WifiService, ProvisioningSsidAndPasswordFormat) {
    Fixture f;
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    // 0->'A', 25->'Z', 26->'a', 51->'z', 52->'0', 61->'9', 62 wraps to 'A',
    // 255 % 62 = 7 -> 'H'.
    f.scripted_random = {0, 25, 26, 51, 52, 61, 62, 255};

    auto first = svc.start_provisioning(0);
    ASSERT_TRUE(first);
    EXPECT_EQ(first.value().ssid, "Wisp-3F2A");
    EXPECT_EQ(first.value().password, "AZaz09AH");
    EXPECT_EQ(first.value().ip, "192.168.4.1");
    EXPECT_TRUE(f.wifi->ap_active.load());
    EXPECT_EQ(f.wifi->ap_ssid(), "Wisp-3F2A");
    EXPECT_EQ(f.wifi->ap_password(), "AZaz09AH");
    ASSERT_TRUE(svc.ap_session().has_value());
    EXPECT_EQ(svc.ap_session()->password, "AZaz09AH");

    // Starting again while active keeps the running session.
    EXPECT_EQ(svc.start_provisioning(10).value().password, "AZaz09AH");

    ASSERT_TRUE(svc.stop_provisioning());
    EXPECT_FALSE(f.wifi->ap_active.load());
    EXPECT_FALSE(svc.ap_session().has_value());
    EXPECT_TRUE(svc.stop_provisioning());  // idempotent

    f.wifi->mac_bytes = {0x02, 0x00, 0x00, 0x00, 0xAB, 0x05};
    auto second = svc.start_provisioning(20);
    ASSERT_TRUE(second);
    EXPECT_EQ(second.value().ssid, "Wisp-AB05");
    EXPECT_TRUE(std::regex_match(second.value().password, std::regex("^[A-Za-z0-9]{8}$")));
    EXPECT_NE(second.value().password, first.value().password);
}

TEST(WifiService, ApClosesAfterTenIdleMinutes) {
    Fixture f;
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    ASSERT_TRUE(svc.start_provisioning(0));

    svc.tick(599999);
    EXPECT_TRUE(f.wifi->ap_active.load());
    svc.tick(600000);
    EXPECT_FALSE(f.wifi->ap_active.load());
    EXPECT_FALSE(svc.ap_session().has_value());

    auto ap = f.topic("net.ap.state");
    ASSERT_EQ(ap.size(), 2u);
    EXPECT_TRUE(ap[0].payload.at("active").as_bool());
    EXPECT_EQ(str_of(ap[0].payload, "ssid"), "Wisp-3F2A");
    EXPECT_FALSE(ap[1].payload.at("active").as_bool());

    // A client resets the idle clock; it restarts when the last one leaves.
    f.clear_events();
    ASSERT_TRUE(svc.start_provisioning(1'000'000));
    f.wifi->ap_client_count = 1;
    svc.tick(1'100'000);
    f.wifi->ap_client_count = 0;
    svc.tick(1'200'000);
    svc.tick(1'799'999);
    EXPECT_TRUE(f.wifi->ap_active.load());
    svc.tick(1'800'000);
    EXPECT_FALSE(f.wifi->ap_active.load());

    ap = f.topic("net.ap.state");
    ASSERT_EQ(ap.size(), 4u);
    EXPECT_EQ(ap[1].payload.at("clients").as_int(), 1);
    EXPECT_EQ(ap[2].payload.at("clients").as_int(), 0);
    EXPECT_FALSE(ap[3].payload.at("active").as_bool());
}

TEST(WifiService, ApLingersThirtySecondsAfterProvisionedConnect) {
    Fixture f;
    f.in_range("Home", -50, "homepass1");
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    ASSERT_TRUE(svc.start_provisioning(0));
    f.wifi->ap_client_count = 1;
    svc.tick(1000);

    ASSERT_TRUE(svc.add_network("Home", "homepass1", 2000));
    svc.tick(2000);
    svc.tick(2250);
    ASSERT_EQ(svc.status().state, WifiState::Connected);

    svc.tick(32249);
    EXPECT_TRUE(f.wifi->ap_active.load());
    svc.tick(32250);
    EXPECT_FALSE(f.wifi->ap_active.load());
    auto ap = f.topic("net.ap.state");
    ASSERT_FALSE(ap.empty());
    EXPECT_FALSE(ap.back().payload.at("active").as_bool());
}

TEST(WifiService, ApStaysOpenWhenOldNetworkReconnectsDuringProvisioning) {
    Fixture f;
    f.in_range("Old", -50, "oldpass11");
    f.in_range("New", -60, "newpass22");
    f.seed({{"Old", "oldpass11"}});
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    svc.tick(0);
    svc.tick(250);
    ASSERT_EQ(svc.status().ssid, "Old");

    ASSERT_TRUE(svc.start_provisioning(1000));
    f.wifi->ap_client_count = 1;
    f.wifi->set_link(WifiLink{});  // the old network drops and comes back
    svc.tick(2000);
    svc.tick(2250);
    ASSERT_EQ(svc.status().state, WifiState::Connected);
    ASSERT_EQ(svc.status().ssid, "Old");

    svc.tick(100'000);
    EXPECT_TRUE(f.wifi->ap_active.load());

    // Only the network the user is provisioning ends the session.
    ASSERT_TRUE(svc.add_network("New", "newpass22", 200'000));
    svc.tick(200'000);
    svc.tick(200'250);
    ASSERT_EQ(svc.status().ssid, "New");
    svc.tick(230'249);
    EXPECT_TRUE(f.wifi->ap_active.load());
    svc.tick(230'250);
    EXPECT_FALSE(f.wifi->ap_active.load());
}

TEST(WifiService, StandbyDisconnectsAfterIdleMinutesAndWakeReconnects) {
    Fixture f;
    ASSERT_TRUE(Settings(*f.backend, "net").set("idle_off_min", 10));
    f.in_range("Home", -50, "homepass1");
    f.seed({{"Home", "homepass1"}});
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    svc.tick(0);
    svc.tick(250);
    ASSERT_EQ(svc.status().state, WifiState::Connected);

    // A short standby does nothing.
    svc.set_standby(true, 1000);
    svc.tick(300'000);
    svc.set_standby(false, 300'000);
    svc.tick(1'000'000);
    EXPECT_EQ(svc.status().state, WifiState::Connected);
    EXPECT_EQ(f.wifi->disconnect_calls.load(), 0);

    svc.set_standby(true, 1'000'000);
    svc.tick(1'599'999);
    EXPECT_EQ(svc.status().state, WifiState::Connected);
    svc.tick(1'600'000);
    EXPECT_EQ(svc.status().state, WifiState::Off);
    EXPECT_EQ(f.wifi->disconnect_calls.load(), 1);

    // Off because of standby is not a failure: no retries while asleep.
    int connects = f.wifi->connect_calls.load();
    svc.tick(2'000'000);
    svc.tick(3'000'000);
    EXPECT_EQ(svc.status().state, WifiState::Off);
    EXPECT_EQ(f.wifi->connect_calls.load(), connects);

    svc.set_standby(false, 3'100'000);
    EXPECT_EQ(svc.status().state, WifiState::Connecting);
    svc.tick(3'100'000);
    svc.tick(3'100'250);
    EXPECT_EQ(svc.status().state, WifiState::Connected);

    // The setting is read when standby starts.
    ASSERT_TRUE(Settings(*f.backend, "net").set("idle_off_min", 1));
    svc.set_standby(true, 4'000'000);
    svc.tick(4'059'999);
    EXPECT_EQ(svc.status().state, WifiState::Connected);
    svc.tick(4'060'000);
    EXPECT_EQ(svc.status().state, WifiState::Off);
}

TEST(WifiService, StateEventsCarryNoPassword) {
    Fixture f;
    f.in_range("Home", -50, "wifiSecret99");
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    auto session = svc.start_provisioning(0);
    ASSERT_TRUE(session);
    f.wifi->ap_client_count = 1;
    ASSERT_TRUE(svc.add_network("Home", "wifiSecret99", 100));
    svc.tick(100);
    svc.tick(350);
    svc.tick(40'000);  // linger expires, AP closes

    auto events = f.all_events();
    int wifi_events = 0;
    int ap_events = 0;
    for (const auto& e : events) {
        std::string json = wisp::to_json(wisp::Value(e.payload));
        EXPECT_EQ(json.find("wifiSecret99"), std::string::npos) << e.topic << " " << json;
        EXPECT_EQ(json.find(session.value().password), std::string::npos) << e.topic << " " << json;
        EXPECT_EQ(json.find("password"), std::string::npos) << e.topic << " " << json;
        if (e.topic == "net.wifi.state") {
            ++wifi_events;
            EXPECT_EQ(e.source, "net.wifi");
            EXPECT_EQ(e.payload.size(), 6u) << json;
            EXPECT_TRUE(e.payload.count("state") && e.payload.count("ssid") &&
                        e.payload.count("ssid_hex") && e.payload.count("ip") &&
                        e.payload.count("rssi") && e.payload.count("reason"))
                << json;
        } else if (e.topic == "net.ap.state") {
            ++ap_events;
            EXPECT_EQ(e.payload.size(), 3u) << json;
            EXPECT_TRUE(e.payload.count("active") && e.payload.count("ssid") &&
                        e.payload.count("clients"))
                << json;
        }
    }
    EXPECT_GE(wifi_events, 2);
    EXPECT_GE(ap_events, 3);  // start, client joined, closed
    auto connected = f.topic("net.wifi.state").back();
    EXPECT_EQ(str_of(connected.payload, "state"), "connected");
    EXPECT_EQ(str_of(connected.payload, "ip"), "192.168.1.50");
    EXPECT_EQ(connected.payload.at("rssi").as_int(), -50);
}

// The setup wizard shows why an attempt failed; "reason" is only set in
// backoff and never carries the password.
TEST(WifiService, BackoffEventsCarryFailureReason) {
    Fixture f;
    f.in_range("Home", -50, "rightpass1");
    f.seed({{"Home", "wrongpass1"}});
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));

    // The password failure comes last: new credentials follow it before
    // the backoff retries the old ones.
    f.wifi->reachable = false;
    tick2(svc, 0);  // router unreachable
    f.wifi->networks.clear();
    tick2(svc, 5000);  // nothing saved in range
    f.wifi->networks.push_back({"Home", -50, true});
    f.wifi->reachable = true;
    tick2(svc, 15000);  // wrong password
    ASSERT_TRUE(svc.add_network("Home", "rightpass1", 20000));
    tick2(svc, 20000);
    ASSERT_EQ(svc.status().state, WifiState::Connected);

    std::vector<std::string> backoff_reasons;
    for (const auto& e : f.topic("net.wifi.state")) {
        std::string json = wisp::to_json(wisp::Value(e.payload));
        EXPECT_EQ(json.find("rightpass1"), std::string::npos) << json;
        EXPECT_EQ(json.find("wrongpass1"), std::string::npos) << json;
        if (str_of(e.payload, "state") == "backoff") {
            backoff_reasons.push_back(str_of(e.payload, "reason"));
        } else {
            EXPECT_EQ(str_of(e.payload, "reason"), "") << json;
        }
    }
    EXPECT_EQ(backoff_reasons, (std::vector<std::string>{"other", "not_found", "auth_failed"}));
}

// A Failed snapshot left over from an attempt on another network must not
// be taken as the outcome of the attempt in flight.
TEST(WifiService, StaleFailedFromAnotherNetworkIsIgnored) {
    Fixture f;
    f.in_range("A", -50, "passwordA");
    f.seed({{"A", "passwordA"}});
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    svc.tick(0);  // connect("A") in flight

    WifiLink stale;
    stale.state = WifiLinkState::Failed;
    stale.ssid = "B";
    stale.reason = wisp::hal::WifiFailReason::AuthFailed;
    f.wifi->set_link(stale);
    svc.tick(250);
    EXPECT_EQ(svc.status().state, WifiState::Connecting);

    WifiLink ok;
    ok.state = WifiLinkState::Connected;
    ok.ssid = "A";
    ok.ip = "192.168.1.50";
    f.wifi->set_link(ok);
    svc.tick(500);
    EXPECT_EQ(svc.status().state, WifiState::Connected);

    // A Failed with no SSID (drivers that don't report one) still counts.
    f.wifi->reachable = false;
    f.wifi->set_link(WifiLink{});
    svc.tick(750);  // drop -> reconnect attempt in flight
    WifiLink anonymous;
    anonymous.state = WifiLinkState::Failed;
    f.wifi->set_link(anonymous);
    svc.tick(1000);
    EXPECT_EQ(svc.status().state, WifiState::Backoff);
}

// add_network() landing while tick() has mutex_ released for its scan must
// win: the stale scan-based plan is dropped and the next tick connects to
// the network the user just added, not the strongest old one.
TEST(WifiService, AddNetworkDuringScanSupersedesThePlannedAttempt) {
    Fixture f;
    f.in_range("Old", -40, "oldpass11");
    f.in_range("New", -70, "newpass22");
    f.seed({{"Old", "oldpass11"}});
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));

    bool fired = false;
    f.wifi->on_scan = [&] {
        if (fired) return;
        fired = true;
        EXPECT_TRUE(svc.add_network("New", "newpass22", 0));
    };
    svc.tick(0);
    ASSERT_TRUE(fired);
    EXPECT_EQ(f.wifi->connect_calls.load(), 0);

    svc.tick(250);
    EXPECT_EQ(f.wifi->connect_calls.load(), 1);
    EXPECT_EQ(f.wifi->link().ssid, "New");
    svc.tick(500);
    EXPECT_EQ(svc.status().state, WifiState::Connected);
    EXPECT_EQ(svc.status().ssid, "New");
}

TEST(WifiService, ForgetAllReportsAFailedPersist) {
    wisp::log::reset_for_test();
    wisp::log::init(64 * 1024);
    {
        Fixture f;
        auto& svc = f.make();
        ASSERT_TRUE(svc.init(0));
        ASSERT_TRUE(svc.add_network("Home", "homeSecret42", 0));

        // A non-empty directory where wifi.json goes makes the write fail.
        std::filesystem::remove(f.wifi_json_path());
        std::filesystem::create_directories(f.wifi_json_path() + "/blocker");

        svc.forget_all();
        EXPECT_TRUE(svc.saved_ssids().empty());
        EXPECT_EQ(svc.status().state, WifiState::Off);

        std::string log = wisp::log::ring_snapshot();
        EXPECT_NE(log.find("not persisted"), std::string::npos) << log;
        EXPECT_EQ(log.find("homeSecret42"), std::string::npos) << log;
    }
    wisp::log::reset_for_test();
}

TEST(WifiService, ScanMarksSavedAndKeepsStrongestDuplicate) {
    Fixture f;
    f.in_range("Home", -70, "homepass1");
    f.in_range("Home", -40, "homepass1");
    f.in_range("Cafe", -60);
    f.in_range("", -30);  // hidden
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    ASSERT_TRUE(svc.add_network("Home", "homepass1", 0));

    auto r = svc.scan();
    ASSERT_TRUE(r);
    ASSERT_EQ(r.value().size(), 2u);
    EXPECT_EQ(r.value()[0].ssid, "Home");
    EXPECT_EQ(r.value()[0].rssi, -40);
    EXPECT_TRUE(r.value()[0].saved);
    EXPECT_TRUE(r.value()[0].secured);
    EXPECT_EQ(r.value()[1].ssid, "Cafe");
    EXPECT_FALSE(r.value()[1].saved);
    EXPECT_FALSE(r.value()[1].secured);
}

TEST(WifiService, ForgettingCurrentNetworkDisconnects) {
    Fixture f;
    f.in_range("A", -40, "passwordA");
    f.in_range("B", -60, "passwordB");
    f.seed({{"A", "passwordA"}, {"B", "passwordB"}});
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    svc.tick(0);
    svc.tick(250);
    ASSERT_EQ(svc.status().ssid, "A");

    ASSERT_TRUE(svc.forget_network("A"));
    EXPECT_EQ(f.wifi->disconnect_calls.load(), 1);
    EXPECT_EQ(svc.status().state, WifiState::Connecting);
    svc.tick(500);
    svc.tick(750);
    EXPECT_EQ(svc.status().ssid, "B");

    ASSERT_TRUE(svc.forget_network("B"));
    EXPECT_EQ(svc.status().state, WifiState::Off);
    svc.tick(1000);
    EXPECT_EQ(svc.status().state, WifiState::Off);
}

TEST(WifiService, ForgetAllClearsEverything) {
    Fixture f;
    f.in_range("A", -40, "passwordA");
    f.seed({{"A", "passwordA"}});
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    svc.tick(0);
    svc.tick(250);
    ASSERT_TRUE(svc.start_provisioning(300));

    svc.forget_all();
    EXPECT_EQ(svc.status().state, WifiState::Off);
    EXPECT_TRUE(svc.saved_ssids().empty());
    EXPECT_FALSE(f.wifi->ap_active.load());
    EXPECT_GE(f.wifi->disconnect_calls.load(), 1);

    auto& again = f.make();
    ASSERT_TRUE(again.init(1000));
    EXPECT_TRUE(again.saved_ssids().empty());
    EXPECT_EQ(again.status().state, WifiState::Off);
}

// Public calls from other threads (HTTP, console, UI) race the tick task.
// The assertions are weak on purpose; the point is that nothing deadlocks
// or corrupts state (run under TSan/ASan to make this bite harder).
TEST(WifiService, ConcurrentCallsAreSafe) {
    Fixture f;
    f.in_range("A", -40, "passwordA");
    f.in_range("B", -60, "passwordB");
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> now{0};
    std::thread ticker([&] {
        while (!stop.load()) {
            svc.tick(now.fetch_add(250));
        }
    });
    std::thread http([&] {
        for (int i = 0; i < 200; ++i) {
            (void)svc.add_network(i % 2 ? "A" : "B", i % 2 ? "passwordA" : "passwordB", now.load());
            (void)svc.scan();
            if (i % 7 == 0) (void)svc.forget_network("A");
        }
    });
    std::thread ui([&] {
        for (int i = 0; i < 200; ++i) {
            (void)svc.status();
            (void)svc.saved_ssids();
            (void)svc.ap_session();
            if (i % 50 == 0) (void)svc.start_provisioning(now.load());
            if (i % 50 == 25) (void)svc.stop_provisioning();
            svc.set_standby(i % 2 == 0, now.load());
        }
    });
    http.join();
    ui.join();
    stop = true;
    ticker.join();

    svc.set_standby(false, now.load());
    EXPECT_LE(svc.saved_ssids().size(), 2u);
}

// SSIDs are raw bytes (32 at most), not necessarily UTF-8: routers set up
// in GBK are common. wifi.json must keep them exactly or the saved network
// stops matching the scan after a reboot.
TEST(WifiService, NonUtf8SsidSurvivesRestart) {
    Fixture f;
    const std::string gbk = "\xc4\xe3\xba\xc3";  // "你好" in GBK
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    ASSERT_TRUE(svc.add_network(gbk, "password1", 0));

    auto& again = f.make();
    ASSERT_TRUE(again.init(0));
    EXPECT_EQ(again.saved_ssids(), (std::vector<std::string>{gbk}));
}

// Rejects the password of `ssid` `times` times in a row, starting at
// `now`; each attempt is the one the backoff schedules next. Returns the
// time just after the last attempt.
uint64_t reject_times(Fixture& f, WifiService& svc, int times, uint64_t now) {
    for (int i = 0; i < times; ++i) {
        tick2(svc, now);
        EXPECT_EQ(svc.status().reason, "auth_failed");
        now = svc.status().retry_at_ms;
    }
    (void)f;
    return now;
}

// One rejected handshake can be a lost EAPOL frame (router reboot, busy
// channel), so a network is only left out of automatic selection after
// three rejections in a row (spec 4.2).
TEST(WifiService, ParksANetworkOnlyAfterThreeConsecutiveRejections) {
    Fixture f;
    f.in_range("B", -40, "passwordB");
    f.seed({{"B", "wrongpassB"}});
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));

    uint64_t now = reject_times(f, svc, 2, 0);
    EXPECT_EQ(f.wifi->connect_calls.load(), 2);
    EXPECT_FALSE(svc.saved_networks()[0].auth_failed);

    now = reject_times(f, svc, 1, now);
    EXPECT_EQ(f.wifi->connect_calls.load(), 3);
    EXPECT_TRUE(svc.saved_networks()[0].auth_failed);

    // Parked: later attempts scan but never retry the rejected password.
    tick2(svc, now);
    tick2(svc, svc.status().retry_at_ms);
    EXPECT_EQ(f.wifi->connect_calls.load(), 3);
    EXPECT_EQ(svc.status().state, WifiState::Backoff);
}

TEST(WifiService, ASuccessfulConnectResetsTheRejectionCount) {
    Fixture f;
    f.in_range("B", -40, "passwordB");
    f.seed({{"B", "samepassB"}});
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    uint64_t now = reject_times(f, svc, 2, 0);

    f.wifi->passwords["B"] = "samepassB";  // the handshake goes through
    tick2(svc, now);
    ASSERT_EQ(svc.status().state, WifiState::Connected);

    f.wifi->passwords["B"] = "passwordB";
    f.wifi->set_link(WifiLink{});
    now = reject_times(f, svc, 2, now + 1000);
    EXPECT_FALSE(svc.saved_networks()[0].auth_failed);
    reject_times(f, svc, 1, now);
    EXPECT_TRUE(svc.saved_networks()[0].auth_failed);
}

// Failures for other reasons say nothing about the password: they neither
// count towards parking nor interrupt a run of rejections.
TEST(WifiService, OtherFailuresDoNotCountAsRejections) {
    Fixture f;
    f.in_range("B", -40, "passwordB");
    f.seed({{"B", "wrongpassB"}});
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    uint64_t now = reject_times(f, svc, 2, 0);

    f.wifi->reachable = false;
    tick2(svc, now);
    EXPECT_EQ(svc.status().reason, "other");
    now = svc.status().retry_at_ms;
    EXPECT_FALSE(svc.saved_networks()[0].auth_failed);

    f.wifi->reachable = true;
    reject_times(f, svc, 1, now);
    EXPECT_TRUE(svc.saved_networks()[0].auth_failed);
}

// Re-adding a network (the user picking it again, with the same or a new
// password) starts its count over.
TEST(WifiService, ReAddingResetsTheRejectionCount) {
    Fixture f;
    f.in_range("B", -40, "passwordB");
    f.seed({{"B", "wrongpassB"}});
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    uint64_t now = reject_times(f, svc, 2, 0);

    ASSERT_TRUE(svc.add_network("B", "wrongpassB", now));
    now = reject_times(f, svc, 2, now);
    EXPECT_FALSE(svc.saved_networks()[0].auth_failed);
    reject_times(f, svc, 1, now);
    EXPECT_TRUE(svc.saved_networks()[0].auth_failed);
}

// A wrong password must not strand the device: the rejected network is left
// out of automatic selection, so the device falls back to another saved
// network instead of retrying the stronger, rejected one forever.
TEST(WifiService, AuthFailedNetworkIsSkippedUntilReAdded) {
    Fixture f;
    f.in_range("A", -70, "passwordA");
    f.in_range("B", -40, "passwordB");
    f.seed({{"A", "passwordA"}});
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    tick2(svc, 0);
    ASSERT_EQ(svc.status().state, WifiState::Connected);
    ASSERT_EQ(svc.status().ssid, "A");

    // The direct attempt on B and the two scans after it all pick B (the
    // stronger one) until its third rejection parks it.
    ASSERT_TRUE(svc.add_network("B", "wrongpassB", 1000));
    uint64_t now = reject_times(f, svc, 3, 1000);
    EXPECT_EQ(f.wifi->connect_calls.load(), 4);
    auto saved = svc.saved_networks();
    ASSERT_EQ(saved.size(), 2u);
    EXPECT_EQ(saved[0].ssid, "A");
    EXPECT_FALSE(saved[0].auth_failed);
    EXPECT_EQ(saved[1].ssid, "B");
    EXPECT_TRUE(saved[1].auth_failed);

    tick2(svc, now);  // B is stronger, but excluded: back to A
    EXPECT_EQ(f.wifi->connect_calls.load(), 5);
    EXPECT_EQ(svc.status().state, WifiState::Connected);
    EXPECT_EQ(svc.status().ssid, "A");

    f.wifi->set_link(WifiLink{});  // A drops; the reconnect still skips B
    tick2(svc, now + 1000);
    EXPECT_EQ(f.wifi->connect_calls.load(), 6);
    EXPECT_EQ(svc.status().ssid, "A");

    // New credentials clear the flag and B is tried straight away.
    ASSERT_TRUE(svc.add_network("B", "passwordB", now + 2000));
    EXPECT_FALSE(svc.saved_networks()[1].auth_failed);
    tick2(svc, now + 2000);
    EXPECT_EQ(svc.status().state, WifiState::Connected);
    EXPECT_EQ(svc.status().ssid, "B");

    // Later automatic selection considers B again (and prefers it).
    f.wifi->set_link(WifiLink{});
    tick2(svc, now + 3000);
    EXPECT_EQ(svc.status().ssid, "B");
}

TEST(WifiService, AuthFailedFlagSurvivesRestart) {
    Fixture f;
    f.in_range("A", -70, "passwordA");
    f.in_range("B", -40, "passwordB");
    f.seed({{"A", "passwordA"}, {"B", "wrongpassB"}});
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    reject_times(f, svc, 3, 0);  // strongest saved network first: B, rejected
    ASSERT_TRUE(svc.saved_networks()[1].auth_failed);

    auto& again = f.make();
    ASSERT_TRUE(again.init(0));
    auto saved = again.saved_networks();
    ASSERT_EQ(saved.size(), 2u);
    EXPECT_FALSE(saved[0].auth_failed);
    EXPECT_TRUE(saved[1].auth_failed);
    tick2(again, 0);
    EXPECT_EQ(again.status().state, WifiState::Connected);
    EXPECT_EQ(again.status().ssid, "A");
    EXPECT_EQ(f.wifi->connect_calls.load(), 4);
}

// With every saved network rejected, the service keeps scanning on the
// usual backoff schedule but never retries the rejected credentials, and
// its status names a rejected network with reason "auth_failed" (not
// "not_found"): only new credentials can help.
TEST(WifiService, OnlyAuthFailedNetworksMeansBackoffWithoutAttempts) {
    Fixture f;
    f.in_range("B", -40, "passwordB");
    f.seed({{"B", "wrongpassB"}, {"C", "passwordC"}});
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    uint64_t now = reject_times(f, svc, 3, 0);
    ASSERT_EQ(f.wifi->connect_calls.load(), 3);
    // C is out of range and not rejected: an ordinary "not found" wait.
    tick2(svc, now);
    EXPECT_EQ(svc.status().reason, "not_found");

    ASSERT_TRUE(svc.forget_network("C"));
    tick2(svc, svc.status().retry_at_ms);
    tick2(svc, svc.status().retry_at_ms);
    EXPECT_EQ(f.wifi->connect_calls.load(), 3);
    EXPECT_EQ(f.wifi->scan_calls.load(), 6);
    EXPECT_EQ(svc.status().state, WifiState::Backoff);
    EXPECT_EQ(svc.status().reason, "auth_failed");
    EXPECT_EQ(svc.status().ssid, "B");
}

// Browsers only see SSIDs through JSON, which cannot carry bytes that are
// not UTF-8; the state event names the network byte-exactly as hex too.
TEST(WifiService, StateEventsCarrySsidHex) {
    Fixture f;
    const std::string gbk = "\xc4\xe3\xba\xc3";
    f.in_range(gbk, -50);
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    ASSERT_TRUE(svc.add_network(gbk, "", 0));
    tick2(svc, 0);
    ASSERT_EQ(svc.status().state, WifiState::Connected);
    auto states = f.topic("net.wifi.state");
    ASSERT_FALSE(states.empty());
    EXPECT_EQ(str_of(states.back().payload, "ssid_hex"), "c4e3bac3");
}

// The device screen counts down to the next attempt ("retrying in N s"),
// so status() says when that is -- only while backing off.
TEST(WifiService, StatusSaysWhenBackoffRetries) {
    Fixture f;
    f.in_range("A", -50, "passwordA");
    f.wifi->reachable = false;
    f.seed({{"A", "passwordA"}});
    auto& svc = f.make();
    ASSERT_TRUE(svc.init(0));
    EXPECT_EQ(svc.status().retry_at_ms, 0u);

    tick2(svc, 1000);
    ASSERT_EQ(svc.status().state, WifiState::Backoff);
    EXPECT_EQ(svc.status().retry_at_ms, 1000u + 5000u);

    f.wifi->reachable = true;
    tick2(svc, 6000);
    ASSERT_EQ(svc.status().state, WifiState::Connected);
    EXPECT_EQ(svc.status().retry_at_ms, 0u);
}
