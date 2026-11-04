#include "wisp/core/scene_store.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <system_error>
#include <utility>

#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "store";
constexpr std::string_view kKv = "kv";
constexpr std::string_view kLogSuffix = ".log";
constexpr std::string_view kJournal = "journal.json";
constexpr size_t kMaxCollectionName = 32;
// Nesting bound of the JSON scanner below; its open containers are kept
// as bits of one uint64_t, so no recursion runs on the 4 KB wisp.fs stack.
constexpr size_t kMaxDepth = 64;
// A failed flush is retried after coalesce_ms, doubling up to this.
constexpr uint32_t kMaxRetryMs = 5000;

constexpr std::string_view kPutHead = R"({"op":"put","id":)";
constexpr std::string_view kDelHead = R"({"op":"del","id":)";
constexpr std::string_view kValueSep = R"(,"v":)";
constexpr std::string_view kLineEnd = "}\n";

using LString = os::LargeString;

struct LessView {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const { return a < b; }
};

template <class K, class V, class Less>
using LargeMap = std::map<K, V, Less, os::LargeAllocator<std::pair<const K, V>>>;

using KvMap = LargeMap<LString, LString, LessView>;
using IdMap = LargeMap<int64_t, LString, std::less<>>;
// nullopt marks a delete.
using KvOverlay = LargeMap<LString, std::optional<LString>, LessView>;
using IdOverlay = LargeMap<int64_t, std::optional<LString>, std::less<>>;

std::span<const uint8_t> bytes(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

bool ends_with(std::string_view s, std::string_view x) {
    return s.size() >= x.size() && s.substr(s.size() - x.size()) == x;
}

bool valid_collection(std::string_view name) {
    if (name.empty() || name.size() > kMaxCollectionName || name == kKv) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

// ---- JSON scanning ---------------------------------------------------
//
// Values stay as the encoded text they arrived in; these helpers only
// validate it and find spans inside it, without building a Value.

constexpr size_t npos = std::string_view::npos;

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

size_t skip_ws(std::string_view s, size_t p) {
    while (p < s.size() && is_ws(s[p])) {
        ++p;
    }
    return p;
}

// `s[p]` is '"'; returns the position after the closing quote, or npos.
size_t skip_string(std::string_view s, size_t p) {
    ++p;
    while (p < s.size()) {
        const auto c = static_cast<unsigned char>(s[p]);
        if (c == '"') {
            return p + 1;
        }
        if (c < 0x20) {
            return npos;
        }
        if (c != '\\') {
            ++p;
            continue;
        }
        if (p + 1 >= s.size()) {
            return npos;
        }
        const char e = s[p + 1];
        if (e == 'u') {
            if (p + 6 > s.size()) {
                return npos;
            }
            for (size_t i = p + 2; i < p + 6; ++i) {
                if (!is_hex(s[i])) {
                    return npos;
                }
            }
            p += 6;
        } else if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' ||
                   e == 'r' || e == 't') {
            p += 2;
        } else {
            return npos;
        }
    }
    return npos;
}

size_t skip_number(std::string_view s, size_t p) {
    const size_t n = s.size();
    if (p < n && s[p] == '-') {
        ++p;
    }
    if (p >= n) {
        return npos;
    }
    if (s[p] == '0') {
        ++p;
    } else if (is_digit(s[p])) {
        while (p < n && is_digit(s[p])) {
            ++p;
        }
    } else {
        return npos;
    }
    if (p < n && s[p] == '.') {
        ++p;
        if (p >= n || !is_digit(s[p])) {
            return npos;
        }
        while (p < n && is_digit(s[p])) {
            ++p;
        }
    }
    if (p < n && (s[p] == 'e' || s[p] == 'E')) {
        ++p;
        if (p < n && (s[p] == '+' || s[p] == '-')) {
            ++p;
        }
        if (p >= n || !is_digit(s[p])) {
            return npos;
        }
        while (p < n && is_digit(s[p])) {
            ++p;
        }
    }
    return p;
}

size_t skip_literal(std::string_view s, size_t p) {
    for (std::string_view lit : {"true", "false", "null"}) {
        if (s.substr(p, lit.size()) == lit) {
            return p + lit.size();
        }
    }
    return npos;
}

// `s[p]` should start an object member's key; returns the position of its
// value (after the ':' and whitespace), or npos.
size_t skip_key(std::string_view s, size_t p) {
    if (p >= s.size() || s[p] != '"') {
        return npos;
    }
    p = skip_string(s, p);
    if (p == npos) {
        return npos;
    }
    p = skip_ws(s, p);
    if (p >= s.size() || s[p] != ':') {
        return npos;
    }
    return skip_ws(s, p + 1);
}

// Returns the position just after the JSON value starting at or after
// `p` (leading whitespace skipped), or npos if it is not well formed.
size_t skip_value(std::string_view s, size_t p) {
    uint64_t objects = 0;  // bit d: the container at depth d is an object
    size_t depth = 0;
    p = skip_ws(s, p);
    for (;;) {
        if (p >= s.size()) {
            return npos;
        }
        const char c = s[p];
        if (c == '{' || c == '[') {
            if (depth == kMaxDepth) {
                return npos;
            }
            const bool obj = c == '{';
            objects = obj ? (objects | (1ull << depth)) : (objects & ~(1ull << depth));
            ++depth;
            p = skip_ws(s, p + 1);
            if (p < s.size() && s[p] == (obj ? '}' : ']')) {
                --depth;
                ++p;
            } else {
                if (obj && (p = skip_key(s, p)) == npos) {
                    return npos;
                }
                continue;
            }
        } else if (c == '"') {
            p = skip_string(s, p);
        } else if (c == 't' || c == 'f' || c == 'n') {
            p = skip_literal(s, p);
        } else {
            p = skip_number(s, p);
        }
        if (p == npos) {
            return npos;
        }
        // After a value: close containers until the next element starts.
        for (;;) {
            if (depth == 0) {
                return p;
            }
            p = skip_ws(s, p);
            if (p >= s.size()) {
                return npos;
            }
            const bool obj = (objects >> (depth - 1)) & 1u;
            if (s[p] == ',') {
                p = skip_ws(s, p + 1);
                if (obj && (p = skip_key(s, p)) == npos) {
                    return npos;
                }
                break;
            }
            if (s[p] != (obj ? '}' : ']')) {
                return npos;
            }
            --depth;
            ++p;
        }
    }
}

bool valid_json(std::string_view s) {
    const size_t end = skip_value(s, 0);
    return end != npos && skip_ws(s, end) == s.size();
}

// Calls f(raw_key, value) for each member of the object `s`, where
// raw_key is the key's text between its quotes (escapes undecoded). Stops
// when f returns false. Returns false if `s` is not one object.
template <class F>
bool for_each_member(std::string_view s, F&& f) {
    size_t p = skip_ws(s, 0);
    if (p >= s.size() || s[p] != '{') {
        return false;
    }
    p = skip_ws(s, p + 1);
    if (p < s.size() && s[p] == '}') {
        return skip_ws(s, p + 1) == s.size();
    }
    for (;;) {
        const size_t key_end = p < s.size() && s[p] == '"' ? skip_string(s, p) : npos;
        if (key_end == npos) {
            return false;
        }
        const std::string_view key = s.substr(p + 1, key_end - p - 2);
        const size_t v = skip_key(s, p);
        const size_t v_end = v == npos ? npos : skip_value(s, v);
        if (v_end == npos) {
            return false;
        }
        if (!f(key, s.substr(v, v_end - v))) {
            return true;
        }
        p = skip_ws(s, v_end);
        if (p >= s.size()) {
            return false;
        }
        if (s[p] == '}') {
            return skip_ws(s, p + 1) == s.size();
        }
        if (s[p] != ',') {
            return false;
        }
        p = skip_ws(s, p + 1);
    }
}

// Calls f(element) for each element of the array `s`. Returns false if
// `s` is not one array.
template <class F>
bool for_each_element(std::string_view s, F&& f) {
    size_t p = skip_ws(s, 0);
    if (p >= s.size() || s[p] != '[') {
        return false;
    }
    p = skip_ws(s, p + 1);
    if (p < s.size() && s[p] == ']') {
        return skip_ws(s, p + 1) == s.size();
    }
    for (;;) {
        const size_t end = skip_value(s, p);
        if (end == npos) {
            return false;
        }
        f(s.substr(p, end - p));
        p = skip_ws(s, end);
        if (p >= s.size()) {
            return false;
        }
        if (s[p] == ']') {
            return skip_ws(s, p + 1) == s.size();
        }
        if (s[p] != ',') {
            return false;
        }
        p = skip_ws(s, p + 1);
    }
}

uint32_t hex4(std::string_view s) {
    uint32_t v = 0;
    for (char c : s.substr(0, 4)) {
        v <<= 4;
        if (is_digit(c)) {
            v |= static_cast<uint32_t>(c - '0');
        } else {
            v |= static_cast<uint32_t>((c | 0x20) - 'a' + 10);
        }
    }
    return v;
}

template <class Out>
void append_utf8(Out& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Decodes the text between a validated JSON string's quotes.
template <class Out>
void unescape(std::string_view raw, Out& out) {
    for (size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        const char e = raw[++i];
        switch (e) {
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                uint32_t cp = hex4(raw.substr(i + 1));
                i += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < raw.size() && raw[i + 1] == '\\' &&
                    raw[i + 2] == 'u') {
                    const uint32_t lo = hex4(raw.substr(i + 3));
                    if (lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                append_utf8(out, cp);
                break;
            }
            default:  // '"', '\\', '/'
                out.push_back(e);
                break;
        }
    }
}

// A key as a JSON string. Bytes >= 0x80 pass through as they are, so a
// key that is not valid UTF-8 still round-trips.
std::string quoted(std::string_view key) {
    std::string out;
    out.reserve(key.size() + 2);
    out.push_back('"');
    for (char ch : key) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(ch);
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out.append(buf);
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('"');
    return out;
}

struct IdText {
    explicit IdText(int64_t id) {
        auto r = std::to_chars(buf, buf + sizeof(buf), id);
        n = static_cast<size_t>(r.ptr - buf);
    }
    std::string_view view() const { return {buf, n}; }

    char buf[24];
    size_t n = 0;
};

uint64_t put_len(size_t id_len, size_t json_len) {
    return kPutHead.size() + id_len + kValueSep.size() + json_len + kLineEnd.size();
}

uint64_t del_len(size_t id_len) { return kDelHead.size() + id_len + kLineEnd.size(); }

void append_line(LString& out, std::string_view id, std::optional<std::string_view> v) {
    if (v) {
        out.append(kPutHead);
        out.append(id);
        out.append(kValueSep);
        out.append(*v);
    } else {
        out.append(kDelHead);
        out.append(id);
    }
    out.append(kLineEnd);
}

struct ParsedLine {
    bool put = false;
    int64_t id = 0;  // collections
    LString key;     // key/value space
    std::string_view value;
};

// Parses one log line (without its '\n') of the key/value log (`kv`) or a
// collection log.
bool parse_line(std::string_view line, bool kv, ParsedLine& out) {
    bool has_op = false;
    bool has_v = false;
    bool bad = false;
    std::string_view id;
    const bool object = for_each_member(line, [&](std::string_view k, std::string_view v) {
        if (k == "op") {
            has_op = v == R"("put")" || v == R"("del")";
            out.put = v == R"("put")";
            bad = bad || !has_op;
        } else if (k == "id") {
            id = v;
        } else if (k == "v") {
            out.value = v;
            has_v = true;
        }
        return true;
    });
    if (!object || bad || !has_op || id.empty() || (out.put && !has_v)) {
        return false;
    }
    if (kv) {
        if (id.front() != '"') {
            return false;
        }
        out.key.clear();
        unescape(id.substr(1, id.size() - 2), out.key);
    } else {
        auto r = std::from_chars(id.data(), id.data() + id.size(), out.id);
        if (r.ec != std::errc() || r.ptr != id.data() + id.size() || out.id < 1) {
            return false;
        }
        if (out.put && out.value.front() != '{') {
            return false;
        }
    }
    return true;
}

// ---- find() helpers --------------------------------------------------

bool key_equals(std::string_view raw, std::string_view key) {
    if (raw.find('\\') == npos) {
        return raw == key;
    }
    LString decoded;
    unescape(raw, decoded);
    return std::string_view(decoded) == key;
}

std::optional<std::string_view> field(std::string_view object, std::string_view key) {
    std::optional<std::string_view> found;
    for_each_member(object, [&](std::string_view k, std::string_view v) {
        if (key_equals(k, key)) {
            found = v;
            return false;
        }
        return true;
    });
    return found;
}

bool is_number(std::string_view v) { return !v.empty() && (v[0] == '-' || is_digit(v[0])); }

double to_double(std::string_view v) {
    double d = 0;
    std::from_chars(v.data(), v.data() + v.size(), d);
    return d;
}

bool value_equals(std::string_view v, const Value& want) {
    switch (want.type()) {
        case Value::Type::Null:
            return v == "null";
        case Value::Type::Bool:
            return v == (want.as_bool() ? "true" : "false");
        case Value::Type::Int: {
            if (!is_number(v)) {
                return false;
            }
            int64_t i = 0;
            auto r = std::from_chars(v.data(), v.data() + v.size(), i);
            if (r.ec == std::errc() && r.ptr == v.data() + v.size()) {
                return i == want.as_int();
            }
            return to_double(v) == want.as_double();
        }
        case Value::Type::Double:
            return is_number(v) && to_double(v) == want.as_double();
        case Value::Type::String: {
            if (v.empty() || v.front() != '"') {
                return false;
            }
            return key_equals(v.substr(1, v.size() - 2), want.as_string());
        }
        case Value::Type::List:
        case Value::Type::Map: {
            if (v.empty() || (v.front() != '[' && v.front() != '{')) {
                return false;
            }
            auto parsed = from_json(v);
            return parsed && parsed.value() == want;
        }
    }
    return false;
}

struct SortKey {
    int64_t id = 0;
    const LString* json = nullptr;
    int cls = 2;  // 0 number, 1 string, 2 missing or anything else
    double num = 0;
    std::string_view raw;  // a string's text between its quotes
    LString decoded;       // ... decoded, when it has escapes
    bool has_decoded = false;

    std::string_view text() const { return has_decoded ? std::string_view(decoded) : raw; }
};

}  // namespace

// ---- state ------------------------------------------------------------

namespace {

struct LogState {
    uint64_t disk = 0;      // file size once the flush in progress (if any) succeeds
    uint64_t live = 0;      // put lines of the live records
    uint64_t appended = 0;  // appended since the file was last rewritten or loaded
    LString pending;        // lines not yet handed to a flush
    bool needs_rewrite = false;
};

struct Collection {
    LogState log;
    IdMap records;
    int64_t next_id = 1;
};

struct CollOverlay {
    IdOverlay records;
    int64_t next_id = 1;
};

struct Index {
    LogState kv_log;
    KvMap kv;
    std::map<std::string, Collection, std::less<>> colls;
};

// A compacted collection ends in a delete of its highest id once that
// record is gone, so a reload does not hand the id out again.
uint64_t marker_bytes(const Collection& c) {
    const int64_t top = c.next_id - 1;
    if (top < 1 || (!c.records.empty() && c.records.rbegin()->first == top)) {
        return 0;
    }
    return del_len(IdText(top).n);
}

uint64_t compacted_bytes(const Collection& c) { return c.log.live + marker_bytes(c); }

void serialize(const KvMap& kv, LString& out) {
    for (const auto& [key, json] : kv) {
        append_line(out, quoted(key), std::string_view(json));
    }
}

void serialize(const Collection& c, LString& out) {
    for (const auto& [id, json] : c.records) {
        append_line(out, IdText(id).view(), std::string_view(json));
    }
    if (marker_bytes(c) != 0) {
        append_line(out, IdText(c.next_id - 1).view(), std::nullopt);
    }
}

LString owned_key(std::string_view k) { return LString(k.data(), k.size()); }
int64_t owned_key(int64_t k) { return k; }

// Applies a put (`v`) or delete to `base` and its accounting; appends the
// line to `pending` when given (not while loading a file's own lines).
template <class Map, class K>
void apply_record(LogState& log, Map& base, K key, std::string_view id,
                  std::optional<std::string_view> v, LString* pending) {
    auto it = base.find(key);
    const uint64_t old_live = it != base.end() ? put_len(id.size(), it->second.size()) : 0;
    if (v) {
        if (it != base.end()) {
            it->second.assign(v->data(), v->size());
        } else {
            base.emplace(owned_key(key), LString(v->data(), v->size()));
        }
        log.live = log.live - old_live + put_len(id.size(), v->size());
    } else {
        if (it == base.end()) {
            return;
        }
        base.erase(it);
        log.live -= old_live;
    }
    if (pending != nullptr) {
        append_line(*pending, id, v);
    }
}

// Applies one parsed log line of file `name` to `ix`.
bool apply_line(Index& ix, std::string_view name, std::string_view line) {
    ParsedLine p;
    const bool kv = name == kKv;
    if (!parse_line(line, kv, p)) {
        return false;
    }
    std::optional<std::string_view> v;
    if (p.put) {
        v = p.value;
    }
    if (kv) {
        apply_record(ix.kv_log, ix.kv, std::string_view(p.key), quoted(p.key), v, nullptr);
        return true;
    }
    auto it = ix.colls.find(name);
    if (it == ix.colls.end()) {
        it = ix.colls.emplace(std::string(name), Collection{}).first;
    }
    Collection& c = it->second;
    apply_record(c.log, c.records, p.id, IdText(p.id).view(), v, nullptr);
    if (p.id >= c.next_id) {
        c.next_id = p.id == std::numeric_limits<int64_t>::max() ? p.id : p.id + 1;
    }
    return true;
}

struct FlushItem {
    std::string name;
    LString lines;    // the pending lines this flush takes
    LString content;  // a rewrite's full file
    bool rewrite = false;
    enum class State { NotRun, Ok, Failed } state = State::NotRun;
    uint64_t prev_disk = 0;
    uint64_t prev_appended = 0;
    bool prev_needs_rewrite = false;
};

}  // namespace

struct SceneStore::Impl {
    Impl(SceneStoreDeps d, std::string data_dir, SceneStoreLimits l)
        : deps(std::move(d)), dir(std::move(data_dir)), limits(l) {}

    SceneStoreDeps deps;
    const std::string dir;
    const SceneStoreLimits limits;

    // Guards everything below except journal_dirty. Never held across I/O.
    mutable os::Mutex mu;
    // Serializes flush jobs with each other and with load(); held across
    // their I/O.
    os::Mutex io_mu;

    bool loaded = false;
    Index ix;
    bool timer_armed = false;
    // The delay of the last retry after a failed flush; 0 after a success.
    uint32_t retry_ms = 0;
    // A committed batch of more than one line is pending. A torn append
    // could keep part of it, so the flush that takes it goes through the
    // journal even when the batch touches a single file.
    bool batch_pending = false;

    int batch_depth = 0;
    // An inner batch was aborted; the outermost commit drops the overlay.
    bool batch_aborted = false;
    KvOverlay kv_ov;
    std::map<std::string, CollOverlay, std::less<>> coll_ov;
    // What committing the overlay would add to the logs and to their
    // compacted size.
    int64_t ov_line_bytes = 0;
    int64_t ov_live_delta = 0;

    // A journal may be on disk whose lines are already in the logs (or back
    // in pending after a failed flush). The next flush must replace it
    // before touching any log, or a replay could append stale lines after
    // newer ones. Guarded by io_mu.
    bool journal_dirty = false;

    std::string path_of(std::string_view name) const {
        return dir + "/" + std::string(name) + std::string(kLogSuffix);
    }
    std::string journal_path() const { return dir + "/" + std::string(kJournal); }

    LogState* log_of(std::string_view name) {
        if (name == kKv) {
            return &ix.kv_log;
        }
        auto it = ix.colls.find(name);
        return it == ix.colls.end() ? nullptr : &it->second.log;
    }

    uint64_t used_locked() const {
        uint64_t total = ix.kv_log.disk + ix.kv_log.pending.size();
        for (const auto& [name, c] : ix.colls) {
            total += c.log.disk + c.log.pending.size();
        }
        return total;
    }

    uint64_t pending_locked() const {
        uint64_t total = ix.kv_log.pending.size();
        for (const auto& [name, c] : ix.colls) {
            total += c.log.pending.size();
        }
        return total;
    }

    int64_t compacted_locked() const {
        int64_t total = static_cast<int64_t>(ix.kv_log.live);
        for (const auto& [name, c] : ix.colls) {
            total += static_cast<int64_t>(compacted_bytes(c));
        }
        return total + ov_live_delta;
    }

    // A write adding `line` bytes to the logs and `live` bytes to their
    // compacted size. It fits if the logs stay within the quota, or if
    // they would once rewritten (the next flush then rewrites, see
    // take_snapshot_locked()). A write that does not grow the compacted
    // size always fits that second test, so a full store can still be
    // emptied.
    //
    // Past the quota, accepted lines pile up in PSRAM until a flush gets to
    // rewrite, and a scene replacing one value in a loop could push
    // megabytes per message. So that path also caps everything pending
    // (overlay included) at one quota. Below the quota pending is never
    // more than that anyway, so the cap only bites here. It is a cap on
    // pending rather than on disk + pending so that it holds however much
    // the files weigh: logs loaded over the quota still accept deletes
    // once a flush has drained what was pending.
    Result<void> check_quota_locked(int64_t line, int64_t live) const {
        const auto quota = static_cast<int64_t>(limits.quota_bytes);
        if (static_cast<int64_t>(used_locked()) + ov_line_bytes + line <= quota) {
            return Result<void>::ok();
        }
        const bool fits_rewritten = live <= 0 || compacted_locked() + live <= quota;
        const bool pending_ok =
            static_cast<int64_t>(pending_locked()) + ov_line_bytes + line <= quota;
        if (fits_rewritten && pending_ok) {
            return Result<void>::ok();
        }
        return Result<void>::err(ErrorCode::QuotaExceeded, "quota_exceeded");
    }

    // A committed write. Returns whether a line was added.
    template <class Map, class K>
    Result<bool> write_base_locked(LogState& log, Map& base, K key, std::string_view id,
                                   std::optional<std::string_view> v) {
        auto it = base.find(key);
        if (!v && it == base.end()) {
            return Result<bool>::ok(false);
        }
        const uint64_t old_live = it != base.end() ? put_len(id.size(), it->second.size()) : 0;
        const uint64_t line = v ? put_len(id.size(), v->size()) : del_len(id.size());
        const uint64_t live = v ? line : 0;
        WISP_TRY(check_quota_locked(static_cast<int64_t>(line),
                                    static_cast<int64_t>(live) - static_cast<int64_t>(old_live)));
        apply_record(log, base, key, id, v, &log.pending);
        return Result<bool>::ok(true);
    }

    // A write inside a batch: recorded in `ov` over `base` (which may be
    // null for a collection the batch created). Every overlay entry stands
    // for the one line commit will write.
    template <class Ov, class Map, class K>
    Result<void> write_overlay_locked(Ov& ov, const Map* base, K key, size_t id_len,
                                      std::optional<std::string_view> v) {
        const typename Map::value_type::second_type* in_base = nullptr;
        if (base != nullptr) {
            auto b = base->find(key);
            if (b != base->end()) {
                in_base = &b->second;
            }
        }
        const int64_t base_live =
            in_base ? static_cast<int64_t>(put_len(id_len, in_base->size())) : 0;
        auto o = ov.find(key);
        int64_t prev_line = 0;
        int64_t prev_live = 0;
        if (o != ov.end()) {
            prev_line = static_cast<int64_t>(o->second ? put_len(id_len, o->second->size())
                                                       : del_len(id_len));
            prev_live = (o->second ? static_cast<int64_t>(put_len(id_len, o->second->size())) : 0) -
                        base_live;
        }
        // Deleting what only the batch added leaves nothing to write.
        const bool drop = !v && in_base == nullptr;
        const int64_t line =
            drop ? 0 : static_cast<int64_t>(v ? put_len(id_len, v->size()) : del_len(id_len));
        const int64_t live = (v ? static_cast<int64_t>(put_len(id_len, v->size())) : 0) - base_live;
        WISP_TRY(check_quota_locked(line - prev_line, live - prev_live));
        ov_line_bytes += line - prev_line;
        ov_live_delta += live - prev_live;
        if (drop) {
            if (o != ov.end()) {
                ov.erase(o);
            }
        } else {
            std::optional<LString> value;
            if (v) {
                value.emplace(v->data(), v->size());
            }
            if (o != ov.end()) {
                o->second = std::move(value);
            } else {
                ov.emplace(owned_key(key), std::move(value));
            }
        }
        return Result<void>::ok();
    }

    // Marks a timer as armed if none is; true if the caller must arm it
    // (after unlocking).
    bool note_pending_locked() {
        if (timer_armed) {
            return false;
        }
        timer_armed = true;
        return true;
    }

    const Collection* coll_locked(std::string_view name) const {
        auto it = ix.colls.find(name);
        return it == ix.colls.end() ? nullptr : &it->second;
    }

    size_t collection_count_locked() const {
        size_t n = ix.colls.size();
        for (const auto& [name, o] : coll_ov) {
            if (ix.colls.find(name) == ix.colls.end()) {
                ++n;
            }
        }
        return n;
    }

    // The record reads see: the overlay first.
    const LString* visible_locked(std::string_view coll, int64_t id) const {
        auto o = coll_ov.find(coll);
        if (o != coll_ov.end()) {
            auto r = o->second.records.find(id);
            if (r != o->second.records.end()) {
                return r->second ? &*r->second : nullptr;
            }
        }
        const Collection* c = coll_locked(coll);
        if (c == nullptr) {
            return nullptr;
        }
        auto r = c->records.find(id);
        return r == c->records.end() ? nullptr : &r->second;
    }

    CollOverlay& overlay_of_locked(std::string_view coll) {
        auto o = coll_ov.find(coll);
        if (o == coll_ov.end()) {
            CollOverlay fresh;
            const Collection* c = coll_locked(coll);
            fresh.next_id = c != nullptr ? c->next_id : 1;
            o = coll_ov.emplace(std::string(coll), std::move(fresh)).first;
        }
        return o->second;
    }

    void clear_overlay_locked() {
        batch_depth = 0;
        batch_aborted = false;
        kv_ov.clear();
        coll_ov.clear();
        ov_line_bytes = 0;
        ov_live_delta = 0;
    }

    // Moves every pending buffer that needs writing into `items`, choosing
    // between appending and rewriting, and updates the accounting as if the
    // I/O succeeds (settle_locked() undoes it for what fails). Returns
    // whether the snapshot holds a committed multi-line batch.
    bool take_snapshot_locked(std::vector<FlushItem>& items) {
        // Writes were accepted on the promise that rewriting fits the
        // quota: rewrite whatever has dead lines.
        const bool over_quota = used_locked() > limits.quota_bytes;
        auto consider = [&](std::string_view name, LogState& log, uint64_t compacted,
                            auto&& write_content) {
            const uint64_t total = log.disk + log.pending.size();
            const uint64_t dead = total > compacted ? total - compacted : 0;
            const bool rewrite = log.needs_rewrite || dead > compacted / 2 ||
                                 log.appended + log.pending.size() > limits.compact_append_bytes ||
                                 (over_quota && dead > 0);
            if (log.pending.empty() && !rewrite) {
                return;
            }
            FlushItem item;
            item.name = std::string(name);
            item.rewrite = rewrite;
            item.prev_disk = log.disk;
            item.prev_appended = log.appended;
            item.prev_needs_rewrite = log.needs_rewrite;
            item.lines.swap(log.pending);
            if (rewrite) {
                write_content(item.content);
                log.disk = item.content.size();
                log.appended = 0;
                log.needs_rewrite = false;
            } else {
                log.disk += item.lines.size();
                log.appended += item.lines.size();
            }
            items.push_back(std::move(item));
        };
        consider(kKv, ix.kv_log, ix.kv_log.live, [&](LString& out) { serialize(ix.kv, out); });
        for (auto& [name, c] : ix.colls) {
            consider(name, c.log, compacted_bytes(c), [&](LString& out) { serialize(c, out); });
        }
        // Every pending line is in the snapshot now, the batch's included.
        return std::exchange(batch_pending, false);
    }

    // Writes the snapshot. Called with io_mu held and mu not held. Lines
    // going to more than one file, or a batch's lines, go through the
    // journal: append() can leave any prefix behind on a power cut.
    Result<void> write_items(std::vector<FlushItem>& items, bool batch) {
        size_t with_lines = 0;
        for (const auto& item : items) {
            with_lines += item.lines.empty() ? 0 : 1;
        }
        const bool use_journal = with_lines > 1 || journal_dirty || (batch && with_lines > 0);
        if (use_journal) {
            LString journal;
            journal.push_back('[');
            bool first = true;
            for (const auto& item : items) {
                std::string_view rest(item.lines);
                while (!rest.empty()) {
                    const size_t nl = rest.find('\n');
                    journal.append(first ? R"({"f":")" : R"(,{"f":")");
                    journal.append(item.name);
                    journal.append(R"(","r":)");
                    journal.append(rest.substr(0, nl));
                    journal.push_back('}');
                    first = false;
                    rest.remove_prefix(nl + 1);
                }
            }
            journal.push_back(']');
            auto r = deps.storage.write_atomic(journal_path(), bytes(journal));
            if (!r) {
                WLOG_W(kTag, "%s: journal write failed: %s", dir.c_str(),
                       r.error().message.c_str());
                return r;
            }
            journal_dirty = true;
        }
        Result<void> result = Result<void>::ok();
        for (auto& item : items) {
            const std::string path = path_of(item.name);
            auto r = item.rewrite ? deps.storage.write_atomic(path, bytes(item.content))
                                  : deps.storage.append(path, bytes(item.lines));
            item.state = r ? FlushItem::State::Ok : FlushItem::State::Failed;
            if (!r) {
                WLOG_W(kTag, "%s: %s failed: %s", path.c_str(), item.rewrite ? "rewrite" : "append",
                       r.error().message.c_str());
                if (result) {
                    result = r;
                }
            }
        }
        if (use_journal && result) {
            auto r = deps.storage.remove_tree(journal_path());
            if (r) {
                journal_dirty = false;
            } else {
                WLOG_W(kTag, "%s: journal not removed: %s", dir.c_str(), r.error().message.c_str());
            }
        }
        return result;
    }

    // Puts what failed (or never ran) back: its lines go before anything
    // written since, its accounting is restored. A failed append may have
    // left a partial line behind if truncating back failed too, so that
    // file is rewritten next time rather than appended to.
    void settle_locked(std::vector<FlushItem>& items) {
        for (auto& item : items) {
            if (item.state == FlushItem::State::Ok) {
                continue;
            }
            LogState* log = log_of(item.name);
            if (log == nullptr) {
                continue;
            }
            log->disk = item.prev_disk;
            log->appended = item.prev_appended;
            log->needs_rewrite = item.prev_needs_rewrite ||
                                 (item.state == FlushItem::State::Failed && !item.rewrite);
            item.lines.append(log->pending);
            log->pending.swap(item.lines);
        }
    }
};

// ---- SceneStore -------------------------------------------------------

std::shared_ptr<SceneStore> SceneStore::create(SceneStoreDeps deps, std::string data_dir,
                                               SceneStoreLimits limits) {
    return std::make_shared<SceneStore>(Private{}, std::move(deps), std::move(data_dir), limits);
}

SceneStore::SceneStore(Private, SceneStoreDeps deps, std::string data_dir, SceneStoreLimits limits)
    : impl_(std::make_unique<Impl>(std::move(deps), std::move(data_dir), limits)) {}

SceneStore::~SceneStore() {
    uint64_t lost = impl_->ix.kv_log.pending.size();
    for (const auto& [name, c] : impl_->ix.colls) {
        lost += c.log.pending.size();
    }
    if (lost != 0) {
        WLOG_W(kTag, "%s: %llu bytes of writes never reached the disk", impl_->dir.c_str(),
               static_cast<unsigned long long>(lost));
    }
}

Result<void> SceneStore::load() {
    Impl& m = *impl_;
    std::lock_guard<os::Mutex> io(m.io_mu);
    Index ix;
    bool journal_dirty = false;

    std::vector<std::string> stems;
    auto listed = m.deps.storage.list_dir(m.dir);
    if (!listed && listed.error().code != ErrorCode::NotFound) {
        return Result<void>::err(listed.error().code, listed.error().message);
    }
    if (listed) {
        for (const auto& e : listed.value()) {
            if (e.kind != EntryKind::File) {
                continue;
            }
            std::string_view name = e.name;
            // A ".new" left by an interrupted write_atomic() is promoted
            // when the log is read under its own name.
            if (ends_with(name, ".new")) {
                name.remove_suffix(4);
            }
            if (!ends_with(name, kLogSuffix)) {
                continue;
            }
            name.remove_suffix(kLogSuffix.size());
            if ((name == kKv || valid_collection(name)) &&
                std::find(stems.begin(), stems.end(), name) == stems.end()) {
                stems.emplace_back(name);
            }
        }
    }

    for (const auto& stem : stems) {
        const std::string path = m.path_of(stem);
        auto data = m.deps.storage.read_file_large(path);
        if (!data) {
            if (data.error().code == ErrorCode::NotFound) {
                continue;
            }
            return Result<void>::err(data.error().code, data.error().message);
        }
        if (stem != kKv && ix.colls.find(stem) == ix.colls.end()) {
            ix.colls.emplace(stem, Collection{});
        }
        const std::string_view all(reinterpret_cast<const char*>(data.value().data()),
                                   data.value().size());
        bool dirty = false;
        size_t pos = 0;
        while (pos < all.size()) {
            const size_t nl = all.find('\n', pos);
            if (nl == npos) {
                WLOG_W(kTag, "%s: dropped %u bytes of a torn last line", path.c_str(),
                       static_cast<unsigned>(all.size() - pos));
                dirty = true;
                break;
            }
            if (!apply_line(ix, stem, all.substr(pos, nl - pos))) {
                WLOG_W(kTag, "%s: skipped an unreadable line at byte %u", path.c_str(),
                       static_cast<unsigned>(pos));
                dirty = true;
            }
            pos = nl + 1;
        }
        LogState* log = stem == kKv ? &ix.kv_log : &ix.colls.find(stem)->second.log;
        log->disk = all.size();
        if (dirty) {
            // Appending after torn bytes would bury them mid-file, so the
            // file holds only valid lines before anything is appended.
            LString content;
            if (stem == kKv) {
                serialize(ix.kv, content);
            } else {
                serialize(ix.colls.find(stem)->second, content);
            }
            WISP_TRY(m.deps.storage.write_atomic(path, bytes(content)));
            log->disk = content.size();
        }
    }

    auto journal = m.deps.storage.read_file_large(m.journal_path());
    if (!journal && journal.error().code != ErrorCode::NotFound) {
        return Result<void>::err(journal.error().code, journal.error().message);
    }
    if (journal) {
        const std::string_view text(reinterpret_cast<const char*>(journal.value().data()),
                                    journal.value().size());
        // (file name, line) views into `text`; a big batch has many.
        os::LargeVector<std::pair<std::string_view, std::string_view>> entries;
        bool ok = for_each_element(text, [&](std::string_view element) {
            std::string_view f;
            std::string_view r;
            for_each_member(element, [&](std::string_view k, std::string_view v) {
                if (k == "f") {
                    f = v;
                } else if (k == "r") {
                    r = v;
                }
                return true;
            });
            entries.emplace_back(f.size() >= 2 ? f.substr(1, f.size() - 2) : std::string_view(), r);
        });
        for (const auto& [name, line] : entries) {
            ParsedLine p;
            ok = ok && (name == kKv || valid_collection(name)) && parse_line(line, name == kKv, p);
        }
        if (!ok) {
            WLOG_W(kTag, "%s: discarding an unreadable journal", m.dir.c_str());
            entries.clear();
        }
        std::map<std::string, LString, std::less<>> replay;
        for (const auto& [name, line] : entries) {
            apply_line(ix, name, line);
            LString& out = replay.try_emplace(std::string(name)).first->second;
            out.append(line);
            out.push_back('\n');
        }
        for (const auto& [name, lines] : replay) {
            WISP_TRY(m.deps.storage.append(m.path_of(name), bytes(lines)));
            LogState* log = name == kKv ? &ix.kv_log : &ix.colls.find(name)->second.log;
            log->disk += lines.size();
            log->appended += lines.size();
        }
        if (!entries.empty()) {
            WLOG_I(kTag, "%s: replayed %u journal lines", m.dir.c_str(),
                   static_cast<unsigned>(entries.size()));
        }
        auto removed = m.deps.storage.remove_tree(m.journal_path());
        if (!removed) {
            WLOG_W(kTag, "%s: journal not removed: %s", m.dir.c_str(),
                   removed.error().message.c_str());
            journal_dirty = true;
        }
    }

    m.journal_dirty = journal_dirty;
    std::lock_guard<os::Mutex> lock(m.mu);
    std::swap(m.ix, ix);
    m.clear_overlay_locked();
    m.loaded = true;
    return Result<void>::ok();
}

std::optional<os::LargeString> SceneStore::kv_get(std::string_view key) const {
    const Impl& m = *impl_;
    std::lock_guard<os::Mutex> lock(m.mu);
    auto o = m.kv_ov.find(key);
    if (o != m.kv_ov.end()) {
        return o->second;
    }
    auto it = m.ix.kv.find(key);
    if (it == m.ix.kv.end()) {
        return std::nullopt;
    }
    return it->second;
}

namespace {

Result<void> check_key(std::string_view key, size_t limit) {
    if (key.empty()) {
        return Result<void>::err(ErrorCode::InvalidArgument, "invalid_key");
    }
    if (key.size() > limit) {
        return Result<void>::err(ErrorCode::InvalidArgument, "key_too_long");
    }
    return Result<void>::ok();
}

Result<void> check_record(std::string_view json, size_t limit, bool object) {
    if (json.size() > limit) {
        return Result<void>::err(ErrorCode::InvalidArgument, "record_too_large");
    }
    if (json.find_first_of("\r\n") != npos || !valid_json(json)) {
        return Result<void>::err(ErrorCode::InvalidArgument, "invalid_json");
    }
    if (object && json[skip_ws(json, 0)] != '{') {
        return Result<void>::err(ErrorCode::InvalidArgument, "record_not_object");
    }
    return Result<void>::ok();
}

Result<void> check_collection(std::string_view name) {
    if (!valid_collection(name)) {
        return Result<void>::err(ErrorCode::InvalidArgument, "invalid_collection");
    }
    return Result<void>::ok();
}

Result<void> not_loaded() { return Result<void>::err(ErrorCode::StorageUnavailable, "not_loaded"); }

}  // namespace

Result<void> SceneStore::kv_set(std::string_view key, std::string_view json) {
    Impl& m = *impl_;
    WISP_TRY(check_key(key, m.limits.key_bytes));
    WISP_TRY(check_record(json, m.limits.record_bytes, false));
    const std::string id = quoted(key);
    bool arm = false;
    {
        std::lock_guard<os::Mutex> lock(m.mu);
        if (!m.loaded) {
            return not_loaded();
        }
        if (m.batch_depth > 0) {
            return m.write_overlay_locked(m.kv_ov, &m.ix.kv, key, id.size(), json);
        }
        auto r = m.write_base_locked(m.ix.kv_log, m.ix.kv, key, id, json);
        WISP_TRY(r);
        arm = r.value() && m.note_pending_locked();
    }
    if (arm) {
        arm_timer();
    }
    return Result<void>::ok();
}

Result<void> SceneStore::kv_del(std::string_view key) {
    Impl& m = *impl_;
    WISP_TRY(check_key(key, m.limits.key_bytes));
    const std::string id = quoted(key);
    bool arm = false;
    {
        std::lock_guard<os::Mutex> lock(m.mu);
        if (!m.loaded) {
            return not_loaded();
        }
        if (m.batch_depth > 0) {
            return m.write_overlay_locked(m.kv_ov, &m.ix.kv, key, id.size(), std::nullopt);
        }
        auto r = m.write_base_locked(m.ix.kv_log, m.ix.kv, key, id, std::nullopt);
        WISP_TRY(r);
        arm = r.value() && m.note_pending_locked();
    }
    if (arm) {
        arm_timer();
    }
    return Result<void>::ok();
}

Result<int64_t> SceneStore::insert(std::string_view coll, std::string_view json) {
    Impl& m = *impl_;
    WISP_TRY(check_collection(coll));
    WISP_TRY(check_record(json, m.limits.record_bytes, true));
    bool arm = false;
    int64_t id = 0;
    {
        std::lock_guard<os::Mutex> lock(m.mu);
        if (!m.loaded) {
            return Result<int64_t>::err(ErrorCode::StorageUnavailable, "not_loaded");
        }
        auto base = m.ix.colls.find(coll);
        const bool exists = base != m.ix.colls.end() || m.coll_ov.find(coll) != m.coll_ov.end();
        if (!exists && m.collection_count_locked() >= m.limits.max_collections) {
            return Result<int64_t>::err(ErrorCode::QuotaExceeded, "too_many_collections");
        }
        if (m.batch_depth > 0) {
            CollOverlay probe;
            const CollOverlay* o = nullptr;
            auto found = m.coll_ov.find(coll);
            if (found != m.coll_ov.end()) {
                o = &found->second;
            } else {
                probe.next_id = base != m.ix.colls.end() ? base->second.next_id : 1;
                o = &probe;
            }
            id = o->next_id;
            if (id == std::numeric_limits<int64_t>::max()) {
                return Result<int64_t>::err(ErrorCode::QuotaExceeded, "ids_exhausted");
            }
            const uint64_t line = put_len(IdText(id).n, json.size());
            WISP_TRY(m.check_quota_locked(static_cast<int64_t>(line), static_cast<int64_t>(line)));
            CollOverlay& ov = m.overlay_of_locked(coll);
            ov.records.emplace(id, LString(json.data(), json.size()));
            ov.next_id = id + 1;
            m.ov_line_bytes += static_cast<int64_t>(line);
            m.ov_live_delta += static_cast<int64_t>(line);
            return Result<int64_t>::ok(id);
        }
        id = base != m.ix.colls.end() ? base->second.next_id : 1;
        if (id == std::numeric_limits<int64_t>::max()) {
            return Result<int64_t>::err(ErrorCode::QuotaExceeded, "ids_exhausted");
        }
        const IdText text(id);
        const uint64_t line = put_len(text.n, json.size());
        WISP_TRY(m.check_quota_locked(static_cast<int64_t>(line), static_cast<int64_t>(line)));
        if (base == m.ix.colls.end()) {
            base = m.ix.colls.emplace(std::string(coll), Collection{}).first;
        }
        Collection& c = base->second;
        apply_record(c.log, c.records, id, text.view(), json, &c.log.pending);
        c.next_id = id + 1;
        arm = m.note_pending_locked();
    }
    if (arm) {
        arm_timer();
    }
    return Result<int64_t>::ok(id);
}

Result<void> SceneStore::update(std::string_view coll, int64_t id, std::string_view json) {
    Impl& m = *impl_;
    WISP_TRY(check_collection(coll));
    WISP_TRY(check_record(json, m.limits.record_bytes, true));
    const IdText text(id);
    bool arm = false;
    {
        std::lock_guard<os::Mutex> lock(m.mu);
        if (!m.loaded) {
            return not_loaded();
        }
        if (m.visible_locked(coll, id) == nullptr) {
            return Result<void>::err(ErrorCode::NotFound, "not_found");
        }
        auto base = m.ix.colls.find(coll);
        if (m.batch_depth > 0) {
            const IdMap* records = base != m.ix.colls.end() ? &base->second.records : nullptr;
            return m.write_overlay_locked(m.overlay_of_locked(coll).records, records, id, text.n,
                                          json);
        }
        Collection& c = base->second;
        auto r = m.write_base_locked(c.log, c.records, id, text.view(), json);
        WISP_TRY(r);
        arm = m.note_pending_locked();
    }
    if (arm) {
        arm_timer();
    }
    return Result<void>::ok();
}

Result<void> SceneStore::remove(std::string_view coll, int64_t id) {
    Impl& m = *impl_;
    WISP_TRY(check_collection(coll));
    const IdText text(id);
    bool arm = false;
    {
        std::lock_guard<os::Mutex> lock(m.mu);
        if (!m.loaded) {
            return not_loaded();
        }
        if (m.visible_locked(coll, id) == nullptr) {
            return Result<void>::err(ErrorCode::NotFound, "not_found");
        }
        auto base = m.ix.colls.find(coll);
        if (m.batch_depth > 0) {
            const IdMap* records = base != m.ix.colls.end() ? &base->second.records : nullptr;
            return m.write_overlay_locked(m.overlay_of_locked(coll).records, records, id, text.n,
                                          std::nullopt);
        }
        Collection& c = base->second;
        auto r = m.write_base_locked(c.log, c.records, id, text.view(), std::nullopt);
        WISP_TRY(r);
        arm = m.note_pending_locked();
    }
    if (arm) {
        arm_timer();
    }
    return Result<void>::ok();
}

Result<os::LargeVector<StoreRecord>> SceneStore::find(std::string_view coll,
                                                      const FindQuery& q) const {
    using Out = os::LargeVector<StoreRecord>;
    if (!valid_collection(coll)) {
        return Result<Out>::err(ErrorCode::InvalidArgument, "invalid_collection");
    }
    const Impl& m = *impl_;
    std::lock_guard<os::Mutex> lock(m.mu);

    // The visible records in id order: the collection with the batch's
    // overlay applied.
    os::LargeVector<SortKey> rows;
    auto add = [&](int64_t id, const LString& json) {
        for (const auto& [name, want] : q.equals) {
            auto v = field(json, name);
            if (!v || !value_equals(*v, want)) {
                return;
            }
        }
        SortKey k;
        k.id = id;
        k.json = &json;
        rows.push_back(std::move(k));
    };
    auto base = m.ix.colls.find(coll);
    auto ov = m.coll_ov.find(coll);
    const IdOverlay* o = ov != m.coll_ov.end() ? &ov->second.records : nullptr;
    if (base != m.ix.colls.end()) {
        for (const auto& [id, json] : base->second.records) {
            if (o == nullptr || o->find(id) == o->end()) {
                add(id, json);
            }
        }
    }
    if (o != nullptr) {
        for (const auto& [id, json] : *o) {
            if (json) {
                add(id, *json);
            }
        }
        std::sort(rows.begin(), rows.end(),
                  [](const SortKey& a, const SortKey& b) { return a.id < b.id; });
    }

    if (!q.sort.empty()) {
        for (auto& k : rows) {
            auto v = field(*k.json, q.sort);
            if (!v) {
                continue;
            }
            if (is_number(*v)) {
                k.cls = 0;
                k.num = to_double(*v);
            } else if (v->front() == '"') {
                k.cls = 1;
                k.raw = v->substr(1, v->size() - 2);
                if (k.raw.find('\\') != npos) {
                    unescape(k.raw, k.decoded);
                    k.has_decoded = true;
                }
            }
        }
        const bool desc = q.desc;
        std::sort(rows.begin(), rows.end(), [desc](const SortKey& a, const SortKey& b) {
            if (a.cls != b.cls) {
                return a.cls < b.cls;
            }
            if (a.cls == 0 && a.num != b.num) {
                return desc ? a.num > b.num : a.num < b.num;
            }
            if (a.cls == 1) {
                const int c = a.text().compare(b.text());
                if (c != 0) {
                    return desc ? c > 0 : c < 0;
                }
            }
            return a.id < b.id;
        });
    }

    Out out;
    if (q.offset < rows.size()) {
        const size_t n = std::min(q.limit, rows.size() - q.offset);
        out.reserve(n);
        for (size_t i = q.offset; i < q.offset + n; ++i) {
            out.push_back(StoreRecord{rows[i].id, *rows[i].json});
        }
    }
    return Result<Out>::ok(std::move(out));
}

void SceneStore::begin_batch() {
    std::lock_guard<os::Mutex> lock(impl_->mu);
    ++impl_->batch_depth;
}

Result<void> SceneStore::commit_batch() {
    Impl& m = *impl_;
    bool arm = false;
    {
        std::lock_guard<os::Mutex> lock(m.mu);
        if (m.batch_depth == 0) {
            return Result<void>::err(ErrorCode::InvalidArgument, "no_batch");
        }
        if (--m.batch_depth > 0) {
            return Result<void>::ok();
        }
        if (m.batch_aborted) {
            m.clear_overlay_locked();
            return Result<void>::err(ErrorCode::InvalidArgument, "batch_aborted");
        }
        size_t lines = 0;
        for (auto& [key, v] : m.kv_ov) {
            std::optional<std::string_view> value;
            if (v) {
                value = std::string_view(*v);
            }
            apply_record(m.ix.kv_log, m.ix.kv, std::string_view(key), quoted(key), value,
                         &m.ix.kv_log.pending);
            ++lines;
        }
        for (auto& [name, o] : m.coll_ov) {
            auto base = m.ix.colls.find(name);
            if (base == m.ix.colls.end()) {
                base = m.ix.colls.emplace(name, Collection{}).first;
            }
            Collection& c = base->second;
            const int64_t prev_next = c.next_id;
            for (auto& [id, v] : o.records) {
                std::optional<std::string_view> value;
                if (v) {
                    value = std::string_view(*v);
                }
                apply_record(c.log, c.records, id, IdText(id).view(), value, &c.log.pending);
                ++lines;
            }
            // An id the batch handed out and then deleted left no line;
            // record the high-water mark so a reload does not reuse it.
            const int64_t top = o.next_id - 1;
            if (top >= prev_next && o.records.find(top) == o.records.end()) {
                append_line(c.log.pending, IdText(top).view(), std::nullopt);
                ++lines;
            }
            c.next_id = std::max(c.next_id, o.next_id);
        }
        m.clear_overlay_locked();
        m.batch_pending = m.batch_pending || lines > 1;
        arm = lines > 0 && m.note_pending_locked();
    }
    if (arm) {
        arm_timer();
    }
    return Result<void>::ok();
}

void SceneStore::abort_batch() {
    Impl& m = *impl_;
    std::lock_guard<os::Mutex> lock(m.mu);
    if (m.batch_depth == 0) {
        return;
    }
    if (--m.batch_depth == 0) {
        m.clear_overlay_locked();
    } else {
        m.batch_aborted = true;
    }
}

void SceneStore::flush(std::function<void(Result<void>)> done) {
    auto self = shared_from_this();
    std::function<void(Result<void>)> on_done = done;
    if (!impl_->deps.submit_io([self, done = std::move(done)] { self->flush_job(done); })) {
        WLOG_W(kTag, "%s: flush not queued", impl_->dir.c_str());
        if (on_done) {
            on_done(Result<void>::err(ErrorCode::Busy, "io_queue_full"));
        }
    }
}

uint64_t SceneStore::used_bytes() const {
    std::lock_guard<os::Mutex> lock(impl_->mu);
    return impl_->used_locked();
}

void SceneStore::arm_timer() { arm_timer(impl_->limits.coalesce_ms); }

void SceneStore::arm_timer(uint32_t delay_ms) {
    auto self = shared_from_this();
    impl_->deps.schedule(delay_ms, [self] { self->on_timer(); });
}

void SceneStore::schedule_retry() {
    Impl& m = *impl_;
    uint32_t delay = 0;
    {
        std::lock_guard<os::Mutex> lock(m.mu);
        if (m.timer_armed) {
            return;  // a write since the failure already armed one
        }
        delay = m.retry_ms == 0 ? m.limits.coalesce_ms
                                : std::min<uint32_t>(m.retry_ms * 2, kMaxRetryMs);
        m.retry_ms = delay;
        m.timer_armed = true;
    }
    arm_timer(delay);
}

void SceneStore::on_timer() {
    auto self = shared_from_this();
    if (!impl_->deps.submit_io([self] { self->flush_job({}); })) {
        WLOG_W(kTag, "%s: flush not queued", impl_->dir.c_str());
        {
            std::lock_guard<os::Mutex> lock(impl_->mu);
            impl_->timer_armed = false;
        }
        schedule_retry();
    }
}

void SceneStore::flush_job(const std::function<void(Result<void>)>& done) {
    Impl& m = *impl_;
    Result<void> result = Result<void>::ok();
    {
        std::lock_guard<os::Mutex> io(m.io_mu);
        std::vector<FlushItem> items;
        bool batch = false;
        {
            // A write after this point finds no armed timer and arms one;
            // every write before it is in the snapshot.
            std::lock_guard<os::Mutex> lock(m.mu);
            m.timer_armed = false;
            batch = m.take_snapshot_locked(items);
        }
        if (!items.empty()) {
            result = m.write_items(items, batch);
            std::lock_guard<os::Mutex> lock(m.mu);
            m.settle_locked(items);
            // The batch's lines are back in pending; the retry must take
            // the journal again (journal_dirty covers it only once the
            // journal itself was written).
            m.batch_pending = m.batch_pending || (batch && !result);
            if (result) {
                m.retry_ms = 0;
            }
        }
    }
    if (!result) {
        // Data waiting in pending is lost on a power cut; retry rather
        // than wait for the next write or the scene's close.
        schedule_retry();
    }
    if (done) {
        done(std::move(result));
    }
}

}  // namespace wisp::core
