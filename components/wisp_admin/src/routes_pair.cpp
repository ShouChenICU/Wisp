// Device info, browser pairing and logout (spec 6.1, 6.3).

#include <string>
#include <vector>

#include "admin_internal.hpp"

namespace wisp::admin::detail {

namespace {

// Tokens never expire on the device (they are revoked instead), so the
// cookie's one-year lifetime is what bounds a browser's session.
constexpr const char* kCookieAttrs = "; HttpOnly; SameSite=Strict; Path=/; Max-Age=";
constexpr const char* kCookieMaxAge = "31536000";

net::ClientInfo client_info(const net::HttpRequest& req) {
    return net::ClientInfo{req.header("user-agent"), req.remote_ip, req.iface};
}

// The token travels only in the HttpOnly cookie, so page scripts never
// see it; the body just confirms the pairing.
net::HttpResponse paired(const std::string& token) {
    net::HttpResponse res = net::HttpResponse::json(200, Value(Map{{"paired", Value(true)}}));
    res.headers.emplace_back("Set-Cookie", "wisp_token=" + token + kCookieAttrs + kCookieMaxAge);
    return res;
}

net::HttpResponse info(AdminDeps& deps, const RequestContext& ctx) {
    return net::HttpResponse::json(200, Value(Map{
                                            {"name", Value(deps.device_name)},
                                            {"fw", Value(deps.firmware_version)},
                                            {"paired", Value(!ctx.client_id.empty())},
                                        }));
}

net::HttpResponse pair_start(AdminDeps& deps, const RequestContext& ctx) {
    // The code goes to the device's screen, never into the response.
    auto r = deps.pairing.request_code(ctx.now_ms);
    if (!r) return service_error(r.error());
    return ok_json(202);
}

net::HttpResponse pair_confirm(AdminDeps& deps, const RequestContext& ctx) {
    const Value* code = ctx.body.find("code");
    if (code == nullptr || code->type() != Value::Type::String) {
        return net::HttpResponse::error(400, "invalid_argument",
                                        "body must be {\"code\": \"<6 digits>\"}");
    }
    auto token = deps.pairing.confirm(code->as_string(), client_info(ctx.req), ctx.now_ms);
    if (!token) return service_error(token.error());
    return paired(token.value());
}

net::HttpResponse pair_ap(AdminDeps& deps, const RequestContext& ctx) {
    // PairingService refuses anything that did not arrive on the hotspot.
    auto token = deps.pairing.pair_from_ap(client_info(ctx.req), ctx.now_ms);
    if (!token) return service_error(token.error());
    return paired(token.value());
}

net::HttpResponse logout(AdminDeps& deps, const RequestContext& ctx) {
    // A concurrent revoke may already have removed the client; the cookie
    // is cleared either way.
    (void)deps.pairing.revoke(ctx.client_id);
    net::HttpResponse res = ok_json(200);
    res.headers.emplace_back("Set-Cookie", std::string("wisp_token=") + kCookieAttrs + "0");
    return res;
}

}  // namespace

void add_pair_routes(std::vector<Route>& routes) {
    using net::Method;
    routes.push_back({Method::Get, "/info", true, info});
    routes.push_back({Method::Post, "/pair/start", true, pair_start});
    routes.push_back({Method::Post, "/pair/confirm", true, pair_confirm});
    routes.push_back({Method::Post, "/pair/ap", true, pair_ap});
    routes.push_back({Method::Post, "/logout", false, logout});
}

}  // namespace wisp::admin::detail
