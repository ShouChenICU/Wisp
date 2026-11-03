// Device info, browser pairing and logout (spec 6.1, 6.3).

#include <algorithm>
#include <cstdint>
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

// 429 for a locked pairing, saying when to try again (spec 6.1): whole
// seconds, rounded up, at least 1, as Retry-After and in the body.
net::HttpResponse locked(AdminDeps& deps, const RequestContext& ctx, const Error& error) {
    const uint64_t remaining_ms = deps.pairing.lock_remaining_ms(ctx.now_ms);
    const auto seconds = static_cast<int64_t>(std::max<uint64_t>(1, (remaining_ms + 999) / 1000));
    net::HttpResponse res =
        net::HttpResponse::json(429, Value(Map{{"error", Value(Map{
                                                             {"code", Value(to_string(error.code))},
                                                             {"message", Value(error.message)},
                                                             {"retry_after_s", Value(seconds)},
                                                         })}}));
    res.headers.emplace_back("Retry-After", std::to_string(seconds));
    return res;
}

net::HttpResponse info(AdminDeps& deps, const RequestContext& ctx) {
    return net::HttpResponse::json(200, Value(Map{
                                            {"name", Value(deps.device_name)},
                                            {"fw", Value(deps.firmware_version)},
                                            {"paired", Value(!ctx.client_id.empty())},
                                            {"boot_id", Value(deps.boot_id)},
                                        }));
}

net::HttpResponse pair_start(AdminDeps& deps, const RequestContext& ctx) {
    // The code goes to the device's screen, never into the response.
    auto r = deps.pairing.request_code(ctx.now_ms);
    if (!r && r.error().code == ErrorCode::RateLimited) return locked(deps, ctx, r.error());
    if (!r) return service_error(r.error());
    return ok_json(202);
}

net::HttpResponse pair_confirm(AdminDeps& deps, const RequestContext& ctx) {
    const Value* code = ctx.body.find("code");
    if (code == nullptr || code->type() != Value::Type::String) {
        return net::HttpResponse::error(400, "invalid_argument",
                                        "body must be {\"code\": \"<6 digits>\"}");
    }
    net::ConfirmFailure why = net::ConfirmFailure::None;
    auto token = deps.pairing.confirm(code->as_string(), client_info(ctx.req), ctx.now_ms, &why);
    if (!token && why == net::ConfirmFailure::Locked) return locked(deps, ctx, token.error());
    if (!token && why == net::ConfirmFailure::ExpiredCode) {
        return net::HttpResponse::error(403, "code_expired",
                                        "the pairing code has expired; request a new one");
    }
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
