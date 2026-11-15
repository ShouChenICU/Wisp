// Device overview, the device registry and system settings (spec 6.1).

#include <charconv>
#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "admin_internal.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/settings_schema.hpp"
#include "wisp/hal/device.hpp"
#include "wisp/hal/device_schema.hpp"

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
            {"caps", Value(hal::caps_of(*dev))},
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

net::HttpResponse get_settings_schema(AdminDeps&, const RequestContext& ctx) {
    // AdminServer always passes the body it built; a context without one is
    // a wiring mistake, answered rather than dereferenced.
    if (ctx.settings_schema == nullptr || *ctx.settings_schema == nullptr) {
        return net::HttpResponse::error(500, "internal", "internal error");
    }
    net::HttpResponse res;
    res.status = 200;
    const std::shared_ptr<const os::LargeString>& body = *ctx.settings_schema;
    res.static_body =
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(body->data()), body->size());
    res.body_owner = body;
    return res;
}

// Any subset of the keys GET /settings lists (the schema rows flagged
// kSettingWeb); SystemSettings rejects other keys (the store-only ones
// included), wrong types and out-of-range values for the whole batch.
net::HttpResponse put_settings(AdminDeps& deps, const RequestContext& ctx) {
    if (ctx.body.type() != Value::Type::Map) {
        return bad_request("body must be a JSON object");
    }
    auto r = deps.settings.set(ctx.body.as_map(), core::SettingSurface::Web);
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
    routes.push_back({Method::Get, "/settings/schema", false, get_settings_schema});
}

namespace {

std::string_view type_name(core::SettingType t) {
    switch (t) {
        case core::SettingType::Int:
            return "int";
        case core::SettingType::Bool:
            return "bool";
        case core::SettingType::Enum:
            return "enum";
        case core::SettingType::HostList:
            return "hosts";
    }
    return "int";
}

std::string_view unit_name(core::SettingUnit u) {
    switch (u) {
        case core::SettingUnit::None:
            return "none";
        case core::SettingUnit::Level:
            return "level";
        case core::SettingUnit::Percent:
            return "percent";
        case core::SettingUnit::Seconds:
            return "seconds";
        case core::SettingUnit::Minutes:
            return "minutes";
        case core::SettingUnit::UtcOffsetMinutes:
            return "utc_offset_minutes";
        case core::SettingUnit::Steps:
            return "steps";
    }
    return "none";
}

void append_int(os::LargeString& out, int64_t v) {
    char num[24];
    out.append(num, std::to_chars(num, num + sizeof num, v).ptr);
}

// ,"key":
void append_key(os::LargeString& out, std::string_view key) {
    out.push_back(',');
    append_json_string(out, key);
    out.push_back(':');
}

}  // namespace

// Member order per type: name, type, label; then Int: min, max, default,
// unit, zero_never (only when set), choices (only when any); Bool:
// default; Enum: values, default (the code); HostList: max_entries,
// max_length, default. Strings go through append_json_string(), the
// escaping to_json() uses.
std::shared_ptr<const os::LargeString> settings_schema_body() {
    auto body = std::allocate_shared<os::LargeString>(os::LargeAllocator<os::LargeString>());
    os::LargeString& out = *body;
    out.append("{\"keys\":[");
    bool first = true;
    for (const core::SettingSpec& spec : core::kSettingsSchema) {
        if ((spec.flags & core::kSettingWeb) == 0) continue;
        if (!first) out.push_back(',');
        first = false;
        out.append("{\"name\":");
        append_json_string(out, spec.name);
        append_key(out, "type");
        append_json_string(out, type_name(spec.type));
        append_key(out, "label");
        append_json_string(out, spec.label);
        switch (spec.type) {
            case core::SettingType::Int:
                append_key(out, "min");
                append_int(out, spec.min);
                append_key(out, "max");
                append_int(out, spec.max);
                append_key(out, "default");
                append_int(out, spec.def_int);
                append_key(out, "unit");
                append_json_string(out, unit_name(spec.unit));
                if ((spec.flags & core::kSettingZeroNever) != 0) {
                    append_key(out, "zero_never");
                    out.append("true");
                }
                if (!spec.int_choices.empty()) {
                    append_key(out, "choices");
                    out.push_back('[');
                    for (size_t i = 0; i < spec.int_choices.size(); ++i) {
                        if (i > 0) out.push_back(',');
                        append_int(out, spec.int_choices[i]);
                    }
                    out.push_back(']');
                }
                break;
            case core::SettingType::Bool:
                append_key(out, "default");
                out.append(spec.def_int != 0 ? "true" : "false");
                break;
            case core::SettingType::Enum:
                append_key(out, "values");
                out.push_back('[');
                for (size_t i = 0; i < spec.enum_values.size(); ++i) {
                    if (i > 0) out.push_back(',');
                    append_json_string(out, spec.enum_values[i]);
                }
                out.push_back(']');
                append_key(out, "default");
                append_json_string(out, core::setting_default_text(core::setting_id_of(spec)));
                break;
            case core::SettingType::HostList:
                append_key(out, "max_entries");
                append_int(out, spec.min);
                append_key(out, "max_length");
                append_int(out, spec.max);
                append_key(out, "default");
                append_json_string(out, spec.def_text);
                break;
        }
        out.push_back('}');
    }
    out.append("]}");
    return body;
}

}  // namespace wisp::admin::detail
