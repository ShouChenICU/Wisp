#pragma once

// String helpers shared inside wisp_agent.

#include <cstddef>
#include <string_view>

#include "wisp/os/large.hpp"

namespace wisp::agent {

// Bytes in the longest prefix of `s` of at most `max_bytes` bytes that
// does not split a character.
inline size_t prefix_bytes(std::string_view s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s.size();
    size_t n = max_bytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return n;
}

inline std::string_view sv(const os::LargeString& s) { return {s.data(), s.size()}; }

}  // namespace wisp::agent
