#pragma once

// Pure, platform-free retry/backoff policy for
// wisp::port::esp::EspSdStorage::online() (dev/esp_storage.cpp): decides,
// given the current mounted state and elapsed time, whether a remount
// attempt should happen *now* -- it performs no I/O itself (no
// esp_board_manager_*, no sdmmc_* calls). Kept header-only and free of
// any ESP-IDF include so it can be unit-tested on the host
// (tests/port_esp/sd_retry_test.cpp) even though wisp_port_esp itself is
// an ESP-only component (never built for the host).
//
// task-18-brief.md's EspSdStorage::online() contract: "已挂载时用
// sdmmc_get_status 检查卡是否仍在，失败则卸载并返回 false；未挂载时距上次
// 尝试已过 2s 才调用 esp_board_manager_init_device_by_name 重试挂载". This
// class tracks exactly the "mounted?" / "when was the last attempt?" state
// that decision needs.

#include <cstdint>

namespace wisp::port::esp {

class SdRetryState {
public:
    // `retry_interval_ms` is the minimum gap between two mount attempts
    // while the card is absent (2000 on the real board -- see
    // EspSdStorage, which matches StorageManager's own 2s refresh()
    // timer, see components/wisp_app/src/app.cpp's kStorageRefreshMs).
    explicit SdRetryState(uint32_t retry_interval_ms) : retry_interval_ms_(retry_interval_ms) {}

    bool mounted() const { return mounted_; }

    // Call once a mount (or a sdmmc_get_status() check on an already-
    // mounted card) has just succeeded.
    void mark_mounted() { mounted_ = true; }

    // Call after a failed mount attempt, or after sdmmc_get_status()
    // reports an already-mounted card is gone: leaves the state "not
    // mounted" and records `now_ms` as the last attempt time, so
    // should_retry() won't return true again until retry_interval_ms_ has
    // elapsed.
    void mark_unmounted(uint64_t now_ms) {
        mounted_ = false;
        last_attempt_ms_ = now_ms;
        attempted_ = true;
    }

    // True exactly when not currently mounted, and either no attempt has
    // ever been made yet, or at least retry_interval_ms_ has elapsed
    // since the last one.
    bool should_retry(uint64_t now_ms) const {
        if (mounted_) {
            return false;
        }
        if (!attempted_) {
            return true;
        }
        return now_ms - last_attempt_ms_ >= retry_interval_ms_;
    }

private:
    uint32_t retry_interval_ms_;
    bool mounted_ = false;
    bool attempted_ = false;
    uint64_t last_attempt_ms_ = 0;
};

}  // namespace wisp::port::esp
