// Device overview, the device registry and system settings (spec 6.1).

#include <set>
#include <string>
#include <vector>

#include "admin_internal.hpp"
#include "wisp/hal/device.hpp"

namespace wisp::admin::detail {

namespace {

net::HttpResponse device(AdminDeps& deps, const RequestContext&) {
    return net::HttpResponse::json(
        200, Value(Map{
                 {"manifest", deps.registry.manifest(deps.board)},
                 {"boot", deps.report.to_value(deps.board, deps.firmware_version)},
                 {"status", deps.device_status ? deps.device_status() : Value(Map{})},
             }));
}

// Registered devices first (sorted by id), each with its boot-report state
// ("ok" when boot recorded nothing for it), then devices boot recorded but
// that never made it into the registry (missing or failed), so the page
// shows what is absent too.
net::HttpResponse devices(AdminDeps& deps, const RequestContext&) {
    List out;
    std::set<std::string> listed;
    for (const auto& dev : deps.registry.all()) {
        const hal::DeviceInfo& info = dev->info();
        auto rep = deps.report.find(info.id);
        out.push_back(Value(Map{
            {"id", Value(info.id)},
            {"class", Value(hal::to_string(info.cls))},
            {"model", Value(info.model)},
            {"state", Value(core::to_string(rep ? rep->state : core::DeviceState::Ok))},
            {"caps", Value(info.caps)},
        }));
        listed.insert(info.id);
    }
    for (const core::DeviceReport& rep : deps.report.entries()) {
        if (!listed.insert(rep.id).second) continue;
        // entries() is in record order and find() returns the latest
        // record, which is the one that counts.
        auto latest = deps.report.find(rep.id);
        out.push_back(Value(Map{
            {"id", Value(rep.id)},
            {"class", Value(hal::to_string(rep.cls))},
            {"model", Value("")},
            {"state", Value(core::to_string(latest ? latest->state : rep.state))},
            {"caps", Value(Map{})},
        }));
    }
    return net::HttpResponse::json(200, Value(Map{{"devices", Value(std::move(out))}}));
}

net::HttpResponse get_settings(AdminDeps& deps, const RequestContext&) {
    return net::HttpResponse::json(200, Value(core::to_map(deps.settings.get())));
}

// Any subset of {brightness, volume, screen_off_s, utc_offset_min}
// (integers) and language ("zh"/"ja"/"en"); SystemSettings rejects unknown keys, wrong
// types and out-of-range values for the whole batch.
net::HttpResponse put_settings(AdminDeps& deps, const RequestContext& ctx) {
    if (ctx.body.type() != Value::Type::Map) {
        return bad_request("body must be a JSON object");
    }
    auto r = deps.settings.set(ctx.body.as_map());
    if (!r) return service_error(r.error());
    return net::HttpResponse::json(200, Value(core::to_map(deps.settings.get())));
}

}  // namespace

void add_device_routes(std::vector<Route>& routes) {
    using net::Method;
    routes.push_back({Method::Get, "/device", false, device});
    routes.push_back({Method::Get, "/devices", false, devices});
    routes.push_back({Method::Get, "/settings", false, get_settings});
    routes.push_back({Method::Put, "/settings", false, put_settings});
}

}  // namespace wisp::admin::detail
