#include "wisp/core/mcp_servers.hpp"

#include <cerrno>
#include <cstdio>
#include <mutex>
#include <span>
#include <string>
#include <utility>

#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/http_rules.hpp"
#include "wisp/core/log.hpp"
#include "wisp/os/flash_guard.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "mcp";
constexpr const char* kFile = "/secrets/mcp.json";
constexpr int64_t kFileVersion = 1;

// The client sets these itself, or SceneHttp drops them: a stored copy
// would clash with the protocol or silently vanish.
constexpr std::string_view kReservedHeaders[] = {
    "accept", "content-type",   "mcp-session-id",    "mcp-protocol-version",
    "host",   "content-length", "transfer-encoding", "connection",
};

// A parsed file is a few KB in many small blocks; on the device a plain
// malloc that small lands in internal RAM (AGENTS.md section 8).
const JsonAlloc& large_alloc() {
    static const JsonAlloc alloc{
        [](size_t n) { return os::alloc(n, os::Mem::Large); },
        [](void* p) { os::dealloc(p); },
        [](void* p, size_t n) { return os::realloc(p, n, os::Mem::Large); },
    };
    return alloc;
}

Error invalid(std::string message) { return Error{ErrorCode::InvalidArgument, std::move(message)}; }

template <typename T>
Result<T> fail(const Error& e) {
    return Result<T>::err(e.code, e.message);
}

os::LargeString large(std::string_view s) { return os::LargeString(s.begin(), s.end()); }
std::string plain(const os::LargeString& s) { return std::string(s.begin(), s.end()); }

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

bool has_control(std::string_view s) {
    for (char c : s) {
        auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) return true;
    }
    return false;
}

bool valid_id(std::string_view id) {
    if (id.empty() || id.size() > kMcpMaxIdBytes) return false;
    for (char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

Result<void> check_name(std::string_view name) {
    if (name.empty() || name.size() > kMcpMaxNameBytes || has_control(name) || !valid_utf8(name)) {
        return fail<void>(invalid("name must be 1-32 bytes of UTF-8 text"));
    }
    return Result<void>::ok();
}

// Never echoes the URL: it can carry a token in its query.
Result<void> check_url(std::string_view url) {
    if (url.size() > kMcpMaxUrlBytes || !valid_utf8(url) || !valid_http_url(url)) {
        return fail<void>(invalid("url must be an http:// or https:// URL of at most 2048 bytes"));
    }
    // URLs are readable through the routes and mcp.servers; credentials
    // are not.
    if (url_has_userinfo(url)) {
        return fail<void>(invalid("url must not carry user info; put credentials in a header"));
    }
    return Result<void>::ok();
}

Result<void> check_timeout(uint32_t ms) {
    if (ms < kMcpMinTimeoutMs || ms > kMcpMaxTimeoutMs) {
        return fail<void>(invalid("timeout_ms must be from 1000 to 120000"));
    }
    return Result<void>::ok();
}

// The value is held to UTF-8 as well, so the file (JSON) keeps it byte for
// byte. No message names the value.
Result<void> check_value(std::string_view value) {
    if (value.empty()) {
        return fail<void>(invalid("a header value must not be empty; leave the header out"));
    }
    if (value.size() > kMcpMaxHeaderValueBytes || !http_header_syntax_ok("x", value) ||
        !valid_utf8(value)) {
        return fail<void>(
            invalid("a header value must be at most 1024 bytes of text without CR, LF or NUL"));
    }
    return Result<void>::ok();
}

Result<void> check_headers(const std::vector<McpHeaderInput>& headers, bool values_required) {
    if (headers.size() > kMcpMaxHeaders) return fail<void>(invalid("at most 8 headers"));
    for (size_t i = 0; i < headers.size(); ++i) {
        const McpHeaderInput& h = headers[i];
        if (!http_header_syntax_ok(h.name, "")) {
            return fail<void>(invalid("a header name must be an HTTP token"));
        }
        for (std::string_view r : kReservedHeaders) {
            if (iequals(h.name, r)) {
                return fail<void>(invalid("header " + h.name + " is set by the MCP client"));
            }
        }
        for (size_t k = 0; k < i; ++k) {
            if (iequals(h.name, headers[k].name)) {
                return fail<void>(invalid("header " + h.name + " is given twice"));
            }
        }
        if (h.value) {
            WISP_TRY(check_value(*h.value));
        } else if (values_required) {
            return fail<void>(invalid("header " + h.name + " needs a value"));
        }
    }
    return Result<void>::ok();
}

// See the file header of mcp_servers.hpp.
std::string id_base(std::string_view name) {
    std::string out;
    bool gap = false;
    for (char c : name) {
        c = lower(c);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            if (gap && !out.empty()) out += '-';
            gap = false;
            out += c;
        } else {
            gap = true;
        }
    }
    // Runs of '-' in the name itself stay as they are; only the ends trim.
    while (out.starts_with('-')) out.erase(0, 1);
    return out;
}

std::string fit_id(std::string base, size_t max) {
    if (base.size() > max) base.resize(max);
    while (base.ends_with('-')) base.pop_back();
    return base;
}

// Deletes a JSON file written by write_atomic() along with its leftovers.
// ".new" first: read_file() would promote a leftover ".new" if the target
// were gone, resurrecting what was deleted. Flash thread only.
void remove_file(const std::string& path) {
    WISP_ASSERT_FLASH_THREAD("McpServerService remove_file");
    for (const std::string& p : {path + ".new", path + ".tmp", path}) {
        if (std::remove(p.c_str()) != 0 && errno != ENOENT) {
            WLOG_W(kTag, "deleting a file of secrets/mcp.json failed (errno %d)", errno);
        }
    }
}

}  // namespace

Value to_value(const McpServerInfo& s) {
    List headers;
    for (const McpHeaderInfo& h : s.headers) {
        headers.push_back(
            Value(Map{{"name", Value(plain(h.name))}, {"has_value", Value(h.has_value)}}));
    }
    return Value(Map{
        {"id", Value(s.id)},
        {"name", Value(plain(s.name))},
        {"url", Value(plain(s.url))},
        {"enabled", Value(s.enabled)},
        {"timeout_ms", Value(static_cast<int64_t>(s.timeout_ms))},
        {"headers", Value(std::move(headers))},
    });
}

McpServerService::McpServerService(StorageManager& storage, EventBus& bus,
                                   LatestSave::SubmitIo submit_io)
    : storage_(storage), saver_(std::move(submit_io)), cell_(bus, McpServersState{}) {}

McpServerService::Stored* McpServerService::find(State& state, std::string_view id) {
    for (Stored& s : state) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

const McpServerService::Stored* McpServerService::find(const State& state, std::string_view id) {
    for (const Stored& s : state) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

McpServerInfo McpServerService::info_of(const Stored& s) {
    McpServerInfo out;
    out.id = s.id;
    out.name = s.name;
    out.url = s.url;
    out.enabled = s.enabled;
    out.timeout_ms = s.timeout_ms;
    for (const StoredHeader& h : s.headers) out.headers.push_back({h.name, !h.value.empty()});
    out.revision = s.revision;
    return out;
}

bool McpServerService::load(std::string_view text) {
    auto doc = parse_json_large(text, large_alloc());
    const JsonView list = doc ? doc.value().root().find("servers") : JsonView{};
    if (!doc || list.type() != JsonView::Type::Array) {
        // The parse error could quote part of the file (values included),
        // so only say that it failed.
        WLOG_W(kTag, "secrets/mcp.json is corrupt; starting without MCP servers");
        return false;
    }
    // Re-validate everything so a hand-edited file cannot smuggle in what
    // create() would have refused.
    State loaded;
    size_t dropped = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        const JsonView e = list.at(i);
        Stored s;
        s.id = std::string(e.find("id").string());
        const std::string_view name = e.find("name").string();
        const std::string_view url = e.find("url").string();
        const JsonView enabled = e.find("enabled");
        const auto timeout = e.find("timeout_ms").integer();
        const JsonView headers = e.find("headers");
        bool ok = loaded.size() < kMcpMaxServers && valid_id(s.id) &&
                  find(loaded, s.id) == nullptr && check_name(name) && check_url(url) &&
                  enabled.type() == JsonView::Type::Bool && timeout.has_value() &&
                  *timeout >= kMcpMinTimeoutMs && *timeout <= kMcpMaxTimeoutMs &&
                  headers.type() == JsonView::Type::Array;
        if (ok) {
            std::vector<McpHeaderInput> in;
            for (size_t k = 0; k < headers.size(); ++k) {
                const JsonView h = headers.at(k);
                if (h.find("value").type() != JsonView::Type::String) {
                    ok = false;
                    break;
                }
                in.push_back(
                    {std::string(h.find("name").string()), std::string(h.find("value").string())});
            }
            ok = ok && check_headers(in, /*values_required=*/true);
            for (const McpHeaderInput& h : ok ? in : std::vector<McpHeaderInput>{}) {
                s.headers.push_back({large(h.name), large(*h.value)});
            }
        }
        if (!ok) {
            ++dropped;
            continue;
        }
        s.name = large(name);
        s.url = large(url);
        s.enabled = enabled.boolean();
        s.timeout_ms = static_cast<uint32_t>(*timeout);
        s.revision = ++revision_;
        loaded.push_back(std::move(s));
    }
    // Ids only: an entry's URL or headers are not for the log.
    if (dropped > 0) WLOG_W(kTag, "dropped %u invalid MCP servers", unsigned(dropped));
    state_ = std::move(loaded);
    return true;
}

Result<void> McpServerService::init() {
    std::lock_guard<os::Mutex> lock(mutex_);
    state_ = State{};
    auto root = storage_.write_root(Area::System);
    if (!root) {
        publish(state_);
        return Result<void>::err(ErrorCode::StorageUnavailable, "system storage unavailable");
    }
    auto bytes = storage_.read_file(root.value() + kFile);
    if (!bytes) {
        publish(state_);
        if (bytes.error().code == ErrorCode::NotFound) return Result<void>::ok();
        return Result<void>::err(bytes.error().code,
                                 "reading secrets/mcp.json: " + bytes.error().message);
    }
    load(std::string_view(reinterpret_cast<const char*>(bytes.value().data()),
                          bytes.value().size()));
    WLOG_I(kTag, "loaded %u MCP servers", unsigned(state_.size()));
    publish(state_);
    return Result<void>::ok();
}

std::vector<McpServerInfo> McpServerService::list() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    std::vector<McpServerInfo> out;
    out.reserve(state_.size());
    for (const Stored& s : state_) out.push_back(info_of(s));
    return out;
}

std::optional<McpServerInfo> McpServerService::get(std::string_view id) const {
    std::lock_guard<os::Mutex> lock(mutex_);
    const Stored* s = find(state_, id);
    if (s == nullptr) return std::nullopt;
    return info_of(*s);
}

Result<McpServerInfo> McpServerService::create(const McpServerInput& in, McpFailureKind* why) {
    if (why != nullptr) *why = McpFailureKind::None;
    using R = Result<McpServerInfo>;
    if (auto r = check_name(in.name); !r) return fail<McpServerInfo>(r.error());
    if (auto r = check_url(in.url); !r) return fail<McpServerInfo>(r.error());
    const uint32_t timeout = in.timeout_ms.value_or(kMcpDefaultTimeoutMs);
    if (auto r = check_timeout(timeout); !r) return fail<McpServerInfo>(r.error());
    if (auto r = check_headers(in.headers, /*values_required=*/true); !r) {
        return fail<McpServerInfo>(r.error());
    }

    std::lock_guard<os::Mutex> lock(mutex_);
    if (state_.size() >= kMcpMaxServers) {
        if (why != nullptr) *why = McpFailureKind::LimitReached;
        return R::err(ErrorCode::QuotaExceeded, "at most 8 MCP servers");
    }
    std::string base = fit_id(id_base(in.name), kMcpMaxIdBytes);
    if (base.empty()) base = "mcp";
    std::string id = base;
    // At most kMcpMaxServers ids are taken, so a free suffix is near.
    for (unsigned n = 2; find(state_, id) != nullptr; ++n) {
        const std::string suffix = "-" + std::to_string(n);
        id = fit_id(base, kMcpMaxIdBytes - suffix.size()) + suffix;
    }

    State next = state_;
    Stored s;
    s.id = id;
    s.name = large(in.name);
    s.url = large(in.url);
    s.enabled = in.enabled;
    s.timeout_ms = timeout;
    for (const McpHeaderInput& h : in.headers)
        s.headers.push_back({large(h.name), large(*h.value)});
    s.revision = revision_ + 1;
    McpServerInfo out = info_of(s);
    next.push_back(std::move(s));

    WISP_TRY(persist(next));
    revision_ = out.revision;
    state_ = std::move(next);
    WLOG_I(kTag, "created MCP server %s (%u headers)", out.id.c_str(),
           unsigned(out.headers.size()));
    publish(state_);
    return R::ok(std::move(out));
}

Result<McpServerInfo> McpServerService::update(std::string_view id, const McpServerUpdate& u) {
    using R = Result<McpServerInfo>;
    if (u.name) {
        if (auto r = check_name(*u.name); !r) return fail<McpServerInfo>(r.error());
    }
    if (u.url) {
        if (auto r = check_url(*u.url); !r) return fail<McpServerInfo>(r.error());
    }
    if (u.timeout_ms) {
        if (auto r = check_timeout(*u.timeout_ms); !r) return fail<McpServerInfo>(r.error());
    }
    if (u.headers) {
        if (auto r = check_headers(*u.headers, /*values_required=*/false); !r) {
            return fail<McpServerInfo>(r.error());
        }
    }

    std::lock_guard<os::Mutex> lock(mutex_);
    State next = state_;
    Stored* s = find(next, id);
    if (s == nullptr) return R::err(ErrorCode::NotFound, "no such MCP server");
    const bool same_origin = !u.url || url_origin(*u.url) == url_origin(plain(s->url));
    if (u.headers) {
        std::vector<StoredHeader> headers;
        for (const McpHeaderInput& h : *u.headers) {
            if (h.value) {
                headers.push_back({large(h.name), large(*h.value)});
                continue;
            }
            if (!same_origin) continue;
            for (const StoredHeader& old : s->headers) {
                if (iequals(plain(old.name), h.name)) {
                    headers.push_back({large(h.name), old.value});
                    break;
                }
            }
        }
        s->headers = std::move(headers);
    } else if (!same_origin) {
        // The values belong to the server they were saved for.
        s->headers.clear();
    }
    if (u.name) s->name = large(*u.name);
    if (u.url) s->url = large(*u.url);
    if (u.enabled) s->enabled = *u.enabled;
    if (u.timeout_ms) s->timeout_ms = *u.timeout_ms;
    s->revision = revision_ + 1;
    McpServerInfo out = info_of(*s);

    WISP_TRY(persist(next));
    revision_ = out.revision;
    state_ = std::move(next);
    WLOG_I(kTag, "updated MCP server %s (%u headers%s)", out.id.c_str(),
           unsigned(out.headers.size()), same_origin ? "" : ", new origin");
    publish(state_);
    return R::ok(std::move(out));
}

Result<void> McpServerService::remove(std::string_view id) {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (find(state_, id) == nullptr) {
        return Result<void>::err(ErrorCode::NotFound, "no such MCP server");
    }
    State next = state_;
    std::erase_if(next, [&](const Stored& s) { return s.id == id; });

    WISP_TRY(persist(next));
    ++revision_;
    state_ = std::move(next);
    WLOG_I(kTag, "removed MCP server %.*s", static_cast<int>(id.size()), id.data());
    publish(state_);
    return Result<void>::ok();
}

void McpServerService::set_prober(McpProber prober) {
    std::lock_guard<os::Mutex> lock(mutex_);
    prober_ = std::move(prober);
}

Result<McpProbe> McpServerService::test(std::string_view id) const {
    McpProber prober;
    std::optional<McpServerTarget> target;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (find(state_, id) == nullptr) {
            return Result<McpProbe>::err(ErrorCode::NotFound, "no such MCP server");
        }
        if (!prober_) {
            return Result<McpProbe>::err(ErrorCode::Unsupported, "no HTTP client on this platform");
        }
        prober = prober_;
    }
    target = resolve_for_request(id);
    if (!target) return Result<McpProbe>::err(ErrorCode::NotFound, "no such MCP server");
    // Up to the server's timeout: never under the lock.
    return Result<McpProbe>::ok(prober(*target));
}

std::optional<McpServerTarget> McpServerService::resolve_for_request(std::string_view id) const {
    std::lock_guard<os::Mutex> lock(mutex_);
    const Stored* s = find(state_, id);
    if (s == nullptr) return std::nullopt;
    McpServerTarget out;
    out.id = s->id;
    out.name = s->name;
    out.url = s->url;
    out.timeout_ms = s->timeout_ms;
    for (const StoredHeader& h : s->headers) out.headers.emplace_back(h.name, h.value);
    out.revision = s->revision;
    return out;
}

void McpServerService::clear() {
    std::lock_guard<os::Mutex> lock(mutex_);
    state_ = State{};
    ++revision_;
    // Through the saver, so a write still queued cannot bring the values
    // back after the file is gone.
    (void)saver_.save([&storage = storage_]() -> Result<void> {
        auto root = storage.write_root(Area::System);
        if (root) remove_file(root.value() + kFile);
        return Result<void>::ok();
    });
    WLOG_I(kTag, "MCP servers cleared");
    publish(state_);
}

Result<void> McpServerService::persist(const State& next) {
    // Written straight into PSRAM: with eight servers of eight 1 KiB
    // values the file is tens of KB.
    os::LargeString json;
    json.append(R"({"version":)");
    json.append(std::to_string(kFileVersion));
    json.append(R"(,"servers":[)");
    for (size_t i = 0; i < next.size(); ++i) {
        const Stored& s = next[i];
        if (i > 0) json.push_back(',');
        json.append(R"({"id":)");
        append_json_string(json, s.id);
        json.append(R"(,"name":)");
        append_json_string(json, std::string_view(s.name.data(), s.name.size()));
        json.append(R"(,"url":)");
        append_json_string(json, std::string_view(s.url.data(), s.url.size()));
        json.append(R"(,"enabled":)");
        json.append(s.enabled ? "true" : "false");
        json.append(R"(,"timeout_ms":)");
        json.append(std::to_string(s.timeout_ms));
        json.append(R"(,"headers":[)");
        for (size_t k = 0; k < s.headers.size(); ++k) {
            if (k > 0) json.push_back(',');
            json.append(R"({"name":)");
            append_json_string(
                json, std::string_view(s.headers[k].name.data(), s.headers[k].name.size()));
            json.append(R"(,"value":)");
            append_json_string(
                json, std::string_view(s.headers[k].value.data(), s.headers[k].value.size()));
            json.push_back('}');
        }
        json.append("]}");
    }
    json.append("]}");
    // The write runs on the flash thread, possibly after this service is
    // gone: it owns its snapshot and finds the path itself.
    return saver_.save([&storage = storage_, snapshot = std::move(json)]() -> Result<void> {
        auto root = storage.write_root(Area::System);
        if (!root) {
            WLOG_E(kTag, "saving secrets/mcp.json failed: system storage unavailable");
            return Result<void>::err(ErrorCode::StorageUnavailable, "system storage unavailable");
        }
        auto r = storage.write_atomic(
            root.value() + kFile,
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(snapshot.data()),
                                     snapshot.size()));
        if (!r) WLOG_E(kTag, "saving secrets/mcp.json failed: %s", r.error().message.c_str());
        return r;
    });
}

void McpServerService::publish(const State& state) {
    McpServersState view;
    view.count = static_cast<uint32_t>(state.size());
    for (const Stored& s : state) view.enabled += s.enabled ? 1 : 0;
    view.revision = revision_;
    cell_.set(view);
}

}  // namespace wisp::core
