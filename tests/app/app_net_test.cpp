// App boot wiring for networking (connectivity spec 4.1): WifiService,
// PairingService and the admin HTTP server, driven through a FakeWifi and
// a FakeHttpServer on a headless App.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/fake_http.hpp"
#include "support/fake_wifi.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/board.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/llm_config.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/settings_json_file.hpp"

using wisp::net::WifiState;
using wisp::test::FakeDisplay;
using wisp::test::FakeHttpServer;
using wisp::test::FakeStorage;
using wisp::test::FakeWifi;
using wisp::test::wait_until;

namespace {

struct NetAppOptions {
    bool with_wifi = true;
    bool with_server = true;
    bool with_captive = true;
    bool always_on = false;
    // Written to <System>/net/wifi.json before boot.
    std::vector<std::pair<std::string, std::string>> saved;
    int64_t idle_off_min = -1;  // -1: leave the setting unset
    bool with_display = false;
};

class NetBoard : public wisp::app::Board {
public:
    NetBoard(std::string root, std::shared_ptr<FakeWifi> wifi, std::shared_ptr<FakeDisplay> display)
        : root_(std::move(root)), wifi_(std::move(wifi)), display_(std::move(display)) {}

    std::string name() const override { return "fake"; }

    void init(wisp::core::DeviceRegistry& registry, wisp::core::BootReport&) override {
        registry.add(std::make_shared<FakeStorage>("storage.int", root_, /*removable=*/false));
        if (wifi_) registry.add(wifi_);
        if (display_) registry.add(display_);
    }

private:
    std::string root_;
    std::shared_ptr<FakeWifi> wifi_;
    std::shared_ptr<FakeDisplay> display_;
};

struct NetHarness {
    std::string root = wisp::test::temp_dir().string();
    std::shared_ptr<FakeWifi> wifi;
    std::shared_ptr<FakeDisplay> display;
    FakeHttpServer* server = nullptr;   // owned by the App's Platform
    FakeHttpServer* captive = nullptr;  // likewise
    std::mutex lines_mutex;
    std::vector<std::string> lines;
    std::unique_ptr<wisp::app::App> app;

    explicit NetHarness(const NetAppOptions& opts) {
        if (opts.with_wifi) {
            wifi = std::make_shared<FakeWifi>("net.wifi");
            wifi->networks = {{"Home", -50, true}, {"Cafe", -75, false}};
            wifi->passwords = {{"Home", "password1"}};
        }
        if (opts.with_display) {
            display = std::make_shared<FakeDisplay>("display.main", 480, 480, /*area_align=*/1);
        }
        if (!opts.saved.empty()) {
            wisp::List nets;
            for (const auto& [ssid, pw] : opts.saved) {
                nets.push_back(wisp::Value(
                    wisp::Map{{"ssid", wisp::Value(ssid)}, {"password", wisp::Value(pw)}}));
            }
            std::filesystem::create_directories(root + "/system/net");
            std::ofstream(root + "/system/net/wifi.json")
                << wisp::to_json(wisp::Value(wisp::Map{{"networks", wisp::Value(nets)}}));
        }

        wisp::app::Platform platform;
        platform.board = std::make_unique<NetBoard>(root, wifi, display);
        platform.settings =
            std::make_unique<wisp::port::sim::JsonFileSettingsBackend>(root + "/settings.json");
        if (opts.idle_off_min >= 0) {
            wisp::core::Settings(*platform.settings, "net").set("idle_off_min", opts.idle_off_min);
        }
        platform.emit_line = [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(lines_mutex);
            lines.push_back(line);
        };
        platform.random = [](uint8_t* out, size_t len) {
            for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(i * 37 + 11);
        };
        if (opts.with_server) {
            auto srv = std::make_unique<FakeHttpServer>();
            server = srv.get();
            platform.http_server = std::move(srv);
        }
        if (opts.with_captive) {
            auto srv = std::make_unique<FakeHttpServer>();
            captive = srv.get();
            platform.captive_server = std::move(srv);
        }
        platform.http_port = 8088;
        platform.http_always_on = opts.always_on;
        app = std::make_unique<wisp::app::App>(std::move(platform));
    }

    std::vector<std::string> snapshot() {
        std::lock_guard<std::mutex> lock(lines_mutex);
        return lines;
    }

    wisp::Value boot_report() {
        for (const auto& line : snapshot()) {
            if (line.rfind("WISP_BOOT_REPORT ", 0) == 0) {
                return wisp::from_json(line.substr(17)).value();
            }
        }
        ADD_FAILURE() << "no WISP_BOOT_REPORT line";
        return {};
    }

    std::string net_note() {
        const wisp::Value report = boot_report();
        const wisp::Value* note = report.find("notes")->find("net");
        return note != nullptr ? note->as_string() : "<absent>";
    }

    int http_lines() {
        auto all = snapshot();
        return static_cast<int>(std::count_if(all.begin(), all.end(), [](const std::string& l) {
            return l == "WISP_HTTP {\"port\":8088}";
        }));
    }
};

wisp::net::HttpRequest info_request() {
    wisp::net::HttpRequest req;
    req.method = wisp::net::Method::Get;
    req.path = "/api/v1/info";
    req.headers["host"] = "192.168.1.50";
    return req;
}

}  // namespace

TEST(AppNet, BootReportHasNetNote) {
    {
        NetHarness h({});
        ASSERT_TRUE(h.app->start());
        EXPECT_EQ(h.net_note(), "off");
        EXPECT_NE(h.app->wifi(), nullptr);
        EXPECT_NE(h.app->pairing(), nullptr);
    }
    {
        NetHarness h({.with_wifi = false});
        ASSERT_TRUE(h.app->start());
        EXPECT_EQ(h.net_note(), "missing");
        EXPECT_EQ(h.app->wifi(), nullptr);
    }
    {
        NetHarness h({.saved = {{"Home", "password1"}}});
        ASSERT_TRUE(h.app->start());
        EXPECT_EQ(h.net_note(), "connecting");
    }
}

TEST(AppNet, StartsHttpServerWhenStaConnects) {
    NetHarness h({});
    ASSERT_TRUE(h.app->start());
    ASSERT_NE(h.app->wifi(), nullptr);
    EXPECT_FALSE(h.server->running());
    EXPECT_EQ(h.server->start_calls.load(), 0);

    ASSERT_TRUE(h.app->wifi()->add_network("Home", "password1", wisp::os::now_ms()));
    ASSERT_TRUE(wait_until([&] { return h.server->running(); }, 3000));
    EXPECT_EQ(h.server->port(), 8088);
    EXPECT_TRUE(wait_until([&] { return h.http_lines() == 1; }, 1000));

    // Requests reach the admin router.
    auto res = h.server->handle(info_request());
    EXPECT_EQ(res.status, 200);
    EXPECT_NE(res.body.find("\"paired\":false"), std::string::npos) << res.body;

    // Losing the only interface stops the server again.
    h.wifi->reachable = false;
    h.wifi->set_link({});
    EXPECT_TRUE(wait_until([&] { return !h.server->running(); }, 3000));

    h.wifi->reachable = true;
    h.app->stop();
    EXPECT_FALSE(h.server->running());
    EXPECT_EQ(h.app->wifi(), nullptr);
}

TEST(AppNet, HotspotStartsHttpServer) {
    NetHarness h({});
    ASSERT_TRUE(h.app->start());
    ASSERT_TRUE(h.app->wifi()->start_provisioning(wisp::os::now_ms()));
    EXPECT_TRUE(wait_until([&] { return h.server->running(); }, 3000));
    ASSERT_TRUE(h.app->wifi()->stop_provisioning());
    EXPECT_TRUE(wait_until([&] { return !h.server->running(); }, 3000));
}

TEST(AppNet, AlwaysOnStartsServerWithoutNetwork) {
    NetHarness h({.always_on = true});
    ASSERT_TRUE(h.app->start());
    EXPECT_TRUE(h.server->running());
    EXPECT_EQ(h.http_lines(), 1);
    EXPECT_EQ(h.server->handle(info_request()).status, 200);

    // A link that comes and goes leaves it running.
    ASSERT_TRUE(h.app->wifi()->add_network("Home", "password1", wisp::os::now_ms()));
    ASSERT_TRUE(
        wait_until([&] { return h.app->wifi()->status().state == WifiState::Connected; }, 3000));
    h.wifi->reachable = false;
    h.wifi->set_link({});
    ASSERT_TRUE(
        wait_until([&] { return h.app->wifi()->status().state == WifiState::Backoff; }, 3000));
    wisp::os::sleep_ms(600);
    EXPECT_TRUE(h.server->running());
    EXPECT_EQ(h.server->stop_calls.load(), 0);
    EXPECT_EQ(h.server->start_calls.load(), 1);
}

TEST(AppNet, FailedServerStartDoesNotFailBoot) {
    NetHarness h({.always_on = true});
    h.server->fail_start = true;
    ASSERT_TRUE(h.app->start());
    EXPECT_FALSE(h.server->running());
    EXPECT_EQ(h.http_lines(), 0);
    // Not retried in a loop while nothing changed.
    wisp::os::sleep_ms(600);
    EXPECT_EQ(h.server->start_calls.load(), 1);
}

TEST(AppNet, StandbyEventReachesWifiService) {
    NetHarness h({.saved = {{"Home", "password1"}}, .idle_off_min = 0});
    ASSERT_TRUE(h.app->start());
    ASSERT_TRUE(
        wait_until([&] { return h.app->wifi()->status().state == WifiState::Connected; }, 3000));

    h.app->power_manager()->enter_standby();
    EXPECT_TRUE(wait_until([&] { return h.app->wifi()->status().state == WifiState::Off; }, 3000));
    EXPECT_GE(h.wifi->disconnect_calls.load(), 1);

    h.app->power_manager()->exit_standby();
    EXPECT_TRUE(
        wait_until([&] { return h.app->wifi()->status().state == WifiState::Connected; }, 3000));
}

TEST(AppNet, BootDoesNotWaitForWifi) {
    NetHarness h({.saved = {{"Home", "password1"}}});
    h.wifi->on_scan = [] { wisp::os::sleep_ms(3000); };
    uint64_t t0 = wisp::os::now_ms();
    ASSERT_TRUE(h.app->start());
    EXPECT_LT(wisp::os::now_ms() - t0, 1000u);
    EXPECT_EQ(h.net_note(), "connecting");
    // The scan does run, on the net task.
    EXPECT_TRUE(wait_until([&] { return h.wifi->scan_calls.load() >= 1; }, 2000));
}

TEST(AppNet, BrightnessSettingReachesTheDisplay) {
    NetHarness h({.with_wifi = false, .with_server = false, .with_display = true});
    ASSERT_TRUE(h.app->start());
    ASSERT_NE(h.app->ui(), nullptr);
    // Called off the UI thread, as an HTTP handler would.
    ASSERT_TRUE(h.app->system_settings().set({{"brightness", 120}}));
    EXPECT_EQ(h.app->system_settings().get().brightness, 120);
    EXPECT_TRUE(wait_until([&] { return h.display->brightness() == 120; }, 2000));
    EXPECT_FALSE(h.app->llm().get().has_key);
}

TEST(AppNet, AdminPortDefaultsTo8900) {
    wisp::app::Platform platform;
    EXPECT_EQ(platform.http_port, 8900);
    EXPECT_EQ(platform.captive_server, nullptr);
}

TEST(AppNet, HotspotRunsCaptiveRedirectOnPort80) {
    NetHarness h({.always_on = true});
    ASSERT_TRUE(h.app->start());
    EXPECT_TRUE(h.server->running());
    EXPECT_FALSE(h.captive->running());
    EXPECT_EQ(h.captive->start_calls.load(), 0);

    ASSERT_TRUE(h.app->wifi()->start_provisioning(wisp::os::now_ms()));
    ASSERT_TRUE(wait_until([&] { return h.captive->running(); }, 3000));
    EXPECT_EQ(h.captive->port(), 80);

    // Any request on it is steered to the admin port's setup page.
    auto req = info_request();
    req.iface = wisp::net::Iface::Ap;
    req.headers["host"] = "connectivitycheck.gstatic.com";
    req.path = "/generate_204";
    auto res = h.captive->handle(req);
    EXPECT_EQ(res.status, 302);
    EXPECT_EQ(wisp::test::response_header(res, "Location"), "http://192.168.4.1:8088/setup.html");

    ASSERT_TRUE(h.app->wifi()->stop_provisioning());
    EXPECT_TRUE(wait_until([&] { return !h.captive->running(); }, 3000));
    // The admin server is unaffected, and WISP_HTTP only ever names it.
    EXPECT_TRUE(h.server->running());
    EXPECT_EQ(h.http_lines(), 1);

    // stop() takes a running captive server down too.
    ASSERT_TRUE(h.app->wifi()->start_provisioning(wisp::os::now_ms()));
    ASSERT_TRUE(wait_until([&] { return h.captive->running(); }, 3000));
    h.app->stop();
    EXPECT_FALSE(h.captive->running());
}

TEST(AppNet, NoCaptiveServerWithoutAdminServer) {
    NetHarness h({.with_server = false});
    ASSERT_TRUE(h.app->start());
    ASSERT_TRUE(h.app->wifi()->start_provisioning(wisp::os::now_ms()));
    wisp::os::sleep_ms(600);
    EXPECT_EQ(h.captive->start_calls.load(), 0);
}
