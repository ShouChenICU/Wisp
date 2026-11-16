#include "wisp/net/mcp_client.hpp"

#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "large_json.hpp"
#include "mcp_wire.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"

namespace wisp::net {

const char* to_string(McpError e) {
    switch (e) {
        case McpError::None:
            return "";
        case McpError::Unavailable:
            return "mcp_unavailable";
        case McpError::RpcError:
            return "mcp_error";
        case McpError::ToolError:
            return "tool_error";
        case McpError::BadResponse:
            return "bad_response";
        case McpError::NotFound:
            return "not_found";
        case McpError::Cancelled:
            return "cancelled";
        case McpError::Timeout:
            return "timeout";
        case McpError::UnsupportedProtocol:
            return "unsupported_protocol";
    }
    return "";
}

namespace {

using detail::McpSession;

constexpr const char* kTag = "mcp";
// How often a wait looks at the caller's cancellation and deadline, as
// http.fetch does.
constexpr uint32_t kPollMs = 20;
// A request whose done() never comes -- one cancelled by someone else
// sharing kAgentHttpId (a probe while the agent turn is stopped), which
// SceneHttp then never answers -- must not hold its caller forever. Past
// its own timeout plus this, the wait gives up as "timeout"; the slack
// covers time spent queued behind other requests on wisp.shttp.
constexpr uint32_t kGuardSlackMs = 15000;
constexpr std::string_view kObjectSchema = R"({"type":"object"})";
constexpr size_t kMaxMessageBytes = 1024;
// How much of a rejected tool name a log line shows.
constexpr size_t kLogNameBytes = 96;

struct Pending {
    os::Semaphore done{1, 0};
    SceneHttpResult result{};
};

// What a request needs besides its own body.
struct Link {
    SceneHttp& http;
    const core::McpServerTarget& target;
    std::string_view client_version;
    const std::function<uint64_t()>& now;
    const McpWait& wait;
};

struct Exchange {
    McpFailure failure{};
    // The response (failure None, not a notification); its "result" is an
    // object.
    std::optional<LargeJson> doc{};
    std::vector<std::pair<std::string, std::string>> headers{};
};

McpFailure failure(McpError e) {
    McpFailure f;
    f.error = e;
    return f;
}

McpFailure unavailable(std::string_view category, int status = 0) {
    McpFailure f;
    f.error = McpError::Unavailable;
    f.category.assign(category.data(), category.size());
    f.http_status = status;
    return f;
}

// SceneHttp's transport errors that are not this client's own business.
std::string_view transport_category(const std::string& error) {
    for (std::string_view known : {"dns", "connect", "tls", "timeout", "offline"})
        if (error == known) return known;
    return "other";
}

// The word status() and the logs show for a failure.
std::string_view failure_word(const McpFailure& f) {
    if (f.error == McpError::Unavailable) return f.category;
    return to_string(f.error);
}

void log_request(std::string_view server_id, std::string_view method, const McpFailure& f,
                 uint64_t started) {
    const auto ms = static_cast<unsigned>(os::now_ms() - started);
    const std::string_view word = failure_word(f);
    if (f.error == McpError::None) {
        WLOG_I(kTag, "%.*s %.*s ok %u ms", static_cast<int>(server_id.size()), server_id.data(),
               static_cast<int>(method.size()), method.data(), ms);
    } else {
        WLOG_W(kTag, "%.*s %.*s error:%.*s %u ms", static_cast<int>(server_id.size()),
               server_id.data(), static_cast<int>(method.size()), method.data(),
               static_cast<int>(word.size()), word.data(), ms);
    }
}

// Sends one JSON-RPC message and waits for its answer (see the file
// header). `id` 0 is a notification: any 2xx answers it, its body unread.
Exchange send(const Link& l, const McpSession& s, std::string_view method, int64_t id,
              os::LargeString body) {
    const bool notification = id == 0;
    const size_t cap = kMcpMaxResponse;
    const uint64_t started = os::now_ms();
    Exchange ex;

    // In PSRAM, and shared with on_body: a request abandoned by a cancel
    // can still be feeding it on wisp.shttp after this returns.
    auto reader =
        std::allocate_shared<detail::McpReader>(os::LargeAllocator<detail::McpReader>{}, id, cap);
    auto pending = std::allocate_shared<Pending>(os::LargeAllocator<Pending>{});

    SceneHttpRequest req;
    req.scene_id = kAgentHttpId;
    req.principal = HttpPrincipal::Agent;
    req.method = Method::Post;
    req.url.assign(l.target.url.data(), l.target.url.size());
    req.headers.reserve(l.target.headers.size() + 4);
    req.headers.emplace_back("Accept", "application/json, text/event-stream");
    req.headers.emplace_back("Content-Type", "application/json");
    for (const auto& [name, value] : l.target.headers)
        req.headers.emplace_back(std::string(name.data(), name.size()),
                                 std::string(value.data(), value.size()));
    if (!s.protocol.empty()) {
        if (!s.id.empty())
            req.headers.emplace_back("Mcp-Session-Id", std::string(s.id.data(), s.id.size()));
        req.headers.emplace_back("MCP-Protocol-Version", s.protocol);
    }
    req.timeout_ms = kMcpRequestTimeoutMs;
    req.max_redirects = 2;
    req.body = std::move(body);
    req.max_body = cap;
    req.stop_is_success = true;
    if (notification) {
        req.on_body = [](std::span<const uint8_t>) { return false; };
    } else {
        req.on_body = [reader](std::span<const uint8_t> bytes) { return reader->feed(bytes); };
    }
    l.http.submit(std::move(req), [pending](SceneHttpResult r) {
        pending->result = std::move(r);
        pending->done.give();
    });

    const uint64_t guard = l.now() + kMcpRequestTimeoutMs + kGuardSlackMs;
    for (;;) {
        if (pending->done.take(kPollMs)) break;
        if (l.wait.cancelled && l.wait.cancelled()) {
            l.http.cancel_scene(kAgentHttpId);
            ex.failure = failure(McpError::Cancelled);
            log_request(l.target.id, method, ex.failure, started);
            return ex;
        }
        const uint64_t now = l.now();
        if (l.wait.deadline_ms != 0 && now >= l.wait.deadline_ms) {
            l.http.cancel_scene(kAgentHttpId);
            ex.failure = failure(McpError::Timeout);
            log_request(l.target.id, method, ex.failure, started);
            return ex;
        }
        if (now >= guard) {
            ex.failure = unavailable("timeout");
            log_request(l.target.id, method, ex.failure, started);
            return ex;
        }
    }

    SceneHttpResult& r = pending->result;
    if (r.error == "too_large") {
        ex.failure = failure(McpError::BadResponse);
    } else if (!r.error.empty()) {
        ex.failure = unavailable(transport_category(r.error));
    } else if (r.status < 200 || r.status >= 300) {
        ex.failure = unavailable("http", r.status);
    } else if (!notification) {
        ex.doc = reader->take();
        if (!ex.doc) {
            ex.failure = failure(McpError::BadResponse);
        } else {
            const JsonView root = ex.doc->root();
            const JsonView err = root.find("error");
            if (err.type() != JsonView::Type::Null) {
                ex.failure = failure(McpError::RpcError);
                ex.failure.rpc_code = err.find("code").integer().value_or(0);
                detail::append_clean_utf8(ex.failure.message, err.find("message").string(),
                                          kMaxMessageBytes);
            } else if (root.find("result").type() != JsonView::Type::Object) {
                ex.failure = failure(McpError::BadResponse);
            }
            if (ex.failure.error != McpError::None) ex.doc.reset();
        }
    }
    ex.headers = std::move(r.headers);
    log_request(l.target.id, method, ex.failure, started);
    return ex;
}

// MCP session ids are visible ASCII (0x21-0x7E); anything else could not
// be sent back as a header anyway.
bool session_id_ok(std::string_view v) {
    if (v.empty() || v.size() > kMcpMaxSessionIdBytes) return false;
    return std::all_of(v.begin(), v.end(), [](char c) { return c >= 0x21 && c <= 0x7e; });
}

McpFailure initialize(const Link& l, McpSession& s) {
    s = McpSession{.next_request = s.next_request};
    const int64_t id = s.next_request++;
    Exchange ex = send(l, s, "initialize", id, detail::mcp_initialize_body(id, l.client_version));
    if (ex.failure.error != McpError::None) return ex.failure;

    const std::string_view protocol =
        ex.doc->root().find("result").find("protocolVersion").string();
    if (protocol != kMcpProtocol && protocol != kMcpProtocolOld) {
        WLOG_W(kTag, "%s: unsupported protocol revision", l.target.id.c_str());
        return failure(McpError::UnsupportedProtocol);
    }
    for (const auto& [name, value] : ex.headers) {
        if (name != "mcp-session-id") continue;
        if (!session_id_ok(value)) {
            WLOG_W(kTag, "%s: unusable session id", l.target.id.c_str());
            return failure(McpError::BadResponse);
        }
        s.id.assign(value.data(), value.size());
    }
    s.protocol.assign(protocol.data(), protocol.size());
    ex = {};

    Exchange note = send(l, s, "notifications/initialized", 0, detail::mcp_initialized_body());
    if (note.failure.error != McpError::None) return note.failure;
    s.initialized = true;
    return {};
}

bool tool_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '.' || c == '-';
    });
}

// A rejected name as a log line may show it: printable ASCII only, cut.
std::string loggable(std::string_view name) {
    std::string out;
    for (char c : name.substr(0, kLogNameBytes)) out += (c >= 0x20 && c <= 0x7e) ? c : '?';
    if (name.size() > kLogNameBytes) out += "...";
    return out;
}

void add_tool(os::LargeVector<McpTool>& tools, JsonView t, std::string_view server_id) {
    const auto sid = static_cast<int>(server_id.size());
    const std::string_view name = t.find("name").string();
    if (!tool_name_ok(name)) {
        WLOG_W(kTag, "%.*s: tool \"%s\" left out: bad name", sid, server_id.data(),
               loggable(name).c_str());
        return;
    }
    McpTool tool;
    const JsonView schema = t.find("inputSchema");
    if (schema.type() == JsonView::Type::Object) {
        schema.append_to(tool.input_schema);
        if (tool.input_schema.size() > kMcpMaxSchemaBytes) {
            WLOG_W(kTag, "%.*s: tool \"%.*s\" left out: schema over %zu bytes", sid,
                   server_id.data(), static_cast<int>(name.size()), name.data(),
                   kMcpMaxSchemaBytes);
            return;
        }
        // JsonView::write() keeps a string's bytes as they are, and the
        // schema goes into the LLM request as it is.
        if (!valid_utf8(tool.input_schema)) tool.input_schema.assign(kObjectSchema);
    } else {
        tool.input_schema.assign(kObjectSchema);
    }
    tool.name.assign(name.data(), name.size());
    detail::append_clean_utf8(tool.description, t.find("description").string(),
                              kMcpMaxDescriptionBytes);
    tools.push_back(std::move(tool));
}

// Initializes when needed, then lists every page (spec 4.2; plan
// decisions 12 and 14). A 404 to a request that carried a session
// re-initializes once and retries that request once.
McpListResult list_all(const Link& l, McpSession& s) {
    McpListResult out;
    const std::string& sid = l.target.id;
    if (!s.initialized) {
        out.failure = initialize(l, s);
        if (out.failure.error != McpError::None) return out;
    }
    bool reinitialized = false;
    os::LargeString cursor;
    for (size_t page = 0; page < kMcpMaxListPages;) {
        const bool had_session = !s.id.empty();
        const int64_t id = s.next_request++;
        Exchange ex = send(l, s, "tools/list", id, detail::mcp_list_body(id, cursor));
        if (ex.failure.error == McpError::Unavailable && ex.failure.http_status == 404 &&
            had_session && !reinitialized) {
            reinitialized = true;
            out.failure = initialize(l, s);
            if (out.failure.error != McpError::None) return out;
            continue;
        }
        if (ex.failure.error != McpError::None) {
            out.failure = std::move(ex.failure);
            return out;
        }
        const JsonView result = ex.doc->root().find("result");
        const JsonView tools = result.find("tools");
        if (tools.type() != JsonView::Type::Array) {
            out.failure = failure(McpError::BadResponse);
            return out;
        }
        const size_t n = tools.size();
        for (size_t i = 0; i < n; ++i) {
            if (out.tools.size() == kMcpMaxTools) {
                WLOG_W(kTag, "%s: more than %zu tools; the rest left out", sid.c_str(),
                       kMcpMaxTools);
                return out;
            }
            add_tool(out.tools, tools.at(i), sid);
        }
        ++page;
        const std::string_view next = result.find("nextCursor").string();
        if (next.empty()) return out;
        if (page == kMcpMaxListPages) {
            WLOG_W(kTag, "%s: tools/list stopped after %zu pages", sid.c_str(), kMcpMaxListPages);
            return out;
        }
        cursor.assign(next.data(), next.size());
    }
    return out;
}

}  // namespace

McpClient::McpClient(SceneHttp& http, core::McpServerService& servers, McpClientConfig config)
    : http_(http), servers_(servers), config_(std::move(config)) {}

McpListResult McpClient::list_tools(std::string_view server_id, const McpWait& wait, bool refresh) {
    McpListResult out;
    const auto info = servers_.get(server_id);
    if (!info || !info->enabled) {
        out.failure = failure(McpError::NotFound);
        return out;
    }
    const auto target = servers_.resolve_for_request(server_id);
    if (!target) {
        out.failure = failure(McpError::NotFound);
        return out;
    }

    // State of servers removed or changed since the last call is dropped
    // (plan decision 21); the cell says whether anything changed at all.
    const uint32_t revision = servers_.cell().get().revision;
    std::vector<core::McpServerInfo> current;
    if (revision != seen_revision_) current = servers_.list();

    McpSession session;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (revision != seen_revision_) {
            seen_revision_ = revision;
            std::erase_if(state_, [&](const Server& s) {
                return std::none_of(current.begin(), current.end(), [&](const auto& c) {
                    return std::string_view(s.id) == c.id && s.revision == c.revision;
                });
            });
        }
        auto it = std::find_if(state_.begin(), state_.end(), [&](const Server& s) {
            return std::string_view(s.id) == target->id;
        });
        if (it != state_.end() && it->revision != target->revision) {
            state_.erase(it);
            it = state_.end();
        }
        if (it == state_.end()) {
            Server fresh;
            fresh.id.assign(target->id);
            fresh.revision = target->revision;
            state_.push_back(std::move(fresh));
            it = state_.end() - 1;
        }
        const uint64_t now = config_.now_ms();
        if (it->failing && now - it->failed_at < kMcpRetryAfterMs) {
            out.failure = it->failure;
            return out;
        }
        if (it->listed && !refresh && now - it->listed_at < kMcpListTtlMs) {
            out.tools = it->tools;
            return out;
        }
        session = it->session;
    }

    const Link link{http_, *target, config_.client_version, config_.now_ms, wait};
    out = list_all(link, session);

    std::lock_guard<os::Mutex> lock(mu_);
    auto it = std::find_if(state_.begin(), state_.end(), [&](const Server& s) {
        return std::string_view(s.id) == target->id && s.revision == target->revision;
    });
    if (it == state_.end()) return out;
    it->session = std::move(session);
    const uint64_t now = config_.now_ms();
    if (out.failure.error == McpError::None) {
        it->listed = true;
        it->listed_at = now;
        it->tools = out.tools;
        it->failing = false;
        it->failure = {};
    } else if (out.failure.error != McpError::Cancelled && out.failure.error != McpError::Timeout) {
        // The caller's own deadline or cancellation is not the server's
        // failure and starts no backoff.
        it->failing = true;
        it->failed_at = now;
        it->failure = out.failure;
    }
    return out;
}

McpCallResult McpClient::call_tool(std::string_view, std::string_view, std::string_view,
                                   const McpWait&) {
    McpCallResult out;
    out.failure = failure(McpError::NotFound);
    return out;
}

std::vector<McpServerStatus> McpClient::status() const {
    const auto infos = servers_.list();
    const uint64_t now = config_.now_ms();
    std::vector<McpServerStatus> out;
    out.reserve(infos.size());
    std::lock_guard<os::Mutex> lock(mu_);
    for (const auto& info : infos) {
        McpServerStatus st;
        st.id = info.id;
        st.name = info.name;
        st.enabled = info.enabled;
        for (const Server& s : state_) {
            if (std::string_view(s.id) != info.id || s.revision != info.revision) continue;
            if (s.listed) st.tools = s.tools.size();
            if (s.failing && now - s.failed_at < kMcpRetryAfterMs)
                st.error = std::string(failure_word(s.failure));
        }
        out.push_back(std::move(st));
    }
    return out;
}

core::McpProbe McpClient::probe(SceneHttp& http, const core::McpServerTarget& target,
                                std::string_view client_version) {
    const std::function<uint64_t()> now = os::now_ms;
    const McpWait wait{};
    const Link link{http, target, client_version, now, wait};
    McpSession session;
    const uint64_t started = os::now_ms();
    McpListResult r = list_all(link, session);

    core::McpProbe p;
    p.latency_ms = static_cast<uint32_t>(os::now_ms() - started);
    p.http_status = r.failure.http_status;
    switch (r.failure.error) {
        case McpError::None:
            p.ok = true;
            p.protocol = session.protocol;
            p.tools.reserve(r.tools.size());
            for (auto& t : r.tools) p.tools.push_back(std::move(t.name));
            break;
        case McpError::Unavailable:
            // The test route's categories (spec 4.3) have no "offline" or
            // "other": both read as "connect" (plan decision 5).
            p.error = r.failure.category == "offline" || r.failure.category == "other"
                          ? "connect"
                          : r.failure.category;
            break;
        case McpError::UnsupportedProtocol:
            p.error = "unsupported_protocol";
            break;
        case McpError::Cancelled:
        case McpError::Timeout:
            p.error = "timeout";
            break;
        default:
            p.error = "bad_response";
            break;
    }
    return p;
}

core::McpProber make_mcp_prober(SceneHttp& http, std::string client_version) {
    return [&http, version = std::move(client_version)](const core::McpServerTarget& target) {
        return McpClient::probe(http, target, version);
    };
}

}  // namespace wisp::net
