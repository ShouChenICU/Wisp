#include "wisp/media/play_queue.hpp"

#include <algorithm>
#include <utility>

namespace wisp::media {

const char* to_string(PlayMode mode) {
    switch (mode) {
        case PlayMode::Sequence:
            return "sequence";
        case PlayMode::RepeatAll:
            return "repeat_all";
        case PlayMode::RepeatOne:
            return "repeat_one";
        case PlayMode::Shuffle:
            return "shuffle";
    }
    return "sequence";
}

std::optional<PlayMode> play_mode_from(std::string_view name) {
    for (PlayMode m :
         {PlayMode::Sequence, PlayMode::RepeatAll, PlayMode::RepeatOne, PlayMode::Shuffle}) {
        if (name == to_string(m)) {
            return m;
        }
    }
    return std::nullopt;
}

uint32_t PlayQueue::draw(uint32_t bound) { return rand_ ? rand_() % bound : 0; }

// A Fisher-Yates permutation of the queue indices. `first` is pinned to the
// front (the item playing now counts as already played this cycle); `avoid`
// is kept off the front when there is a choice (the last item of the
// previous cycle must not play twice in a row).
void PlayQueue::draw_perm(std::optional<size_t> first, std::optional<size_t> avoid) {
    const size_t n = items_.size();
    perm_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        perm_[i] = static_cast<uint32_t>(i);
    }
    perm_pos_ = 0;
    if (n == 0) {
        return;
    }
    size_t lo = 0;
    if (first) {
        std::swap(perm_[0], perm_[*first]);
        lo = 1;
    }
    if (rand_) {
        for (size_t i = n; i > lo + 1; --i) {
            std::swap(perm_[i - 1], perm_[lo + draw(static_cast<uint32_t>(i - lo))]);
        }
    }
    if (!first && avoid && n > 1 && perm_[0] == *avoid) {
        std::swap(perm_[0], perm_[1]);
    }
}

PlayQueue::SetResult PlayQueue::set(std::vector<std::string> items, size_t start) {
    SetResult r;
    r.truncated = items.size() > kMaxQueue;
    const size_t n = std::min(items.size(), kMaxQueue);
    items_.clear();
    items_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        items_.emplace_back(items[i].data(), items[i].size());
    }
    if (n == 0) {
        cur_.reset();
        perm_.clear();
        perm_pos_ = 0;
        return r;
    }
    cur_ = start < n ? start : 0;
    if (mode_ == PlayMode::Shuffle) {
        draw_perm(cur_, std::nullopt);
    }
    return r;
}

void PlayQueue::set_mode(PlayMode mode, std::function<uint32_t()> rand) {
    const bool entering = mode == PlayMode::Shuffle && mode_ != PlayMode::Shuffle;
    mode_ = mode;
    rand_ = std::move(rand);
    if (mode != PlayMode::Shuffle) {
        perm_.clear();
        perm_pos_ = 0;
    } else if (entering || perm_.size() != items_.size()) {
        draw_perm(cur_, std::nullopt);
    }
}

std::optional<size_t> PlayQueue::step_by_order(bool forward, bool wrap) {
    const size_t n = items_.size();
    if (n == 0 || !cur_) {
        return std::nullopt;
    }
    size_t i = *cur_;
    if (forward) {
        if (i + 1 < n) {
            ++i;
        } else if (wrap) {
            i = 0;
        } else {
            return std::nullopt;
        }
    } else if (i > 0) {
        --i;
    } else if (wrap) {
        i = n - 1;
    } else {
        return cur_;  // Sequence stays at the first item
    }
    cur_ = i;
    return cur_;
}

std::optional<size_t> PlayQueue::shuffle_step(bool forward) {
    const size_t n = perm_.size();
    if (n == 0 || !cur_) {
        return std::nullopt;
    }
    if (forward) {
        if (perm_pos_ + 1 < n) {
            ++perm_pos_;
        } else {
            draw_perm(std::nullopt, *cur_);
        }
    } else {
        perm_pos_ = perm_pos_ > 0 ? perm_pos_ - 1 : n - 1;
    }
    cur_ = perm_[perm_pos_];
    return cur_;
}

std::optional<size_t> PlayQueue::advance_auto() {
    if (mode_ == PlayMode::RepeatOne) {
        return cur_;
    }
    return next();
}

std::optional<size_t> PlayQueue::next() {
    if (mode_ == PlayMode::Shuffle) {
        return shuffle_step(true);
    }
    return step_by_order(true, mode_ != PlayMode::Sequence);
}

std::optional<size_t> PlayQueue::prev(uint32_t position_s) {
    if (!cur_) {
        return std::nullopt;
    }
    if (position_s > kPrevRestartS) {
        return cur_;
    }
    if (mode_ == PlayMode::Shuffle) {
        return shuffle_step(false);
    }
    return step_by_order(false, mode_ != PlayMode::Sequence);
}

size_t PlayQueue::remove_prefix(std::string_view prefix) {
    const size_t n = items_.size();
    os::LargeVector<int64_t> remap(n, -1);  // old index -> new index
    os::LargeVector<os::LargeString> kept;
    kept.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        std::string_view s{items_[i].data(), items_[i].size()};
        if (s.substr(0, prefix.size()) != prefix) {
            remap[i] = static_cast<int64_t>(kept.size());
            kept.push_back(std::move(items_[i]));
        }
    }
    const size_t removed = n - kept.size();
    if (removed == 0) {
        return 0;
    }
    const bool cur_removed = cur_ && remap[*cur_] < 0;
    const size_t old_cur = cur_.value_or(0);
    items_ = std::move(kept);
    if (items_.empty()) {
        cur_.reset();
        perm_.clear();
        perm_pos_ = 0;
        return removed;
    }
    // A queue that had already run out stays so: a shuffle must not pick a
    // new current item out of the remaining ones.
    if (!cur_) {
        perm_.clear();
        perm_pos_ = 0;
        return removed;
    }
    if (mode_ == PlayMode::Shuffle) {
        os::LargeVector<uint32_t> np;
        np.reserve(items_.size());
        size_t new_pos = 0;
        for (size_t p = 0; p < perm_.size(); ++p) {
            if (p == perm_pos_) {
                new_pos = np.size();  // the current, or its successor if removed
            }
            if (remap[perm_[p]] >= 0) {
                np.push_back(static_cast<uint32_t>(remap[perm_[p]]));
            }
        }
        if (perm_pos_ >= perm_.size()) {
            new_pos = np.size();
        }
        perm_ = std::move(np);
        perm_pos_ = new_pos;
        if (cur_ && !cur_removed) {
            cur_ = static_cast<size_t>(remap[*cur_]);
        } else if (perm_pos_ < perm_.size()) {
            cur_ = perm_[perm_pos_];
        } else {
            draw_perm(std::nullopt, std::nullopt);
            cur_ = perm_[0];
        }
        return removed;
    }
    if (!cur_removed) {
        cur_ = static_cast<size_t>(remap[*cur_]);
        return removed;
    }
    // First kept item after the removed current one.
    size_t next_kept = items_.size();
    for (size_t i = old_cur + 1; i < n; ++i) {
        if (remap[i] >= 0) {
            next_kept = static_cast<size_t>(remap[i]);
            break;
        }
    }
    if (next_kept < items_.size()) {
        cur_ = next_kept;
    } else if (mode_ == PlayMode::Sequence) {
        cur_.reset();
    } else {
        cur_ = 0;
    }
    return removed;
}

void PlayQueue::clear() {
    items_.clear();
    perm_.clear();
    perm_pos_ = 0;
    cur_.reset();
}

}  // namespace wisp::media
