#pragma once

// SayParser (MVP spec 4.3): splits the streamed reply text into what is
// spoken (inside <say>...</say>) and what is only shown (the body).
//
// - Deltas may be cut anywhere, a tag included ("<sa" | "y>"): a suffix
//   that could still become a tag is held back until the next feed().
// - Tags are exact and case-sensitive; "<SAY>" is plain text.
// - A stray "</say>" outside a say and a nested "<say>" inside one are
//   dropped.
// - State spans every model request of one turn, so text between tool
//   calls can speak too; reset() at the start of each turn.
// - finish() flushes what is held back to the current side (an unclosed
//   say ends as say). When no <say> appeared this turn it returns the
//   body's first sentence outside fenced code (a block between lines
//   starting "```", fence lines included, is never spoken), for speaking
//   only; the body was already delivered unchanged.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace wisp::voice {

class SayParser {
public:
    struct Out {
        std::function<void(std::string_view)> say, body;
    };

    explicit SayParser(Out out);

    void feed(std::string_view delta);
    std::string finish();
    void reset();
    bool saw_say() const { return saw_say_; }

private:
    void emit(std::string_view text);
    // Adds the part of `text` outside fenced code to body_head_.
    void add_head(std::string_view text);

    Out out_;
    std::string pending_;
    // The start of this turn's body, kept only until it holds a complete
    // sentence (bounded): all finish() needs for its fallback.
    std::string body_head_;
    // The fence filter add_head() runs: the current line while it may
    // still turn out to be a fence (blanks and up to two backticks), what
    // the line turned out to be, and whether a fence is open.
    enum class HeadLine : uint8_t { Undecided, Fence, Text };
    std::string head_line_;
    HeadLine head_kind_ = HeadLine::Undecided;
    bool head_code_ = false;
    bool inside_ = false;
    bool saw_say_ = false;
};

struct SaySplit {
    std::string say, body;
};

// A whole reply through SayParser: the say and body text it streams.
SaySplit split_say(std::string_view whole);

// The first non-blank sentence (ending in one of "。！？!?；;" or a
// newline, terminator kept, surrounding whitespace trimmed), or the whole
// trimmed text when it has no terminator.
std::string first_sentence(std::string_view text);

}  // namespace wisp::voice
