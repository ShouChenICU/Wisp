// Paired browsers, reboot and factory reset (spec 6.1).

#include <optional>
#include <string>
#include <vector>

#include "admin_internal.hpp"

namespace wisp::admin::detail {

namespace {

// Ids and labels only: the token hashes stay inside PairingService.
net::HttpResponse list_clients(AdminDeps& deps, const RequestContext& ctx) {
    List out;
    for (const net::PairedClient& c : deps.pairing.clients()) {
        out.push_back(Value(Map{
            {"id", Value(c.id)},
            {"label", Value(c.label)},
            {"created", Value(c.created_unix)},
            {"last_seen", Value(c.last_seen_unix)},
            {"current", Value(c.id == ctx.client_id)},
        }));
    }
    return net::HttpResponse::json(200, Value(Map{{"clients", Value(std::move(out))}}));
}

net::HttpResponse revoke_client(AdminDeps& deps, const RequestContext& ctx) {
    std::optional<std::string> id = percent_decode(ctx.param);
    if (!id) return bad_request("malformed %-escape in client id");
    auto r = deps.pairing.revoke(*id);
    if (!r) return service_error(r.error());
    return no_content();
}

// Replies first: the timer calls AdminDeps::reboot kRebootDelayMs later.
// A second request while one is pending does not re-arm it.
net::HttpResponse reboot(AdminDeps& deps, const RequestContext& ctx) {
    if (!deps.reboot || ctx.reboot_timer == nullptr) {
        return net::HttpResponse::error(503, "unavailable", "reboot is not available");
    }
    ctx.reboot_timer->start();
    return ok_json(202);
}

// Only asks the device to show its confirmation dialog; the device does
// the erasing (and publishes the outcome) once the user confirms there.
net::HttpResponse factory_reset(AdminDeps& deps, const RequestContext&) {
    if (!deps.request_factory_reset) {
        return net::HttpResponse::error(503, "unavailable", "factory reset is not available");
    }
    deps.request_factory_reset();
    return ok_json(202);
}

}  // namespace

void add_system_routes(std::vector<Route>& routes) {
    using net::Method;
    routes.push_back({Method::Get, "/clients", false, list_clients});
    routes.push_back({Method::Delete, "/clients/*", false, revoke_client});
    routes.push_back({Method::Post, "/system/reboot", false, reboot});
    routes.push_back({Method::Post, "/system/factory-reset", false, factory_reset});
}

}  // namespace wisp::admin::detail
