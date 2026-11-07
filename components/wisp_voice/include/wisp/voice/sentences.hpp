#pragma once

// SentenceSplitter (MVP spec 4.4): cuts the streamed say text into
// sentences for TTS, at "。！？!?；;" and newlines (terminator kept).
//
// At most max_chars code points are spoken per turn: the sentence that
// reaches the budget is emitted cut at it, and everything after is
// dropped (capped()). Blank sentences are skipped. Deltas may end in the
// middle of a UTF-8 sequence. One splitter per turn.

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include "wisp/voice/limits.hpp"

namespace wisp::voice {

class SentenceSplitter {
public:
    explicit SentenceSplitter(size_t max_chars = kTtsMaxChars);

    void feed(std::string_view say_delta, const std::function<void(std::string)>& sentence);
    void flush(const std::function<void(std::string)>& sentence);
    bool capped() const { return capped_; }

private:
    void emit(const std::function<void(std::string)>& sentence);

    size_t max_chars_;
    size_t used_ = 0;
    size_t pending_chars_ = 0;
    std::string pending_;
    std::string partial_;  // an incomplete UTF-8 sequence
    bool capped_ = false;
};

}  // namespace wisp::voice
