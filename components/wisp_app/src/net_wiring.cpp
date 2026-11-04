#include "net_wiring.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "wisp/app/ram_budget.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/hal/clock.hpp"
#include "wisp/hal/wifi.hpp"

namespace wisp::app {

namespace {

constexpr const char* kTag = "net";

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
                                                     std::move(unix_now), platform.pairing);
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
        const net::WifiState state = wifi_->status().state;
        wifi_state_.store(static_cast<int>(state));
        deps_.report.set_note("net", state == net::WifiState::Connecting ? "connecting" : "off");

        // Only for a prompt reaction: run() reads the state itself, standby
        // from PowerManager, so a late event cannot flip it.
        for (const char* topic : {"power.state", "net.wifi.state", "net.ap.state"}) {
            subscriptions_.push_back(
                deps_.bus.subscribe(topic, [this](const core::Event&) { wake_.give(); }));
        }
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
        // One id per boot, however often networking is restarted: a change
        // tells a browser the device rebooted (spec 6.2).
        static const std::string boot_id = admin::make_boot_id(platform.random);
        admin_ = std::make_unique<admin::AdminServer>(admin::AdminDeps{
            .registry = deps_.registry,
            .report = deps_.report,
            .storage = deps_.storage,
            .bus = deps_.bus,
            .wifi = *wifi_,
            .pairing = *pairing_,
            .models = deps_.models,
            .settings = deps_.system_settings,
            .assets = has_assets_ ? &assets_ : nullptr,
            .board = platform.board->name(),
            .firmware_version = platform.firmware_version,
            .boot_id = boot_id,
            .http_port = platform.http_port,
            .device_status = [this] { return device_status(); },
            .reboot = platform.reboot,
            .request_factory_reset = deps_.request_factory_reset,
            .firmware = deps_.firmware,
            .scenes = deps_.scenes,
            .scene_host = deps_.scene_host,
            .request_scene_delete = deps_.request_scene_delete,
        });
        reconcile_http(now);  // http_always_on serves from boot
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
    if (captive_serving_) {
        deps_.platform.captive_server->stop();
        captive_serving_ = false;
    }
    captive_retry_.reset();
    if (serving_) {
        deps_.platform.http_server->stop();
        serving_ = false;
    }
    start_retry_.reset();
    for (core::SubscriptionId id : subscriptions_) deps_.bus.unsubscribe(id);
    subscriptions_.clear();
    sta_up_.store(false);
    ap_up_.store(false);
    wifi_state_.store(-1);
    admin_.reset();
    wifi_.reset();
    pairing_.reset();
    assets_ = net::AssetPack();
    has_assets_ = false;
}

bool NetWiring::flush_admin_ws(uint32_t timeout_ms) {
    return admin_ ? admin_->flush_ws(timeout_ms) : true;
}

std::optional<net::WifiState> NetWiring::wifi_state() const {
    const int state = wifi_state_.load();
    if (state < 0) return std::nullopt;
    return static_cast<net::WifiState>(state);
}

void NetWiring::run() {
    while (!stopping_.load()) {
        const uint64_t now = os::now_ms();
        if (wifi_) {
            if (deps_.power_manager != nullptr) {
                wifi_->set_standby(deps_.power_manager->standby(), now);  // no-op if unchanged
            }
            wifi_->tick(now);
            const net::WifiState state = wifi_->status().state;
            wifi_state_.store(static_cast<int>(state));
            sta_up_.store(state == net::WifiState::Connected);
            ap_up_.store(wifi_->ap_status().active);
        }
        pairing_->tick(now);
        if (deps_.firmware != nullptr) deps_.firmware->tick(now);
        reconcile_http(now);
        (void)wake_.take(kNetTickMs);
    }
}

void NetWiring::StartRetry::failed(uint64_t now_ms) {
    at_ms = now_ms + wait_ms;
    wait_ms = std::min(wait_ms * 2, kStartRetryMaxMs);
}

void NetWiring::reconcile_http(uint64_t now_ms) {
    reconcile_captive(now_ms);
    net::HttpServer* server = deps_.platform.http_server.get();
    if (server == nullptr || !admin_) return;

    const bool want = deps_.platform.http_always_on || sta_up_.load() || ap_up_.load();
    if (!want) {
        start_retry_.reset();
        if (serving_) {
            server->stop();
            serving_ = false;
            WLOG_I(kTag, "no network interface up; admin server stopped");
        }
        return;
    }
    if (serving_ || !start_retry_.due(now_ms)) return;

    admin::AdminServer* admin = admin_.get();
    auto r = server->start(
        deps_.platform.http_port,
        [admin](const net::HttpRequest& req) { return admin->handle(req); }, admin->ws_handlers(),
        admin->stream_handler());
    if (!r) {
        WLOG_E(kTag, "admin server failed to start on port %u: %s; retrying in %u s",
               static_cast<unsigned>(deps_.platform.http_port), r.error().message.c_str(),
               static_cast<unsigned>(start_retry_.wait_ms / 1000));
        log_internal_ram();
        start_retry_.failed(now_ms);
        return;
    }
    start_retry_.reset();
    serving_ = true;
    WLOG_I(kTag, "admin server listening on port %u", static_cast<unsigned>(server->port()));
    log_internal_ram();
    deps_.platform.emit_line(
        "WISP_HTTP " + to_json(Value(Map{{"port", Value(static_cast<int64_t>(server->port()))}})));
}

void NetWiring::reconcile_captive(uint64_t now_ms) {
    net::HttpServer* server = deps_.platform.captive_server.get();
    if (server == nullptr || !admin_) return;

    if (!ap_up_.load()) {
        captive_retry_.reset();
        if (captive_serving_) {
            server->stop();
            captive_serving_ = false;
            WLOG_I(kTag, "hotspot down; captive redirect stopped");
        }
        return;
    }
    if (captive_serving_ || !captive_retry_.due(now_ms)) return;

    auto r = server->start(kCaptivePort, admin_->captive_handler(), net::WsHandlers{});
    if (!r) {
        WLOG_E(kTag, "captive redirect failed to start on port %u: %s; retrying in %u s",
               static_cast<unsigned>(kCaptivePort), r.error().message.c_str(),
               static_cast<unsigned>(captive_retry_.wait_ms / 1000));
        log_internal_ram();
        captive_retry_.failed(now_ms);
        return;
    }
    captive_retry_.reset();
    captive_serving_ = true;
    WLOG_I(kTag, "captive redirect listening on port %u", static_cast<unsigned>(kCaptivePort));
    log_internal_ram();
}

void NetWiring::log_internal_ram() const {
    if (!deps_.platform.heap_info) return;
    const InternalRam ram = internal_ram(deps_.platform.heap_info());
    if (ram.free < 0) return;
    if (internal_ram_low(ram)) {
        WLOG_W(kTag,
               "internal RAM low: %lld B free, largest block %lld B (idle floor %lld / %lld B, "
               "AGENTS.md section 5)",
               static_cast<long long>(ram.free), static_cast<long long>(ram.largest),
               static_cast<long long>(kIdleInternalFreeMin),
               static_cast<long long>(kIdleInternalLargestMin));
    } else {
        WLOG_I(kTag, "internal RAM: %lld B free, largest block %lld B",
               static_cast<long long>(ram.free), static_cast<long long>(ram.largest));
    }
}

Value NetWiring::device_status() const {
    Map status{
        // Same base as the log timestamps (os::now_ms(), spec 6.2).
        {"uptime_ms", Value(static_cast<int64_t>(os::now_ms()))},
        {"heap", deps_.platform.heap_info ? deps_.platform.heap_info() : Value(Map{})},
    };
    // PowerMonitor's last sample rather than a read of the power chip: the
    // monitor owns that device and samples it every second anyway.
    Value battery;  // null when unknown
    if (deps_.power_monitor != nullptr) {
        if (auto st = deps_.power_monitor->last_status()) {
            battery = Value(Map{
                {"percent", Value(st->battery_percent)},
                {"charging", Value(st->charging)},
                {"usb_present", Value(st->usb_present)},
            });
        }
    }
    status.emplace("battery", std::move(battery));
    return Value(std::move(status));
}

}  // namespace wisp::app
