#include "wisp/core/occupancy.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

namespace wisp::core {

const char* to_string(OccupancyMode m) {
    return m == OccupancyMode::Exclusive ? "exclusive" : "shared";
}

OccupancyRegistry::OccupancyRegistry(std::function<uint64_t()> now_ms)
    : now_ms_(std::move(now_ms)) {}

Result<void> OccupancyRegistry::acquire(std::string device, std::string holder,
                                        OccupancyMode mode) {
    std::lock_guard<os::Mutex> lock(mutex_);
    bool held = false;
    for (const OccupancyEntry& e : entries_) {
        if (e.device != device) continue;
        if (e.holder == holder) {
            held = true;
            continue;
        }
        if (e.mode == OccupancyMode::Exclusive || mode == OccupancyMode::Exclusive)
            return Result<void>::err(ErrorCode::Busy, device + " is held by " + e.holder);
    }
    if (!held) entries_.push_back({std::move(device), std::move(holder), mode, now_ms_()});
    return Result<void>::ok();
}

void OccupancyRegistry::release(std::string_view device, std::string_view holder) {
    std::lock_guard<os::Mutex> lock(mutex_);
    std::erase_if(entries_, [&](const OccupancyEntry& e) {
        return e.device == device && e.holder == holder;
    });
}

std::vector<OccupancyEntry> OccupancyRegistry::list() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    return entries_;
}

}  // namespace wisp::core
