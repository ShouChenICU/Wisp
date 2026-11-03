// Model profiles (spec 5.3, 6.1): thin adapters over
// core::ModelProfileService.
//
// API keys are write-only: a profile in a response carries only has_key,
// and the test result carries the status code and an error category,
// never the upstream body (an error body may quote the key back).
// Unknown fields are rejected so a typo does not silently do nothing.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "admin_internal.hpp"

namespace wisp::admin::detail {

namespace {

// The service's error, with spec 6.1's dedicated code when it has one.
net::HttpResponse model_error(const Error& error, core::ModelFailure why) {
    if (why == core::ModelFailure::None) return service_error(error);
    return net::HttpResponse::error(net::http_status_for(error.code), core::to_string(why),
                                    error.message);
}

std::optional<std::string> profile_id(const RequestContext& ctx) {
    return percent_decode(ctx.param);
}

// Fills `out` from a JSON string; false on any other type.
bool take_string(const Value& v, std::optional<std::string>& out) {
    if (v.type() != Value::Type::String) return false;
    out = v.as_string();
    return true;
}

// A flat object of string values.
bool take_options(const Value& v, std::optional<core::ModelOptions>& out) {
    if (v.type() != Value::Type::Map) return false;
    core::ModelOptions options;
    for (const auto& [k, item] : v.as_map()) {
        if (item.type() != Value::Type::String) return false;
        options[k] = item.as_string();
    }
    out = std::move(options);
    return true;
}

// The fields create and update share. nullopt: not one of them; false: the
// right field with the wrong type.
std::optional<bool> take_common(std::string_view key, const Value& value, core::ProfileUpdate& u) {
    if (key == "name") return take_string(value, u.name);
    if (key == "preset") return take_string(value, u.preset);
    if (key == "base_url") return take_string(value, u.base_url);
    if (key == "model") return take_string(value, u.model);
    if (key == "options") return take_options(value, u.options);
    if (key == "api_key") return take_string(value, u.api_key);
    if (key == "api_key_from") return take_string(value, u.api_key_from);
    return std::nullopt;
}

net::HttpResponse get_profiles(AdminDeps& deps, const RequestContext&) {
    return net::HttpResponse::json(200, core::to_value(deps.models.get()));
}

net::HttpResponse get_presets(AdminDeps& deps, const RequestContext&) {
    List out;
    for (const core::ModelPreset& p : deps.models.presets()) out.push_back(core::to_value(p));
    return net::HttpResponse::json(200, Value(Map{{"presets", Value(std::move(out))}}));
}

// {kind, name, preset, base_url?, model?, options?, api_key? | api_key_from?}
net::HttpResponse create_profile(AdminDeps& deps, const RequestContext& ctx) {
    constexpr std::string_view kShape =
        "body must be {kind, name, preset, base_url?, model?, options?, api_key? | api_key_from?}";
    if (ctx.body.type() != Value::Type::Map) return bad_request(kShape);
    core::ProfileUpdate fields;
    std::optional<std::string> kind;
    for (const auto& [key, value] : ctx.body.as_map()) {
        std::optional<bool> ok = key == "kind" ? std::optional<bool>(take_string(value, kind))
                                               : take_common(key, value, fields);
        if (!ok || !*ok) return bad_request(kShape);
    }
    std::optional<core::ModelKind> parsed = kind ? core::parse_model_kind(*kind) : std::nullopt;
    if (!parsed) return bad_request("kind must be \"llm\", \"asr\" or \"tts\"");
    if (!fields.name || !fields.preset) return bad_request(kShape);

    core::ModelFailure why = core::ModelFailure::None;
    auto r = deps.models.create(
        core::ProfileInput{
            .kind = *parsed,
            .name = *fields.name,
            .preset = *fields.preset,
            .base_url = fields.base_url,
            .model = fields.model,
            .options = fields.options,
            .api_key = fields.api_key,
            .api_key_from = fields.api_key_from,
        },
        &why);
    if (!r) return model_error(r.error(), why);
    return net::HttpResponse::json(201, core::to_value(r.value()));
}

// Any subset of the create fields except kind.
net::HttpResponse update_profile(AdminDeps& deps, const RequestContext& ctx) {
    constexpr std::string_view kShape =
        "body must be {name?, preset?, base_url?, model?, options?, api_key? | api_key_from?}";
    std::optional<std::string> id = profile_id(ctx);
    if (!id) return bad_request("malformed %-escape in profile id");
    if (ctx.body.type() != Value::Type::Map) return bad_request(kShape);
    core::ProfileUpdate u;
    for (const auto& [key, value] : ctx.body.as_map()) {
        std::optional<bool> ok = take_common(key, value, u);
        if (!ok || !*ok) return bad_request(kShape);
    }
    core::ModelFailure why = core::ModelFailure::None;
    auto r = deps.models.update(*id, u, &why);
    if (!r) return model_error(r.error(), why);
    return net::HttpResponse::json(200, core::to_value(r.value()));
}

net::HttpResponse delete_profile(AdminDeps& deps, const RequestContext& ctx) {
    std::optional<std::string> id = profile_id(ctx);
    if (!id) return bad_request("malformed %-escape in profile id");
    core::ModelFailure why = core::ModelFailure::None;
    auto r = deps.models.remove(*id, &why);
    if (!r) return model_error(r.error(), why);
    return no_content();
}

// {id}; answers with the whole state, like GET /ai/profiles.
net::HttpResponse activate_profile(AdminDeps& deps, const RequestContext& ctx) {
    const Value* id = ctx.body.find("id");
    if (ctx.body.type() != Value::Type::Map || ctx.body.as_map().size() != 1 || id == nullptr ||
        id->type() != Value::Type::String) {
        return bad_request("body must be {\"id\": \"<profile id>\"}");
    }
    auto r = deps.models.activate(id->as_string());
    if (!r) return service_error(r.error());
    return net::HttpResponse::json(200, core::to_value(deps.models.get()));
}

// Blocks the HTTP thread for up to the probe's 10 s timeout. "error" is
// null on success, else the category from core::ModelProbe.
net::HttpResponse test_profile(AdminDeps& deps, const RequestContext& ctx) {
    std::optional<std::string> id = profile_id(ctx);
    if (!id) return bad_request("malformed %-escape in profile id");
    auto r = deps.models.test(*id);
    if (!r && r.error().code == ErrorCode::Unsupported) {
        return net::HttpResponse::error(503, "unavailable", "no HTTP client on this platform");
    }
    if (!r) return service_error(r.error());
    core::ModelProbe& p = r.value();
    List models;
    for (std::string& m : p.models) models.push_back(Value(std::move(m)));
    return net::HttpResponse::json(200, Value(Map{
                                            {"ok", Value(p.ok)},
                                            {"error", p.error.empty() ? Value() : Value(p.error)},
                                            {"http_status", Value(p.http_status)},
                                            {"latency_ms", Value(int64_t{p.latency_ms})},
                                            {"models", Value(std::move(models))},
                                        }));
}

}  // namespace

void add_ai_routes(std::vector<Route>& routes) {
    using net::Method;
    routes.push_back({Method::Get, "/ai/profiles", false, get_profiles});
    routes.push_back({Method::Post, "/ai/profiles", false, create_profile});
    routes.push_back({Method::Put, "/ai/profiles/*", false, update_profile});
    routes.push_back({Method::Delete, "/ai/profiles/*", false, delete_profile});
    routes.push_back({Method::Post, "/ai/profiles/*/test", false, test_profile});
    routes.push_back({Method::Get, "/ai/presets", false, get_presets});
    routes.push_back({Method::Post, "/ai/active", false, activate_profile});
}

}  // namespace wisp::admin::detail
