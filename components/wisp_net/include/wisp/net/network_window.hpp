#pragma once

// NetworkWindow (scene-device spec 4.4): how scene requests get out while
// deep standby keeps Wi-Fi off.
//
// In deep standby the scenes' requests wait in SceneHttp's deferred list
// (set_deferring(true)). At most every kIntervalMs -- counted from
// entering deep standby or from the end of the last window -- and only
// while something waits, a window opens: one network wake lock
// ("system.netwindow", "deferred requests"), which PowerManager answers
// by going back to standby, where Wi-Fi comes on and reconnects. Once the
// station is connected the deferred requests are released; the window
// closes when the proxy is idle and a time-sync result has arrived (Wi-Fi
// coming up starts an attempt; at most kTimeSyncWaitMs after connecting
// is waited for one). It gives up kConnectTimeoutMs after opening without
// a connection and closes kMaxOpenMs after opening whatever is still
// under way. Closing releases the lock, so PowerManager returns to deep
// standby; the requests that remain keep waiting for the next window.
//
// Outside deep standby nothing is deferred: in standby Wi-Fi is on and
// requests go out at once. Two exceptions keep deferring on in standby:
// an open window that has not connected yet, and the few seconds right
// after a window closes while no other network lock is held, which is only
// PowerManager not having gone back down yet. Waking up (active or dim)
// while a window is open closes it.
//
// Logs (Info, tag "net"): "netwindow: open" and "netwindow: closed
// (<reason>)", reason done / no wifi / timeout / awake.
//
// Threading: tick() is called from one task (NetWiring's "wisp.net");
// note_time_result() and open() may be called from any thread.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

#include "wisp/core/wakelock.hpp"
#include "wisp/hal/device.hpp"

namespace wisp::net {

struct NetworkWindowDeps {
    core::WakeLockRegistry& locks;
    std::function<size_t()> pending;          // SceneHttp::deferred_count
    std::function<bool()> http_idle;          // SceneHttp::idle
    std::function<void(bool)> set_deferring;  // SceneHttp::set_deferring
    std::function<bool()> wifi_connected;
};

class NetworkWindow {
public:
    static constexpr uint64_t kIntervalMs = 15 * 60 * 1000;
    static constexpr uint64_t kConnectTimeoutMs = 60000;
    static constexpr uint64_t kTimeSyncWaitMs = 30000;
    static constexpr uint64_t kMaxOpenMs = 5 * 60 * 1000;

    explicit NetworkWindow(NetworkWindowDeps deps);

    NetworkWindow(const NetworkWindow&) = delete;
    NetworkWindow& operator=(const NetworkWindow&) = delete;

    // `level` is PowerManager::level() now.
    void tick(uint64_t now_ms, hal::PowerLevel level);
    // A system.time event arrived (a sync attempt finished either way).
    void note_time_result();
    bool open() const { return open_.load(); }

private:
    void deferring(bool on);
    void close(uint64_t now_ms, const char* why);

    NetworkWindowDeps deps_;
    core::WakeLock lock_;
    std::atomic<bool> open_{false};
    std::atomic<bool> time_result_{false};
    // What set_deferring was last told; nullopt before the first tick.
    std::optional<bool> deferring_;
    // When the next window may open; nullopt outside deep standby.
    std::optional<uint64_t> next_ms_;
    uint64_t opened_ms_ = 0;
    std::optional<uint64_t> connected_ms_;
    // Set when a window closes: until then PowerManager may still be on
    // its way back to deep standby.
    std::optional<uint64_t> closing_until_ms_;
};

}  // namespace wisp::net
