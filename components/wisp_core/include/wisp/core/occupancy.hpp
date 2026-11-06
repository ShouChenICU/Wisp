#pragma once

// OccupancyRegistry (scene-device spec 6.3): who is using which device, and
// how. A holder ("system.<module>" or "scene.<id>") takes a device id
// ("imu.main") in shared or exclusive mode. Shared holders coexist; an
// exclusive one excludes every other holder. This release only records:
// SensorService takes "imu.main" shared for each holder that subscribes,
// and the admin API lists the table. Preemption and asking the user are
// left to later sub-projects.
//
// Thread-safe: every method may be called from any thread.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

enum class OccupancyMode : uint8_t { Shared, Exclusive };

// "shared", "exclusive".
const char* to_string(OccupancyMode m);

struct OccupancyEntry {
    std::string device, holder;
    OccupancyMode mode = OccupancyMode::Shared;
    uint64_t since_ms = 0;
};

class OccupancyRegistry {
public:
    // `now_ms` stamps since_ms; tests pass a clock they move by hand.
    explicit OccupancyRegistry(std::function<uint64_t()> now_ms = os::now_ms);

    OccupancyRegistry(const OccupancyRegistry&) = delete;
    OccupancyRegistry& operator=(const OccupancyRegistry&) = delete;

    // Busy when it conflicts: an exclusive holder, or exclusive over any
    // holder. The same holder acquiring again is Ok (no second entry; its
    // entry stays as it was).
    Result<void> acquire(std::string device, std::string holder, OccupancyMode mode);
    // Nothing happens if `holder` does not hold `device`.
    void release(std::string_view device, std::string_view holder);
    // Every entry, in the order they were taken.
    std::vector<OccupancyEntry> list() const;

private:
    std::function<uint64_t()> now_ms_;
    mutable os::Mutex mutex_;
    std::vector<OccupancyEntry> entries_;
};

}  // namespace wisp::core
