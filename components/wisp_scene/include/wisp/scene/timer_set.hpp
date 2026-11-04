#pragma once

// TimerSet: the deadlines of one scene's wisp.timer.after/every timers
// (scene-runtime-core spec 3.4, 5.1). Plain bookkeeping on a millisecond
// clock the caller passes in; the scene task owns it and runs the
// callbacks of the ids due() hands back.
//
// - At most kMaxTimers at once, none shorter than kMinIntervalMs.
// - due(now) returns the ids whose deadline has come, earliest first. An
//   after timer is gone once returned; an every timer moves on by its
//   period, keeping its phase, or to now + period when it fell more than
//   a period behind, so a late poll fires it once, not once per period
//   missed.
// - pause()/resume() (screen off/on): nothing is due while paused, and
//   next_due() is empty so the task does not wake up for it. Deadlines
//   are not shifted: on resume whatever passed meanwhile is due once (an
//   every timer then runs a period from now, even if it missed less than
//   a period), the rest keeps its time.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/os/large.hpp"

namespace wisp::scene {

class TimerSet {
public:
    static constexpr uint32_t kMinIntervalMs = 50;
    static constexpr size_t kMaxTimers = 32;

    // InvalidArgument below kMinIntervalMs, QuotaExceeded when
    // kMaxTimers are set. Ids are never 0.
    Result<uint32_t> after(uint32_t ms, uint64_t now);
    Result<uint32_t> every(uint32_t ms, uint64_t now);
    // False if `id` is not set (fired after timer, cancelled, unknown).
    bool cancel(uint32_t id);
    bool contains(uint32_t id) const;
    std::vector<uint32_t> due(uint64_t now);
    std::optional<uint64_t> next_due() const;
    void pause(uint64_t now);
    void resume(uint64_t now);
    bool paused() const { return paused_; }
    size_t size() const { return timers_.size(); }
    void clear() { timers_.clear(); }

private:
    struct Timer {
        uint32_t id;
        uint32_t period;  // 0: an after timer
        uint64_t due;
    };

    Result<uint32_t> add(uint32_t ms, uint64_t now, bool repeat);

    os::LargeVector<Timer> timers_;
    uint32_t next_id_ = 1;
    bool paused_ = false;
};

}  // namespace wisp::scene
