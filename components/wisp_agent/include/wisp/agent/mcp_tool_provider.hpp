#pragma once

// McpToolProvider (llm-tool-providers spec 4.1-4.4): the tools of the
// configured MCP servers, reached through tools.find and tools.call as
// "<server id>:<tool>". A thin adapter over net::McpClient, which owns the
// sessions, the tool caches and the 60 s backoff of a failed server.
//
// Everything a server says reaches the model wrapped as external data,
// external_data("mcp:<server id>", ...): descriptions, results and error
// messages. A tools.find over every server gets kMcpSearchBudgetMs in all
// (and never past the turn's deadline): servers it did not reach in time
// are listed unavailable with "timeout", which starts no backoff, since
// it is the budget's doing and not the server's.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/agent/tool_provider.hpp"
#include "wisp/core/mcp_servers.hpp"
#include "wisp/net/mcp_client.hpp"
#include "wisp/os/os.hpp"

namespace wisp::agent {

inline constexpr uint64_t kMcpSearchBudgetMs = 20000;

struct McpProviderConfig {
    uint64_t search_budget_ms = kMcpSearchBudgetMs;
    // The client's clock (McpClientConfig::now_ms): McpWait deadlines are
    // compared with it.
    std::function<uint64_t()> now_ms = os::now_ms;
};

class McpToolProvider final : public ToolProvider {
public:
    // Both must outlive the provider.
    McpToolProvider(net::McpClient& client, core::McpServerService& servers,
                    McpProviderConfig config = {});

    std::string_view id() const override;
    ToolFindFields find_fields() const override;
    // Every name with a ':' (scene tool names have none).
    bool owns(std::string_view full_name) const override;
    // Enabled servers in configuration order; `scope` a server id or "".
    void search(std::string_view query, std::string_view scope, size_t max, bool details,
                ToolCtx& ctx, ToolSearch& out) override;
    // Cached tools of enabled servers.
    size_t count() const override;
    // Enabled servers in `scope` ("" for all) in their backoff window, with
    // the remembered category.
    void report_unavailable(ToolSearch& out, std::string_view scope) const override;
    std::optional<ToolInfo> describe(std::string_view full_name, ToolCtx& ctx) override;
    ToolOutput call(std::string_view full_name, const Value& args, ToolCtx& ctx) override;

    // One entry per enabled server, in configuration order: "<id> (<n>
    // tools)", "<id> (not connected yet)" or "<id> (unavailable:
    // <category>)" (plan decision 18). Memory only.
    std::vector<std::string> summary_lines() const;
    // The client's, for mcp.servers.
    std::vector<net::McpServerStatus> status() const;

private:
    // The wait a request made for `ctx` may take, ending at `limit_ms` on
    // config_.now_ms's clock (0: none) or the turn's deadline, if sooner.
    net::McpWait wait_for(const ToolCtx& ctx, uint64_t limit_ms) const;

    net::McpClient& client_;
    core::McpServerService& servers_;
    McpProviderConfig config_;
};

}  // namespace wisp::agent
