#pragma once

// ReasoningKeep: the reasoning of the running turn's replies, kept so that
// the turn's later requests can echo it under LlmReasoning::Echo (spec 3.3).
// It never reaches the store and ends with the turn. Internal to
// wisp_agent; wisp.agent only.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "wisp/agent/context.hpp"
#include "wisp/agent/limits.hpp"
#include "wisp/os/large.hpp"

namespace wisp::agent {

class ReasoningKeep {
public:
    explicit ReasoningKeep(size_t cap = kReasoningKeepBytes) : cap_(cap) {}

    // Ignored for seq 0 or empty text; a text larger than the cap alone is
    // not kept. Then the oldest entries go until the total is within the
    // cap (spec decision 3).
    void add(uint64_t seq, std::string_view text) {
        if (seq == 0 || text.empty() || text.size() > cap_) return;
        const auto at = std::upper_bound(
            items_.begin(), items_.end(), seq,
            [](uint64_t s, const std::pair<uint64_t, os::LargeString>& e) { return s < e.first; });
        items_.emplace(at, seq, os::LargeString(text.data(), text.size()));
        bytes_ += text.size();
        size_t drop = 0;
        while (bytes_ > cap_) bytes_ -= items_[drop++].second.size();
        items_.erase(items_.begin(), items_.begin() + static_cast<std::ptrdiff_t>(drop));
        // Moving a short string moves its bytes: the views are rebuilt
        // after every change.
        refs_.clear();
        refs_.reserve(items_.size());
        for (const auto& [s, t] : items_) refs_.push_back({.seq = s, .text = {t.data(), t.size()}});
    }

    // Ascending seq; valid until the next add().
    std::span<const ReasoningRef> refs() const { return {refs_.data(), refs_.size()}; }
    size_t bytes() const { return bytes_; }

private:
    os::LargeVector<std::pair<uint64_t, os::LargeString>> items_;
    os::LargeVector<ReasoningRef> refs_;
    size_t cap_;
    size_t bytes_ = 0;
};

}  // namespace wisp::agent
