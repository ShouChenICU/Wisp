#include "wisp/shell/body_text.hpp"

#include <utility>

namespace wisp::shell {

namespace {

constexpr std::string_view kFence = "```";
constexpr std::string_view kBullet = "\xE2\x80\xA2 ";  // "• "

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

}  // namespace

os::LargeVector<BodyBlock> light_markdown(std::string_view body) {
    os::LargeVector<BodyBlock> out;
    BodyBlock cur;
    bool started = false;  // cur has a line (an empty one counts)
    auto flush = [&] {
        if (started && (cur.code || !blank(cur.text))) out.push_back(std::move(cur));
        cur = BodyBlock{};
        started = false;
    };
    bool code = false;
    while (true) {
        const size_t nl = body.find('\n');
        std::string_view line = body.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (trim_left(line).starts_with(kFence)) {
            flush();
            code = !code;
            cur.code = code;
        } else {
            if (started) cur.text.push_back('\n');
            if (code) {
                cur.text.append(line.data(), line.size());
            } else {
                append_text_line(cur.text, line);
            }
            started = true;
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

}  // namespace wisp::shell
