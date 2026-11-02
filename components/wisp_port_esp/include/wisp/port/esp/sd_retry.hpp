#pragma once

// Pure, platform-free state machine for
// wisp::port::esp::EspSdStorage (dev/esp_storage.cpp): tracks whether the
// SD card is mounted, has just been observed gone (but not yet unmounted),
// or is unmounted and due (or not yet due) for a retry -- and, given the
// current state and elapsed time, whether a mount attempt should happen
// *now*. It performs no I/O itself (no esp_board_manager_*, no sdmmc_*
// calls, no mutex/task of its own) so it can be unit-tested on the host
// (tests/port_esp/sd_retry_test.cpp) even though wisp_port_esp itself is
// an ESP-only component (never built for the host).
//
// Fix round 1 (task-18-report.md): originally this only tracked
// "mounted?" / "when was the last attempt?", and EspSdStorage::online()
// itself did every transition, including the slow esp_board_manager_*/
// sdmmc_* calls -- called from whatever thread asked (the UI thread every
// 100ms, or the shared esp_timer task every 2s), which stalled the UI and
// risked blocking os_esp.cpp's "Timer callbacks must be short" rule. Now
// EspSdStorage runs a dedicated low-priority worker task that owns every
// state transition below except the Removed -> Unmounted one (see
// mark_removed()'s comment); online() itself only ever reads state and,
// at most, performs that one specific fast transition.
//
// The three states this tracks:
//   - Mounted: the card is mounted and was last confirmed present.
//   - Removed: the card was mounted, but the worker's last presence probe
//     (sdmmc_get_status()) failed. The mount point/bmgr device are
//     *not* unmounted yet -- EspSdStorage::online() does that itself (see
//     mark_removed()'s comment) -- so a Removed state is necessarily
//     transient, lasting only until the next online() call.
//   - Unmounted: no card mounted. should_retry() governs how often the
//     worker is allowed to attempt esp_board_manager_init_device_by_name()
//     again.

#include <cstdint>

namespace wisp::port::esp {

enum class SdCardState : uint8_t { Unmounted, Mounted, Removed };

class SdRetryState {
public:
    // `retry_interval_ms` is the minimum gap between two mount attempts
    // while the card is absent (2000 on the real board -- see
    // EspSdStorage, which matches StorageManager's own 2s refresh()
    // timer, see components/wisp_app/src/app.cpp's kStorageRefreshMs).
    explicit SdRetryState(uint32_t retry_interval_ms) : retry_interval_ms_(retry_interval_ms) {}

    SdCardState state() const { return state_; }
    bool mounted() const { return state_ == SdCardState::Mounted; }
    bool removed() const { return state_ == SdCardState::Removed; }

    // Call once a mount attempt (or the worker's periodic presence probe
    // on an already-mounted card) has just succeeded.
    void mark_mounted() { state_ = SdCardState::Mounted; }

    // Call when the worker's presence probe (sdmmc_get_status()) on a
    // Mounted card fails. Deliberately does *not* become Unmounted
    // directly, and does not touch last-attempt bookkeeping: the actual
    // unmount (esp_board_manager_deinit_device_by_name()) is done by
    // EspSdStorage::online() itself, which is always called under
    // StorageManager's mutex (so no FATFS I/O can be in flight -- see
    // esp_storage.hpp's class comment) -- the worker thread must not do
    // that unmount concurrently with a caller that might be mid-way
    // through using the still-"mounted" filesystem. A no-op if not
    // currently Mounted (defensive; the worker is the only caller and
    // only calls this from the Mounted branch of its own loop).
    void mark_removed() {
        if (state_ == SdCardState::Mounted) {
            state_ = SdCardState::Removed;
        }
    }

    // Call after: (a) a failed mount attempt, or (b) online() has just
    // finished unmounting a Removed card. Leaves the state Unmounted and
    // records `now_ms` as the last attempt time, so should_retry() won't
    // return true again until retry_interval_ms_ has elapsed.
    void mark_unmounted(uint64_t now_ms) {
        state_ = SdCardState::Unmounted;
        last_attempt_ms_ = now_ms;
        attempted_ = true;
    }

    // True exactly when Unmounted, and either no attempt has ever been
    // made yet, or at least retry_interval_ms_ has elapsed since the last
    // one. Always false while Mounted or Removed (a Removed card isn't a
    // candidate for a fresh mount attempt until online() has unmounted it
    // and called mark_unmounted()).
    bool should_retry(uint64_t now_ms) const {
        if (state_ != SdCardState::Unmounted) {
            return false;
        }
        if (!attempted_) {
            return true;
        }
        return now_ms - last_attempt_ms_ >= retry_interval_ms_;
    }

private:
    uint32_t retry_interval_ms_;
    SdCardState state_ = SdCardState::Unmounted;
    bool attempted_ = false;
    uint64_t last_attempt_ms_ = 0;
};

}  // namespace wisp::port::esp
