#pragma once

// The admin backend's request router (spec 6.1, 6.3). The platform
// HttpServer hands every request to AdminServer::handle(); routing,
// filtering and authentication all happen here, so both servers (the
// simulator's socket server and esp_http_server) share one behaviour and
// tests drive it with in-memory requests.
//
// Pipeline, in order:
//   1. Host check (DNS-rebinding defence): after stripping a port the
//      Host must be a dotted-quad IPv4 address, "wisp.local" or
//      "localhost"; requests that arrived on the setup hotspot are exempt
//      because its DNS answers every name with the device, so captive
//      portal probes carry foreign Host values. Otherwise 403 bad_host.
//   2. Any method other than GET/POST/PUT/DELETE (notably OPTIONS) is 405
//      with no CORS headers: never answering a CORS preflight is what
//      keeps other origins from sending the X-Wisp header.
//   3. Captive-portal probe paths: 302 to the hotspot's setup page when
//      the request came from the hotspot, 404 otherwise.
//   4. Every non-/api/ path is a static file from the asset pack (gzip
//      body, ETag, 304 on a matching If-None-Match).
//   5. API: every route except the pairing whitelist needs a valid
//      wisp_token cookie (401 unauthorized), then POST/PUT/DELETE need
//      "X-Wisp: 1" (403 csrf, whitelist included), then the body must be
//      at most 8 KiB (413) and, when non-empty, valid JSON (400 bad_json).
//
// Threading: handle() runs on HTTP server threads, possibly several at
// once. AdminServer itself holds no mutable state; every service it calls
// is thread-safe.
//
// Secrets: tokens, pairing codes and cookie values never reach a log line
// or a response body.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "wisp/base/value.hpp"
#include "wisp/core/boot_report.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/llm_config.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/net/asset_pack.hpp"
#include "wisp/net/http.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/net/wifi_service.hpp"

namespace wisp::admin {

struct AdminDeps {
    core::DeviceRegistry& registry;
    core::BootReport& report;
    core::StorageManager& storage;
    core::EventBus& bus;
    net::WifiService& wifi;
    net::PairingService& pairing;
    core::LlmConfigService& llm;
    core::SystemSettings& settings;
    net::HttpClient* http_client;
    const net::AssetPack* assets;  // may be null: API only, every static path is 404
    std::string board;
    std::string firmware_version;
    std::string device_name = "Wisp";
    std::function<Value()> device_status;  // uptime_ms, heap, battery, ...
    std::function<void()> reboot;
    // Shows a confirmation dialog on the device; the outcome is published
    // as an event rather than returned.
    std::function<void()> request_factory_reset;
    // Monotonic os::now_ms() timebase; the only clock the router uses.
    std::function<uint64_t()> now_ms;
};

namespace detail {
struct Route;
}

class AdminServer {
public:
    explicit AdminServer(AdminDeps deps);
    ~AdminServer();

    AdminServer(const AdminServer&) = delete;
    AdminServer& operator=(const AdminServer&) = delete;

    net::HttpResponse handle(const net::HttpRequest& req);

    // WebSocket callbacks for /api/v1/ws. Until the log/event hub exists
    // every upgrade is rejected.
    net::WsHandlers ws_handlers();

private:
    net::HttpResponse handle_api(const net::HttpRequest& req);

    AdminDeps deps_;
    std::vector<detail::Route> routes_;
};

}  // namespace wisp::admin
