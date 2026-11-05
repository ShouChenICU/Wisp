#include "wisp/scene/minute_ticker.hpp"

namespace wisp::scene {

namespace {

constexpr uint64_t kMinuteMs = 60000;

std::optional<int64_t> valid(std::optional<int64_t> wall_s) {
    if (wall_s && *wall_s >= MinuteTicker::kValidSince) return wall_s;
    return std::nullopt;
}

}  // namespace

bool MinuteTicker::poll(std::optional<int64_t> wall_s, uint64_t mono_ms) {
    const auto wall = valid(wall_s);
    bool fire = forced_;
    forced_ = false;
    if (wall) {
        const int64_t minute = *wall / 60;
        // None: the first look. Mono: the clock was just set.
        if (mode_ == Mode::Mono || (mode_ == Mode::Wall && minute != minute_)) fire = true;
        mode_ = Mode::Wall;
        minute_ = minute;
    } else if (mode_ != Mode::Mono) {
        mode_ = Mode::Mono;
        mono_due_ = mono_ms + kMinuteMs;
    } else if (mono_ms >= mono_due_) {
        fire = true;
        mono_due_ = mono_ms + kMinuteMs;
    }
    if (fire && mode_ == Mode::Mono) mono_due_ = mono_ms + kMinuteMs;
    if (fire) fired_minute_ = mode_ == Mode::Wall ? std::optional<int64_t>(minute_) : std::nullopt;
    return fire;
}

bool MinuteTicker::sync(std::optional<int64_t> wall_s, uint64_t mono_ms) {
    const auto wall = valid(wall_s);
    if (!(wall && fired_minute_ == *wall / 60)) forced_ = true;
    return poll(wall_s, mono_ms);
}

uint64_t MinuteTicker::next_poll_ms(std::optional<int64_t> wall_s, uint64_t mono_ms) const {
    if (forced_) return 0;
    const auto wall = valid(wall_s);
    if (wall) {
        if (mode_ != Mode::Wall || *wall / 60 != minute_) return 0;
        return static_cast<uint64_t>(60 - *wall % 60) * 1000;
    }
    if (mode_ != Mode::Mono) return 0;
    return mono_due_ > mono_ms ? mono_due_ - mono_ms : 0;
}

}  // namespace wisp::scene
