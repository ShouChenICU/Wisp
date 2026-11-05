// NetWiring end to end (scene-device spec 4.1, 4.3, 4.4): a real
// PowerManager (headless), WifiService over a FakeWifi, the NetworkWindow
// and a standalone SceneHttp, driven by the "wisp.net" task on the real
// clock. The 15-minute window itself is covered by network_window_test;
// here: the time-sync attempt holds "system.ntp", deep standby makes
// scene requests wait, and waking releases them only once Wi-Fi is up.

#include "net_wiring.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/fake_http.hpp"
#include "support/fake_sntp.hpp"
#include "support/fake_wifi.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/boot_report.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/model_profiles.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/net/scene_http.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/settings_json_file.hpp"

namespace {

using wisp::hal::PowerLevel;
using wisp::test::FakeHttpClient;
using wisp::test::FakeSntp;
using wisp::test::FakeStorage;
using wisp::test::FakeWifi;
using wisp::test::wait_until;

// What the scenes' proxy sees of the network: each request records
// whether the station was connected when it went out.
class LinkCheckingClient : public wisp::net::HttpClient {
public:
    LinkCheckingClient(FakeHttpClient& inner, FakeWifi& wifi) : inner_(inner), wifi_(wifi) {}

    wisp::net::HttpClientResponse request(const wisp::net::HttpClientRequest& req) override {
        const bool up =
            wifi_.radio_on.load() && wifi_.link().state == wisp::hal::WifiLinkState::Connected;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            link_up_.push_back(up);
        }
        return inner_.request(req);
    }

    std::vector<bool> link_up() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return link_up_;
    }

private:
    FakeHttpClient& inner_;
    FakeWifi& wifi_;
    mutable std::mutex mutex_;
    std::vector<bool> link_up_;
};

bool holds(wisp::core::WakeLockRegistry& locks, const std::string& holder) {
    for (const auto& l : locks.list())
        if (l.holder == holder && l.kind == wisp::core::WakeLockKind::Network) return true;
    return false;
}

struct Rig {
    Rig() {
        wisp::log::reset_for_test();
        wisp::log::init(64 * 1024);
        registry.add(std::make_shared<FakeStorage>("storage.int", dir, /*removable=*/false));
        wifi = std::make_shared<FakeWifi>("net.wifi");
        wifi->networks = {{"Home", -50, true}};
        wifi->passwords = {{"Home", "password1"}};
        // A scan that takes a while, like a real one: the radio is on but
        // the station not yet connected.
        wifi->on_scan = [this] { wisp::os::sleep_ms(scan_ms.load()); };
        registry.add(wifi);
        std::filesystem::create_directories(dir + "/system/net");
        std::ofstream(dir + "/system/net/wifi.json") << wisp::to_json(wisp::Value(wisp::Map{
            {"networks",
             wisp::Value(wisp::List{wisp::Value(wisp::Map{
                 {"ssid", wisp::Value("Home")}, {"password", wisp::Value("password1")}})})}}));
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        bus.start();
        backend =
            std::make_unique<wisp::port::sim::JsonFileSettingsBackend>(dir + "/settings.json");
        auto random = [](uint8_t* out, size_t len) {
            for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(i * 37 + 11);
        };
        models = std::make_unique<wisp::core::ModelProfileService>(*storage, bus, random);
        EXPECT_TRUE(models->init());
        system_settings = std::make_unique<wisp::core::SystemSettings>(*backend, nullptr);

        // Records whether the attempt runs under its lock.
        auto sntp = std::make_unique<FakeSntp>([] { return int64_t{1767225600}; });
        sntp->on_query = [this](const std::string&) {
            if (holds(locks, "system.ntp")) ntp_locked = true;
            ++ntp_queries;
        };
        platform.sntp = std::move(sntp);
        platform.random = random;

        pm = std::make_unique<wisp::app::PowerManager>(wisp::app::PowerManagerDeps{
            .bus = bus,
            .registry = registry,
            .locks = locks,
            .settings = wisp::core::Settings(*backend, "power"),
            .display = nullptr,
        });

        client = std::make_unique<LinkCheckingClient>(inner, *wifi);
        http = std::make_unique<wisp::net::SceneHttp>(*client);
        EXPECT_TRUE(http->start());
        http->set_hosts("s", {"api.example.com"});

        net = std::make_unique<wisp::app::NetWiring>(wisp::app::NetWiringDeps{
            .platform = platform,
            .registry = registry,
            .report = report,
            .bus = bus,
            .storage = *storage,
            .models = *models,
            .system_settings = *system_settings,
            .power_manager = pm.get(),
            .wakelocks = &locks,
            .scene_http = http.get(),
        });
    }

    ~Rig() {
        net.reset();
        http.reset();
        pm.reset();
        bus.stop();
        wisp::log::reset_for_test();
    }

    void submit(const std::string& url) {
        wisp::net::SceneHttpRequest req;
        req.scene_id = "s";
        req.instance = 1;
        req.method = wisp::net::Method::Get;
        req.url = url;
        http->submit(std::move(req), [this](wisp::net::SceneHttpResult r) {
            std::lock_guard<std::mutex> lock(results_mutex);
            results.push_back(std::move(r));
        });
    }

    size_t result_count() {
        std::lock_guard<std::mutex> lock(results_mutex);
        return results.size();
    }

    std::string dir = wisp::test::temp_dir().string();
    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    wisp::core::BootReport report;
    wisp::core::WakeLockRegistry locks{bus};
    std::shared_ptr<FakeWifi> wifi;
    std::unique_ptr<wisp::core::StorageManager> storage;
    std::unique_ptr<wisp::port::sim::JsonFileSettingsBackend> backend;
    std::unique_ptr<wisp::core::ModelProfileService> models;
    std::unique_ptr<wisp::core::SystemSettings> system_settings;
    wisp::app::Platform platform;
    std::atomic<uint32_t> scan_ms{0};
    std::atomic<bool> ntp_locked{false};
    std::atomic<int> ntp_queries{0};
    std::unique_ptr<wisp::app::PowerManager> pm;
    FakeHttpClient inner;
    std::unique_ptr<LinkCheckingClient> client;
    std::unique_ptr<wisp::net::SceneHttp> http;
    std::unique_ptr<wisp::app::NetWiring> net;
    std::mutex results_mutex;
    std::vector<wisp::net::SceneHttpResult> results;
};

TEST(NetWiring, DeepStandbyDefersSceneRequestsUntilWifiIsBack) {
    Rig r;
    r.net->start();

    // Boot: the station connects and a time-sync attempt runs under
    // "system.ntp", which goes once it is over.
    ASSERT_TRUE(
        wait_until([&] { return r.net->wifi_state() == wisp::net::WifiState::Connected; }, 5000));
    ASSERT_TRUE(wait_until([&] { return r.ntp_queries.load() > 0; }, 5000));
    EXPECT_TRUE(r.ntp_locked.load());
    EXPECT_TRUE(wait_until([&] { return !holds(r.locks, "system.ntp"); }, 5000));

    // Awake and connected: a request goes straight out.
    r.submit("https://api.example.com/now");
    ASSERT_TRUE(wait_until([&] { return r.result_count() == 1; }, 5000));

    // Deep standby: the radio goes off and requests wait.
    r.pm->enter_standby();
    r.pm->tick(wisp::os::now_ms() + 3'600'000);
    ASSERT_EQ(r.pm->level(), PowerLevel::DeepStandby);
    ASSERT_TRUE(wait_until([&] { return !r.wifi->radio_on.load(); }, 5000));
    r.submit("https://api.example.com/later");
    wisp::os::sleep_ms(600);
    EXPECT_EQ(r.http->deferred_count(), 1u);
    EXPECT_EQ(r.inner.calls(), 1u);

    // Waking turns the radio on; the request goes out once the station is
    // connected, never onto a link that is still coming up.
    r.scan_ms = 500;
    r.pm->exit_standby();
    ASSERT_TRUE(wait_until([&] { return r.result_count() == 2; }, 5000));
    {
        std::lock_guard<std::mutex> lock(r.results_mutex);
        EXPECT_EQ(r.results[1].error, "");
        EXPECT_TRUE(r.results[1].deferred);
    }
    EXPECT_EQ(r.client->link_up(), (std::vector<bool>{true, true}));
    EXPECT_EQ(r.http->deferred_count(), 0u);
}

}  // namespace
