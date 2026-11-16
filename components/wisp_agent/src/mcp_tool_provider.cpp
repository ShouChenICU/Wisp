#include "wisp/agent/mcp_tool_provider.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "tools_json.hpp"
#include "wisp/agent/tool_index.hpp"
#include "wisp/base/json.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::agent {

namespace {

using net::McpError;
using net::McpFailure;
using net::McpTool;

std::string_view view(const os::LargeString& s) { return {s.data(), s.size()}; }

ui::Lang lang_of(const ToolCtx& ctx) { return ui::parse_lang(ctx.lang).value_or(ui::Lang::Zh); }

os::LargeString source_of(std::string_view server_id) {
    os::LargeString source = "mcp:";
    source.append(server_id.data(), server_id.size());
    return source;
}

os::LargeString full_name_of(std::string_view server_id, std::string_view tool) {
    os::LargeString name;
    name.reserve(server_id.size() + 1 + tool.size());
    name.append(server_id.data(), server_id.size());
    name.push_back(':');
    name.append(tool.data(), tool.size());
    return name;
}

// The client normalised the schema to a JSON object's compact text when it
// listed the tool, so it goes into tools.find as it is.
ToolInfo to_info(std::string_view server_id, std::string_view server_name, const McpTool& tool,
                 bool details) {
    ToolInfo info;
    info.name = full_name_of(server_id, view(tool.name));
    info.description =
        detail::external_data_large(view(source_of(server_id)), view(tool.description));
    if (details) info.parameters = tool.input_schema;
    info.origin.assign(server_name.data(), server_name.size());
    info.available = true;
    return info;
}

void add_issue(ToolSearch& out, std::string_view server_id, std::string_view error) {
    ToolSourceIssue issue;
    issue.source.assign(server_id.data(), server_id.size());
    issue.error.assign(error.data(), error.size());
    out.unavailable.push_back(std::move(issue));
}

// A server the client knows to be failing (in its backoff window).
void add_known_failure(ToolSearch& out, const net::McpServerStatus& s) {
    if (!s.error.empty()) add_issue(out, s.id, s.error);
}

// The word tools.find shows for a server that could not be listed.
std::string_view search_error(const McpFailure& f) {
    if (f.error == McpError::Unavailable) return f.category;
    return net::to_string(f.error);
}

// {"ok":false,"error":code,"message":<wrapped text>}, built in PSRAM: a
// tool's error text can be as large as its result (1 MiB).
ToolOutput wrapped_error(std::string_view code, std::string_view server_id, std::string_view text) {
    ToolOutput out;
    out.ok = false;
    out.result_json = R"({"ok":false,"error":)";
    append_json_string(out.result_json, code);
    out.result_json.append(",\"message\":");
    detail::append_external(out.result_json, view(source_of(server_id)), text);
    out.result_json.push_back('}');
    return out;
}

ToolOutput failure_output(const McpFailure& f, std::string_view server_id) {
    const std::string id(server_id);
    switch (f.error) {
        case McpError::Unavailable: {
            std::string message = f.category;
            if (f.category == "http") message += " " + std::to_string(f.http_status);
            return tool_error("mcp_unavailable", message);
        }
        case McpError::RpcError:
            return tool_error("mcp_error",
                              external_data(view(source_of(server_id)), view(f.message)),
                              Map{{"code", Value(f.rpc_code)}});
        case McpError::NotFound:
            return tool_error("not_found", "no such tool on MCP server " + id);
        case McpError::BadResponse:
            return tool_error("bad_response", "MCP server " + id + " sent an unusable response");
        case McpError::Cancelled:
            return tool_error("cancelled", "the turn was stopped");
        case McpError::Timeout:
            return tool_error("timeout", "MCP server " + id + " did not answer in the turn's time");
        case McpError::UnsupportedProtocol:
            return tool_error("unsupported_protocol",
                              "MCP server " + id + " speaks an unsupported protocol revision");
        case McpError::ToolError:
        case McpError::None:
            break;
    }
    return tool_error("internal", "unexpected MCP failure");
}

}  // namespace

McpToolProvider::McpToolProvider(net::McpClient& client, core::McpServerService& servers,
                                 McpProviderConfig config)
    : client_(client), servers_(servers), config_(std::move(config)) {}

std::string_view McpToolProvider::id() const { return "mcp"; }

ToolFindFields McpToolProvider::find_fields() const {
    return {.origin = "server_name", .available = "available"};
}

bool McpToolProvider::owns(std::string_view full_name) const {
    return full_name.find(':') != std::string_view::npos;
}

net::McpWait McpToolProvider::wait_for(const ToolCtx& ctx, uint64_t limit_ms) const {
    net::McpWait wait;
    wait.cancelled = ctx.cancelled;
    wait.deadline_ms = limit_ms;
    if (ctx.deadline_ms != 0) {
        // ctx.deadline_ms is on os::now_ms(); the client compares with
        // config_.now_ms, which a test may run ahead (the same in
        // production). An expired deadline stays non-zero (0 is "none").
        const uint64_t real = os::now_ms();
        const uint64_t left = ctx.deadline_ms > real ? ctx.deadline_ms - real : 0;
        const uint64_t turn = std::max<uint64_t>(config_.now_ms() + left, 1);
        if (wait.deadline_ms == 0 || turn < wait.deadline_ms) wait.deadline_ms = turn;
    }
    return wait;
}

void McpToolProvider::search(std::string_view query, std::string_view scope, size_t max,
                             bool details, ToolCtx& ctx, ToolSearch& out) {
    const net::McpWait wait = wait_for(ctx, config_.now_ms() + config_.search_budget_ms);
    const std::vector<net::McpServerStatus> known = client_.status();
    size_t got = 0;
    for (const core::McpServerInfo& server : servers_.list()) {
        if (!server.enabled || (!scope.empty() && scope != server.id)) continue;
        if (got >= max) {
            // No room left, so no request; a known failure is still named.
            for (const auto& s : known)
                if (s.id == server.id) add_known_failure(out, s);
            continue;
        }
        if (ctx.cancelled && ctx.cancelled()) return;
        // The budget is spent: the rest are not reached, which is not
        // their failure (no backoff).
        if (config_.now_ms() >= wait.deadline_ms) {
            add_issue(out, server.id, "timeout");
            continue;
        }
        const McpFailure f =
            client_.visit_tools(server.id, wait, [&](const os::LargeVector<McpTool>& tools) {
                for (const McpTool& tool : tools) {
                    if (got >= max) break;
                    const os::LargeString name = full_name_of(server.id, view(tool.name));
                    if (!match_query(query, view(name), view(tool.description))) continue;
                    out.tools.push_back(to_info(server.id, view(server.name), tool, details));
                    ++got;
                }
            });
        switch (f.error) {
            case McpError::None:
            case McpError::NotFound:  // disabled or removed meanwhile
                break;
            case McpError::Cancelled:
                return;
            default:
                add_issue(out, server.id, search_error(f));
                break;
        }
    }
}

size_t McpToolProvider::count() const {
    size_t total = 0;
    for (const auto& s : client_.status())
        if (s.enabled) total += s.tools.value_or(0);
    return total;
}

void McpToolProvider::report_unavailable(ToolSearch& out, std::string_view scope) const {
    for (const auto& s : client_.status())
        if (s.enabled && (scope.empty() || scope == s.id)) add_known_failure(out, s);
}

std::optional<ToolInfo> McpToolProvider::describe(std::string_view full_name, ToolCtx& ctx) {
    const size_t colon = full_name.find(':');
    if (colon == std::string_view::npos) return std::nullopt;
    const std::string_view server_id = full_name.substr(0, colon);
    const std::string_view tool_name = full_name.substr(colon + 1);
    const auto server = servers_.get(server_id);
    if (!server || !server->enabled) return std::nullopt;
    std::optional<ToolInfo> info;
    client_.visit_tools(server_id, wait_for(ctx, 0), [&](const os::LargeVector<McpTool>& tools) {
        for (const McpTool& tool : tools) {
            if (view(tool.name) != tool_name) continue;
            info = to_info(server_id, view(server->name), tool, true);
            return;
        }
    });
    return info;
}

ToolOutput McpToolProvider::call(std::string_view full_name, const Value& args, ToolCtx& ctx) {
    const size_t colon = full_name.find(':');
    if (colon == std::string_view::npos)
        return tool_error("not_found", "no MCP tool named " + std::string(full_name));
    const std::string server_id(full_name.substr(0, colon));
    const std::string_view tool_name = full_name.substr(colon + 1);
    const auto server = servers_.get(server_id);
    if (!server) return tool_error("not_found", "no MCP server " + server_id);
    if (!server->enabled)
        return tool_error("not_found", "MCP server " + server_id + " is disabled");
    if (args.type() != Value::Type::Map)
        return tool_error("bad_args", "arguments must be a JSON object", Map{{"field", ""}});

    const net::McpWait wait = wait_for(ctx, 0);
    bool found = false;
    os::LargeString schema;
    const McpFailure listed =
        client_.visit_tools(server_id, wait, [&](const os::LargeVector<McpTool>& tools) {
            for (const McpTool& tool : tools) {
                if (view(tool.name) != tool_name) continue;
                found = true;
                schema = tool.input_schema;
                return;
            }
        });
    if (listed.error != McpError::None) return failure_output(listed, server_id);
    if (!found) {
        return tool_error("not_found",
                          "MCP server " + server_id + " has no tool " + std::string(tool_name));
    }
    // Spec 4.2 "Arguments": checked here when the schema is in the subset
    // validate_args() knows; otherwise the server judges them.
    if (auto parsed = from_json(view(schema))) {
        std::string why;
        if (schema_supported(parsed.value(), &why)) {
            if (const auto field = validate_args(parsed.value(), args, &why))
                return tool_error("bad_args", why, Map{{"field", *field}});
        }
    }

    const net::McpCallResult r = client_.call_tool(server_id, tool_name, to_json(args), wait);
    if (r.failure.error != McpError::None) return failure_output(r.failure, server_id);
    if (r.is_error) return wrapped_error("tool_error", server_id, view(r.text));

    ToolOutput out;
    out.result_json = R"({"ok":true,"result":)";
    detail::append_external(out.result_json, view(source_of(server_id)), view(r.text));
    out.result_json.push_back('}');
    out.note =
        ui::tr_fill_n(ui::StrId::AgentNoteToolCall, lang_of(ctx), {view(server->name), tool_name});
    return out;
}

std::vector<std::string> McpToolProvider::summary_lines() const {
    std::vector<std::string> lines;
    for (const auto& s : client_.status()) {
        if (!s.enabled) continue;
        std::string line = s.id;
        if (!s.error.empty()) {
            line += " (unavailable: " + s.error + ")";
        } else if (s.tools) {
            line += " (" + std::to_string(*s.tools) + " tools)";
        } else {
            line += " (not connected yet)";
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

std::vector<net::McpServerStatus> McpToolProvider::status() const { return client_.status(); }

}  // namespace wisp::agent
