#pragma once

// Shared fixture for the wisp_admin router tests: real services (Wi-Fi
// over a FakeWifi, pairing, LLM config, system settings) over a temp
// directory, a FakeHttpClient for the LLM probe, counters for the reboot
// and factory-reset hooks, the fixture WispPack that tools/pack_assets.py
// builds from tests/net/fixtures/web, and a settable monotonic clock.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/fake_http.hpp"
#include "support/fake_wifi.hpp"
#include "support/temp_dir.hpp"
#include "wisp/admin/admin_server.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/boot_report.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/llm_config.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/net/asset_pack.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/port/sim/settings_json_file.hpp"

namespace wisp::test {

struct AdminFixture {
    explicit AdminFixture(bool with_assets = true) : dir(temp_dir().string()) {
        registry.add(std::make_shared<FakeStorage>("storage.int", dir, /*removable=*/false));
        storage = std::make_unique<core::StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        bus.start();
        backend = std::make_unique<port::sim::JsonFileSettingsBackend>(dir + "/settings.json");
        fake_wifi = std::make_shared<FakeWifi>("net.wifi");
        auto random = [this](uint8_t* out, size_t len) {
            for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(rng());
        };
        wifi = std::make_unique<net::WifiService>(fake_wifi, *storage, bus,
                                                  core::Settings(*backend, "net"), random);
        EXPECT_TRUE(wifi->init(now));
        pairing = std::make_unique<net::PairingService>(*storage, bus, random,
                                                        [] { return int64_t{1767225600}; });
        EXPECT_TRUE(pairing->init());
        llm = std::make_unique<core::LlmConfigService>(*storage);
        EXPECT_TRUE(llm->init());
        settings = std::make_unique<core::SystemSettings>(*backend, nullptr);

        if (with_assets) {
            std::ifstream in(WISP_FIXTURE_WPAK, std::ios::binary);
            pack_bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            auto parsed = net::AssetPack::parse(pack_bytes);
            EXPECT_TRUE(parsed) << "fixture pack " << WISP_FIXTURE_WPAK;
            if (parsed) assets = std::move(parsed.value());
        }

        boot_id = admin::make_boot_id(random);
        srv = std::make_unique<admin::AdminServer>(make_deps());
    }

    // The router's dependencies over this fixture's services. Without
    // `with_clock`, now_ms is left at its default.
    admin::AdminDeps make_deps(bool with_clock = true) {
        admin::AdminDeps deps{
            .registry = registry,
            .report = report,
            .storage = *storage,
            .bus = bus,
            .wifi = *wifi,
            .pairing = *pairing,
            .llm = *llm,
            .settings = *settings,
            .http_client = &http,
            .assets = assets ? &*assets : nullptr,
            .board = "sim",
            .firmware_version = "0.2.0-test",
            .device_name = "Wisp",
            .boot_id = boot_id,
            .device_status = [] { return Value(Map{}); },
            .reboot = [this] { ++reboots; },
            .request_factory_reset = [this] { ++factory_resets; },
        };
        if (with_clock) deps.now_ms = [this] { return now.load(); };
        return deps;
    }

    ~AdminFixture() {
        srv.reset();
        pairing.reset();
        wifi.reset();
        bus.stop();
    }

    // Pairs through the service directly (not the routes) and returns the
    // token, for tests that only need an authenticated browser.
    std::string token() {
        // Codes are issued at most once per 10 s: step the clock past that
        // when an earlier token() just used one.
        now += pairing->code_wait_ms(now);
        EXPECT_TRUE(pairing->request_code(now));
        auto code = pairing->active_code(now);
        EXPECT_TRUE(code.has_value());
        auto t = pairing->confirm(code.value_or(""), net::ClientInfo{"test", "192.168.1.23"}, now);
        EXPECT_TRUE(t);
        return t ? t.value() : std::string();
    }

    std::string dir;
    core::DeviceRegistry registry;
    core::EventBus bus;
    core::BootReport report;
    std::unique_ptr<core::StorageManager> storage;
    std::unique_ptr<port::sim::JsonFileSettingsBackend> backend;
    std::shared_ptr<FakeWifi> fake_wifi;
    std::unique_ptr<net::WifiService> wifi;
    std::unique_ptr<net::PairingService> pairing;
    std::unique_ptr<core::LlmConfigService> llm;
    std::unique_ptr<core::SystemSettings> settings;
    std::vector<uint8_t> pack_bytes;
    std::optional<net::AssetPack> assets;
    FakeHttpClient http;
    std::atomic<int> reboots{0};
    std::atomic<int> factory_resets{0};
    std::atomic<uint64_t> now{1000};
    std::mt19937 rng{12345};
    std::string boot_id;  // made from rng once, like a boot
    std::unique_ptr<admin::AdminServer> srv;
};

// Parses a response body; a non-JSON body yields Null (and a failure).
inline Value body_json(const net::HttpResponse& res) {
    auto v = from_json(res.body);
    EXPECT_TRUE(v) << "not JSON: " << res.body;
    return v ? v.value() : Value();
}

// The "code" of an {"error":{"code","message"}} body, or "".
inline std::string error_code(const net::HttpResponse& res) {
    Value v = body_json(res);
    const Value* err = v.find("error");
    const Value* code = err ? err->find("code") : nullptr;
    return code && code->type() == Value::Type::String ? code->as_string() : std::string();
}

}  // namespace wisp::test
