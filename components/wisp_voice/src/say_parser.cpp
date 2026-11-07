#include "wisp/voice/say_parser.hpp"

#include <algorithm>
#include <utility>

#include "text.hpp"
#include "wisp/voice/limits.hpp"

namespace wisp::voice {

namespace {

constexpr std::string_view kOpen = "<say>";
constexpr std::string_view kClose = "</say>";

// The fallback sentence never needs more than the spoken budget; UTF-8
// takes at most 4 bytes per code point.
constexpr size_t kBodyHeadMax = kTtsMaxChars * 4;

// The longest suffix of `s` that is a proper prefix of `tag`.
size_t partial_tag(std::string_view s, std::string_view tag) {
    for (size_t n = std::min(s.size(), tag.size() - 1); n > 0; --n) {
        if (s.substr(s.size() - n) == tag.substr(0, n)) return n;
    }
    return 0;
}

// Whether `s` already holds a complete non-blank sentence.
bool has_sentence(std::string_view s) {
    bool content = false;
    for (size_t i = 0; i < s.size();) {
        const size_t n = std::min(detail::utf8_len(static_cast<unsigned char>(s[i])), s.size() - i);
        const std::string_view cp = s.substr(i, n);
        if (detail::is_terminator(cp) && content) return true;
        if (!(n == 1 && detail::is_space(cp[0]))) content = true;
        i += n;
    }
    return false;
}

}  // namespace

SayParser::SayParser(Out out) : out_(std::move(out)) {}

void SayParser::emit(std::string_view text) {
    if (text.empty()) return;
    if (inside_) {
        if (out_.say) out_.say(text);
        return;
    }
    if (body_head_.size() < kBodyHeadMax && !has_sentence(body_head_)) {
        body_head_.append(text.substr(0, kBodyHeadMax - body_head_.size()));
    }
    if (out_.body) out_.body(text);
}

void SayParser::feed(std::string_view delta) {
    pending_.append(delta);
    for (;;) {
        const std::string_view tag = inside_ ? kClose : kOpen;
        const std::string_view other = inside_ ? kOpen : kClose;
        const size_t i = pending_.find(tag);
        const size_t j = pending_.find(other);
        if (j != std::string::npos && (i == std::string::npos || j < i)) {
            // A stray close outside a say, or a nested open inside one.
            emit(std::string_view(pending_).substr(0, j));
            pending_.erase(0, j + other.size());
            continue;
        }
        if (i != std::string::npos) {
            emit(std::string_view(pending_).substr(0, i));
            pending_.erase(0, i + tag.size());
            inside_ = !inside_;
            if (inside_) saw_say_ = true;
            continue;
        }
        const size_t keep = std::max(partial_tag(pending_, kOpen), partial_tag(pending_, kClose));
        emit(std::string_view(pending_).substr(0, pending_.size() - keep));
        pending_.erase(0, pending_.size() - keep);
        break;
    }
}

std::string SayParser::finish() {
    emit(pending_);
    pending_.clear();
    if (saw_say_) return {};
    return first_sentence(body_head_);
}

void SayParser::reset() {
    pending_.clear();
    body_head_.clear();
    inside_ = false;
    saw_say_ = false;
}

SaySplit split_say(std::string_view whole) {
    SaySplit split;
    SayParser parser({[&split](std::string_view d) { split.say.append(d); },
                      [&split](std::string_view d) { split.body.append(d); }});
    parser.feed(whole);
    parser.finish();
    return split;
}

std::string first_sentence(std::string_view text) {
    size_t start = 0;
    for (size_t i = 0; i < text.size();) {
        const size_t n =
            std::min(detail::utf8_len(static_cast<unsigned char>(text[i])), text.size() - i);
        if (n < detail::utf8_len(static_cast<unsigned char>(text[i]))) break;  // truncated tail
        i += n;
        if (detail::is_terminator(text.substr(i - n, n))) {
            const std::string_view sentence = detail::trim(text.substr(start, i - start));
            if (!sentence.empty()) return std::string(sentence);
            start = i;
        }
    }
    // No terminator: the rest, minus an incomplete trailing sequence.
    std::string_view rest = text.substr(start);
    size_t end = 0;
    for (size_t i = 0; i < rest.size();) {
        const size_t n = detail::utf8_len(static_cast<unsigned char>(rest[i]));
        if (i + n > rest.size()) break;
        i += n;
        end = i;
    }
    return std::string(detail::trim(rest.substr(0, end)));
}

}  // namespace wisp::voice
