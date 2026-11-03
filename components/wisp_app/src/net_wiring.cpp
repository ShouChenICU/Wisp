#include "net_wiring.hpp"

#include <string>
#include <utility>

#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/hal/clock.hpp"
#include "wisp/hal/power.hpp"
#include "wisp/hal/wifi.hpp"

namespace wisp::app {

namespace {

constexpr const char* kTag = "net";

bool payload_string_is(const core::Event& e, const char* key, const char* want) {
    auto it = e.payload.find(key);
    return it != e.payload.end() && it->second.type() == Value::Type::String &&
           it->second.as_string() == want;
}

bool payload_bool(const core::Event& e, const char* key) {
    auto it = e.payload.find(key);
    return it != e.payload.end() && it->second.type() == Value::Type::Bool && it->second.as_bool();
}

}  // namespace

NetWiring::NetWiring(NetWiringDeps deps) : deps_(deps) {}

NetWiring::~NetWiring() { stop(); }

void NetWiring::start() {
    Platform& platform = deps_.platform;
    const uint64_t now = os::now_ms();

    auto wifi_dev = deps_.registry.get<hal::Wifi>("net.wifi");
    if (!platform.random) {
        // Tokens, codes and the hotspot password all need a strong RNG;
        // never fall back to a weak one.
        WLOG_E(kTag, "Platform::random missing; networking disabled");
        deps_.report.set_note("net", "missing");
        return;
    }

    auto clock = deps_.registry.get<hal::Clock>("clock.main");
    auto unix_now = [clock]() -> int64_t {
        if (!clock) return 0;
        auto t = clock->get_unix();
        return t ? t.value() : 0;
    };
    pairing_ = std::make_unique<net::PairingService>(deps_.storage, deps_.bus, platform.random,
                                                     std::move(unix_now));
    if (auto r = pairing_->init(); !r) {
        WLOG_W(kTag, "pairing: %s", r.error().message.c_str());
    }

    if (wifi_dev) {
        wifi_ = std::make_unique<net::WifiService>(wifi_dev, deps_.storage, deps_.bus,
                                                   core::Settings(*platform.settings, "net"),
                                                   platform.random);
        if (auto r = wifi_->init(now); !r) {
            WLOG_W(kTag, "wifi: %s", r.error().message.c_str());
        }
        deps_.report.set_note(
            "net", wifi_->status().state == net::WifiState::Connecting ? "connecting" : "off");

        net::WifiService* wifi = wifi_.get();
        subscriptions_.push_back(deps_.bus.subscribe("power.state", [wifi](const core::Event& e) {
            wifi->set_standby(payload_string_is(e, "state", "standby"), os::now_ms());
        }));
        subscriptions_.push_back(
            deps_.bus.subscribe("net.wifi.state", [this](const core::Event& e) {
                sta_up_.store(payload_string_is(e, "state", "connected"));
                wake_.give();
            }));
        subscriptions_.push_back(deps_.bus.subscribe("net.ap.state", [this](const core::Event& e) {
            ap_up_.store(payload_bool(e, "active"));
            wake_.give();
        }));
    } else {
        deps_.report.set_note("net", "missing");
    }

    if (!platform.assets.empty()) {
        auto pack = net::AssetPack::parse(platform.assets);
        if (pack) {
            assets_ = std::move(pack.value());
            has_assets_ = true;
        } else {
            WLOG_W(kTag, "web assets unusable (%s); serving the API only",
                   pack.error().message.c_str());
        }
    }

    // AdminDeps holds the Wi-Fi service by reference, so without a radio
    // there is no admin backend (nothing to set up the device with anyway).
    if (platform.http_server && wifi_) {
        admin_ = std::make_unique<admin::AdminServer>(admin::AdminDeps{
            .registry = deps_.registry,
            .report = deps_.report,
            .storage = deps_.storage,
            .bus = deps_.bus,
            .wifi = *wifi_,
            .pairing = *pairing_,
            .llm = deps_.llm,
            .settings = deps_.system_settings,
            .http_client = platform.http_client.get(),
            .assets = has_assets_ ? &assets_ : nullptr,
            .board = platform.board->name(),
            .firmware_version = platform.firmware_version,
            .device_status = [this] { return device_status(); },
            .reboot = platform.reboot,
            // The on-device confirmation dialog is not wired yet; the route
            // answers 503 until it is.
            .request_factory_reset = {},
        });
        reconcile_http();  // http_always_on serves from boot
    }

    stopping_.store(false);
    task_ = os::Task::spawn({.name = "wisp.net", .stack_bytes = 6144}, [this] { run(); });
}

void NetWiring::stop() {
    if (task_) {
        stopping_.store(true);
        wake_.give();
        task_->join();
        task_.reset();
    }
    if (serving_) {
        deps_.platform.http_server->stop();
        serving_ = false;
    }
    start_failed_ = false;
    for (core::SubscriptionId id : subscriptions_) deps_.bus.unsubscribe(id);
    subscriptions_.clear();
    sta_up_.store(false);
    ap_up_.store(false);
    admin_.reset();
    wifi_.reset();
    pairing_.reset();
    assets_ = net::AssetPack();
    has_assets_ = false;
}

void NetWiring::run() {
    while (!stopping_.load()) {
        const uint64_t now = os::now_ms();
        if (wifi_) wifi_->tick(now);
        pairing_->tick(now);
        reconcile_http();
        (void)wake_.take(kNetTickMs);
    }
}

void NetWiring::reconcile_http() {
    net::HttpServer* server = deps_.platform.http_server.get();
    if (server == nullptr || !admin_) return;

    const bool want = deps_.platform.http_always_on || sta_up_.load() || ap_up_.load();
    if (!want) {
        start_failed_ = false;
        if (serving_) {
            server->stop();
            serving_ = false;
            WLOG_I(kTag, "no network interface up; admin server stopped");
        }
        return;
    }
    if (serving_ || start_failed_) return;

    admin::AdminServer* admin = admin_.get();
    auto r = server->start(
        deps_.platform.http_port,
        [admin](const net::HttpRequest& req) { return admin->handle(req); }, admin->ws_handlers());
    if (!r) {
        start_failed_ = true;
        WLOG_E(kTag, "admin server failed to start on port %u: %s",
               static_cast<unsigned>(deps_.platform.http_port), r.error().message.c_str());
        return;
    }
    serving_ = true;
    WLOG_I(kTag, "admin server listening on port %u", static_cast<unsigned>(server->port()));
    deps_.platform.emit_line(
        "WISP_HTTP " + to_json(Value(Map{{"port", Value(static_cast<int64_t>(server->port()))}})));
}

Value NetWiring::device_status() const {
    Map status{
        {"uptime_ms", Value(static_cast<int64_t>(os::now_ms() - deps_.boot_t0_ms))},
        {"heap", deps_.platform.heap_info ? deps_.platform.heap_info() : Value(Map{})},
    };
    Value battery;  // null when unknown
    if (auto power = deps_.registry.get<hal::Power>("power.main")) {
        if (auto st = power->status(); st) {
            battery = Value(Map{
                {"percent", Value(st.value().battery_percent)},
                {"charging", Value(st.value().charging)},
                {"usb_present", Value(st.value().usb_present)},
            });
        }
    }
    status.emplace("battery", std::move(battery));
    return Value(std::move(status));
}

}  // namespace wisp::app
