#pragma once

// An App with a display, a FakeWifi and the networking services built, for
// the device-side network UI and factory-reset tests. Unlike
// self_check_screen_test.cpp's harness it supplies Platform::random (so
// WifiService/PairingService exist), a reboot hook that counts calls and
// records whether it ran on the UI thread, every emitted line, and
// optionally a FakeHttpServer serving the admin router and a
// FakeFirmwareUpdater behind the firmware upload.

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/fake_firmware.hpp"
#include "support/fake_http.hpp"
#include "support/fake_sntp.hpp"
#include "support/fake_wifi.hpp"
#include "support/temp_dir.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/board.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/hal/audio.hpp"
#include "wisp/hal/device.hpp"
#include "wisp/net/http.hpp"
#include "wisp/port/sim/settings_json_file.hpp"
#include "wisp/shell/shell.hpp"
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
    // Writes every `saved` network as parked (auth_failed), as three
    // rejected passwords in a row on an earlier boot leave it.
    bool saved_auth_failed = false;
    bool wifi_reachable = true;
    bool with_server = false;
    // Platform::pairing.code_ttl_ms; 0 keeps the two-minute default.
    uint32_t pairing_ttl_ms = 0;
    // Platform::pairing.min_code_interval_ms; -1 keeps the 10 s default.
    int64_t pairing_interval_ms = -1;
    // Platform::firmware (a FakeFirmwareUpdater for board "test_board").
    bool with_firmware = false;
    std::string firmware_version = "0.2.0";
    // Registers a FakeTouch as "touch.main" (the back gesture needs one).
    bool with_touch = false;
    // Platform::scenes_dir / scenes_data_dir (dev mode); empty: off.
    std::string scenes_dir;
    std::string scenes_data_dir;
    // Platform::sntp (a FakeSntp answering kSntpTime) and a set_wall_clock
    // that records each call in UiNetHarness::wall_clock_sets.
    bool with_sntp = false;
    // Registered as "audio_out.main" when set (the notification sounds).
    std::shared_ptr<wisp::hal::AudioOut> audio_out;
    // Registered as "imu.main" when set.
    std::shared_ptr<wisp::hal::Imu> imu;
    // Registered as they are (a mic, a talk key, ...).
    std::vector<std::shared_ptr<wisp::hal::Device>> extra_devices;
    // Platform::http_client, when set.
    std::function<std::unique_ptr<wisp::net::HttpClient>()> http_client;
};

// 2026-01-01 00:00:00 UTC, what the FakeSntp answers.
inline constexpr int64_t kSntpTime = 1767225600;

class UiNetBoard : public wisp::app::Board {
public:
    UiNetBoard(std::string root, std::shared_ptr<FakeWifi> wifi,
               std::shared_ptr<FakeDisplay> display, std::shared_ptr<FakeTouch> touch = nullptr,
               std::shared_ptr<wisp::hal::AudioOut> audio_out = nullptr,
               std::shared_ptr<wisp::hal::Imu> imu = nullptr,
               std::vector<std::shared_ptr<wisp::hal::Device>> extra = {})
        : root_(std::move(root)),
          wifi_(std::move(wifi)),
          display_(std::move(display)),
          touch_(std::move(touch)),
          audio_out_(std::move(audio_out)),
          imu_(std::move(imu)),
          extra_(std::move(extra)) {}

    std::string name() const override { return "fake-net-ui"; }

    void init(wisp::core::DeviceRegistry& registry, wisp::core::BootReport&) override {
        registry.add(std::make_shared<FakeStorage>("storage.int", root_, /*removable=*/false));
        registry.add(std::make_shared<FakeButton>("button.power"));
        if (wifi_) registry.add(wifi_);
        if (display_) registry.add(display_);
        if (touch_) registry.add(touch_);
        if (audio_out_) registry.add(audio_out_);
        if (imu_) registry.add(imu_);
        for (const auto& d : extra_) registry.add(d);
    }

private:
    std::string root_;
    std::shared_ptr<FakeWifi> wifi_;
    std::shared_ptr<FakeDisplay> display_;
    std::shared_ptr<FakeTouch> touch_;
    std::shared_ptr<wisp::hal::AudioOut> audio_out_;
    std::shared_ptr<wisp::hal::Imu> imu_;
    std::vector<std::shared_ptr<wisp::hal::Device>> extra_;
};

struct UiNetHarness {
    std::string root = temp_dir().string();
    std::shared_ptr<FakeWifi> wifi;
    std::shared_ptr<FakeDisplay> display;
    std::shared_ptr<FakeTouch> touch;
    FakeHttpServer* server = nullptr;         // owned by the App's Platform
    FakeFirmwareUpdater* firmware = nullptr;  // likewise
    FakeSntp* sntp = nullptr;                 // likewise
    std::mutex wall_clock_mutex;
    std::vector<int64_t> wall_clock_sets;
    std::atomic<int> reboots{0};
    std::atomic<bool> rebooted_on_ui_thread{false};
    std::unique_ptr<wisp::app::App> app;

    std::mutex lines_mutex;
    std::vector<std::string> lines;  // Platform::emit_line's output

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
        if (opts.with_touch) touch = std::make_shared<FakeTouch>("touch.main");
        if (!opts.saved.empty()) {
            wisp::List nets;
            for (const auto& [ssid, pw] : opts.saved) {
                wisp::Map entry{{"ssid", wisp::Value(ssid)}, {"password", wisp::Value(pw)}};
                if (opts.saved_auth_failed) entry.emplace("auth_failed", wisp::Value(true));
                nets.push_back(wisp::Value(std::move(entry)));
            }
            std::filesystem::create_directories(root + "/system/net");
            std::ofstream(root + "/system/net/wifi.json")
                << wisp::to_json_raw(wisp::Value(wisp::Map{{"networks", wisp::Value(nets)}}));
        }

        wisp::app::Platform platform;
        platform.board = std::make_unique<UiNetBoard>(root, wifi, display, touch, opts.audio_out,
                                                      opts.imu, opts.extra_devices);
        if (opts.http_client) platform.http_client = opts.http_client();
        platform.settings =
            std::make_unique<wisp::port::sim::JsonFileSettingsBackend>(settings_path());
        if (!opts.lang.empty()) {
            (void)wisp::core::Settings(*platform.settings, "sys").set("lang", opts.lang);
        }
        platform.emit_line = [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(lines_mutex);
            lines.push_back(line);
        };
        platform.firmware_version = opts.firmware_version;
        if (opts.with_firmware) {
            auto fw = std::make_unique<FakeFirmwareUpdater>();
            firmware = fw.get();
            platform.firmware = std::move(fw);
        }
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
        if (opts.with_sntp) {
            auto fake = std::make_unique<FakeSntp>([] { return kSntpTime; });
            sntp = fake.get();
            platform.sntp = std::move(fake);
            platform.set_wall_clock = [this](int64_t t) {
                std::lock_guard<std::mutex> lock(wall_clock_mutex);
                wall_clock_sets.push_back(t);
                return wisp::Result<void>::ok();
            };
        }
        platform.scenes_dir = opts.scenes_dir;
        platform.scenes_data_dir = opts.scenes_data_dir;
        if (opts.pairing_ttl_ms != 0) platform.pairing.code_ttl_ms = opts.pairing_ttl_ms;
        if (opts.pairing_interval_ms >= 0) {
            platform.pairing.min_code_interval_ms = static_cast<uint32_t>(opts.pairing_interval_ms);
        }
        app = std::make_unique<wisp::app::App>(std::move(platform));
    }

    ~UiNetHarness() {
        if (events_sub != 0) app->bus().unsubscribe(events_sub);
        app->stop();
    }

    std::string settings_path() const { return root + "/settings.json"; }

    // Whether a line starting with `prefix` has been emitted.
    bool emitted(const std::string& prefix) {
        std::lock_guard<std::mutex> lock(lines_mutex);
        for (const auto& l : lines) {
            if (l.rfind(prefix, 0) == 0) return true;
        }
        return false;
    }

    // Starts the App and records every event under `prefix` from then on.
    bool start(const std::string& prefix = "") {
        if (!app->start()) return false;
        events_sub = app->bus().subscribe(prefix, [this](const wisp::core::Event& e) {
            std::lock_guard<std::mutex> lock(events_mutex);
            events.push_back(e);
        });
        return true;
    }

    // Opens the hardware diagnostics (the self-check screen) over the home
    // screen the App boots into; true once they are in front.
    bool open_diagnostics() {
        if (app->ui() == nullptr || app->shell() == nullptr) return false;
        bool open = false;
        app->ui()->run_blocking([&] {
            app->shell()->open_diagnostics();
            open = app->shell()->diagnostics_open();
        });
        return open;
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
