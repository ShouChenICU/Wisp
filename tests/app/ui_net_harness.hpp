#pragma once

// An App with a display, a FakeWifi and the networking services built, for
// the device-side network UI and factory-reset tests. Unlike
// self_check_screen_test.cpp's harness it supplies Platform::random (so
// WifiService/PairingService exist), a reboot hook that counts calls and
// records whether it ran on the UI thread, and optionally a FakeHttpServer
// serving the admin router.

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
#include "support/fake_wifi.hpp"
#include "support/temp_dir.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/board.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/port/sim/settings_json_file.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::test {

struct UiNetOptions {
    bool with_wifi = true;
    bool with_display = true;
    std::string shape = "rounded_rect";
    // Settings sys/lang to boot with; empty: unset (Chinese).
    std::string lang;
    // Written to <System>/net/wifi.json before boot.
    std::vector<std::pair<std::string, std::string>> saved;
    bool wifi_reachable = true;
    bool with_server = false;
    // Platform::pairing.code_ttl_ms; 0 keeps the two-minute default.
    uint32_t pairing_ttl_ms = 0;
};

class UiNetBoard : public wisp::app::Board {
public:
    UiNetBoard(std::string root, std::shared_ptr<FakeWifi> wifi,
               std::shared_ptr<FakeDisplay> display)
        : root_(std::move(root)), wifi_(std::move(wifi)), display_(std::move(display)) {}

    std::string name() const override { return "fake-net-ui"; }

    void init(wisp::core::DeviceRegistry& registry, wisp::core::BootReport&) override {
        registry.add(std::make_shared<FakeStorage>("storage.int", root_, /*removable=*/false));
        registry.add(std::make_shared<FakeButton>("button.power"));
        if (wifi_) registry.add(wifi_);
        if (display_) registry.add(display_);
    }

private:
    std::string root_;
    std::shared_ptr<FakeWifi> wifi_;
    std::shared_ptr<FakeDisplay> display_;
};

struct UiNetHarness {
    std::string root = temp_dir().string();
    std::shared_ptr<FakeWifi> wifi;
    std::shared_ptr<FakeDisplay> display;
    FakeHttpServer* server = nullptr;  // owned by the App's Platform
    std::atomic<int> reboots{0};
    std::atomic<bool> rebooted_on_ui_thread{false};
    std::unique_ptr<wisp::app::App> app;

    std::mutex events_mutex;
    std::vector<wisp::core::Event> events;
    wisp::core::SubscriptionId events_sub = 0;

    explicit UiNetHarness(const UiNetOptions& opts = {}) {
        if (opts.with_wifi) {
            wifi = std::make_shared<FakeWifi>("net.wifi");
            wifi->networks = {{"Home", -50, true}, {"Cafe", -75, false}};
            wifi->passwords = {{"Home", "password1"}};
            wifi->reachable = opts.wifi_reachable;
        }
        if (opts.with_display) {
            display = std::make_shared<FakeDisplay>("display.main", 480, 480, /*area_align=*/1,
                                                    opts.shape);
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
        platform.board = std::make_unique<UiNetBoard>(root, wifi, display);
        platform.settings =
            std::make_unique<wisp::port::sim::JsonFileSettingsBackend>(settings_path());
        if (!opts.lang.empty()) {
            (void)wisp::core::Settings(*platform.settings, "sys").set("lang", opts.lang);
        }
        platform.emit_line = [](const std::string&) {};
        platform.random = [](uint8_t* out, size_t len) {
            static std::atomic<uint32_t> counter{1};
            for (size_t i = 0; i < len; ++i) {
                out[i] = static_cast<uint8_t>(counter.fetch_add(97) * 31 + i * 7);
            }
        };
        platform.reboot = [this] {
            if (app && app->ui() != nullptr && app->ui()->on_ui_thread()) {
                rebooted_on_ui_thread.store(true);
            }
            reboots.fetch_add(1);
        };
        if (opts.with_server) {
            auto srv = std::make_unique<FakeHttpServer>();
            server = srv.get();
            platform.http_server = std::move(srv);
            platform.http_always_on = true;
        }
        if (opts.pairing_ttl_ms != 0) platform.pairing.code_ttl_ms = opts.pairing_ttl_ms;
        app = std::make_unique<wisp::app::App>(std::move(platform));
    }

    ~UiNetHarness() {
        if (events_sub != 0) app->bus().unsubscribe(events_sub);
        app->stop();
    }

    std::string settings_path() const { return root + "/settings.json"; }

    // Starts the App and records every event under `prefix` from then on.
    bool start(const std::string& prefix = "") {
        if (!app->start()) return false;
        events_sub = app->bus().subscribe(prefix, [this](const wisp::core::Event& e) {
            std::lock_guard<std::mutex> lock(events_mutex);
            events.push_back(e);
        });
        return true;
    }

    std::vector<wisp::core::Event> topic(const std::string& t) {
        app->bus().wait_idle(2000);
        std::lock_guard<std::mutex> lock(events_mutex);
        std::vector<wisp::core::Event> out;
        for (const auto& e : events) {
            if (e.topic == t) out.push_back(e);
        }
        return out;
    }
};

}  // namespace wisp::test
