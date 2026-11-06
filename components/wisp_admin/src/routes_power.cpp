// GET /system/power and GET /system/occupancy (scene-device spec 4.5, 6.3):
// thin views of PowerManager (through AdminDeps::power_status), the
// WakeLockRegistry and the OccupancyRegistry. Holders and reasons are the
// names code chose ("system.ntp", "scene.<id>"), never user data.

#include <cstdint>
#include <string>
#include <vector>

#include "admin_internal.hpp"
#include "wisp/core/occupancy.hpp"
#include "wisp/core/wakelock.hpp"

namespace wisp::admin::detail {

namespace {

net::HttpResponse unavailable(const char* what) {
    return net::HttpResponse::error(503, "unavailable", what);
}

// How long since `since_ms`; never negative, should a registry's clock
// read ahead of the router's.
int64_t held_ms(uint64_t now_ms, uint64_t since_ms) {
    return now_ms > since_ms ? static_cast<int64_t>(now_ms - since_ms) : 0;
}

// `v[key]` when it is a `type`, else `fallback`.
Value field(const Value& v, const char* key, Value::Type type, Value fallback) {
    const Value* f = v.find(key);
    return f != nullptr && f->type() == type ? *f : fallback;
}

net::HttpResponse get_power(AdminDeps& deps, const RequestContext& ctx) {
    if (deps.wakelocks == nullptr) return unavailable("power management is not available");
    const Value status = deps.power_status ? deps.power_status() : Value();
    List locks;
    for (const core::WakeLockInfo& l : deps.wakelocks->list()) {
        locks.push_back(Value(Map{
            {"id", Value(static_cast<int64_t>(l.id))},
            {"holder", Value(l.holder)},
            {"kind", Value(core::to_string(l.kind))},
            {"reason", Value(l.reason)},
            {"held_ms", Value(held_ms(ctx.now_ms, l.since_ms))},
        }));
    }
    return net::HttpResponse::json(
        200, Value(Map{
                 {"level", field(status, "level", Value::Type::String, Value("active"))},
                 {"light_sleep",
                  field(status, "light_sleep", Value::Type::String, Value("unsupported"))},
                 {"deferred", field(status, "deferred", Value::Type::Int, Value(int64_t{0}))},
                 {"locks", Value(std::move(locks))},
             }));
}

net::HttpResponse get_occupancy(AdminDeps& deps, const RequestContext& ctx) {
    if (deps.occupancy == nullptr) return unavailable("peripheral use is not tracked");
    List items;
    for (const core::OccupancyEntry& e : deps.occupancy->list()) {
        items.push_back(Value(Map{
            {"device", Value(e.device)},
            {"holder", Value(e.holder)},
            {"mode", Value(core::to_string(e.mode))},
            {"held_ms", Value(held_ms(ctx.now_ms, e.since_ms))},
        }));
    }
    return net::HttpResponse::json(200, Value(Map{{"items", Value(std::move(items))}}));
}

}  // namespace

void add_power_routes(std::vector<Route>& routes) {
    using net::Method;
    routes.push_back({Method::Get, "/system/power", false, get_power});
    routes.push_back({Method::Get, "/system/occupancy", false, get_occupancy});
}

}  // namespace wisp::admin::detail
