#include "wisp/net/mcp_client.hpp"

#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "large_json.hpp"
#include "mcp_wire.hpp"
#include "secret_scrub.hpp"
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
    // At the caller's cancel or deadline: cancel the request (the agent's
    // own), or just stop waiting for it (a probe, which shares
    // kAgentHttpId with the agent's requests).
    bool cancel_at_stop = true;
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

bool same_ascii_nocase(std::string_view a, std::string_view b) {
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                                              [&](char x, char y) { return lower(x) == lower(y); });
}

// Whether `text` quotes any of the server's header values, whole or in
// part (spec 4.4 "Scrubbing"). Only the authorization headers begin with
// a public auth scheme (RFC 9110 11.6.2, 11.7.2), so only theirs is
// skipped; any other value is secret from its first byte.
bool quotes_a_credential(std::string_view text, const core::McpServerTarget& target) {
    return std::any_of(target.headers.begin(), target.headers.end(), [&](const auto& h) {
        const std::string_view name(h.first);
        const std::string_view value(h.second);
        const bool auth = same_ascii_nocase(name, "Authorization") ||
                          same_ascii_nocase(name, "Proxy-Authorization");
        return detail::holds_secret_piece(text, auth ? detail::credential_part(value) : value);
    });
}

// A server's message as it may reach the model: kWithheldMessage when it
// quotes a credential, else valid UTF-8 cut to `max_bytes`.
void set_server_text(os::LargeString& out, std::string_view text,
                     const core::McpServerTarget& target, size_t max_bytes) {
    out.clear();
    if (quotes_a_credential(text, target)) {
        out.assign(detail::kWithheldMessage.data(), detail::kWithheldMessage.size());
        return;
    }
    detail::append_clean_utf8(out, text, max_bytes);
}

// Sends one JSON-RPC message and waits for its answer (see the file
// header). `id` 0 is a notification: any 2xx answers it, its body unread.
// `timeout_ms` and `cap` bound the request and its answer.
Exchange send(const Link& l, const McpSession& s, std::string_view method, int64_t id,
              os::LargeString body, uint32_t timeout_ms = kMcpRequestTimeoutMs,
              size_t cap = kMcpMaxResponse) {
    const bool notification = id == 0;
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
    req.timeout_ms = timeout_ms;
    // SceneHttp drops only the authorization headers when a hop leaves the
    // origin, but every stored value belongs to the origin it was saved for
    // (spec 4.3): such a server's 3xx is its answer ("http 3xx").
    req.max_redirects = l.target.headers.empty() ? 2 : 0;
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

    // kMcpWaitSlackMs: see its comment; past it the wait gives up as "timeout".
    const uint64_t guard = l.now() + timeout_ms + kMcpWaitSlackMs;
    for (;;) {
        if (pending->done.take(kPollMs)) break;
        if (l.wait.cancelled && l.wait.cancelled()) {
            if (l.cancel_at_stop) l.http.cancel_scene(kAgentHttpId);
            ex.failure = failure(McpError::Cancelled);
            log_request(l.target.id, method, ex.failure, started);
            return ex;
        }
        const uint64_t now = l.now();
        if (l.wait.deadline_ms != 0 && now >= l.wait.deadline_ms) {
            if (l.cancel_at_stop) l.http.cancel_scene(kAgentHttpId);
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
                set_server_text(ex.failure.message, err.find("message").string(), l.target,
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
        // A page answered within one poll of send() never lets it look at
        // the caller, so the caller is asked between pages too.
        if (l.wait.cancelled && l.wait.cancelled()) {
            out.failure = failure(McpError::Cancelled);
            return out;
        }
        if (l.wait.deadline_ms != 0 && l.now() >= l.wait.deadline_ms) {
            out.failure = failure(McpError::Timeout);
            return out;
        }
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

bool has_tool(const os::LargeVector<McpTool>& tools, std::string_view name) {
    return std::any_of(tools.begin(), tools.end(),
                       [&](const McpTool& t) { return std::string_view(t.name) == name; });
}

// Sends tools/call, initializing first when needed. A 404 to a request
// that carried a session re-initializes once and retries the call once
// (the session rule of spec 4.2; never any other retry).
Exchange call_once(const Link& l, McpSession& s, std::string_view tool, std::string_view args_json,
                   uint32_t timeout_ms) {
    Exchange ex;
    if (!s.initialized) {
        ex.failure = initialize(l, s);
        if (ex.failure.error != McpError::None) return ex;
    }
    bool reinitialized = false;
    for (;;) {
        const bool had_session = !s.id.empty();
        const int64_t id = s.next_request++;
        ex = send(l, s, "tools/call", id, detail::mcp_call_body(id, tool, args_json), timeout_ms,
                  kMcpMaxCallResponse);
        if (ex.failure.error == McpError::Unavailable && ex.failure.http_status == 404 &&
            had_session && !reinitialized) {
            reinitialized = true;
            ex = {};
            ex.failure = initialize(l, s);
            if (ex.failure.error != McpError::None) return ex;
            continue;
        }
        return ex;
    }
}

// Whether a JSON-RPC error to tools/call says the tool is unknown: the
// invalid-params code servers use for it, or a message that says so.
bool says_unknown_tool(const McpFailure& f) {
    if (f.rpc_code == -32602) return true;
    std::string lower(std::string_view(f.message).substr(0, kMaxMessageBytes));
    for (char& c : lower)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return lower.find("tool") != std::string::npos &&
           (lower.find("unknown") != std::string::npos ||
            lower.find("not found") != std::string::npos);
}

// The bytes base64 `data` decodes to; padding and line breaks are not data.
size_t base64_decoded_size(std::string_view data) {
    const size_t chars = static_cast<size_t>(std::count_if(data.begin(), data.end(), [](char c) {
        return c != '=' && c != '\r' && c != '\n' && c != ' ' && c != '\t';
    }));
    return chars * 3 / 4;
}

// A tools/call result as the text the model gets (spec 4.4 "Results";
// plan decisions 3 and 7): the content items in order, joined with "\n",
// then structuredContent when no item was text. All of it valid UTF-8, as
// the LLM request body takes it unrepaired.
os::LargeString call_text(JsonView result) {
    constexpr size_t kMaxText = kMcpMaxCallResponse;
    constexpr size_t kMaxField = 1024;
    os::LargeString out;
    bool any = false;
    auto next = [&] {
        if (any) out += '\n';
        any = true;
    };
    auto field = [&](std::string_view v) {
        detail::append_clean_utf8(out, v, std::min(kMaxText, out.size() + kMaxField));
    };
    bool has_text = false;
    const JsonView content = result.find("content");
    const size_t n = content.type() == JsonView::Type::Array ? content.size() : 0;
    for (size_t i = 0; i < n; ++i) {
        const JsonView item = content.at(i);
        const std::string_view type = item.find("type").string();
        if (type == "text") {
            has_text = true;
            next();
            detail::append_clean_utf8(out, item.find("text").string(), kMaxText);
        } else if (type == "image") {
            next();
            out += "[image omitted: ";
            field(item.find("mimeType").string());
            out += ", ";
            const std::string bytes =
                std::to_string(base64_decoded_size(item.find("data").string()));
            out.append(bytes.data(), bytes.size());
            out += " bytes]";
        } else if (type == "resource_link") {
            next();
            out += "[resource: ";
            field(item.find("name").string());
            out += ' ';
            field(item.find("uri").string());
            out += ']';
        } else if (type == "audio") {
            next();
            out += "[audio omitted]";
        } else if (type == "resource") {
            next();
            out += "[resource omitted]";
        }
    }
    const JsonView structured = result.find("structuredContent");
    if (!has_text && structured.type() != JsonView::Type::Null) {
        os::LargeString json;
        structured.append_to(json);
        next();
        // A string inside may hold bytes that are not UTF-8; replacing them
        // keeps the JSON valid (every byte past ASCII is inside a string).
        detail::append_clean_utf8(out, json, kMaxText);
    }
    return out;
}

}  // namespace

McpClient::McpClient(SceneHttp& http, core::McpServerService& servers, McpClientConfig config)
    : http_(http), servers_(servers), config_(std::move(config)) {}

McpListResult McpClient::list_tools(std::string_view server_id, const McpWait& wait, bool refresh) {
    McpListResult out;
    out.failure = visit_tools(
        server_id, wait, [&](const os::LargeVector<McpTool>& tools) { out.tools = tools; },
        refresh);
    return out;
}

McpFailure McpClient::visit_tools(std::string_view server_id, const McpWait& wait,
                                  const std::function<void(const os::LargeVector<McpTool>&)>& fn,
                                  bool refresh) {
    const auto info = servers_.get(server_id);
    if (!info || !info->enabled) return failure(McpError::NotFound);
    const auto target = servers_.resolve_for_request(server_id);
    if (!target) return failure(McpError::NotFound);

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
        if (it->failing && now - it->failed_at < kMcpRetryAfterMs) return it->failure;
        if (it->listed && !refresh && now - it->listed_at < kMcpListTtlMs) {
            fn(it->tools);
            return {};
        }
        session = it->session;
    }

    const Link link{http_, *target, config_.client_version, config_.now_ms, wait};
    McpListResult out = list_all(link, session);

    std::lock_guard<os::Mutex> lock(mu_);
    auto it = std::find_if(state_.begin(), state_.end(), [&](const Server& s) {
        return std::string_view(s.id) == target->id && s.revision == target->revision;
    });
    if (it == state_.end()) {
        // The configuration changed during the request: nothing to keep.
        if (out.failure.error == McpError::None) fn(out.tools);
        return out.failure;
    }
    it->session = std::move(session);
    const uint64_t now = config_.now_ms();
    if (out.failure.error == McpError::None) {
        it->listed = true;
        it->listed_at = now;
        it->tools = std::move(out.tools);
        it->failing = false;
        it->failure = {};
        fn(it->tools);
    } else if (out.failure.error != McpError::Cancelled && out.failure.error != McpError::Timeout) {
        // The caller's own deadline or cancellation is not the server's
        // failure and starts no backoff.
        it->failing = true;
        it->failed_at = now;
        it->failure = out.failure;
    }
    return out.failure;
}

McpCallResult McpClient::call_tool(std::string_view server_id, std::string_view tool,
                                   std::string_view args_json, const McpWait& wait) {
    McpCallResult out;
    bool known = false;
    out.failure = visit_tools(server_id, wait, [&](const os::LargeVector<McpTool>& tools) {
        known = has_tool(tools, tool);
    });
    if (out.failure.error != McpError::None) return out;
    const auto target = servers_.resolve_for_request(server_id);
    if (!known || !target) {
        out.failure = failure(McpError::NotFound);
        return out;
    }

    auto find_state = [&] {
        return std::find_if(state_.begin(), state_.end(), [&](const Server& s) {
            return std::string_view(s.id) == target->id && s.revision == target->revision;
        });
    };
    McpSession session;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (auto it = find_state(); it != state_.end()) session = it->session;
    }
    const Link link{http_, *target, config_.client_version, config_.now_ms, wait};
    Exchange ex = call_once(link, session, tool, args_json, target->timeout_ms);
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (auto it = find_state(); it != state_.end()) it->session = std::move(session);
    }
    // A failed call starts no backoff: the server answered its list, and a
    // slow or failing tool says little about its others.

    if (ex.failure.error == McpError::RpcError && says_unknown_tool(ex.failure)) {
        // Plan decision 16: refresh once; the call itself is not retried.
        bool still_listed = false;
        const McpFailure fresh = visit_tools(
            server_id, wait,
            [&](const os::LargeVector<McpTool>& tools) { still_listed = has_tool(tools, tool); },
            /*refresh=*/true);
        if (fresh.error == McpError::None && !still_listed) {
            out.failure = failure(McpError::NotFound);
            return out;
        }
    }
    if (ex.failure.error != McpError::None) {
        out.failure = std::move(ex.failure);
        return out;
    }
    const JsonView result = ex.doc->root().find("result");
    out.is_error = result.find("isError").boolean();
    out.text = call_text(result);
    // A tool's error text is a server message like a JSON-RPC error's.
    if (out.is_error && quotes_a_credential(out.text, *target))
        out.text.assign(detail::kWithheldMessage.data(), detail::kWithheldMessage.size());
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
                                std::string_view client_version, uint32_t timeout_ms) {
    const std::function<uint64_t()> now = os::now_ms;
    const uint64_t started = os::now_ms();
    const McpWait wait{.deadline_ms = started + timeout_ms, .cancelled = {}};
    const Link link{http, target, client_version, now, wait, /*cancel_at_stop=*/false};
    McpSession session;
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
