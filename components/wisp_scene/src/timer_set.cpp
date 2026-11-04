#include "wisp/scene/timer_set.hpp"

#include <algorithm>

namespace wisp::scene {

Result<uint32_t> TimerSet::after(uint32_t ms, uint64_t now) { return add(ms, now, false); }

Result<uint32_t> TimerSet::every(uint32_t ms, uint64_t now) { return add(ms, now, true); }

Result<uint32_t> TimerSet::add(uint32_t ms, uint64_t now, bool repeat) {
    if (ms < kMinIntervalMs)
        return Result<uint32_t>::err(ErrorCode::InvalidArgument, "interval_too_small");
    if (timers_.size() >= kMaxTimers)
        return Result<uint32_t>::err(ErrorCode::QuotaExceeded, "too_many_timers");
    // Ids are unique among at most kMaxTimers live ones even after the
    // counter wraps.
    while (next_id_ == 0 || contains(next_id_)) ++next_id_;
    const uint32_t id = next_id_++;
    if (timers_.capacity() == 0) timers_.reserve(kMaxTimers);
    timers_.push_back({id, repeat ? ms : 0, now + ms});
    return Result<uint32_t>::ok(id);
}

bool TimerSet::cancel(uint32_t id) {
    auto it =
        std::find_if(timers_.begin(), timers_.end(), [id](const Timer& t) { return t.id == id; });
    if (it == timers_.end()) return false;
    timers_.erase(it);
    return true;
}

bool TimerSet::contains(uint32_t id) const {
    return std::any_of(timers_.begin(), timers_.end(), [id](const Timer& t) { return t.id == id; });
}

std::vector<uint32_t> TimerSet::due(uint64_t now) {
    std::vector<uint32_t> out;
    if (paused_) return out;
    // (deadline, id) pairs of what is due, earliest first.
    std::vector<std::pair<uint64_t, uint32_t>> fired;
    for (auto& t : timers_) {
        if (t.due > now) continue;
        fired.emplace_back(t.due, t.id);
        if (t.period != 0) {
            t.due += t.period;
            if (t.due <= now) t.due = now + t.period;
        }
    }
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                 [now](const Timer& t) { return t.period == 0 && t.due <= now; }),
                  timers_.end());
    std::sort(fired.begin(), fired.end());
    out.reserve(fired.size());
    for (const auto& f : fired) out.push_back(f.second);
    return out;
}

std::optional<uint64_t> TimerSet::next_due() const {
    if (paused_ || timers_.empty()) return std::nullopt;
    uint64_t next = timers_.front().due;
    for (const auto& t : timers_) next = std::min(next, t.due);
    return next;
}

void TimerSet::pause(uint64_t) { paused_ = true; }

void TimerSet::resume(uint64_t) { paused_ = false; }

}  // namespace wisp::scene
