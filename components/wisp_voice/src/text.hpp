#pragma once

// UTF-8 helpers shared by the say parser and the sentence splitter.

#include <cstddef>
#include <string_view>

namespace wisp::voice::detail {

// The byte length a UTF-8 sequence starting with `lead` has (1 for a
// stray continuation byte, so malformed text still advances).
inline size_t utf8_len(unsigned char lead) {
    if (lead >= 0xF0) return 4;
    if (lead >= 0xE0) return 3;
    if (lead >= 0xC0) return 2;
    return 1;
}

// One code point (as its UTF-8 bytes) ends a sentence: "。！？!?；;" or a
// newline.
inline bool is_terminator(std::string_view cp) {
    if (cp.size() == 1) {
        const char c = cp[0];
        return c == '!' || c == '?' || c == ';' || c == '\n';
    }
    return cp == "。" || cp == "！" || cp == "？" || cp == "；";
}

inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

inline bool is_blank(std::string_view s) {
    for (char c : s) {
        if (!is_space(c)) return false;
    }
    return true;
}

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

}  // namespace wisp::voice::detail
