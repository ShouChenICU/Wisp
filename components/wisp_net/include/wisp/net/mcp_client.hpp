#pragma once

// McpClient: the agent's client for MCP servers over streamable HTTP
// (spec 4.2-4.4 of docs/superpowers/specs/2026-11-15-llm-tool-providers-design.md).
//
// Every message is one JSON-RPC 2.0 POST through SceneHttp's Agent
// principal (scene id kAgentHttpId, at most 2 redirects), with
// "Accept: application/json, text/event-stream", "Content-Type:
// application/json" and the server's stored headers; after initialize also
// Mcp-Session-Id (when the server gave one) and MCP-Protocol-Version. The
// answer is read as it arrives (SceneHttpRequest::on_body on wisp.shttp):
// a body starting with '{' is collected and parsed at the end; anything
// else is read as server-sent events, requests and notifications skipped,
// and the transfer stops (stop_is_success) at the response with the
// request's id, so a server that keeps its stream open costs nothing.
//
// Per server, in memory only and keyed by its id and revision (a change of
// the server's configuration drops both): the session (id, negotiated
// revision), the tool list (kept kMcpListTtlMs) and the last failure, which
// keeps the server from being contacted for kMcpRetryAfterMs. A 404 to a
// request that carried a session re-initializes once and retries that
// request once. A wait that the caller's deadline or cancellation ended is
// not the server's failure and is not remembered.
//
// Memory: request bodies, response buffers, parsed documents
// (parse_json_large()) and the tool cache are in PSRAM (os::Large*).
//
// Privacy: one log line per request, "mcp: <server id> <method>
// ok|error:<code> <ms> ms"; never a URL, a header, arguments or results.
// Tool descriptions are made valid UTF-8 when listed (the LLM request body
// is not repaired later); schemas that are not valid UTF-8 are replaced.
// McpFailure::message carries a server's text unscrubbed: whatever hands
// it to the model must scrub it first (spec 4.4 "Scrubbing").

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/core/mcp_servers.hpp"
#include "wisp/net/scene_http.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::net {

inline constexpr std::string_view kMcpProtocol = "2025-06-18";
inline constexpr std::string_view kMcpProtocolOld = "2025-03-26";
// initialize, notifications/initialized, each tools/list page.
inline constexpr uint32_t kMcpRequestTimeoutMs = 15000;
// How much longer than its own timeout a wait gives a request whose answer
// never comes: one queued behind others on wisp.shttp, or one cancelled by
// another holder of kAgentHttpId, which SceneHttp then never answers.
inline constexpr uint32_t kMcpWaitSlackMs = 15000;
// probe()'s whole budget, however many requests and pages it takes (the
// admin test route waits for it): one request's timeout plus the slack.
inline constexpr uint32_t kMcpProbeTimeoutMs = kMcpRequestTimeoutMs + kMcpWaitSlackMs;
inline constexpr uint64_t kMcpListTtlMs = 10 * 60 * 1000;
inline constexpr uint64_t kMcpRetryAfterMs = 60 * 1000;
inline constexpr size_t kMcpMaxTools = 128;
inline constexpr size_t kMcpMaxListPages = 16;
inline constexpr size_t kMcpMaxDescriptionBytes = 1024;
inline constexpr size_t kMcpMaxSchemaBytes = 16 * 1024;
inline constexpr size_t kMcpMaxSessionIdBytes = 256;
// initialize, tools/list.
inline constexpr size_t kMcpMaxResponse = 512 * 1024;
inline constexpr size_t kMcpMaxCallResponse = AgentHttpLimits::kMaxResponse;

enum class McpError : uint8_t {
    None,
    Unavailable,
    RpcError,
    ToolError,
    BadResponse,
    NotFound,
    Cancelled,
    Timeout,
    UnsupportedProtocol
};

// "", "mcp_unavailable", "mcp_error", "tool_error", "bad_response",
// "not_found", "cancelled", "timeout", "unsupported_protocol".
const char* to_string(McpError e);

struct McpFailure {
    McpError error = McpError::None;
    // Unavailable only: dns connect tls timeout offline http other.
    std::string category{};
    int http_status = 0;
    int64_t rpc_code = 0;
    // Server text only for RpcError/ToolError, valid UTF-8 and unscrubbed
    // (see the file header).
    os::LargeString message{};
};

// A listed tool: name [A-Za-z0-9_.-]{1,64}; description valid UTF-8, at
// most kMcpMaxDescriptionBytes; input_schema compact JSON of an object
// ({"type":"object"} when the server gave none, or not an object).
struct McpTool {
    os::LargeString name{};
    os::LargeString description{};
    os::LargeString input_schema{};
};

// deadline_ms 0: none (each request still ends at its own timeout).
struct McpWait {
    uint64_t deadline_ms = 0;
    std::function<bool()> cancelled{};
};

struct McpListResult {
    McpFailure failure{};
    os::LargeVector<McpTool> tools{};
};

struct McpCallResult {
    McpFailure failure{};
    bool is_error = false;
    os::LargeString text{};
};

struct McpServerStatus {
    std::string id{};
    os::LargeString name{};
    bool enabled = true;
    // nullopt: never listed (or dropped by a configuration change).
    std::optional<size_t> tools{};
    // The last failure's category while backing off, else "".
    std::string error{};
};

namespace detail {
// A server's MCP session as McpClient keeps it between requests.
struct McpSession {
    bool initialized = false;
    os::LargeString id{};      // Mcp-Session-Id; "" when the server gave none
    std::string protocol{};    // the negotiated revision
    int64_t next_request = 1;  // JSON-RPC ids
};
}  // namespace detail

struct McpClientConfig {
    std::string client_version{};
    // Compared with McpWait::deadline_ms; os::now_ms in production.
    std::function<uint64_t()> now_ms = os::now_ms;
};

// Blocking; one caller at a time (wisp.agent). Never from wisp.shttp, whose
// lane it waits on. status() is memory only and may be called from any
// thread.
class McpClient {
public:
    McpClient(SceneHttp& http, core::McpServerService& servers, McpClientConfig config = {});
    McpClient(const McpClient&) = delete;
    McpClient& operator=(const McpClient&) = delete;

    // The server's tools: the cached list while younger than kMcpListTtlMs
    // (unless `refresh`), else initialize if needed and tools/list,
    // following nextCursor for at most kMcpMaxListPages pages and keeping
    // at most kMcpMaxTools tools. NotFound for an unknown or disabled
    // server, without a request; the remembered failure, without a
    // request, within kMcpRetryAfterMs of it.
    McpListResult list_tools(std::string_view server_id, const McpWait& wait, bool refresh = false);
    McpCallResult call_tool(std::string_view server_id, std::string_view tool,
                            std::string_view args_json, const McpWait& wait);
    // Every configured server, in configuration order.
    std::vector<McpServerStatus> status() const;

    // Initializes and lists once, sharing nothing with any client's cache
    // (the admin test route). It shares the Agent principal's scene id, so
    // a cancelled agent turn (cancel_scene(kAgentHttpId)) cancels a probe
    // running at the same moment too; it then reports "timeout". It ends
    // after `timeout_ms` in all ("timeout"), and then abandons its request
    // rather than cancel it, which would cancel the agent's too; the
    // request still ends at its own timeout.
    static core::McpProbe probe(SceneHttp& http, const core::McpServerTarget& target,
                                std::string_view client_version,
                                uint32_t timeout_ms = kMcpProbeTimeoutMs);

private:
    // One server's state; see the file header.
    struct Server {
        os::LargeString id{};
        uint32_t revision = 0;
        detail::McpSession session{};
        bool listed = false;
        uint64_t listed_at = 0;
        os::LargeVector<McpTool> tools{};
        bool failing = false;
        uint64_t failed_at = 0;
        McpFailure failure{};
    };

    SceneHttp& http_;
    core::McpServerService& servers_;
    McpClientConfig config_;
    // Guards state_ and seen_revision_; never held across a wait.
    mutable os::Mutex mu_;
    os::LargeVector<Server> state_{};
    uint32_t seen_revision_ = 0;
};

core::McpProber make_mcp_prober(SceneHttp& http, std::string client_version);

}  // namespace wisp::net
