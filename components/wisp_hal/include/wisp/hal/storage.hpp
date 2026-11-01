#pragma once

#include <cstdint>
#include <string>

#include "wisp/base/result.hpp"
#include "wisp/hal/device.hpp"

namespace wisp::hal {

struct Capacity {
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
};

// caps keys: "removable" (bool), "fs" (string, filesystem type e.g. "fat",
// "littlefs").
class Storage : public Device {
public:
    static constexpr DeviceClass kClass = DeviceClass::Storage;

    virtual std::string mount_point() const = 0;

    // Allowed to probe/remount on each call (e.g. to detect a removable
    // card that was inserted or removed since the last call).
    virtual bool online() = 0;

    virtual Result<Capacity> capacity() = 0;
};

}  // namespace wisp::hal
