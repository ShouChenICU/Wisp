#pragma once

// Test-only checks for text that reaches a browser: a WebSocket text frame
// with invalid UTF-8 makes the browser drop the connection (RFC 6455), and
// JSON.parse() rejects raw control characters inside strings.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wisp::test {

// Strict UTF-8 (RFC 3629): no overlong forms, no surrogates, nothing past
// U+10FFFF, no truncated sequences.
inline bool is_valid_utf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto b = static_cast<uint8_t>(s[i]);
        size_t len = 0;
        uint8_t lo = 0x80;
        uint8_t hi = 0xBF;
        if (b < 0x80) {
            ++i;
            continue;
        } else if (b >= 0xC2 && b <= 0xDF) {
            len = 2;
        } else if (b >= 0xE0 && b <= 0xEF) {
            len = 3;
            if (b == 0xE0) lo = 0xA0;
            if (b == 0xED) hi = 0x9F;
        } else if (b >= 0xF0 && b <= 0xF4) {
            len = 4;
            if (b == 0xF0) lo = 0x90;
            if (b == 0xF4) hi = 0x8F;
        } else {
            return false;
        }
        if (i + len > s.size()) return false;
        for (size_t k = 1; k < len; ++k) {
            const auto c = static_cast<uint8_t>(s[i + k]);
            const uint8_t min = k == 1 ? lo : 0x80;
            const uint8_t max = k == 1 ? hi : 0xBF;
            if (c < min || c > max) return false;
        }
        i += len;
    }
    return true;
}

inline bool has_control_bytes(std::string_view s) {
    for (char c : s) {
        if (static_cast<uint8_t>(c) < 0x20) return true;
    }
    return false;
}

}  // namespace wisp::test
