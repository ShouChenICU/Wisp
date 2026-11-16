#pragma once

// McpServerService (spec 4.3 of
// docs/superpowers/specs/2026-11-15-llm-tool-providers-design.md): the
// MCP servers the agent may reach -- name, URL, enabled, timeout and the
// HTTP headers (usually a bearer token) each request carries.
//
// Everything, header values included, lives in the System area as
// "secrets/mcp.json":
//   {"version":1, "servers":[{id,name,url,enabled,timeout_ms,
//                             headers:[{name,value}]}]}
//
// Header values are write-only, as the model profiles' API keys are
// (model_profiles.hpp; AGENTS.md section 6). Nothing returned by a public
// method carries a value or any part of it -- reads show
// {name, has_value} -- except resolve_for_request(), which exists only for
// net::McpClient; REST routes, Lua bindings and agent tools must never
// call it. test() hands the values to the prober and nowhere else. Nothing
// here logs a value, a URL, a file's contents or a URL+value pair, and no
// error message echoes a value.
//
// A value belongs to the origin (scheme://host:port; scheme and host
// case-insensitive, default ports made explicit, path ignored;
// core::url_origin()) of the URL it was saved with. An update() that moves
// the URL to another origin keeps only the values it gives anew; every
// other header is dropped rather than sent to a host the owner never gave
// it to.
//
// Ids come from the name on create() and never change: lowercase, every
// run of characters outside [a-z0-9_-] becomes one '-', '-' trimmed from
// both ends, at most 16 bytes, "mcp" when nothing is left; a taken id gets
// "-2", "-3", ... with the base shortened to stay within 16 bytes.
//
// cell() holds {count, enabled, revision} -- no URLs, no headers -- for the
// MCP client, which reads it to notice a change; the revision moves on
// every change. Each server has its own revision too, taken from the same
// counter, so a server removed and made again under its old id never
// repeats one a client keyed a session by. Revisions live in memory only.
//
// Thread-safe: the admin HTTP handlers, the agent's tools and the MCP
// client may call in concurrently. test() runs the prober outside the
// lock.
//
// Saving (spec 3.5 of docs/superpowers/specs/2026-11-06-flash-thread-design.md):
// with a `submit_io`, a change is applied in memory and its file write
// queued on the flash thread (core::LatestSave), the newest state winning;
// init() reads flash and must itself run on the flash thread or before it
// starts.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/latest_save.hpp"
#include "wisp/core/state_cell.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

inline constexpr size_t kMcpMaxServers = 8;
inline constexpr size_t kMcpMaxHeaders = 8;
inline constexpr size_t kMcpMaxIdBytes = 16;
inline constexpr size_t kMcpMaxNameBytes = 32;
inline constexpr size_t kMcpMaxUrlBytes = 2048;
inline constexpr size_t kMcpMaxHeaderValueBytes = 1024;
inline constexpr uint32_t kMcpMinTimeoutMs = 1000;
// net::AgentHttpLimits::kMaxTimeoutMs, which wisp_core cannot include.
inline constexpr uint32_t kMcpMaxTimeoutMs = 120000;
inline constexpr uint32_t kMcpDefaultTimeoutMs = 60000;

struct McpHeaderInfo {
    os::LargeString name{};
    bool has_value = false;
};

// What the outside world may see of a server: never a header value.
struct McpServerInfo {
    std::string id{};
    os::LargeString name{};
    os::LargeString url{};
    bool enabled = true;
    uint32_t timeout_ms = kMcpDefaultTimeoutMs;
    std::vector<McpHeaderInfo> headers{};
    uint32_t revision = 0;  // moves on every change of this server
};

struct McpHeaderInput {
    std::string name{};
    std::optional<std::string> value{};
};

// create(). Every header needs a value. timeout_ms left out takes
// kMcpDefaultTimeoutMs.
struct McpServerInput {
    std::string name{};
    std::string url{};
    bool enabled = true;
    std::optional<uint32_t> timeout_ms{};
    std::vector<McpHeaderInput> headers{};
};

// update(): fields left out keep their value. `headers` replaces the whole
// list: an entry without a value keeps the stored value of the same name
// (case-insensitive) while the origin stays the same, and is dropped
// otherwise; an entry with a value sets it ("" is refused: leave the entry
// out instead).
struct McpServerUpdate {
    std::optional<std::string> name{};
    std::optional<std::string> url{};
    std::optional<bool> enabled{};
    std::optional<uint32_t> timeout_ms{};
    std::optional<std::vector<McpHeaderInput>> headers{};
};

// What McpClient needs to send a request: internal, see the file header.
struct McpServerTarget {
    std::string id{};
    os::LargeString name{};
    os::LargeString url{};
    uint32_t timeout_ms = 0;
    std::vector<std::pair<os::LargeString, os::LargeString>> headers{};  // with values
    uint32_t revision = 0;
};

struct McpServersState {
    uint32_t count = 0;
    uint32_t enabled = 0;
    uint32_t revision = 0;
    bool operator==(const McpServersState&) const = default;
};

// POST /mcp/servers/{id}/test's answer. `error` is "" or one of: "dns",
// "connect", "tls", "timeout", "http", "unsupported_protocol",
// "bad_response".
struct McpProbe {
    bool ok = false;
    std::string error{};
    int http_status = 0;  // 0 when no HTTP response arrived
    uint32_t latency_ms = 0;
    std::string protocol{};
    std::vector<os::LargeString> tools{};
};

// Initializes and lists against `target`; the client lives in wisp_net,
// which wisp_core cannot depend on, so App injects it.
using McpProber = std::function<McpProbe(const McpServerTarget&)>;

// Failures with their own error code in the API; None for the generic
// ones (invalid_argument, not_found, ...).
enum class McpFailureKind { None, LimitReached };

class McpServerService {
public:
    // `submit_io` queues the file writes on the flash thread; empty: every
    // change is written on the calling thread.
    McpServerService(StorageManager& storage, EventBus& bus, LatestSave::SubmitIo submit_io = {});

    McpServerService(const McpServerService&) = delete;
    McpServerService& operator=(const McpServerService&) = delete;

    // Loads secrets/mcp.json. A missing file means no servers. A corrupt
    // file logs a warning and is treated as empty (the next change
    // rewrites it); entries a hand edit made invalid are dropped. A storage
    // read failure is returned, leaving the service empty. Flash: on the
    // flash thread or before it starts.
    Result<void> init();

    // In configuration order.
    std::vector<McpServerInfo> list() const;
    std::optional<McpServerInfo> get(std::string_view id) const;

    // Validates everything before changing anything: on any error nothing
    // changes, in memory or on disk. Rules: name 1-32 bytes of UTF-8 text;
    // url at most 2048 bytes, http(s) with a host, no whitespace or control
    // bytes, no user info ("put credentials in a header"), kept byte for
    // byte; timeout_ms 1000-120000; at most 8 headers with distinct names
    // (case-insensitive), each an RFC 9110 token other than the ones the
    // client sets or SceneHttp drops (Accept, Content-Type, Mcp-Session-Id,
    // MCP-Protocol-Version, Host, Content-Length, Transfer-Encoding,
    // Connection), with a value of 1-1024 bytes of UTF-8 without CR, LF or
    // NUL. InvalidArgument for all of these. The ninth server is
    // QuotaExceeded and sets *why to LimitReached.
    Result<McpServerInfo> create(const McpServerInput& in, McpFailureKind* why = nullptr);

    // Same rules; NotFound for an unknown id. See McpServerUpdate and the
    // file header for what happens to header values.
    Result<McpServerInfo> update(std::string_view id, const McpServerUpdate& u);

    // NotFound for an unknown id.
    Result<void> remove(std::string_view id);

    // Runs the prober against the server. NotFound for an unknown id,
    // Unsupported when no prober was set (no HTTP client on this
    // platform). The result never contains a header value.
    Result<McpProbe> test(std::string_view id) const;

    // nullptr clears it.
    void set_prober(McpProber prober);

    // Internal: the server with its header values, nullopt for an unknown
    // id. Only for McpClient -- see the file header.
    std::optional<McpServerTarget> resolve_for_request(std::string_view id) const;

    const StateCell<McpServersState>& cell() const { return cell_; }

    // Called once after LatestSave::kFailuresToReport failed writes in a
    // row (see core::LatestSave). Set it before the first change.
    void set_on_persist_failed(std::function<void()> fn) {
        saver_.set_on_persist_failed(std::move(fn));
    }

    // Forgets every server and deletes the file (factory reset), in place
    // of any write still queued. Failures to delete are logged, not
    // returned: the caller reboots regardless.
    void clear();

private:
    struct StoredHeader {
        os::LargeString name{};
        os::LargeString value{};
    };
    struct Stored {
        std::string id{};
        os::LargeString name{};
        os::LargeString url{};
        bool enabled = true;
        uint32_t timeout_ms = kMcpDefaultTimeoutMs;
        std::vector<StoredHeader> headers{};
        uint32_t revision = 0;
    };
    using State = std::vector<Stored>;

    Result<void> persist(const State& next);
    void publish(const State& state);
    // Loads the file's bytes into state_; false when it did not parse.
    bool load(std::string_view text);
    static McpServerInfo info_of(const Stored& s);
    static Stored* find(State& state, std::string_view id);
    static const Stored* find(const State& state, std::string_view id);

    StorageManager& storage_;
    LatestSave saver_;
    mutable os::Mutex mutex_;
    State state_;
    McpProber prober_;
    // Revisions handed out so far; never goes back, not even on clear().
    uint32_t revision_ = 0;
    StateCell<McpServersState> cell_;
};

// The public JSON shape, with no header value in it:
// {id, name, url, enabled, timeout_ms, headers:[{name, has_value}]}.
Value to_value(const McpServerInfo& s);

}  // namespace wisp::core
