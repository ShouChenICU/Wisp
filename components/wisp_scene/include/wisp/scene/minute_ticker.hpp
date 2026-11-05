#pragma once

// MinuteTicker: when wisp.device's "minute" event fires (scene-runtime-core
// spec 3.4, 5.2). The scene task polls it with the wall clock (seconds
// since the epoch, nullopt or anything before kValidSince when the RTC is
// not set) and its monotonic clock.
//
// - Set clock: fires when the wall minute differs from the one it last
//   saw, so once per minute, once for a jump of any size either way, and
//   after a jump back the next one is at the new time's next minute.
// - Unset clock: fires every 60 s of the monotonic clock instead.
// - The clock becoming set fires at once (the time is known now), and
//   whole minutes follow; becoming unset again starts the 60 s count.
// - The first poll only takes its bearings; force() (screen on) makes the
//   next poll fire, once, whatever the clock says.
// - sync() (a network time sync set the clock) fires once for the minute
//   the clock now reads, unless that minute already fired: a poll between
//   the clock moving and the sync reaching the scene task may have seen
//   the jump first.

#include <cstdint>
#include <optional>

namespace wisp::scene {

class MinuteTicker {
public:
    static constexpr int64_t kValidSince = 1704067200;  // 2024-01-01T00:00:00Z

    // True: fire the minute event now.
    bool poll(std::optional<int64_t> wall_s, uint64_t mono_ms);
    // How long the task may sleep before polling again (0: now).
    uint64_t next_poll_ms(std::optional<int64_t> wall_s, uint64_t mono_ms) const;
    void force() { forced_ = true; }
    // The wall clock was set: a poll that fires now unless the wall minute
    // of `wall_s` is the one the last fire was for. True: fire now.
    bool sync(std::optional<int64_t> wall_s, uint64_t mono_ms);

private:
    enum class Mode : uint8_t { None, Wall, Mono };

    Mode mode_ = Mode::None;
    int64_t minute_ = 0;     // Wall: the minute last seen
    uint64_t mono_due_ = 0;  // Mono: when the next one fires
    bool forced_ = false;
    // The wall minute the last fire was for; none after a monotonic fire.
    std::optional<int64_t> fired_minute_;
};

}  // namespace wisp::scene
