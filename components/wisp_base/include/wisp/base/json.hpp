#pragma once

#include <cstddef>
#include <cstdint>
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

// Parses `s` as JSON into a Value. On a parse error, returns
// ErrorCode::InvalidArgument with ArduinoJson's error text in the message.
Result<Value> from_json(std::string_view s);

// The same, refusing input nested deeper than `max_depth` levels (the
// outermost array or object is level 1) with ErrorCode::QuotaExceeded.
// Parsing recurses once per level, so this bounds the stack it needs; the
// parser itself enforces it, so no string can hide a level from it.
Result<Value> from_json(std::string_view s, uint8_t max_depth);

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
