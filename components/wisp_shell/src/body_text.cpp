#include "wisp/shell/body_text.hpp"

#include <utility>

#include "wisp/ui/strings.hpp"

namespace wisp::shell {

namespace {

constexpr std::string_view kFence = "```";
constexpr std::string_view kBullet = "\xE2\x80\xA2 ";   // "• "
constexpr std::string_view kEllipsis = "\xE2\x80\xA6";  // "…"

bool blank(std::string_view s) { return s.find_first_not_of(" \t\r\n") == std::string_view::npos; }

std::string_view trim_left(std::string_view s) {
    const size_t i = s.find_first_not_of(" \t");
    return i == std::string_view::npos ? std::string_view{} : s.substr(i);
}

// Appends `line` with its list marker turned into a bullet and every "**"
// dropped.
void append_text_line(os::LargeString& out, std::string_view line) {
    const size_t indent = line.size() - trim_left(line).size();
    std::string_view rest = line.substr(indent);
    out.append(line.data(), indent);
    if (rest.starts_with("- ") || rest.starts_with("* ")) {
        out.append(kBullet.data(), kBullet.size());
        rest.remove_prefix(2);
    }
    for (size_t at = rest.find("**"); at != std::string_view::npos; at = rest.find("**")) {
        out.append(rest.data(), at);
        rest.remove_prefix(at + 2);
    }
    out.append(rest.data(), rest.size());
}

// The fence's tag, when it is short and plain enough to show.
std::string fence_lang(std::string_view fence_line) {
    std::string_view tag = trim_left(fence_line).substr(kFence.size());
    while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t')) tag.remove_suffix(1);
    tag = trim_left(tag);
    if (tag.empty() || tag.size() > kCodeLangMax) return {};
    for (char c : tag) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '+' || c == '#' || c == '.' || c == '_' ||
                        c == '-';
        if (!ok) return {};
    }
    return std::string(tag);
}

bool utf8_lead(char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }

// Appends `s` to `out` while `left` code points remain; false when `s`
// did not fit whole (it is then cut at a code point boundary).
bool append_capped(os::LargeString& out, std::string_view s, size_t& left) {
    size_t i = 0;
    for (; i < s.size(); ++i) {
        if (!utf8_lead(s[i])) continue;
        if (left == 0) break;
        --left;
    }
    out.append(s.data(), i);
    return i == s.size();
}

}  // namespace

os::LargeVector<BodyBlock> light_markdown(std::string_view body, size_t max_chars) {
    os::LargeVector<BodyBlock> out;
    BodyBlock cur;
    bool started = false;  // cur has a line (an empty one counts)
    auto flush = [&] {
        if (started && (cur.code || !blank(cur.text))) out.push_back(std::move(cur));
        cur = BodyBlock{};
        started = false;
    };
    bool code = false;
    size_t left = max_chars;
    os::LargeString line_out;
    while (true) {
        const size_t nl = body.find('\n');
        std::string_view line = body.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (trim_left(line).starts_with(kFence)) {
            flush();
            code = !code;
            cur.code = code;
            if (code) {
                cur.lang = fence_lang(line);
                started = true;
            }
        } else if (code) {
            // A line begun after the last newline counts; an empty one
            // there has not begun yet.
            if (nl != std::string_view::npos || !line.empty()) ++cur.lines;
        } else {
            line_out.clear();
            if (started) line_out.push_back('\n');
            append_text_line(line_out, line);
            started = true;
            if (!append_capped(cur.text, line_out, left)) {
                cur.text.append(kEllipsis.data(), kEllipsis.size());
                break;
            }
        }
        if (nl == std::string_view::npos) break;
        body.remove_prefix(nl + 1);
    }
    flush();
    // Trailing blank lines of a text block (before a fence, say) add
    // nothing but height.
    for (BodyBlock& b : out) {
        if (b.code) continue;
        while (!b.text.empty() && (b.text.back() == '\n' || b.text.back() == ' '))
            b.text.pop_back();
        size_t lead = 0;
        while (lead < b.text.size() && b.text[lead] == '\n') ++lead;
        b.text.erase(0, lead);
    }
    return out;
}

std::string code_fold_text(const BodyBlock& b) {
    const std::string n = std::to_string(b.lines);
    if (b.lang.empty()) return ui::tr_fill(ui::StrId::BodyCode, n);
    const std::string_view values[] = {b.lang, n};
    return ui::tr_fill_n(ui::StrId::BodyCodeLang, values);
}

}  // namespace wisp::shell
