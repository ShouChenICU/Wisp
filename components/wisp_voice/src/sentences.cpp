#include "wisp/voice/sentences.hpp"

#include <utility>

#include "text.hpp"

namespace wisp::voice {

SentenceSplitter::SentenceSplitter(size_t max_chars) : max_chars_(max_chars) {}

void SentenceSplitter::emit(const std::function<void(std::string)>& sentence) {
    if (!detail::is_blank(pending_)) {
        used_ += pending_chars_;
        sentence(std::move(pending_));
    }
    pending_.clear();
    pending_chars_ = 0;
}

void SentenceSplitter::feed(std::string_view say_delta,
                            const std::function<void(std::string)>& sentence) {
    for (char c : say_delta) {
        if (capped_) return;
        partial_.push_back(c);
        if (partial_.size() < detail::utf8_len(static_cast<unsigned char>(partial_[0]))) continue;
        if (used_ >= max_chars_) {
            capped_ = true;
            partial_.clear();
            return;
        }
        pending_.append(partial_);
        ++pending_chars_;
        const bool ends = detail::is_terminator(partial_);
        partial_.clear();
        if (ends || used_ + pending_chars_ >= max_chars_) emit(sentence);
    }
}

void SentenceSplitter::flush(const std::function<void(std::string)>& sentence) {
    partial_.clear();
    if (!capped_) emit(sentence);
}

}  // namespace wisp::voice
