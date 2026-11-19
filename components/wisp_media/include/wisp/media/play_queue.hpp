#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/os/large.hpp"

namespace wisp::media {

enum class PlayMode : uint8_t { Sequence, RepeatAll, RepeatOne, Shuffle };

const char* to_string(PlayMode mode);
std::optional<PlayMode> play_mode_from(std::string_view name);

inline constexpr size_t kMaxQueue = 1000;
// prev() restarts the current item when the position is past this.
inline constexpr uint32_t kPrevRestartS = 3;

// The play queue of the music player (media spec 4.2): which item plays
// next in each mode. Pure and single-threaded; the player owns it under its
// own lock. Items live in Mem::Large because up to kMaxQueue paths would
// otherwise land in internal RAM.
class PlayQueue {
public:
    struct SetResult {
        bool truncated = false;
    };

    // Replaces the queue, keeping the mode. Items beyond kMaxQueue are
    // dropped. `start` past the end falls back to the first item.
    SetResult set(std::vector<std::string> items, size_t start);

    // `rand` supplies the shuffle permutation; without one a shuffle keeps
    // queue order. Entering Shuffle makes the current item the first of a
    // new permutation; leaving it keeps the current item and continues in
    // queue order from it.
    void set_mode(PlayMode mode, std::function<uint32_t()> rand);
    PlayMode mode() const { return mode_; }

    std::optional<size_t> current() const { return cur_; }
    // Valid for i < size(). The view holds until the queue changes.
    std::string_view item(size_t i) const { return {items_[i].data(), items_[i].size()}; }
    size_t size() const { return items_.size(); }

    // The current track ended. RepeatOne gives the same index; Sequence
    // gives nullopt after the last item (the queue is left as it is; the
    // player clears it and goes idle).
    std::optional<size_t> advance_auto();
    // User skip: by queue order in every mode, so RepeatOne wraps like
    // RepeatAll at the ends. Sequence gives nullopt after the last item.
    std::optional<size_t> next();
    // Past kPrevRestartS the current index again (restart); otherwise the
    // previous item. Sequence stays at the first item; the other modes wrap.
    std::optional<size_t> prev(uint32_t position_s);

    // Scene uninstall: removes items starting with `prefix`; returns how many.
    // A removed current item hands over to the next remaining one (in
    // Shuffle the next of the permutation). When nothing follows in Sequence
    // mode, current() becomes empty while items remain, which the player
    // treats as the end of the queue.
    size_t remove_prefix(std::string_view prefix);

    void clear();

private:
    std::optional<size_t> step_by_order(bool forward, bool wrap);
    void draw_perm(std::optional<size_t> first, std::optional<size_t> avoid);
    uint32_t draw(uint32_t bound);
    std::optional<size_t> shuffle_step(bool forward);

    os::LargeVector<os::LargeString> items_;
    std::optional<size_t> cur_;
    PlayMode mode_ = PlayMode::Sequence;
    std::function<uint32_t()> rand_;
    os::LargeVector<uint32_t> perm_;
    size_t perm_pos_ = 0;
};

}  // namespace wisp::media
