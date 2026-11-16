// MCP servers (spec 4.3 of
// docs/superpowers/specs/2026-11-15-llm-tool-providers-design.md): thin
// adapters over core::McpServerService.
//
// Header values are write-only, as the model profiles' API keys are
// (routes_ai.cpp): a server in a response carries each header's name and
// has_value only, and the test result carries the probe's category,
// latency, protocol and tool names, never anything the server sent beyond
// them. No route logs a request body, and no refusal repeats a value.
// Unknown fields are rejected so a typo does not silently do nothing.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "admin_internal.hpp"
#include "wisp/core/mcp_servers.hpp"

namespace wisp::admin::detail {

namespace {

net::HttpResponse unavailable() {
    return net::HttpResponse::error(503, "unavailable", "MCP servers are not available");
}

// The service's error, with "limit_reached" for the ninth server.
net::HttpResponse mcp_error(const Error& error, core::McpFailureKind why) {
    if (why == core::McpFailureKind::LimitReached) {
        return net::HttpResponse::error(net::http_status_for(error.code), "limit_reached",
                                        error.message);
    }
    return service_error(error);
}

std::optional<std::string> server_id(const RequestContext& ctx) {
    return percent_decode(ctx.param);
}

bool take_string(const Value& v, std::optional<std::string>& out) {
    if (v.type() != Value::Type::String) return false;
    out = v.as_string();
    return true;
}

bool take_bool(const Value& v, std::optional<bool>& out) {
    if (v.type() != Value::Type::Bool) return false;
    out = v.as_bool();
    return true;
}

// Any non-negative integer that fits; the service checks the range and
// says what it is.
bool take_timeout(const Value& v, std::optional<uint32_t>& out) {
    if (v.type() != Value::Type::Int) return false;
    const int64_t ms = v.as_int();
    if (ms < 0 || ms > static_cast<int64_t>(UINT32_MAX)) return false;
    out = static_cast<uint32_t>(ms);
    return true;
}

// [{name, value?}]: whether a value is required is the service's call
// (create needs one; update keeps the stored value without one).
bool take_headers(const Value& v, std::optional<std::vector<core::McpHeaderInput>>& out) {
    if (v.type() != Value::Type::List) return false;
    std::vector<core::McpHeaderInput> headers;
    for (const Value& entry : v.as_list()) {
        if (entry.type() != Value::Type::Map) return false;
        std::optional<std::string> name;
        core::McpHeaderInput h;
        for (const auto& [key, item] : entry.as_map()) {
            bool ok = false;
            if (key == "name") ok = take_string(item, name);
            if (key == "value") ok = take_string(item, h.value);
            if (!ok) return false;
        }
        if (!name) return false;
        h.name = std::move(*name);
        headers.push_back(std::move(h));
    }
    out = std::move(headers);
    return true;
}

// The fields create and update share, into an update. false: an unknown
// field or a wrong type.
bool take_field(std::string_view key, const Value& value, core::McpServerUpdate& u) {
    if (key == "name") return take_string(value, u.name);
    if (key == "url") return take_string(value, u.url);
    if (key == "enabled") return take_bool(value, u.enabled);
    if (key == "timeout_ms") return take_timeout(value, u.timeout_ms);
    if (key == "headers") return take_headers(value, u.headers);
    return false;
}

net::HttpResponse list_servers(AdminDeps& deps, const RequestContext&) {
    if (deps.mcp == nullptr) return unavailable();
    List servers;
    for (const core::McpServerInfo& s : deps.mcp->list()) servers.push_back(core::to_value(s));
    return net::HttpResponse::json(
        200, Value(Map{{"servers", Value(std::move(servers))},
                       {"max", Value(static_cast<int64_t>(core::kMcpMaxServers))}}));
}

// {name, url, enabled?, timeout_ms?, headers?:[{name, value}]}
net::HttpResponse create_server(AdminDeps& deps, const RequestContext& ctx) {
    if (deps.mcp == nullptr) return unavailable();
    constexpr std::string_view kShape =
        "body must be {name, url, enabled?, timeout_ms?, headers?:[{name, value}]}";
    if (ctx.body.type() != Value::Type::Map) return bad_request(kShape);
    core::McpServerUpdate fields;
    for (const auto& [key, value] : ctx.body.as_map()) {
        if (!take_field(key, value, fields)) return bad_request(kShape);
    }
    if (!fields.name || !fields.url) return bad_request(kShape);

    core::McpServerInput in;
    in.name = std::move(*fields.name);
    in.url = std::move(*fields.url);
    if (fields.enabled) in.enabled = *fields.enabled;
    in.timeout_ms = fields.timeout_ms;
    if (fields.headers) in.headers = std::move(*fields.headers);
    core::McpFailureKind why = core::McpFailureKind::None;
    auto r = deps.mcp->create(in, &why);
    if (!r) return mcp_error(r.error(), why);
    return net::HttpResponse::json(201, core::to_value(r.value()));
}

// Any subset of the create fields. `headers` replaces the list; an entry
// without a value keeps the stored one only while the origin stays the
// same, so the answer's headers show the web card which were dropped.
net::HttpResponse update_server(AdminDeps& deps, const RequestContext& ctx) {
    if (deps.mcp == nullptr) return unavailable();
    constexpr std::string_view kShape =
        "body must be {name?, url?, enabled?, timeout_ms?, headers?:[{name, value?}]}";
    std::optional<std::string> id = server_id(ctx);
    if (!id) return bad_request("malformed %-escape in server id");
    if (ctx.body.type() != Value::Type::Map) return bad_request(kShape);
    core::McpServerUpdate u;
    for (const auto& [key, value] : ctx.body.as_map()) {
        if (!take_field(key, value, u)) return bad_request(kShape);
    }
    auto r = deps.mcp->update(*id, u);
    if (!r) return service_error(r.error());
    return net::HttpResponse::json(200, core::to_value(r.value()));
}

net::HttpResponse delete_server(AdminDeps& deps, const RequestContext& ctx) {
    if (deps.mcp == nullptr) return unavailable();
    std::optional<std::string> id = server_id(ctx);
    if (!id) return bad_request("malformed %-escape in server id");
    auto r = deps.mcp->remove(*id);
    if (!r) return service_error(r.error());
    return no_content();
}

// {ok, error, latency_ms, protocol, tools}: "error" is null on success,
// else the category from core::McpProbe. Blocks the HTTP thread for up to
// the probe's budget (net::kMcpProbeTimeoutMs). The probe sends on the
// agent's HTTP id, so cancelling an agent turn at the same moment cancels
// the probe too (it then reports "timeout"); accepted for a test button.
net::HttpResponse test_server(AdminDeps& deps, const RequestContext& ctx) {
    if (deps.mcp == nullptr) return unavailable();
    std::optional<std::string> id = server_id(ctx);
    if (!id) return bad_request("malformed %-escape in server id");
    auto r = deps.mcp->test(*id);
    if (!r && r.error().code == ErrorCode::Unsupported) {
        return net::HttpResponse::error(503, "unavailable", "no HTTP client on this platform");
    }
    if (!r) return service_error(r.error());
    const core::McpProbe& p = r.value();
    List tools;
    for (const os::LargeString& t : p.tools)
        tools.push_back(Value(std::string(t.data(), t.size())));
    return net::HttpResponse::json(200,
                                   Value(Map{
                                       {"ok", Value(p.ok)},
                                       {"error", p.error.empty() ? Value() : Value(p.error)},
                                       {"latency_ms", Value(static_cast<int64_t>(p.latency_ms))},
                                       {"protocol", Value(p.protocol)},
                                       {"tools", Value(std::move(tools))},
                                   }));
}

}  // namespace

void add_mcp_routes(std::vector<Route>& routes) {
    using net::Method;
    routes.push_back({Method::Get, "/mcp/servers", false, list_servers});
    routes.push_back({Method::Post, "/mcp/servers", false, create_server});
    routes.push_back({Method::Put, "/mcp/servers/*", false, update_server});
    routes.push_back({Method::Delete, "/mcp/servers/*", false, delete_server});
    routes.push_back({Method::Post, "/mcp/servers/*/test", false, test_server});
}

}  // namespace wisp::admin::detail
