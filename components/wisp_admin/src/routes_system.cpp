// Paired browsers, reboot, factory reset (spec 6.1) and network time
// (spec 5.6).

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

net::HttpResponse no_time_sync() {
    return net::HttpResponse::error(503, "unavailable", "network time is not available");
}

// {"synced", "last_sync", "server", "time"}: last_sync and time (the
// current UTC time as the last sync implies it) are null until a sync.
net::HttpResponse get_time(AdminDeps& deps, const RequestContext&) {
    if (deps.time_sync == nullptr) return no_time_sync();
    const net::TimeSyncStatus st = deps.time_sync->status();
    const std::optional<int64_t> now = deps.time_sync->now_unix();
    return net::HttpResponse::json(200,
                                   Value(Map{
                                       {"synced", Value(st.synced)},
                                       {"last_sync", st.synced ? Value(st.last_sync) : Value()},
                                       {"server", Value(st.server)},
                                       {"time", now ? Value(*now) : Value()},
                                   }));
}

// Queues an attempt and answers at once; its outcome arrives as the
// system.time event.
net::HttpResponse sync_time(AdminDeps& deps, const RequestContext&) {
    if (deps.time_sync == nullptr) return no_time_sync();
    if (!deps.time_sync->sync_now()) {
        return net::HttpResponse::error(409, "no_network", "the device has no network connection");
    }
    return ok_json(202);
}

}  // namespace

void add_system_routes(std::vector<Route>& routes) {
    using net::Method;
    routes.push_back({Method::Get, "/clients", false, list_clients});
    routes.push_back({Method::Delete, "/clients/*", false, revoke_client});
    routes.push_back({Method::Post, "/system/reboot", false, reboot});
    routes.push_back({Method::Post, "/system/factory-reset", false, factory_reset});
    routes.push_back({Method::Get, "/system/time", false, get_time});
    routes.push_back({Method::Post, "/system/time/sync", false, sync_time});
}

}  // namespace wisp::admin::detail
