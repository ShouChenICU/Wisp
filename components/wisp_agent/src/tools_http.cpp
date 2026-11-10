// http.fetch (agent-tools spec 3): a thin adapter over SceneHttp's Agent
// principal. The tool itself is the only place that knows the request's
// own defaults (timeout_ms 30000, max_bytes 16384): SceneHttp's own
// default (SceneHttpLimits::timeout_ms) is the Scene principal's, so an
// Agent request that left timeout_ms at 0 would get that one instead.
//
// The response cache (spec 3.3) is one slot, held here rather than in
// HttpToolDeps/AgentWiring: there is one agent core per process, so a
// second place to agree on its lifetime would buy nothing. AgentService
// drops it at the end of every turn and from new_conversation()
// (http_tools_drop_cache()).

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wisp/agent/html_text.hpp"
#include "wisp/agent/limits.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/json.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::agent {

namespace {

char lower_char(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (lower_char(a[i]) != lower_char(b[i])) return false;
    }
    return true;
}

std::string lower_str(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = lower_char(c);
    return out;
}

const std::string& str_arg(const Value& a, const char* key, const std::string& fallback) {
    const Value* v = a.find(key);
    return v != nullptr && v->type() == Value::Type::String ? v->as_string() : fallback;
}

int64_t int_arg(const Value& a, const char* key, int64_t fallback) {
    const Value* v = a.find(key);
    if (v == nullptr) return fallback;
    if (v->type() == Value::Type::Int) return v->as_int();
    if (v->type() == Value::Type::Double) return static_cast<int64_t>(v->as_double());
    return fallback;
}

bool bool_arg(const Value& a, const char* key, bool fallback) {
    const Value* v = a.find(key);
    return v != nullptr && v->type() == Value::Type::Bool ? v->as_bool() : fallback;
}

bool method_of(std::string_view s, net::Method& m) {
    if (s == "GET") {
        m = net::Method::Get;
    } else if (s == "POST") {
        m = net::Method::Post;
    } else if (s == "PUT") {
        m = net::Method::Put;
    } else if (s == "PATCH") {
        m = net::Method::Patch;
    } else if (s == "DELETE") {
        m = net::Method::Delete;
    } else if (s == "HEAD") {
        m = net::Method::Head;
    } else {
        return false;
    }
    return true;
}

// The host out of a URL the Agent principal already accepted (http:// or
// https://, a parseable authority): no user info, no port, lowercased.
std::string host_of(std::string_view url) {
    const size_t scheme = url.find("://");
    const size_t start = scheme == std::string_view::npos ? 0 : scheme + 3;
    size_t end = url.find_first_of("/?#", start);
    if (end == std::string_view::npos) end = url.size();
    std::string_view authority = url.substr(start, end - start);
    const size_t at = authority.rfind('@');
    if (at != std::string_view::npos) authority = authority.substr(at + 1);
    const size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos) authority = authority.substr(0, colon);
    return lower_str(authority);
}

// text/*, *json*, *xml*, *javascript*, application/x-www-form-urlencoded
// (spec 3.1); an absent content-type is not evidence either way, so it is
// treated as text (most APIs that return plain JSON or text do not always
// set one).
bool is_text_content_type(std::string_view content_type) {
    if (content_type.empty()) return true;
    const size_t semi = content_type.find(';');
    std::string_view base =
        semi == std::string_view::npos ? content_type : content_type.substr(0, semi);
    while (!base.empty() && (base.back() == ' ' || base.back() == '\t')) base.remove_suffix(1);
    while (!base.empty() && (base.front() == ' ' || base.front() == '\t')) base.remove_prefix(1);
    const std::string lowered = lower_str(base);
    if (lowered.rfind("text/", 0) == 0) return true;
    if (lowered.find("json") != std::string::npos) return true;
    if (lowered.find("xml") != std::string::npos) return true;
    if (lowered.find("javascript") != std::string::npos) return true;
    return lowered == "application/x-www-form-urlencoded";
}

// Duplicate header names joined with ", ", at most the first 32 pairs
// (spec 3.1).
Value headers_value(const std::vector<std::pair<std::string, std::string>>& headers) {
    Map out;
    const size_t keep = std::min<size_t>(headers.size(), 32);
    for (size_t i = 0; i < keep; ++i) {
        const auto& [name, value] = headers[i];
        auto it = out.find(name);
        if (it == out.end()) {
            out[name] = Value(value);
        } else {
            std::string merged = it->second.as_string();
            merged += ", ";
            merged += value;
            it->second = Value(std::move(merged));
        }
    }
    return Value(std::move(out));
}

ui::Lang lang_of(const ToolCtx& ctx) { return ui::parse_lang(ctx.lang).value_or(ui::Lang::Zh); }

// The cached response (agent-tools spec 3.3): one slot, PSRAM for the raw
// body and (built on first need) its "as":"text" conversion.
struct CacheEntry {
    uint64_t id = 0;
    int status = 0;
    std::string final_url;
    std::string host;
    std::string content_type;
    std::vector<std::pair<std::string, std::string>> headers;
    bool body_truncated = false;
    bool is_text = false;
    os::LargeBuffer raw;
    bool text_built = false;
    os::LargeString text;

    std::string_view raw_view() const {
        return {reinterpret_cast<const char*>(raw.data()), raw.size()};
    }
};

struct Cache {
    os::Mutex mu;
    uint64_t next_id = 1;
    bool has = false;
    CacheEntry entry;
};

Cache& cache_state() {
    static Cache c;
    return c;
}

bool parse_ref(std::string_view ref, uint64_t* id) {
    if (ref.size() < 2 || ref[0] != 'r') return false;
    uint64_t v = 0;
    for (size_t i = 1; i < ref.size(); ++i) {
        if (ref[i] < '0' || ref[i] > '9') return false;
        v = v * 10 + static_cast<uint64_t>(ref[i] - '0');
    }
    *id = v;
    return true;
}

// Rounds `end` (0 <= end <= data.size()) back to a UTF-8 character
// boundary when it would otherwise split one (spec 3.1): the cut moves
// earlier, never later, so returned_bytes never claims a byte of a
// character it did not also return in full.
size_t utf8_boundary(std::string_view data, size_t end) {
    if (end == 0 || end >= data.size()) return end;
    if ((static_cast<unsigned char>(data[end]) & 0xC0) != 0x80) return end;
    size_t start = end;
    while (start > 0 && (static_cast<unsigned char>(data[start]) & 0xC0) == 0x80) --start;
    return start;
}

// Builds the result for one slice of `e`'s response, without touching
// the network: used for a fresh fetch and for a `ref` call alike.
ToolOutput slice_of(CacheEntry& e, const std::string& as, int64_t offset_arg,
                    int64_t max_bytes_arg) {
    const bool want_text = as == "text";
    std::string_view data;
    if (e.is_text) {
        if (want_text) {
            if (!e.text_built) {
                e.text = html_to_text(e.raw_view());
                e.text_built = true;
            }
            data = {e.text.data(), e.text.size()};
        } else {
            data = e.raw_view();
        }
    }
    const size_t total = e.is_text ? data.size() : e.raw.size();
    size_t offset = offset_arg < 0 ? 0 : static_cast<size_t>(offset_arg);
    if (offset > total) offset = total;
    const size_t max_bytes =
        max_bytes_arg <= 0 ? kHttpDefaultMaxBytes : static_cast<size_t>(max_bytes_arg);
    size_t end = offset + max_bytes;
    if (end > total) end = total;
    if (e.is_text) end = utf8_boundary(data, end);
    if (end < offset) end = offset;

    Map out{
        {"status", Value(static_cast<int64_t>(e.status))},
        {"final_url", Value(e.final_url)},
        {"content_type", Value(e.content_type)},
        {"headers", headers_value(e.headers)},
        {"ref", Value("r" + std::to_string(e.id))},
        {"total_bytes", Value(static_cast<int64_t>(total))},
        {"offset", Value(static_cast<int64_t>(offset))},
        {"returned_bytes", Value(static_cast<int64_t>(end - offset))},
        {"more", Value(end < total)},
    };
    if (e.is_text) {
        out["body"] = Value(external_data("http:" + e.host, data.substr(offset, end - offset)));
    }
    if (e.body_truncated) out["body_truncated"] = Value(true);
    out["ok"] = Value(true);
    ToolOutput result;
    result.result = Value(std::move(out));
    return result;
}

constexpr const char* kSchema = R"({"type":"object","properties":{
"url":{"type":"string","maxLength":8192},
"method":{"type":"string","enum":["GET","POST","PUT","PATCH","DELETE","HEAD"]},
"headers":{"type":"object"},
"body":{"type":["string","object"]},
"timeout_ms":{"type":"integer","minimum":1000,"maximum":120000},
"follow":{"type":"boolean"},
"as":{"type":"string","enum":["raw","text"]},
"offset":{"type":"integer","minimum":0},
"max_bytes":{"type":"integer","minimum":1,"maximum":65536},
"ref":{"type":"string"}
}})";

struct Fetch {
    net::SceneHttp& http;

    ToolOutput run(const Value& args, ToolCtx& ctx) {
        const std::string& ref = str_arg(args, "ref", {});
        if (!ref.empty()) return from_ref(ref, args);

        const std::string& url = str_arg(args, "url", {});
        if (url.empty()) return tool_error("bad_args", "'url' is required", Map{{"field", "url"}});
        net::Method method;
        if (!method_of(str_arg(args, "method", "GET"), method)) {
            return tool_error("bad_args", "'method' must be GET, POST, PUT, PATCH, DELETE or HEAD",
                              Map{{"field", "method"}});
        }

        net::SceneHttpRequest req;
        req.principal = net::HttpPrincipal::Agent;
        req.instance = 1;
        req.method = method;
        req.url = url;
        bool has_content_type = false;
        if (const Value* h = args.find("headers"); h != nullptr) {
            if (h->type() != Value::Type::Map) {
                return tool_error("bad_args", "'headers' must be an object",
                                  Map{{"field", "headers"}});
            }
            for (const auto& [name, value] : h->as_map()) {
                if (value.type() != Value::Type::String) {
                    return tool_error("bad_args", "'headers." + name + "' must be a string",
                                      Map{{"field", "headers." + name}});
                }
                if (!has_content_type && ieq(name, "content-type")) has_content_type = true;
                req.headers.emplace_back(name, value.as_string());
            }
        }
        std::string body_text;
        if (const Value* b = args.find("body"); b != nullptr && !b->is_null()) {
            if (b->type() == Value::Type::String) {
                body_text = b->as_string();
            } else {
                body_text = to_json(*b);
                if (!has_content_type) req.headers.emplace_back("Content-Type", "application/json");
            }
        }
        req.body.assign(body_text.data(), body_text.size());
        req.timeout_ms = static_cast<uint32_t>(int_arg(args, "timeout_ms", kHttpDefaultTimeoutMs));
        req.max_redirects =
            bool_arg(args, "follow", false) ? net::AgentHttpLimits::kMaxRedirects : uint8_t{0};

        struct Wait {
            os::Semaphore sem{1, 0};
            net::SceneHttpResult result;
        };
        auto w = std::make_shared<Wait>();
        http.submit(std::move(req), [w](net::SceneHttpResult r) {
            w->result = std::move(r);
            w->sem.give();
        });
        constexpr uint32_t kPollMs = 20;
        for (;;) {
            if (w->sem.take(kPollMs)) break;
            if (ctx.cancelled && ctx.cancelled()) {
                http.cancel_scene(std::string(net::kAgentHttpId));
                return tool_error("cancelled", "the turn was stopped");
            }
        }
        net::SceneHttpResult result = std::move(w->result);
        if (!result.error.empty()) return tool_error(result.error, result.error);

        const std::string final_url = result.final_url.empty() ? url : result.final_url;
        const std::string host = host_of(final_url);

        Cache& c = cache_state();
        std::lock_guard<os::Mutex> lock(c.mu);
        CacheEntry e;
        e.id = c.next_id++;
        e.status = result.status;
        e.final_url = final_url;
        e.host = host;
        for (const auto& [name, value] : result.headers) {
            if (name == "content-type") {
                e.content_type = value;
                break;
            }
        }
        e.is_text = is_text_content_type(e.content_type);
        const size_t keep = std::min<size_t>(result.headers.size(), 32);
        e.headers.assign(result.headers.begin(), result.headers.begin() + static_cast<long>(keep));
        e.body_truncated = result.body_truncated;
        e.raw = std::move(result.body);
        c.entry = std::move(e);
        c.has = true;

        ToolOutput out =
            slice_of(c.entry, str_arg(args, "as", "raw"), int_arg(args, "offset", 0),
                     int_arg(args, "max_bytes", static_cast<int64_t>(kHttpDefaultMaxBytes)));
        out.note = ui::tr_fill_n(ui::StrId::AgentNoteFetch, lang_of(ctx), {std::string_view(host)});
        return out;
    }

    ToolOutput from_ref(const std::string& ref, const Value& args) {
        uint64_t id = 0;
        if (!parse_ref(ref, &id)) return tool_error("no_ref", "unknown or expired ref");
        Cache& c = cache_state();
        std::lock_guard<os::Mutex> lock(c.mu);
        if (!c.has || c.entry.id != id) return tool_error("no_ref", "unknown or expired ref");
        return slice_of(c.entry, str_arg(args, "as", "raw"), int_arg(args, "offset", 0),
                        int_arg(args, "max_bytes", static_cast<int64_t>(kHttpDefaultMaxBytes)));
    }
};

}  // namespace

Result<void> add_http_tools(ToolRegistry& reg, HttpToolDeps deps) {
    auto fetch = std::make_shared<Fetch>(Fetch{deps.http});
    return reg.add(ToolDef{
        .name = "http.fetch",
        .description =
            "Fetches a URL over http:// or https:// (GET by default). Reads the response as raw "
            "bytes or, with as:\"text\", HTML stripped to plain text; a large response pages "
            "with offset/max_bytes, or re-read with ref without a new request. Binary responses "
            "report their size and content type but not their body.",
        .schema_json = kSchema,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Agent,
        .timeout_ms = kHttpFetchToolMs,
        .run = [fetch](const Value& args, ToolCtx& ctx) { return fetch->run(args, ctx); },
    });
}

void http_tools_drop_cache() {
    Cache& c = cache_state();
    std::lock_guard<os::Mutex> lock(c.mu);
    c.has = false;
    c.entry = CacheEntry{};
}

}  // namespace wisp::agent
