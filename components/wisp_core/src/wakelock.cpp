#include "wisp/core/wakelock.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <utility>

#include "wisp/base/value.hpp"

namespace wisp::core {

const char* to_string(WakeLockKind kind) {
    switch (kind) {
        case WakeLockKind::Network:
            return "network";
        case WakeLockKind::Cpu:
            return "cpu";
        case WakeLockKind::Screen:
            return "screen";
    }
    return "cpu";
}

WakeLockRegistry::WakeLockRegistry(EventBus& bus, std::function<uint64_t()> now_ms)
    : bus_(bus), now_ms_(std::move(now_ms)) {}

uint32_t WakeLockRegistry::acquire(std::string holder, WakeLockKind kind, std::string reason) {
    WakeLockInfo info;
    info.holder = std::move(holder);
    info.kind = kind;
    info.reason = std::move(reason);
    info.since_ms = now_ms_();
    {
        // Published under the lock so the events come in the order the
        // registry changed: a concurrent release is never seen before its
        // acquire. EventBus::publish() never blocks.
        std::lock_guard<os::Mutex> lock(mutex_);
        info.id = next_free_id();
        auto pos = std::find_if(entries_.begin(), entries_.end(),
                                [&](const Entry& e) { return e.info.id > info.id; });
        entries_.insert(pos, Entry{info, 0});
        publish("acquire", info);
    }
    notify_change();
    return info.id;
}

bool WakeLockRegistry::release(uint32_t id) {
    WakeLockInfo info;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        auto it = entries_.begin();
        while (it != entries_.end() && it->info.id != id) ++it;
        if (it == entries_.end()) return false;
        info = std::move(it->info);
        entries_.erase(it);
        publish("release", info);
    }
    notify_change();
    return true;
}

size_t WakeLockRegistry::release_holder(std::string_view holder) {
    std::vector<WakeLockInfo> released;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->info.holder == holder) {
                publish("release", it->info);
                released.push_back(std::move(it->info));
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!released.empty()) notify_change();
    return released.size();
}

bool WakeLockRegistry::held(WakeLockKind kind) const {
    std::lock_guard<os::Mutex> lock(mutex_);
    for (const Entry& e : entries_) {
        if (e.info.kind == kind) return true;
    }
    return false;
}

std::vector<WakeLockInfo> WakeLockRegistry::list() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    std::vector<WakeLockInfo> out;
    out.reserve(entries_.size());
    for (const Entry& e : entries_) out.push_back(e.info);
    return out;
}

uint32_t WakeLockRegistry::next_free_id() {
    // After 2^32 acquires the counter wraps; skip 0 ("no lock") and any id
    // a lock taken long ago still holds.
    while (true) {
        const uint32_t id = next_id_++;
        if (id == 0) continue;
        bool taken = false;
        for (const Entry& e : entries_) {
            if (e.info.id == id) {
                taken = true;
                break;
            }
        }
        if (!taken) return id;
    }
}

void WakeLockRegistry::set_on_change(std::function<void()> cb) {
    auto next = cb ? std::make_shared<const std::function<void()>>(std::move(cb)) : nullptr;
    {
        std::lock_guard<os::Mutex> lock(cb_mutex_);
        on_change_ = std::move(next);
    }
    // Wait out calls already running (they may hold the old callback).
    while (true) {
        {
            std::lock_guard<os::Mutex> lock(cb_mutex_);
            if (in_callback_ == 0) return;
        }
        os::sleep_ms(1);
    }
}

std::vector<WakeLockInfo> WakeLockRegistry::due_reminders(uint64_t now_ms) {
    std::lock_guard<os::Mutex> lock(mutex_);
    std::vector<WakeLockInfo> due;
    for (Entry& e : entries_) {
        const uint64_t held_ms = now_ms > e.info.since_ms ? now_ms - e.info.since_ms : 0;
        const uint64_t multiples = held_ms / kReminderMs;
        if (multiples > e.reminded) {
            e.reminded = multiples;
            due.push_back(e.info);
        }
    }
    return due;
}

void WakeLockRegistry::publish(const char* op, const WakeLockInfo& info) {
    Event e;
    e.topic = "power.wakelock";
    e.source = "power";
    e.payload = Properties{
        {"op", Value(op)},
        {"holder", Value(info.holder)},
        {"kind", Value(to_string(info.kind))},
        {"reason", Value(info.reason)},
    };
    bus_.publish(std::move(e));
}

void WakeLockRegistry::notify_change() {
    std::shared_ptr<const std::function<void()>> cb;
    {
        std::lock_guard<os::Mutex> lock(cb_mutex_);
        cb = on_change_;
        if (!cb) return;
        ++in_callback_;
    }
    // Called with no registry lock held, so a callback that is slow or
    // reads the registry never holds up another thread's acquire/release.
    (*cb)();
    std::lock_guard<os::Mutex> lock(cb_mutex_);
    --in_callback_;
}

WakeLock::WakeLock(WakeLockRegistry& reg, std::string holder, WakeLockKind kind, std::string reason)
    : reg_(&reg), id_(reg.acquire(std::move(holder), kind, std::move(reason))) {}

WakeLock::~WakeLock() { release(); }

WakeLock::WakeLock(WakeLock&& other) noexcept
    : reg_(std::exchange(other.reg_, nullptr)), id_(std::exchange(other.id_, 0)) {}

WakeLock& WakeLock::operator=(WakeLock&& other) noexcept {
    if (this != &other) {
        release();
        reg_ = std::exchange(other.reg_, nullptr);
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

void WakeLock::release() {
    if (reg_ != nullptr && id_ != 0) reg_->release(id_);
    reg_ = nullptr;
    id_ = 0;
}

bool WakeLock::held() const { return reg_ != nullptr && id_ != 0; }

}  // namespace wisp::core
