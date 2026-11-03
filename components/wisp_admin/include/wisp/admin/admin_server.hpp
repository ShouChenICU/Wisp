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
//      "localhost". On the setup hotspot, whose DNS answers every name
//      with the device, a foreign Host is tolerated only for captive-
//      portal probe paths, and any other GET under a foreign Host is
//      redirected to the setup page; everything else (in particular every
//      API call, /pair/ap included) is 403 bad_host on every interface.
//   2. Any method other than GET/POST/PUT/DELETE (notably OPTIONS) is 405
//      with no CORS headers: never answering a CORS preflight is what
//      keeps other origins from sending the X-Wisp header.
//   3. Captive-portal probe paths: 302 to the hotspot's setup page
//      (http://<hotspot ip>:<http_port>/setup.html) when the request came
//      from the hotspot, 404 otherwise.
//   4. Every non-/api/ path is a static file from the asset pack (gzip
//      body, ETag, 304 on a matching If-None-Match).
//   5. API: every route except the pairing whitelist needs a valid
//      wisp_token cookie (401 unauthorized), then POST/PUT/DELETE need
//      "X-Wisp: 1" (403 csrf, whitelist included), then the body must be
//      at most 8 KiB (413) and, when non-empty, valid JSON (400 bad_json).
//
// WebSocket (/api/v1/ws): the upgrade passes the same Host check and needs
// the same wisp_token cookie as the API; accepted connections are handed
// to a WsHub (wisp/admin/ws_hub.hpp) that AdminServer owns and whose
// sender task runs for the AdminServer's lifetime. Revoking a client
// (logout, DELETE /clients/{id}, factory reset) closes the connections
// its token opened, through a PairingService revoke listener.
//
// Threading: handle() runs on HTTP server threads, possibly several at
// once. AdminServer's own mutable state is the reboot timer (os::Timer
// is safe to start from any thread) and the WsHub (thread-safe); every
// service it calls is thread-safe. The reboot route only arms that timer, which calls
// AdminDeps::reboot about 500 ms later on the timer's thread so the 202
// is sent first; destroying the AdminServer cancels a pending reboot.
//
// Secrets: tokens, pairing codes and cookie values never reach a log line
// or a response body.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "wisp/admin/ws_hub.hpp"
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
#include "wisp/net/random.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/os/os.hpp"

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
    // This boot's id (make_boot_id()), /info's "boot" and the WebSocket
    // hello's: a browser seeing it change knows the device restarted and
    // its log seq numbers started over (spec 6.1, 6.2).
    std::string boot_id;
    // The port the admin server listens on (spec 6.1); redirects to the
    // setup page point at it.
    uint16_t http_port = 8900;
    std::function<Value()> device_status;  // uptime_ms, heap, battery, ...
    std::function<void()> reboot;
    // Shows a confirmation dialog on the device; the outcome is published
    // as an event rather than returned.
    std::function<void()> request_factory_reset;
    // Monotonic os::now_ms() timebase; the only clock the router uses.
    // Tests inject their own; left empty, AdminServer uses os::now_ms().
    std::function<uint64_t()> now_ms = [] { return os::now_ms(); };
};

// A fresh boot id: 8 random bytes as 16 lowercase hex characters. Made
// once per boot by the caller and passed in as AdminDeps::boot_id.
std::string make_boot_id(const net::RandomFn& random);

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

    // Handler for the hotspot's port-80 server, which exists only so that
    // phones' captive-portal probes open the setup page: every request,
    // whatever its method, path or Host, is a 302 (Cache-Control:
    // no-store) to http://<hotspot ip>:<http_port>/setup.html, or 503
    // while no hotspot is up. No API, static files or WebSocket. Captures
    // this AdminServer, like ws_handlers().
    net::HttpHandler captive_handler();

    // WebSocket callbacks for /api/v1/ws. on_open refuses an upgrade with
    // a foreign Host or without a valid wisp_token cookie, and otherwise
    // lets the hub decide (at most two connections). The handlers capture
    // this AdminServer, so the HttpServer must be stopped before it is
    // destroyed.
    net::WsHandlers ws_handlers();

private:
    net::HttpResponse handle_api(const net::HttpRequest& req);

    AdminDeps deps_;
    std::vector<detail::Route> routes_;
    // Declared after deps_ so it is destroyed (stopped) first.
    std::unique_ptr<os::Timer> reboot_timer_;
    std::unique_ptr<WsHub> hub_;
    // Serializes a WebSocket upgrade's token check + hub_->open() against
    // the revoke listener closing connections.
    os::Mutex ws_auth_mutex_;
    int revoke_listener_ = 0;
};

}  // namespace wisp::admin
