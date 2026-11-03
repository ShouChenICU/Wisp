#pragma once

// Pieces shared by AdminServer's pipeline (admin_server.cpp), its request
// filters (filters.cpp) and the route groups (routes_*.cpp). Not part of
// wisp_admin's public interface.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/admin/admin_server.hpp"
#include "wisp/base/error.hpp"
#include "wisp/base/value.hpp"
#include "wisp/net/http.hpp"
#include "wisp/os/os.hpp"

namespace wisp::admin::detail {

// Spec 6.3: larger request bodies are refused with 413.
constexpr size_t kMaxBodyBytes = 8192;

constexpr std::string_view kApiPrefix = "/api/v1";

// What a route handler gets besides the services: the request, the clock
// reading taken once for it, the caller's identity and the decoded body.
struct RequestContext {
    const net::HttpRequest& req;
    uint64_t now_ms = 0;
    // Paired client id when the request carried a valid wisp_token; empty
    // otherwise (only possible on whitelisted routes).
    std::string client_id;
    // The path segment a route's "*" matched, still URL-encoded.
    std::string param;
    // The JSON body; Null when the body was empty.
    Value body;
    // AdminServer's one-shot timer that calls AdminDeps::reboot, so the
    // reboot route can answer before the device goes down.
    os::Timer* reboot_timer = nullptr;
};

using RouteFn = net::HttpResponse (*)(AdminDeps& deps, const RequestContext& ctx);

struct Route {
    net::Method method;
    // Path after kApiPrefix. One segment may be "*", which matches exactly
    // one non-empty segment, handed to the handler as RequestContext::param.
    std::string_view path;
    // Reachable without a token (the pairing whitelist). CSRF still applies.
    bool is_public;
    RouteFn fn;
};

// Route groups; each appends its routes to the table.
void add_pair_routes(std::vector<Route>& routes);
void add_device_routes(std::vector<Route>& routes);  // /device, /devices, /settings
void add_wifi_routes(std::vector<Route>& routes);
void add_ai_routes(std::vector<Route>& routes);      // /ai/*
void add_system_routes(std::vector<Route>& routes);  // /clients, /system/*

// How long the reboot route waits before calling AdminDeps::reboot, so its
// 202 reaches the browser first.
constexpr uint32_t kRebootDelayMs = 500;

// --- filters.cpp ---

// Host check (spec 6.3), ignoring a ":port" suffix: a dotted-quad IPv4
// address, "wisp.local" or "localhost". The interface is not considered;
// AdminServer::handle() decides what a foreign Host on the hotspot gets.
bool host_allowed(const net::HttpRequest& req);
// No Origin header (a non-browser client), or an http(s) Origin whose
// host[:port] is exactly the Host header (case-insensitive). Browsers
// send Origin on every WebSocket upgrade, so this refuses a cross-site
// page's upgrade even if a cookie were to come along with it.
bool origin_matches_host(const net::HttpRequest& req);
bool is_probe_path(std::string_view path);
// 302 to the hotspot's setup page; nullopt when no hotspot is up.
std::optional<net::HttpResponse> setup_redirect(const AdminDeps& deps);
// setup_redirect() when the request came from the hotspot, else 404.
net::HttpResponse probe_response(const AdminDeps& deps, const net::HttpRequest& req);
bool is_mutating(net::Method m);
// "X-Wisp: 1" present (header names arrive lowercased).
bool csrf_ok(const net::HttpRequest& req);

// --- admin_server.cpp ---

// The {"error":{...}} envelope for a service error, with the status from
// net::http_status_for() and the snake_case ErrorCode name as "code". The
// service's message is passed on only for 4xx statuses: server-side
// failures (storage errors and the like) can name on-device paths, so 5xx
// responses carry a generic message instead.
net::HttpResponse service_error(const Error& error);
// Empty-object JSON body with the given status.
net::HttpResponse ok_json(int status = 200);
// 204 with an empty body.
net::HttpResponse no_content();
// 400 invalid_argument with `message`.
net::HttpResponse bad_request(std::string_view message);
// Decodes %XX escapes in a path segment ('+' stays literal, as in any URL
// path). nullopt for a truncated or non-hex escape.
std::optional<std::string> percent_decode(std::string_view s);

}  // namespace wisp::admin::detail
