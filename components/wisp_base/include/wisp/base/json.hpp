#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"

namespace wisp {

// Serializes `v` to compact JSON (no extra whitespace). Map keys are
// emitted in std::map's natural (lexicographic) order.
//
// For anything a browser or another program reads (HTTP bodies, WebSocket
// frames, machine-readable log lines): control bytes are escaped and any
// ill-formed UTF-8 in strings is replaced by U+FFFD, so the output is
// always valid UTF-8 that JSON.parse() accepts. That replacement is lossy.
std::string to_json(const Value& v);

// For persistence: the same, except string bytes that are not valid UTF-8
// are kept as they are, so from_json() gives back exactly what was stored
// (an SSID in GBK, for instance). Control bytes are still escaped. The
// output is not necessarily valid UTF-8; never send it to a browser.
std::string to_json_raw(const Value& v);

// The size of to_json(v) in bytes, or limit + 1 as soon as it is known
// to exceed `limit`. Walks `v` without building the text, so a caller
// can refuse an oversized value without an allocation proportional to it
// (no allocation proportional to the value; measuring each double builds
// a small fixed-size temporary document).
size_t json_size(const Value& v, size_t limit);

// Parses `s` as JSON into a Value. On a parse error, returns
// ErrorCode::InvalidArgument with ArduinoJson's error text in the message.
Result<Value> from_json(std::string_view s);

// The same, refusing input nested deeper than `max_depth` levels (the
// outermost array or object is level 1) with ErrorCode::QuotaExceeded.
// Parsing recurses once per level, so this bounds the stack it needs; the
// parser itself enforces it, so no string can hide a level from it.
Result<Value> from_json(std::string_view s, uint8_t max_depth);

// Where parse_json_large() takes its memory. wisp_base sits below wisp_os
// and cannot reach os::alloc(Mem::Large) itself, so the caller passes it
// in (wisp_net's large_json_alloc()). reallocate(nullptr, n) must behave
// like allocate(n); every function may return nullptr on failure.
struct JsonAlloc {
    void* (*allocate)(size_t) = nullptr;
    void (*deallocate)(void*) = nullptr;
    void* (*reallocate)(void*, size_t) = nullptr;
};

// A read-only view into a LargeJson; valid while that document lives.
// Copying a view copies two pointers. A default view is Null.
class JsonView {
public:
    enum class Type : uint8_t { Null, Bool, Int, Double, String, Array, Object };
    Type type() const;
    JsonView find(std::string_view key) const;  // Null view: not an object, or absent
    JsonView at(size_t i) const;                // Null view past the end, or not an array
    size_t size() const;                        // members of an array/object, else 0
    std::string_view string() const;            // "" unless String; points into the document
    std::optional<int64_t> integer() const;     // Int only
    bool boolean(bool fallback = false) const;  // `fallback` unless Bool
    // Compact JSON of this subtree, in pieces, without a temporary copy on
    // the heap. Control bytes inside strings are escaped, as to_json()
    // does; other bytes are written as the document holds them.
    void write(void (*sink)(void* ctx, const char* p, size_t n), void* ctx) const;
    // Calls write() with S::append; `S` is any std::basic_string<char>.
    template <class S>
    void append_to(S& out) const {
        write([](void* ctx, const char* p, size_t n) { static_cast<S*>(ctx)->append(p, n); }, &out);
    }

private:
    friend struct JsonViewAccess;
    // An ArduinoJson JsonVariantConst, which is two pointers (static_assert
    // in json.cpp); kept opaque so json.cpp stays ArduinoJson's only user.
    alignas(void*) unsigned char v_[2 * sizeof(void*)] = {};
};

// A parsed document whose every byte lives in the JsonAlloc it was parsed
// with. Move-only; a moved-from document's root() is Null.
class LargeJson {
public:
    LargeJson(LargeJson&& other) noexcept;
    LargeJson& operator=(LargeJson&& other) noexcept;
    LargeJson(const LargeJson&) = delete;
    LargeJson& operator=(const LargeJson&) = delete;
    ~LargeJson();
    JsonView root() const;

private:
    friend Result<LargeJson> parse_json_large(std::string_view, const JsonAlloc&, uint8_t);
    LargeJson() = default;
    void* impl_ = nullptr;  // {Allocator adapter, JsonDocument}, itself placed with the JsonAlloc
};

// InvalidArgument on a parse error (ArduinoJson's text), QuotaExceeded past
// max_depth (as from_json(s, max_depth)), NoMemory when the allocator fails.
// Every byte of the document comes from `mem`; strings are copied out of
// `text`, which need not outlive the result.
Result<LargeJson> parse_json_large(std::string_view text, const JsonAlloc& mem,
                                   uint8_t max_depth = 32);

// Whether `s` is well-formed UTF-8: exactly the strings to_json() keeps
// without replacing anything.
bool valid_utf8(std::string_view s);

namespace json_detail {
// Length of the well-formed UTF-8 sequence starting at s[i], or 0 when
// there is none; then *bad is how many bytes form the maximal ill-formed
// prefix (at least 1), which becomes one U+FFFD.
size_t utf8_sequence(std::string_view s, size_t i, size_t* bad);
}  // namespace json_detail

// Appends `s` as a JSON string literal, quotes included: byte for byte
// what to_json(Value(s)) gives, without building a Value or a temporary
// copy. For writing a large JSON document straight into its final buffer
// (an os::LargeString in PSRAM); `S` is any std::basic_string<char>.
template <class S>
void append_json_string(S& out, std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    size_t i = 0;
    while (i < s.size()) {
        const auto b = static_cast<unsigned char>(s[i]);
        if (b >= 0x80) {
            size_t bad = 0;
            const size_t len = json_detail::utf8_sequence(s, i, &bad);
            if (len == 0) {
                out.append("\xEF\xBF\xBD");
                i += bad;
            } else {
                out.append(s.data() + i, len);
                i += len;
            }
            continue;
        }
        switch (b) {
            case '"':
                out.append("\\\"");
                break;
            case '\\':
                out.append("\\\\");
                break;
            case '\b':
                out.append("\\b");
                break;
            case '\f':
                out.append("\\f");
                break;
            case '\n':
                out.append("\\n");
                break;
            case '\r':
                out.append("\\r");
                break;
            case '\t':
                out.append("\\t");
                break;
            default:
                if (b < 0x20) {
                    const char esc[] = {'\\', 'u', '0', '0', kHex[b >> 4], kHex[b & 0xF]};
                    out.append(esc, sizeof(esc));
                } else {
                    out.push_back(static_cast<char>(b));
                }
        }
        ++i;
    }
    out.push_back('"');
}

}  // namespace wisp
