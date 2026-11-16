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
//
// A create or update with a body is streamed (AdminServer::stream_handler())
// into PSRAM, because the largest configuration the service accepts is
// well past the 8 KiB every buffered API body is held to: eight 1024-byte
// header values alone are 8 KiB. A short one sent to handle() takes the
// same path.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "admin_internal.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/mcp_servers.hpp"
#include "wisp/os/large.hpp"

namespace wisp::admin::detail {

namespace {

constexpr std::string_view kServersPath = "/api/v1/mcp/servers";

// The largest create or update body: the service's bounds, with every byte
// of a string counted twice (a quote or backslash arrives escaped), plus
// generous allowances for the field names, punctuation and whitespace of
// each header entry and of the request around them.
constexpr size_t kHeaderEntryOverheadBytes = 64;
constexpr size_t kRequestOverheadBytes = 512;
constexpr uint64_t kMaxServerBodyBytes =
    kRequestOverheadBytes + 2 * (core::kMcpMaxNameBytes + core::kMcpMaxUrlBytes) +
    core::kMcpMaxHeaders * (kHeaderEntryOverheadBytes +
                            2 * (core::kMcpMaxHeaderNameBytes + core::kMcpMaxHeaderValueBytes));

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
net::HttpResponse create_from(AdminDeps& deps, const Value& body) {
    if (deps.mcp == nullptr) return unavailable();
    constexpr std::string_view kShape =
        "body must be {name, url, enabled?, timeout_ms?, headers?:[{name, value}]}";
    if (body.type() != Value::Type::Map) return bad_request(kShape);
    core::McpServerUpdate fields;
    for (const auto& [key, value] : body.as_map()) {
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

net::HttpResponse create_server(AdminDeps& deps, const RequestContext& ctx) {
    return create_from(deps, ctx.body);
}

// Any subset of the create fields. `headers` replaces the list; an entry
// without a value keeps the stored one only while the origin stays the
// same, so the answer's headers show the web card which were dropped.
// `raw_id` is the path segment, still URL-encoded.
net::HttpResponse update_from(AdminDeps& deps, std::string_view raw_id, const Value& body) {
    if (deps.mcp == nullptr) return unavailable();
    constexpr std::string_view kShape =
        "body must be {name?, url?, enabled?, timeout_ms?, headers?:[{name, value?}]}";
    std::optional<std::string> id = percent_decode(raw_id);
    if (!id) return bad_request("malformed %-escape in server id");
    if (body.type() != Value::Type::Map) return bad_request(kShape);
    core::McpServerUpdate u;
    for (const auto& [key, value] : body.as_map()) {
        if (!take_field(key, value, u)) return bad_request(kShape);
    }
    auto r = deps.mcp->update(*id, u);
    if (!r) return service_error(r.error());
    return net::HttpResponse::json(200, core::to_value(r.value()));
}

net::HttpResponse update_server(AdminDeps& deps, const RequestContext& ctx) {
    return update_from(deps, ctx.param, ctx.body);
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

// Collects a create (no id) or update body (Content-Length bytes, at most
// kMaxServerBodyBytes) in PSRAM, then hands it to the route's handler.
class ServerSink final : public net::BodySink {
public:
    ServerSink(AdminDeps& deps, std::optional<std::string> raw_id, os::LargeBuffer buffer)
        : deps_(deps), raw_id_(std::move(raw_id)), buffer_(std::move(buffer)) {}

    std::optional<net::HttpResponse> on_data(std::span<const uint8_t> chunk) override {
        if (chunk.size() > buffer_.size() - received_) {
            buffer_ = os::LargeBuffer();
            return with_no_store(bad_request("body longer than its Content-Length"));
        }
        std::memcpy(buffer_.data() + received_, chunk.data(), chunk.size());
        received_ += chunk.size();
        return std::nullopt;
    }

    net::HttpResponse on_end() override {
        net::HttpResponse res = finish();
        buffer_ = os::LargeBuffer();
        return with_no_store(std::move(res));
    }

    void on_abort() override { buffer_ = os::LargeBuffer(); }

private:
    static net::HttpResponse with_no_store(net::HttpResponse res) {
        res.headers.emplace_back("Cache-Control", "no-store");
        return res;
    }

    net::HttpResponse finish() {
        if (received_ != buffer_.size()) return bad_request("body shorter than its Content-Length");
        auto parsed = from_json(
            std::string_view(reinterpret_cast<const char*>(buffer_.data()), buffer_.size()));
        // Free the copy that holds the header values before the service
        // makes its own.
        buffer_ = os::LargeBuffer();
        if (!parsed) {
            return net::HttpResponse::error(400, "bad_json", "request body is not valid JSON");
        }
        if (raw_id_) return update_from(deps_, *raw_id_, parsed.value());
        return create_from(deps_, parsed.value());
    }

    AdminDeps& deps_;
    std::optional<std::string> raw_id_;
    os::LargeBuffer buffer_;
    size_t received_ = 0;
};

}  // namespace

std::optional<net::StreamStart> mcp_stream(AdminDeps& deps, const net::HttpRequest& head) {
    std::optional<std::string> raw_id;
    const std::string_view path = head.path;
    if (head.method == net::Method::Post && path == kServersPath) {
        // create
    } else if (head.method == net::Method::Put && path.size() > kServersPath.size() + 1 &&
               path.substr(0, kServersPath.size()) == kServersPath &&
               path[kServersPath.size()] == '/' &&
               path.find('/', kServersPath.size() + 1) == std::string_view::npos) {
        raw_id = std::string(path.substr(kServersPath.size() + 1));
    } else {
        return std::nullopt;
    }
    // No body: handle() answers it with the route's shape error, as before.
    if (head.content_length == 0) return std::nullopt;
    if (auto refused = stream_refusal(deps, head)) return stream_answer(std::move(*refused));
    if (deps.mcp == nullptr) return stream_answer(unavailable());
    if (head.content_length > kMaxServerBodyBytes) {
        return stream_answer(net::HttpResponse::error(
            413, "payload_too_large", "request body over the largest MCP server configuration"));
    }
    os::LargeBuffer buffer = os::LargeBuffer::allocate(static_cast<size_t>(head.content_length));
    if (!buffer.ok()) {
        return stream_answer(
            net::HttpResponse::error(503, "no_memory", "no memory for the request body"));
    }
    return net::StreamStart{
        std::make_unique<ServerSink>(deps, std::move(raw_id), std::move(buffer)), {}};
}

void add_mcp_routes(std::vector<Route>& routes) {
    using net::Method;
    routes.push_back({Method::Get, "/mcp/servers", false, list_servers});
    routes.push_back({Method::Post, "/mcp/servers", false, create_server});
    routes.push_back({Method::Put, "/mcp/servers/*", false, update_server});
    routes.push_back({Method::Delete, "/mcp/servers/*", false, delete_server});
    routes.push_back({Method::Post, "/mcp/servers/*/test", false, test_server});
}

}  // namespace wisp::admin::detail
