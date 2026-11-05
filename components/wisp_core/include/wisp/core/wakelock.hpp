#pragma once

// Wake locks (spec 4.3): a holder ("system.<module>" or "scene.<id>") keeps
// part of the device awake while it holds a lock of some kind:
//   network  keeps Wi-Fi on: no deep_standby;
//   cpu      keeps automatic light sleep off;
//   screen   keeps the screen at active: no dim, no idle screen-off.
// WakeLockRegistry only records the locks and reports changes; what each
// kind does is up to wisp::app::PowerManager, which reads held().
//
// Every acquire and release publishes "power.wakelock" (source "power",
// payload {"op": "acquire"|"release", "holder", "kind", "reason"}) and then
// runs the on_change callback.
//
// Thread-safe: every method may be called from any thread.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/core/event_bus.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

enum class WakeLockKind : uint8_t { Network, Cpu, Screen };

// "network", "cpu", "screen".
const char* to_string(WakeLockKind kind);

struct WakeLockInfo {
    uint32_t id = 0;
    std::string holder;  // "system.<module>" or "scene.<id>"
    WakeLockKind kind = WakeLockKind::Cpu;
    std::string reason;
    uint64_t since_ms = 0;
};

class WakeLockRegistry {
public:
    // A lock held this long, and every further multiple of it, earns one
    // reminder (spec 4.3).
    static constexpr uint64_t kReminderMs = 30 * 60 * 1000;

    // `now_ms` stamps since_ms; tests pass a clock they move by hand.
    explicit WakeLockRegistry(EventBus& bus, std::function<uint64_t()> now_ms = os::now_ms);

    WakeLockRegistry(const WakeLockRegistry&) = delete;
    WakeLockRegistry& operator=(const WakeLockRegistry&) = delete;

    // Records a new lock and returns its id (> 0, never reused).
    uint32_t acquire(std::string holder, WakeLockKind kind, std::string reason);
    // False if `id` is not held (never acquired, or already released).
    bool release(uint32_t id);
    // Releases every lock of `holder`; returns how many there were.
    size_t release_holder(std::string_view holder);

    bool held(WakeLockKind kind) const;
    // Every lock currently held, by id.
    std::vector<WakeLockInfo> list() const;

    // Runs after every acquire/release that changed something, outside
    // the registry's own lock, on the caller's thread. Calls are
    // serialized with each other and with set_on_change(), so once
    // set_on_change() returns the previous callback is not running and
    // never runs again. The callback may read the registry (held(),
    // list()) but must not acquire, release or call set_on_change().
    void set_on_change(std::function<void()> cb);

    // Locks whose held time crossed the next multiple of kReminderMs
    // since their last reminder. Each multiple is reported once; several
    // crossed at once give one reminder.
    std::vector<WakeLockInfo> due_reminders(uint64_t now_ms);

private:
    struct Entry {
        WakeLockInfo info;
        uint64_t reminded = 0;  // multiples of kReminderMs already reported
    };

    void publish(const char* op, const WakeLockInfo& info);
    void notify_change();

    EventBus& bus_;
    std::function<uint64_t()> now_ms_;

    mutable os::Mutex mutex_;     // guards entries_ and next_id_
    std::vector<Entry> entries_;  // ascending id
    uint32_t next_id_ = 1;

    // Held while the callback runs and while it is replaced.
    os::Mutex cb_mutex_;
    std::function<void()> on_change_;
};

// Holds one lock for its lifetime. Move-only; a default-constructed or
// moved-from WakeLock holds nothing. The registry must outlive it.
class WakeLock {
public:
    WakeLock() = default;
    WakeLock(WakeLockRegistry& reg, std::string holder, WakeLockKind kind, std::string reason);
    ~WakeLock();

    WakeLock(WakeLock&& other) noexcept;
    WakeLock& operator=(WakeLock&& other) noexcept;
    WakeLock(const WakeLock&) = delete;
    WakeLock& operator=(const WakeLock&) = delete;

    void release();
    bool held() const;

private:
    WakeLockRegistry* reg_ = nullptr;
    uint32_t id_ = 0;
};

}  // namespace wisp::core
