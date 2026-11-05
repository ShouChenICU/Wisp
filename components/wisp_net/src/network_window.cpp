#include "wisp/net/network_window.hpp"

#include <utility>

#include "wisp/core/log.hpp"

namespace wisp::net {

namespace {

constexpr const char* kTag = "net";
constexpr const char* kHolder = "system.netwindow";
constexpr const char* kReason = "deferred requests";
// How long after a window closes standby is still taken for PowerManager
// on its way back down (it reacts to the released lock at once; its own
// tick is once a second).
constexpr uint64_t kSettleMs = 5000;

}  // namespace

NetworkWindow::NetworkWindow(NetworkWindowDeps deps) : deps_(std::move(deps)) {}

void NetworkWindow::deferring(bool on) {
    if (deferring_ == on) return;
    deferring_ = on;
    deps_.set_deferring(on);
}

void NetworkWindow::close(uint64_t now_ms, const char* why) {
    lock_.release();
    open_.store(false);
    connected_ms_.reset();
    next_ms_ = now_ms + kIntervalMs;
    closing_until_ms_ = now_ms + kSettleMs;
    WLOG_I(kTag, "netwindow: closed (%s)", why);
}

void NetworkWindow::note_time_result() { time_result_.store(true); }

void NetworkWindow::tick_open(uint64_t now_ms) {
    if (connected_ms_ && !deps_.wifi_connected()) {
        // The station dropped mid-window: wait for it again rather than
        // fail the requests on a dead link.
        connected_ms_.reset();
        connect_from_ms_ = now_ms;
        deferring(true);
    }
    if (!connected_ms_) {
        if (deps_.wifi_connected()) {
            connected_ms_ = now_ms;
            deferring(false);
        } else if (now_ms - connect_from_ms_ >= kConnectTimeoutMs) {
            close(now_ms, "no wifi");
            return;
        }
    }
    if (connected_ms_ && deps_.http_idle() &&
        (time_result_.load() || now_ms - *connected_ms_ >= kTimeSyncWaitMs)) {
        deferring(true);
        close(now_ms, "done");
        return;
    }
    if (now_ms - opened_ms_ >= kMaxOpenMs) {
        deferring(true);
        close(now_ms, "timeout");
    }
}

void NetworkWindow::tick(uint64_t now_ms, hal::PowerLevel level) {
    const bool awake = level == hal::PowerLevel::Active || level == hal::PowerLevel::Dim;

    if (open_.load()) {
        if (!awake) {
            tick_open(now_ms);
            return;
        }
        // Wi-Fi stays on for the user now. A window that had not connected
        // yet leaves its requests waiting for Wi-Fi like any wake (below).
        close(now_ms, "awake");
        closing_until_ms_.reset();
    }

    if (level == hal::PowerLevel::DeepStandby) {
        closing_until_ms_.reset();
        wake_ms_.reset();
        if (!next_ms_) next_ms_ = now_ms + kIntervalMs;
        deferring(true);
        if (now_ms >= *next_ms_ && deps_.pending() > 0) {
            lock_ = core::WakeLock(deps_.locks, kHolder, core::WakeLockKind::Network, kReason);
            open_.store(true);
            opened_ms_ = now_ms;
            connect_from_ms_ = now_ms;
            connected_ms_.reset();
            time_result_.store(false);
            WLOG_I(kTag, "netwindow: open");
        }
        return;
    }

    // Standby right after a window closed, with no other network lock:
    // PowerManager is about to go back to deep standby, and Wi-Fi with it.
    if (closing_until_ms_ && now_ms < *closing_until_ms_ && level == hal::PowerLevel::Standby &&
        !deps_.locks.held(core::WakeLockKind::Network)) {
        return;
    }
    closing_until_ms_.reset();
    next_ms_.reset();
    // Just out of deep standby, or out of a window that never connected:
    // Wi-Fi is on its way up, and releasing the requests now would only
    // fail them with no route. They wait for the station, or until the
    // connect timeout.
    if (deferring_ == true) {
        if (!wake_ms_) wake_ms_ = now_ms;
        if (!deps_.wifi_connected() && now_ms - *wake_ms_ < kConnectTimeoutMs) return;
    }
    wake_ms_.reset();
    deferring(false);
}

}  // namespace wisp::net
