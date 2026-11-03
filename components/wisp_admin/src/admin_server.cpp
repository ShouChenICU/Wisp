#include "wisp/admin/admin_server.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "admin_internal.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/os/os.hpp"

namespace wisp::admin {

namespace detail {

net::HttpResponse service_error(const Error& error) {
    const char* code = to_string(error.code);
    const int status = net::http_status_for(error.code);
    if (status >= 500) return net::HttpResponse::error(status, code, "internal error");
    return net::HttpResponse::error(status, code, error.message.empty() ? code : error.message);
}

net::HttpResponse ok_json(int status) { return net::HttpResponse::json(status, Value(Map{})); }

net::HttpResponse no_content() {
    net::HttpResponse res;
    res.status = 204;
    return res;
}

net::HttpResponse bad_request(std::string_view message) {
    return net::HttpResponse::error(400, "invalid_argument", message);
}

std::optional<std::string> percent_decode(std::string_view s) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out.push_back(s[i]);
            continue;
        }
        if (i + 2 >= s.size()) return std::nullopt;
        const int hi = hex(s[i + 1]);
        const int lo = hex(s[i + 2]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return out;
}

}  // namespace detail

namespace {

using detail::Route;

constexpr const char* kTag = "admin";

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// If-None-Match may list several tags, weak ones prefixed "W/", or "*".
bool etag_matches(std::string_view if_none_match, std::string_view etag) {
    while (!if_none_match.empty()) {
        size_t comma = if_none_match.find(',');
        std::string_view tag = trim(if_none_match.substr(0, comma));
        if (tag.substr(0, 2) == "W/") tag.remove_prefix(2);
        if (tag == "*" || tag == etag) return true;
        if (comma == std::string_view::npos) break;
        if_none_match.remove_prefix(comma + 1);
    }
    return false;
}

net::HttpResponse serve_static(const net::AssetPack* assets, const net::HttpRequest& req) {
    if (req.method != net::Method::Get) {
        return net::HttpResponse::error(405, "method_not_allowed", "static files are GET only");
    }
    const net::AssetEntry* entry = assets != nullptr ? assets->find(req.path) : nullptr;
    if (entry == nullptr) return net::HttpResponse::error(404, "not_found", "no such resource");

    net::HttpResponse res;
    res.content_type = entry->mime;
    res.headers.emplace_back("ETag", entry->etag);
    // Revalidate every time: a firmware update replaces the pack, and the
    // ETag makes an unchanged file cost only a 304.
    res.headers.emplace_back("Cache-Control", "no-cache");
    if (etag_matches(req.header("if-none-match"), entry->etag)) {
        res.status = 304;
        return res;
    }
    if (entry->gzip) res.headers.emplace_back("Content-Encoding", "gzip");
    res.static_body = entry->data;
    return res;
}

struct RouteMatch {
    const Route* route = nullptr;
    std::string param;
    bool path_known = false;  // some route has this path, under another method
};

RouteMatch match_route(const std::vector<Route>& routes, net::Method method, std::string_view sub) {
    RouteMatch m;
    for (const Route& r : routes) {
        std::string param;
        bool path_ok = false;
        if (r.path.size() >= 2 && r.path.substr(r.path.size() - 2) == "/*") {
            std::string_view prefix = r.path.substr(0, r.path.size() - 1);
            if (sub.size() > prefix.size() && sub.substr(0, prefix.size()) == prefix) {
                std::string_view rest = sub.substr(prefix.size());
                if (rest.find('/') == std::string_view::npos) {
                    path_ok = true;
                    param = std::string(rest);
                }
            }
        } else {
            path_ok = sub == r.path;
        }
        if (!path_ok) continue;
        if (r.method == method) {
            m.route = &r;
            m.param = std::move(param);
            return m;
        }
        m.path_known = true;
    }
    return m;
}

}  // namespace

AdminServer::AdminServer(AdminDeps deps) : deps_(std::move(deps)) {
    if (!deps_.now_ms) deps_.now_ms = [] { return os::now_ms(); };
    reboot_timer_ =
        std::make_unique<os::Timer>("wisp.reboot", detail::kRebootDelayMs, false, [this] {
            if (deps_.reboot) deps_.reboot();
        });
    detail::add_pair_routes(routes_);
    detail::add_device_routes(routes_);
    detail::add_wifi_routes(routes_);
    detail::add_llm_routes(routes_);
    detail::add_system_routes(routes_);
    hub_ = std::make_unique<WsHub>(deps_.bus);
    hub_->start();
    // Logout, DELETE /clients/{id} and the factory reset's revoke_all()
    // all end here, so a revoked token also loses its live stream.
    revoke_listener_ =
        deps_.pairing.add_revoke_listener([this](const std::optional<std::string>& id) {
            std::lock_guard<os::Mutex> lock(ws_auth_mutex_);
            if (id) {
                hub_->close_client(*id);
            } else {
                hub_->close_all();
            }
        });
}

AdminServer::~AdminServer() {
    // Waits out a revocation still closing connections through hub_.
    deps_.pairing.remove_revoke_listener(revoke_listener_);
    hub_.reset();
    // Cancels a reboot that has not fired yet and waits out one in
    // progress, so the callback never sees a destroyed deps_.
    reboot_timer_.reset();
}

net::HttpResponse AdminServer::handle(const net::HttpRequest& req) {
    if (!detail::host_allowed(req)) {
        // The hotspot's DNS answers every name with the device, so phones
        // probe for a captive portal under foreign names; those probes and
        // other plain page loads are steered to the setup page. Nothing
        // else gets through: a page rebound to the hotspot address (a
        // phone on a hotspot without internet may still resolve names
        // over cellular) would otherwise be same-origin with the API and
        // could pair itself through /pair/ap.
        if (req.iface == net::Iface::Ap) {
            if (detail::is_probe_path(req.path)) return detail::probe_response(deps_, req);
            if (req.method == net::Method::Get) {
                if (auto redirect = detail::setup_redirect(deps_)) return std::move(*redirect);
            }
        }
        WLOG_D(kTag, "refused a request with a foreign Host header");
        return net::HttpResponse::error(403, "bad_host", "unexpected Host header");
    }
    // Includes OPTIONS: the device never answers a CORS preflight, which
    // is what keeps other origins from sending X-Wisp (spec 6.3).
    if (req.method == net::Method::Other) {
        return net::HttpResponse::error(405, "method_not_allowed", "method not allowed");
    }
    if (detail::is_probe_path(req.path)) return detail::probe_response(deps_, req);
    std::string_view path = req.path;
    if (path == "/api" || path.substr(0, 5) == "/api/") {
        net::HttpResponse res = handle_api(req);
        res.headers.emplace_back("Cache-Control", "no-store");
        return res;
    }
    return serve_static(deps_.assets, req);
}

net::HttpResponse AdminServer::handle_api(const net::HttpRequest& req) {
    const uint64_t now = deps_.now_ms();

    std::string_view sub;
    std::string_view path = req.path;
    if (path.size() > detail::kApiPrefix.size() &&
        path.substr(0, detail::kApiPrefix.size()) == detail::kApiPrefix &&
        path[detail::kApiPrefix.size()] == '/') {
        sub = path.substr(detail::kApiPrefix.size());
    }
    RouteMatch m = sub.empty() ? RouteMatch{} : match_route(routes_, req.method, sub);

    std::string client_id;
    const std::string token = req.cookie("wisp_token");
    if (!token.empty()) {
        auto who = deps_.pairing.authenticate(token, now);
        if (who) client_id = std::move(who.value());
    }

    // Unknown paths are 401 too, so anonymous callers cannot map the API.
    const bool is_public = m.route != nullptr && m.route->is_public;
    if (!is_public && client_id.empty()) {
        return net::HttpResponse::error(401, "unauthorized", "pair this browser first");
    }
    if (detail::is_mutating(req.method) && !detail::csrf_ok(req)) {
        return net::HttpResponse::error(403, "csrf", "missing X-Wisp: 1 header");
    }
    if (req.body.size() > detail::kMaxBodyBytes) {
        return net::HttpResponse::error(413, "payload_too_large", "request body over 8 KiB");
    }
    if (m.route == nullptr) {
        return m.path_known
                   ? net::HttpResponse::error(405, "method_not_allowed", "method not allowed")
                   : net::HttpResponse::error(404, "not_found", "no such endpoint");
    }

    Value body;
    if (!req.body.empty()) {
        auto parsed = from_json(req.body);
        if (!parsed) {
            return net::HttpResponse::error(400, "bad_json", "request body is not valid JSON");
        }
        body = std::move(parsed.value());
    }

    detail::RequestContext ctx{
        req, now, std::move(client_id), std::move(m.param), std::move(body), reboot_timer_.get()};
    return m.route->fn(deps_, ctx);
}

net::WsHandlers AdminServer::ws_handlers() {
    net::WsHandlers ws;
    ws.on_open = [this](std::shared_ptr<net::WsConnection> conn, const net::HttpRequest& req) {
        // A WebSocket upgrade is a GET the browser makes with the page's
        // cookies, so it gets the API's Host check and token check; the
        // hotspot's captive-portal leniency does not apply here.
        if (!detail::host_allowed(req)) {
            WLOG_D(kTag, "refused a WebSocket upgrade with a foreign Host header");
            return false;
        }
        const std::string token = req.cookie("wisp_token");
        // Held from the token check until the hub has the connection: a
        // revocation racing this upgrade either fails the check or finds
        // the connection to close, never neither.
        std::lock_guard<os::Mutex> lock(ws_auth_mutex_);
        std::string client_id;
        if (!token.empty()) {
            auto who = deps_.pairing.authenticate(token, deps_.now_ms());
            if (who) client_id = std::move(who.value());
        }
        if (client_id.empty()) {
            WLOG_D(kTag, "refused an unauthenticated WebSocket upgrade");
            return false;
        }
        return hub_->open(std::move(conn), std::move(client_id));
    };
    ws.on_message = [this](int id, std::string_view text) { hub_->on_message(id, text); };
    ws.on_close = [this](int id) { hub_->close(id); };
    return ws;
}

}  // namespace wisp::admin
