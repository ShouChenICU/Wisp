#include "wisp/base/json.hpp"

// ArduinoJson's default (2 bytes of length) stops strings at 65535 bytes,
// but a stored tool result or a file written by the agent can be 64 KiB
// or a little more. This is the only file that includes ArduinoJson, so
// the setting cannot differ between two users of its templates.
#define ARDUINOJSON_STRING_LENGTH_SIZE 4
#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace wisp {

namespace {

void write_value(JsonVariant dst, const Value& v) {
    switch (v.type()) {
        case Value::Type::Null:
            // JsonVariant starts out null; nothing to do.
            break;
        case Value::Type::Bool:
            dst.set(v.as_bool());
            break;
        case Value::Type::Int:
            dst.set(v.as_int());
            break;
        case Value::Type::Double:
            dst.set(v.as_double());
            break;
        case Value::Type::String:
            dst.set(v.as_string());
            break;
        case Value::Type::List: {
            JsonArray arr = dst.to<JsonArray>();
            for (const Value& item : v.as_list()) {
                write_value(arr.add<JsonVariant>(), item);
            }
            break;
        }
        case Value::Type::Map: {
            JsonObject obj = dst.to<JsonObject>();
            for (const auto& [key, value] : v.as_map()) {
                write_value(obj[key].to<JsonVariant>(), value);
            }
            break;
        }
    }
}

Value read_value(JsonVariantConst src) {
    if (src.isNull()) {
        return Value();
    }
    if (src.is<JsonArrayConst>()) {
        List list;
        for (JsonVariantConst item : src.as<JsonArrayConst>()) {
            list.push_back(read_value(item));
        }
        return Value(std::move(list));
    }
    if (src.is<JsonObjectConst>()) {
        Map map;
        for (JsonPairConst kv : src.as<JsonObjectConst>()) {
            map.emplace(kv.key().c_str(), read_value(kv.value()));
        }
        return Value(std::move(map));
    }
    // Order matters: an integer value also satisfies is<double>(), so Int
    // must be checked first to keep Int/Double distinct on round-trip.
    if (src.is<int64_t>()) {
        return Value(src.as<int64_t>());
    }
    if (src.is<double>()) {
        return Value(src.as<double>());
    }
    if (src.is<bool>()) {
        return Value(src.as<bool>());
    }
    if (src.is<const char*>()) {
        return Value(std::string(src.as<const char*>()));
    }
    return Value();
}

// ArduinoJson escapes only the control bytes JSON gives a short form to
// (and NUL), and copies other bytes as they are. JSON.parse() rejects the
// remaining raw control characters, so every byte below 0x20 becomes a
// 6-character escape (backslash, 'u', four hex digits); from_json() turns
// it back into the same byte. With `replace_invalid_utf8`, every
// ill-formed UTF-8 sequence also becomes U+FFFD: a WebSocket text frame
// that is not valid UTF-8 drops the connection (RFC 6455), and strings
// come from logs cut at a byte limit, SSIDs and other device data.
// Compact output has no whitespace and multi-byte text only occurs inside
// strings, so a byte-level pass cannot touch the JSON structure. Output
// that needs no change is returned as is; otherwise the copy starts at
// the first byte that does, keeping the pass linear.
std::string fix_up(std::string json, bool replace_invalid_utf8) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    bool copying = false;
    // Switches to building `out`, seeded with the unchanged bytes before i.
    auto start_copy = [&](size_t i) {
        if (copying) return;
        out.reserve(json.size() + 16);
        out.append(json, 0, i);
        copying = true;
    };
    size_t i = 0;
    while (i < json.size()) {
        const auto b = static_cast<uint8_t>(json[i]);
        if (b < 0x20) {
            start_copy(i);
            const char esc[] = {'\\', 'u', '0', '0', kHex[b >> 4], kHex[b & 0xF]};
            out.append(esc, sizeof(esc));
            ++i;
            continue;
        }
        size_t keep = 1;  // bytes at i that pass through unchanged
        if (b >= 0x80) {
            size_t bad = 0;
            keep = json_detail::utf8_sequence(json, i, &bad);
            if (keep == 0 && replace_invalid_utf8) {
                start_copy(i);
                out += "\xEF\xBF\xBD";
                i += bad;
                continue;
            }
            if (keep == 0) keep = 1;
        }
        if (copying) out.append(json, i, keep);
        i += keep;
    }
    return copying ? out : json;
}

std::string serialize(const Value& v) {
    JsonDocument doc;
    write_value(doc.to<JsonVariant>(), v);
    std::string out;
    serializeJson(doc, out);
    return out;
}

// A sink for append_json_string() that only counts.
struct CountingSink {
    size_t n = 0;
    void push_back(char) { ++n; }
    void append(const char*, size_t len) { n += len; }
    void append(const char* s) { n += std::strlen(s); }
};

size_t int_size(int64_t i) {
    // Through uint64_t, so INT64_MIN has a magnitude too.
    uint64_t magnitude = i < 0 ? 0 - static_cast<uint64_t>(i) : static_cast<uint64_t>(i);
    size_t digits = 1;
    while (magnitude >= 10) {
        magnitude /= 10;
        ++digits;
    }
    return digits + (i < 0 ? 1 : 0);
}

// ArduinoJson's own float formatting, measured on a one-value document
// so the count cannot drift from serialize().
size_t double_size(double d) {
    JsonDocument doc;
    doc.set(d);
    return measureJson(doc);
}

// Adds the size of `v` to `n`; false once `n` exceeds `limit`.
bool add_json_size(const Value& v, size_t limit, size_t& n) {
    auto add = [&](size_t k) {
        n += k;
        return n <= limit;
    };
    // A string never gets shorter when escaped, so its raw size plus the
    // quotes already tells when it cannot fit, before reading it.
    auto add_string = [&](const std::string& s) {
        if (s.size() + 2 > limit - n) {
            n = limit + 1;
            return false;
        }
        CountingSink sink;
        append_json_string(sink, s);
        return add(sink.n);
    };
    switch (v.type()) {
        case Value::Type::Null:
            return add(4);
        case Value::Type::Bool:
            return add(v.as_bool() ? 4 : 5);
        case Value::Type::Int:
            return add(int_size(v.as_int()));
        case Value::Type::Double:
            return add(double_size(v.as_double()));
        case Value::Type::String:
            return add_string(v.as_string());
        case Value::Type::List: {
            const List& list = v.as_list();
            if (!add(list.empty() ? 2 : 1 + list.size())) return false;  // [ , , ]
            for (const Value& item : list) {
                if (!add_json_size(item, limit, n)) return false;
            }
            return true;
        }
        case Value::Type::Map: {
            const Map& map = v.as_map();
            if (!add(map.empty() ? 2 : 1 + 2 * map.size())) return false;  // { : , : }
            for (const auto& [key, value] : map) {
                if (!add_string(key) || !add_json_size(value, limit, n)) return false;
            }
            return true;
        }
    }
    return true;
}

}  // namespace

size_t json_size(const Value& v, size_t limit) {
    size_t n = 0;
    return add_json_size(v, limit, n) ? n : limit + 1;
}

bool valid_utf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        if (static_cast<uint8_t>(s[i]) < 0x80) {
            ++i;
            continue;
        }
        size_t bad = 0;
        const size_t len = json_detail::utf8_sequence(s, i, &bad);
        if (len == 0) return false;
        i += len;
    }
    return true;
}

size_t json_detail::utf8_sequence(std::string_view s, size_t i, size_t* bad) {
    const auto b = static_cast<uint8_t>(s[i]);
    size_t len = 0;
    uint8_t lo = 0x80;
    uint8_t hi = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) {
        len = 2;
    } else if (b >= 0xE0 && b <= 0xEF) {
        len = 3;
        if (b == 0xE0) lo = 0xA0;  // overlong
        if (b == 0xED) hi = 0x9F;  // UTF-16 surrogates
    } else if (b >= 0xF0 && b <= 0xF4) {
        len = 4;
        if (b == 0xF0) lo = 0x90;  // overlong
        if (b == 0xF4) hi = 0x8F;  // past U+10FFFF
    } else {
        *bad = 1;
        return 0;
    }
    for (size_t k = 1; k < len; ++k) {
        const uint8_t min = k == 1 ? lo : 0x80;
        const uint8_t max = k == 1 ? hi : 0xBF;
        if (i + k >= s.size() || static_cast<uint8_t>(s[i + k]) < min ||
            static_cast<uint8_t>(s[i + k]) > max) {
            *bad = k;
            return 0;
        }
    }
    return len;
}

std::string to_json(const Value& v) { return fix_up(serialize(v), /*replace_invalid_utf8=*/true); }

std::string to_json_raw(const Value& v) {
    return fix_up(serialize(v), /*replace_invalid_utf8=*/false);
}

Result<Value> from_json(std::string_view s) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, s.data(), s.size());
    if (err) {
        return Result<Value>::err(ErrorCode::InvalidArgument, err.c_str());
    }
    return Result<Value>::ok(read_value(doc.as<JsonVariantConst>()));
}

Result<Value> from_json(std::string_view s, uint8_t max_depth) {
    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, s.data(), s.size(), DeserializationOption::NestingLimit(max_depth));
    if (err == DeserializationError::TooDeep) {
        return Result<Value>::err(ErrorCode::QuotaExceeded,
                                  "nested deeper than " + std::to_string(max_depth) + " levels");
    }
    if (err) {
        return Result<Value>::err(ErrorCode::InvalidArgument, err.c_str());
    }
    // The document is at most max_depth deep, and so is this recursion.
    return Result<Value>::ok(read_value(doc.as<JsonVariantConst>()));
}

// ---- parse_json_large() ----------------------------------------------------

static_assert(sizeof(JsonVariantConst) <= sizeof(JsonView{}) &&
                  alignof(JsonVariantConst) <= alignof(void*),
              "JsonView::v_ must hold a JsonVariantConst");
static_assert(std::is_trivially_copyable_v<JsonVariantConst> &&
                  std::is_trivially_destructible_v<JsonVariantConst>,
              "JsonView copies and drops v_ as plain bytes");

struct JsonViewAccess {
    static JsonVariantConst get(const JsonView& view) {
        JsonVariantConst v;
        std::memcpy(static_cast<void*>(&v), view.v_, sizeof v);
        return v;
    }
    static JsonView make(JsonVariantConst v) {
        JsonView view;
        std::memcpy(view.v_, static_cast<const void*>(&v), sizeof v);
        return view;
    }
};

namespace {

class AllocAdapter : public ArduinoJson::Allocator {
public:
    explicit AllocAdapter(const JsonAlloc& mem) : mem_(mem) {}
    void* allocate(size_t size) override { return mem_.allocate(size); }
    void deallocate(void* ptr) override { mem_.deallocate(ptr); }
    void* reallocate(void* ptr, size_t new_size) override { return mem_.reallocate(ptr, new_size); }
    const JsonAlloc& mem() const { return mem_; }

private:
    JsonAlloc mem_;
};

// The document keeps a pointer to its allocator, so both live together at
// one address that a LargeJson move does not change.
struct LargeJsonImpl {
    explicit LargeJsonImpl(const JsonAlloc& mem) : alloc(mem), doc(&alloc) {}
    AllocAdapter alloc;
    JsonDocument doc;
};

void destroy_impl(void* p) {
    if (p == nullptr) return;
    auto* impl = static_cast<LargeJsonImpl*>(p);
    // Copied out first: the adapter holding it dies with the document.
    void (*deallocate)(void*) = impl->alloc.mem().deallocate;
    impl->~LargeJsonImpl();
    deallocate(impl);
}

// Batches ArduinoJson's byte-at-a-time output into fixed chunks for the
// sink, escaping control bytes on the way (see fix_up()). Compact output
// has no whitespace, so any byte below 0x20 is inside a string.
class SinkWriter {
public:
    SinkWriter(void (*sink)(void*, const char*, size_t), void* ctx) : sink_(sink), ctx_(ctx) {}
    ~SinkWriter() { flush(); }
    SinkWriter(const SinkWriter&) = delete;
    SinkWriter& operator=(const SinkWriter&) = delete;

    size_t write(uint8_t c) {
        put(c);
        return 1;
    }
    size_t write(const uint8_t* s, size_t n) {
        for (size_t i = 0; i < n; ++i) put(s[i]);
        return n;
    }
    void flush() {
        if (n_ > 0) sink_(ctx_, buf_, n_);
        n_ = 0;
    }

private:
    void put(uint8_t c) {
        if (c >= 0x20) {
            if (n_ == sizeof buf_) flush();
            buf_[n_++] = static_cast<char>(c);
            return;
        }
        static constexpr char kHex[] = "0123456789abcdef";
        const char esc[] = {'\\', 'u', '0', '0', kHex[c >> 4], kHex[c & 0xF]};
        if (n_ + sizeof esc > sizeof buf_) flush();
        std::memcpy(buf_ + n_, esc, sizeof esc);
        n_ += sizeof esc;
    }

    void (*sink_)(void*, const char*, size_t);
    void* ctx_;
    char buf_[256] = {};
    size_t n_ = 0;
};

}  // namespace

JsonView::Type JsonView::type() const {
    const JsonVariantConst v = JsonViewAccess::get(*this);
    if (v.isNull()) return Type::Null;
    if (v.is<JsonObjectConst>()) return Type::Object;
    if (v.is<JsonArrayConst>()) return Type::Array;
    if (v.is<bool>()) return Type::Bool;
    // An integer also satisfies is<double>(), so Int is checked first, as
    // read_value() does.
    if (v.is<int64_t>()) return Type::Int;
    if (v.is<double>()) return Type::Double;
    if (v.is<JsonString>()) return Type::String;
    return Type::Null;
}

JsonView JsonView::find(std::string_view key) const {
    const JsonVariantConst v = JsonViewAccess::get(*this);
    if (!v.is<JsonObjectConst>()) return {};
    return JsonViewAccess::make(v.as<JsonObjectConst>()[JsonString(key.data(), key.size())]);
}

JsonView JsonView::at(size_t i) const {
    const JsonVariantConst v = JsonViewAccess::get(*this);
    if (!v.is<JsonArrayConst>()) return {};
    return JsonViewAccess::make(v.as<JsonArrayConst>()[i]);
}

size_t JsonView::size() const {
    const JsonVariantConst v = JsonViewAccess::get(*this);
    if (v.is<JsonObjectConst>() || v.is<JsonArrayConst>()) return v.size();
    return 0;
}

std::string_view JsonView::string() const {
    const JsonVariantConst v = JsonViewAccess::get(*this);
    if (!v.is<JsonString>()) return {};
    const JsonString s = v.as<JsonString>();
    return {s.c_str(), s.size()};
}

std::optional<int64_t> JsonView::integer() const {
    const JsonVariantConst v = JsonViewAccess::get(*this);
    if (v.is<bool>() || !v.is<int64_t>()) return std::nullopt;
    return v.as<int64_t>();
}

bool JsonView::boolean(bool fallback) const {
    const JsonVariantConst v = JsonViewAccess::get(*this);
    return v.is<bool>() ? v.as<bool>() : fallback;
}

void JsonView::write(void (*sink)(void* ctx, const char* p, size_t n), void* ctx) const {
    SinkWriter writer(sink, ctx);
    serializeJson(JsonViewAccess::get(*this), writer);
}

LargeJson::LargeJson(LargeJson&& other) noexcept : impl_(std::exchange(other.impl_, nullptr)) {}

LargeJson& LargeJson::operator=(LargeJson&& other) noexcept {
    if (this != &other) {
        destroy_impl(impl_);
        impl_ = std::exchange(other.impl_, nullptr);
    }
    return *this;
}

LargeJson::~LargeJson() { destroy_impl(impl_); }

JsonView LargeJson::root() const {
    if (impl_ == nullptr) return {};
    return JsonViewAccess::make(
        static_cast<const LargeJsonImpl*>(impl_)->doc.as<JsonVariantConst>());
}

Result<LargeJson> parse_json_large(std::string_view text, const JsonAlloc& mem, uint8_t max_depth) {
    void* storage = mem.allocate(sizeof(LargeJsonImpl));
    if (storage == nullptr) {
        return Result<LargeJson>::err(ErrorCode::NoMemory, "out of memory for a JSON document");
    }
    LargeJson out;
    out.impl_ = new (storage) LargeJsonImpl(mem);
    auto* impl = static_cast<LargeJsonImpl*>(out.impl_);
    // (const char*, size_t) makes ArduinoJson copy every string into the
    // document; it would otherwise point into `text`.
    const DeserializationError err = deserializeJson(
        impl->doc, text.data(), text.size(), DeserializationOption::NestingLimit(max_depth));
    if (err == DeserializationError::TooDeep) {
        return Result<LargeJson>::err(
            ErrorCode::QuotaExceeded,
            "nested deeper than " + std::to_string(max_depth) + " levels");
    }
    if (err == DeserializationError::NoMemory) {
        return Result<LargeJson>::err(ErrorCode::NoMemory, "out of memory for a JSON document");
    }
    if (err) {
        return Result<LargeJson>::err(ErrorCode::InvalidArgument, err.c_str());
    }
    return Result<LargeJson>::ok(std::move(out));
}

}  // namespace wisp
