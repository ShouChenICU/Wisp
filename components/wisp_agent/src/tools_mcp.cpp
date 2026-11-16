// mcp.servers, mcp.add, mcp.remove and mcp.enable (llm-tool-providers spec
// 4.3, decision 7): thin adapters over core::McpServerService, with the
// tool counts and remembered failures of McpToolProvider::status().
//
// They run on the agent's thread: the service changes memory and queues
// its file write on the flash thread (core::LatestSave), and status() is
// memory only. No confirmation: adding a server gives it no credentials,
// since header values are set by the owner on the web page alone, and
// every change shows the user a note.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tool_io.hpp"
#include "wisp/agent/mcp_tool_provider.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::agent {

namespace {

using detail::arg_str;
using detail::done;

constexpr uint32_t kMcpToolMs = 5000;
constexpr const char* kAddSchema =
    R"({"type":"object","properties":{"name":{"type":"string","maxLength":32},)"
    R"("url":{"type":"string"}},"required":["name","url"]})";
constexpr const char* kIdSchema =
    R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})";
constexpr const char* kEnableSchema =
    R"({"type":"object","properties":{"id":{"type":"string"},"enabled":{"type":"boolean"}},)"
    R"("required":["id","enabled"]})";

ui::Lang lang_of(const ToolCtx& ctx) { return ui::parse_lang(ctx.lang).value_or(ui::Lang::Zh); }

std::string text(const os::LargeString& s) { return std::string(s.data(), s.size()); }

ToolOutput not_found(const std::string& id) {
    return tool_error("not_found", "no MCP server " + id);
}

Value server_value(const core::McpServerInfo& s,
                   const std::vector<net::McpServerStatus>& statuses) {
    Value tools;  // null: never listed
    Value error;  // null: not failing
    for (const net::McpServerStatus& st : statuses) {
        if (st.id != s.id) continue;
        if (st.tools) tools = Value(static_cast<int64_t>(*st.tools));
        if (!st.error.empty()) error = Value(st.error);
    }
    // Names only: a header's value never leaves the service.
    List headers;
    for (const core::McpHeaderInfo& h : s.headers) headers.push_back(Value(text(h.name)));
    return Value(Map{{"id", Value(s.id)},
                     {"name", Value(text(s.name))},
                     {"url", Value(text(s.url))},
                     {"enabled", Value(s.enabled)},
                     {"tools", std::move(tools)},
                     {"error", std::move(error)},
                     {"headers", Value(std::move(headers))}});
}

}  // namespace

Result<void> add_mcp_tools(ToolRegistry& reg, McpToolDeps deps) {
    // Lives as long as the tools, so in PSRAM with the registry.
    auto d = std::allocate_shared<McpToolDeps>(os::LargeAllocator<McpToolDeps>(), deps);

    WISP_TRY(reg.add({
        .name = "mcp.servers",
        .description = "The configured MCP servers: id, name, URL, whether enabled, how many "
                       "tools each offers (null before the first tools_find reaches it), the "
                       "last failure's category (null when none) and the names of the headers "
                       "the owner set; at most `max` servers.",
        .thread = ToolThread::Agent,
        .timeout_ms = kMcpToolMs,
        .run =
            [d](const Value&, ToolCtx&) {
                const std::vector<net::McpServerStatus> statuses = d->provider.status();
                List items;
                for (const core::McpServerInfo& s : d->servers.list())
                    items.push_back(server_value(s, statuses));
                return done({{"servers", Value(std::move(items))},
                             {"max", Value(static_cast<int64_t>(core::kMcpMaxServers))}});
            },
    }));

    WISP_TRY(reg.add({
        .name = "mcp.add",
        .description = "Adds an MCP server (streamable HTTP) by name and http(s) URL, enabled; "
                       "its tools are then reached through tools_find and tools_call. Headers "
                       "such as an API token cannot be set here: the owner adds them on the web "
                       "Models page.",
        .schema_json = kAddSchema,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Agent,
        .timeout_ms = kMcpToolMs,
        .run =
            [d](const Value& a, ToolCtx& ctx) {
                // Only name and url are read: a "headers" argument is never
                // stored.
                core::McpServerInput in;
                in.name = arg_str(a, "name");
                in.url = arg_str(a, "url");
                in.enabled = true;
                core::McpFailureKind why = core::McpFailureKind::None;
                auto r = d->servers.create(in, &why);
                if (!r) {
                    if (why == core::McpFailureKind::LimitReached)
                        return tool_error("limit_reached", r.error().message);
                    if (r.error().code == ErrorCode::InvalidArgument)
                        return tool_error("bad_args", r.error().message);
                    return detail::fail(r.error());
                }
                const core::McpServerInfo& s = r.value();
                ToolOutput out = done({{"server", Value(Map{{"id", Value(s.id)},
                                                            {"name", Value(text(s.name))},
                                                            {"url", Value(text(s.url))},
                                                            {"enabled", Value(s.enabled)}})}});
                out.note = ui::tr_fill(ui::StrId::AgentNoteMcpAdded, lang_of(ctx), text(s.name));
                return out;
            },
    }));

    WISP_TRY(reg.add({
        .name = "mcp.remove",
        .description = "Removes an MCP server, with the headers the owner set for it.",
        .schema_json = kIdSchema,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Agent,
        .timeout_ms = kMcpToolMs,
        .run =
            [d](const Value& a, ToolCtx& ctx) {
                const std::string& id = arg_str(a, "id");
                const std::optional<core::McpServerInfo> s = d->servers.get(id);
                if (!s) return not_found(id);
                auto r = d->servers.remove(id);
                if (!r) return detail::fail(r.error());
                ToolOutput out = done();
                out.note = ui::tr_fill(ui::StrId::AgentNoteMcpRemoved, lang_of(ctx), text(s->name));
                return out;
            },
    }));

    return reg.add({
        .name = "mcp.enable",
        .description = "Enables or disables an MCP server; a disabled server keeps its "
                       "settings and headers, and tools_find leaves it out.",
        .schema_json = kEnableSchema,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Agent,
        .timeout_ms = kMcpToolMs,
        .run =
            [d](const Value& a, ToolCtx& ctx) {
                const std::string& id = arg_str(a, "id");
                const Value* flag = a.find("enabled");
                const bool enabled = flag != nullptr && flag->as_bool();
                core::McpServerUpdate u;
                u.enabled = enabled;
                auto r = d->servers.update(id, u);
                if (!r) return detail::fail(r.error());
                ToolOutput out = done({{"enabled", Value(r.value().enabled)}});
                out.note = ui::tr_fill(
                    enabled ? ui::StrId::AgentNoteMcpEnabled : ui::StrId::AgentNoteMcpDisabled,
                    lang_of(ctx), text(r.value().name));
                return out;
            },
    });
}

}  // namespace wisp::agent
