#pragma once

// The result of the last time-sync attempt, as net::TimeSyncService keeps
// it in a state cell (spec 3.3 of the state-cells design). It lives in
// wisp_core, not beside the service, so that core components can follow
// it without depending on wisp_net.

#include <cstdint>

#include "wisp/base/fixed_string.hpp"

namespace wisp::core {

// Event::source of system.time: the service has always published it with
// an empty source.
inline constexpr char kTimeSyncSource[] = "";

struct TimeSyncResult {
    enum class Outcome : uint8_t { None, Synced, Failed };
    // None until the first attempt of this boot finishes.
    Outcome outcome = Outcome::None;
    // The server that answered, or for a failure the last one asked ("" when
    // the list was empty). ntp_servers entries are at most 63 characters
    // (SystemSettings refuses longer ones, stored or new).
    FixedString<63> server;
    int64_t time = 0;  // UTC seconds; Synced only
    // Finished attempts this boot, plus clocks set by other means
    // (TimeSyncService::note_external_sync()). Every attempt moves it, so
    // two equal outcomes in a row are still two changes, and two events.
    uint32_t attempts = 0;
    bool operator==(const TimeSyncResult&) const = default;
};

}  // namespace wisp::core
