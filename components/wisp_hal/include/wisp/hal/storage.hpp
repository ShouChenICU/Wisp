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

    // Fix round 2 (task-18b-review.md, second addendum): a hint, not a
    // hardware operation -- unlike hal::Imu/Touch::set_power(), there is
    // nothing here that can fail, so this returns void rather than
    // Result<void>. A removable-media device whose online()/capacity()
    // bookkeeping is kept fresh by a background worker polling real
    // hardware (e.g. EspSdStorage's CMD13 presence probe and mount-retry
    // worker) should pause that polling while `standby` is true and
    // resume it once it's false again -- online()/capacity() themselves
    // keep returning whatever was last cached, never blocking and never
    // touching the hardware, so callers (StorageManager::status()/
    // refresh()) are unaffected either way. The default here is a no-op:
    // most Storage implementations (e.g. always-present internal flash,
    // and the simulator's directory-backed fake) have no such background
    // polling to pause in the first place.
    virtual void set_standby(bool /*standby*/) {}
};

}  // namespace wisp::hal
