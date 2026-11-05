#include "wisp/core/wakelock.hpp"

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
        std::lock_guard<os::Mutex> lock(mutex_);
        info.id = next_id_++;
        if (next_id_ == 0) next_id_ = 1;  // 0 means "no lock" to callers
        entries_.push_back(Entry{info, 0});
    }
    publish("acquire", info);
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
    }
    publish("release", info);
    notify_change();
    return true;
}

size_t WakeLockRegistry::release_holder(std::string_view holder) {
    std::vector<WakeLockInfo> released;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->info.holder == holder) {
                released.push_back(std::move(it->info));
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const WakeLockInfo& info : released) publish("release", info);
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

void WakeLockRegistry::set_on_change(std::function<void()> cb) {
    std::lock_guard<os::Mutex> lock(cb_mutex_);
    on_change_ = std::move(cb);
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
    std::lock_guard<os::Mutex> lock(cb_mutex_);
    if (on_change_) on_change_();
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
