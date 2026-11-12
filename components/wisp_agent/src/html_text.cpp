#include "wisp/agent/html_text.hpp"

#include <array>
#include <cstdint>

namespace wisp::agent {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

bool starts_with_ci(std::string_view s, size_t at, std::string_view prefix) {
    if (at + prefix.size() > s.size()) return false;
    return iequals(s.substr(at, prefix.size()), prefix);
}

void append_utf8(os::LargeString& out, uint32_t cp) {
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

void append_replacement(os::LargeString& out) { out.append("\xEF\xBF\xBD", 3); }

bool is_surrogate(uint32_t cp) { return cp >= 0xD800 && cp <= 0xDFFF; }

// Length of the well-formed UTF-8 sequence starting at html[i], or 0 when
// there is none (an invalid lead byte, a truncated or malformed
// continuation, or a sequence past U+10FFFF).
size_t utf8_len(std::string_view html, size_t i) {
    const auto b0 = static_cast<unsigned char>(html[i]);
    size_t n;
    uint32_t min_cp;
    if (b0 < 0x80) return 1;
    if ((b0 & 0xE0) == 0xC0) {
        n = 2;
        min_cp = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        n = 3;
        min_cp = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        n = 4;
        min_cp = 0x10000;
    } else {
        return 0;
    }
    if (i + n > html.size()) return 0;
    uint32_t cp = b0 & (0x7Fu >> n);
    for (size_t k = 1; k < n; ++k) {
        const auto b = static_cast<unsigned char>(html[i + k]);
        if ((b & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < min_cp || cp > 0x10FFFF || is_surrogate(cp)) return 0;
    return n;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    const char lc = lower(c);
    if (lc >= 'a' && lc <= 'f') return lc - 'a' + 10;
    return -1;
}

// `html[i]` is just past the "&". Tries to parse and decode an entity;
// on success appends the decoded character(s) to `raw` and advances `i`
// past the closing ';'. On failure (unrecognized or unterminated), leaves
// `i` as it was and the caller emits the "&" literally.
bool decode_entity(std::string_view html, size_t& i, os::LargeString& raw) {
    struct Named {
        std::string_view name;
        uint32_t cp;
    };
    static constexpr Named kNamed[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
    };
    size_t j = i;
    if (j < html.size() && html[j] == '#') {
        ++j;
        bool hex = j < html.size() && (html[j] == 'x' || html[j] == 'X');
        if (hex) ++j;
        const size_t digits_start = j;
        uint64_t cp = 0;
        bool overflow = false;
        while (j < html.size() &&
               (hex ? hex_digit(html[j]) >= 0 : (html[j] >= '0' && html[j] <= '9'))) {
            const int d = hex ? hex_digit(html[j]) : html[j] - '0';
            cp = cp * (hex ? 16 : 10) + static_cast<uint64_t>(d);
            if (cp > 0x10FFFF) overflow = true;
            ++j;
        }
        if (j == digits_start || j >= html.size() || html[j] != ';') return false;
        ++j;
        if (overflow || cp > 0x10FFFF || is_surrogate(static_cast<uint32_t>(cp))) {
            append_replacement(raw);
        } else {
            append_utf8(raw, static_cast<uint32_t>(cp));
        }
        i = j;
        return true;
    }
    // Named: letters only, up to the longest name above.
    size_t k = j;
    while (k < html.size() && k - j < 8 &&
           ((html[k] >= 'a' && html[k] <= 'z') || (html[k] >= 'A' && html[k] <= 'Z'))) {
        ++k;
    }
    if (k >= html.size() || html[k] != ';' || k == j) return false;
    const std::string_view name = html.substr(j, k - j);
    for (const Named& e : kNamed) {
        if (iequals(name, e.name)) {
            append_utf8(raw, e.cp);
            i = k + 1;
            return true;
        }
    }
    return false;
}

bool is_block_tag(std::string_view name) {
    static constexpr std::string_view kBlock[] = {"p",  "div", "br", "li", "h1", "h2",
                                                  "h3", "h4",  "h5", "h6", "tr"};
    for (std::string_view b : kBlock) {
        if (iequals(name, b)) return true;
    }
    return false;
}

bool is_raw_text_tag(std::string_view name) {
    return iequals(name, "script") || iequals(name, "style") || iequals(name, "noscript");
}

// `html[i]` is the '<' of a (non-comment) tag. Returns the tag's name,
// lowercased, and whether it is a closing tag; `i` ends just past the
// tag's name (and any "/t"), not necessarily at '>' (attributes may
// follow).
std::string_view tag_name(std::string_view html, size_t& i, bool* closing) {
    size_t j = i + 1;
    *closing = j < html.size() && html[j] == '/';
    if (*closing) ++j;
    const size_t start = j;
    while (j < html.size() &&
           ((html[j] >= 'a' && html[j] <= 'z') || (html[j] >= 'A' && html[j] <= 'Z') ||
            (html[j] >= '0' && html[j] <= '9'))) {
        ++j;
    }
    i = j;
    return html.substr(start, j - start);
}

// Builds the raw text, with block tags left as '\n' and entities decoded,
// but whitespace not yet collapsed.
os::LargeString extract(std::string_view html) {
    os::LargeString raw;
    size_t i = 0;
    while (i < html.size()) {
        const char c = html[i];
        // "<" only opens a tag/comment when followed by a letter, '/' or
        // '!' (spec 3.1 amended); otherwise ("1 < 2", a trailing "<") it
        // is plain text.
        const char next = i + 1 < html.size() ? html[i + 1] : '\0';
        const bool tag_start = next == '!' || next == '/' || (next >= 'a' && next <= 'z') ||
                               (next >= 'A' && next <= 'Z');
        if (c == '<' && tag_start) {
            if (starts_with_ci(html, i, "<!--")) {
                const size_t end = html.find("-->", i + 4);
                i = end == std::string_view::npos ? html.size() : end + 3;
                continue;
            }
            size_t name_end = i;
            bool closing = false;
            const std::string_view name = tag_name(html, name_end, &closing);
            if (!closing && is_raw_text_tag(name)) {
                const size_t gt = html.find('>', name_end);
                if (gt == std::string_view::npos) {
                    i = html.size();
                    continue;
                }
                const std::string close_tag = "</" + std::string(name);
                const size_t close_at = [&] {
                    size_t from = gt + 1;
                    while (from < html.size()) {
                        if (starts_with_ci(html, from, close_tag)) return from;
                        ++from;
                    }
                    return std::string_view::npos;
                }();
                if (close_at == std::string_view::npos) {
                    i = html.size();
                    continue;
                }
                const size_t close_gt = html.find('>', close_at);
                i = close_gt == std::string_view::npos ? html.size() : close_gt + 1;
                continue;
            }
            const size_t gt = html.find('>', i + 1);
            if (gt == std::string_view::npos) {
                i = html.size();
                continue;
            }
            if (is_block_tag(name)) raw.push_back('\n');
            i = gt + 1;
            continue;
        }
        if (c == '&') {
            size_t after = i + 1;
            if (decode_entity(html, after, raw)) {
                i = after;
                continue;
            }
            raw.push_back('&');
            ++i;
            continue;
        }
        const size_t n = utf8_len(html, i);
        if (n == 0) {
            append_replacement(raw);
            ++i;
            continue;
        }
        raw.append(html.data() + i, n);
        i += n;
    }
    return raw;
}

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

os::LargeString collapse(const os::LargeString& raw) {
    os::LargeString out;
    size_t i = 0;
    while (i < raw.size()) {
        if (is_ws(raw[i])) {
            // A run with at least one blank line (two or more newlines)
            // collapses to exactly one blank line, keeping the paragraph
            // break; a run with a single line break stays a single "\n";
            // a run with none is inline whitespace, one space (spec 3.1
            // amended).
            size_t newlines = 0;
            size_t j = i;
            while (j < raw.size() && is_ws(raw[j])) {
                if (raw[j] == '\n') ++newlines;
                ++j;
            }
            if (newlines == 0) {
                out.push_back(' ');
            } else if (newlines == 1) {
                out.push_back('\n');
            } else {
                out.push_back('\n');
                out.push_back('\n');
            }
            i = j;
        } else {
            out.push_back(raw[i]);
            ++i;
        }
    }
    size_t begin = 0;
    while (begin < out.size() && is_ws(out[begin])) ++begin;
    size_t end = out.size();
    while (end > begin && is_ws(out[end - 1])) --end;
    return os::LargeString(out.data() + begin, end - begin);
}

}  // namespace

os::LargeString html_to_text(std::string_view html) { return collapse(extract(html)); }

}  // namespace wisp::agent
