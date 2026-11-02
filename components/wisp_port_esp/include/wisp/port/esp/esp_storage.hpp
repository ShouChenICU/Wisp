#pragma once

// ESP-IDF wisp::hal::Storage devices (task-18-brief.md):
//
//   - EspInternalStorage ("storage.int"/"littlefs", caps removable=false,
//     fs="littlefs"): mounts the "storage" flash partition (see
//     firmware/partitions.csv, spec 7.4) with esp_vfs_littlefs_register()
//     directly -- this is *not* a bmgr device (board_devices.yaml has no
//     fs_littlefs entry): bmgr's own littlefs device type exists but
//     task-18-brief.md calls for esp_vfs_littlefs_register() directly, so
//     that is what EspInternalStorage::create() does. Always mounted at
//     "/int"; never removable, so online() is always true once
//     constructed. A mount failure (even after esp_littlefs's own
//     internal format-and-retry, format_if_mount_failed=true) means this
//     device is never constructed at all -- board.cpp then doesn't
//     register "storage.int", which is exactly the signal
//     core::StorageManager::init() uses to report the app's read-only
//     degrade (AGENTS.md's storage rule; spec 6.1's "内置 Flash 存储挂载
//     失败...进入只读降级").
//
//   - EspSdStorage ("storage.sd", caps removable=true, fs="fat"): wraps
//     bmgr's SDMMC fs_fat device (name/model passed in by board.cpp --
//     see the constructor; AGENTS.md's "板子无关" rule keeps the actual
//     bmgr device name/chip model string out of this ESP-only *portable*
//     component). Always registered, even when no card is present at
//     boot (board.cpp then records DeviceState::Missing/"no card" --
//     task-18-brief.md).
//
//     Threading (fix round 1 -- see task-18-report.md's "Fix round 1"
//     section for the review this responds to): every slow/blocking
//     operation -- esp_board_manager_{,de}init_device_by_name(),
//     sdmmc_get_status() (a CMD13 round-trip), esp_vfs_fat_info() (can
//     scan the FAT table on some cards) -- runs on a dedicated,
//     low-priority worker os::Task this class owns, *never* on whichever
//     thread calls online()/capacity(). Those two methods only ever read
//     a mutex-protected snapshot of state the worker maintains, so they
//     are cheap and safe to call as often as callers already do (e.g.
//     core::StorageManager::status(), polled every 100ms by the self
//     -check screen's UI-thread timer, and core::StorageManager::refresh(),
//     driven by a 2s os::Timer that -- per os/os.hpp's Timer class
//     comment -- dispatches on ESP-IDF's single shared esp_timer task, so
//     a slow refresh() would delay every other Timer callback in the
//     process, not just this one).
//
//     The worker's state machine (SdRetryState, sd_retry.hpp):
//       - Unmounted: attempts esp_board_manager_init_device_by_name()
//         every kRetryIntervalMs (2s) via SdRetryState::should_retry().
//       - Mounted: probes presence with sdmmc_get_status() every
//         ~kProbeIntervalMs (1s) and refreshes the cached capacity (see
//         below) right after a successful probe.
//       - Removed: set by the worker when a presence probe fails. The
//         worker does *not* unmount from here -- see sd_retry.hpp's
//         mark_removed() comment for why: online() does that itself,
//         specifically because online() is always called under
//         core::StorageManager's own mutex, which guarantees no FATFS
//         I/O against this mount point can be in flight at that moment,
//         so a same-thread esp_board_manager_deinit_device_by_name() call
//         there is both safe and fast (no card I/O, just driver/bmgr
//         bookkeeping).
//
//     card_/the SdRetryState/the cached capacity are all guarded by one
//     os::Mutex (state_mutex_) internal to this class -- *not*
//     StorageManager's mutex, since per the ruling above the worker must
//     be able to mount/probe without holding that one (nothing uses "/sd"
//     until online() has returned true, so there is nothing for the
//     worker to race against there).
//
//     Concurrent hardware access between the worker's CMD13 probe and
//     ordinary file I/O against the mounted FAT filesystem (from whatever
//     other thread is using it) is already serialized below this class,
//     without any lock of its own needed for that specific purpose:
//       - Every SDMMC command -- including sdmmc_get_status()'s CMD13 and
//         every command FatFS's own disk I/O issues -- funnels through
//         sdmmc_send_cmd() (components/sdmmc/sdmmc_cmd.c), which calls
//         `(*card->host.do_transaction)(slot, cmd)`; for this board's
//         SDMMC host that function pointer is
//         sd_host_slot_sdmmc_do_transaction()
//         (components/esp_driver_sdmmc/src/sd_trans_sdmmc.c), whose very
//         first line is `xSemaphoreTake(slot_ctx->ctlr->mutex,
//         portMAX_DELAY)` -- one mutex per SDMMC controller, held for the
//         whole transaction.
//       - FatFS's own volume operations (e.g. esp_vfs_fat_info()'s
//         f_getfree(), and every file read/write) are independently
//         serialized by FatFS's own per-volume mutex: ESP-IDF's
//         components/fatfs/src/ffconf.h sets FF_FS_REENTRANT=1, and
//         components/fatfs/port/freertos/ffsystem.c's ff_mutex_take()/
//         ff_mutex_give() guard every FatFS entry point with it.
//     So a probe/capacity-refresh from the worker racing a write from
//     another task is safe at both the SDMMC-command layer and the
//     FatFS-volume layer; this class's own state_mutex_ only protects its
//     own bookkeeping (card_, the retry state machine, the capacity
//     cache), not the hardware.
//
//     capacity() is cached (refreshed right after a successful mount, and
//     again after every successful presence probe while mounted -- see
//     above) rather than calling esp_vfs_fat_info() on the caller's
//     thread every time, for the same reason online() no longer calls
//     sdmmc_get_status() synchronously: esp_vfs_fat_info() can be slow
//     (FatFS may need to scan the whole FAT table for the free-cluster
//     count on some cards) and capacity() is polled just as often as
//     online() (the same StorageManager::status() call reads both).
//
//     Logging: a missing card being retried every 2s used to spam the
//     log forever (esp_board_manager/sdmmc/vfs_fat's own ESP_LOGE/ESP_LOGW
//     calls on every failed attempt, not just wisp::log's -- see
//     dev/esp_storage.cpp's ScopedMountLogSuppress). The worker silences
//     those specific tags for the duration of each failed mount attempt
//     only, and this class's own WLOG_* lines log a mount/removal
//     transition once, not on every retry.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "sdmmc_cmd.h"
#include "wisp/base/result.hpp"
#include "wisp/hal/storage.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/esp/sd_retry.hpp"

namespace wisp::port::esp {

class EspInternalStorage : public wisp::hal::Storage {
public:
    // esp_vfs_littlefs_register({base_path="/int",
    // partition_label="storage", format_if_mount_failed=true}). Fails
    // with ErrorCode::DeviceFailure if that returns anything other than
    // ESP_OK.
    static Result<std::shared_ptr<EspInternalStorage>> create();

    // No esp_vfs_littlefs_unregister(): this device lives for the
    // process's lifetime, like every other ESP HAL device Wisp registers.
    ~EspInternalStorage() override = default;

    const wisp::hal::DeviceInfo& info() const override { return info_; }
    std::string mount_point() const override { return "/int"; }
    // Internal flash is never removable: always true once this device
    // exists at all (a mount failure means create() never returns one).
    bool online() override { return true; }
    // esp_littlefs_info("storage", &total, &used).
    Result<wisp::hal::Capacity> capacity() override;

private:
    EspInternalStorage();

    wisp::hal::DeviceInfo info_;
};

class EspSdStorage : public wisp::hal::Storage {
public:
    // `mount_point` is the fs_sdcard bmgr device's configured mount point
    // ("/sd" -- board_devices.yaml). `bmgr_device_name`/`model` are
    // board-specific facts board.cpp passes in (AGENTS.md's "板子无关"
    // rule: this ESP-only *portable* component must not itself name a
    // specific board's bmgr device or chip model -- fix round 1, these
    // used to be hard-coded here). `already_mounted`/`card` reflect
    // whether esp_board_manager_init() (called by board.cpp before
    // constructing this device) already succeeded in mounting the device
    // at boot; `card` must be non-null whenever `already_mounted` is
    // true, and is ignored otherwise.
    EspSdStorage(std::string mount_point, std::string bmgr_device_name, std::string model,
                 bool already_mounted, sdmmc_card_t* card);

    // Stops and joins the worker task (see the file comment) before
    // destroying anything the worker might still touch.
    ~EspSdStorage() override;

    EspSdStorage(const EspSdStorage&) = delete;
    EspSdStorage& operator=(const EspSdStorage&) = delete;

    const wisp::hal::DeviceInfo& info() const override { return info_; }
    std::string mount_point() const override { return mount_point_; }
    // Cheap (see the file comment): reads worker-maintained state under
    // state_mutex_. If the worker has flagged the card Removed, this
    // performs the actual esp_board_manager_deinit_device_by_name() call
    // itself (safe and fast here specifically -- see file comment) and
    // returns false; otherwise returns whether the state is Mounted.
    // Never attempts a new mount itself -- that is the worker's job.
    bool online() override;
    // Returns the capacity cached by the worker (see file comment).
    // Fails with ErrorCode::StorageUnavailable if not currently mounted.
    Result<wisp::hal::Capacity> capacity() override;

private:
    static constexpr uint32_t kRetryIntervalMs = 2000;
    static constexpr uint32_t kProbeIntervalMs = 1000;
    // Granularity the worker sleeps in while idle, so stopping it (see
    // the destructor) doesn't have to wait out a full probe/retry period.
    static constexpr uint32_t kWorkerPollMs = 100;

    void worker_main();
    // Attempts esp_board_manager_init_device_by_name(bmgr_device_name_)
    // at time `now`; on success, updates card_/the capacity cache and
    // marks the state Mounted. Called only from the worker task.
    bool try_mount(uint64_t now);
    // Re-reads esp_vfs_fat_info(mount_point_) into the capacity cache.
    // Called only from the worker task, with state_mutex_ *not* held
    // (esp_vfs_fat_info() itself is the potentially-slow call this exists
    // to keep off every other thread -- see file comment); takes the lock
    // itself just to publish the result.
    void refresh_capacity_cache();

    wisp::hal::DeviceInfo info_;
    std::string mount_point_;
    std::string bmgr_device_name_;

    mutable wisp::os::Mutex state_mutex_;  // guards every member below
    SdRetryState retry_;
    sdmmc_card_t* card_ = nullptr;
    wisp::hal::Capacity cached_capacity_{};
    bool capacity_valid_ = false;
    // Set once a "no card"/"card lost" transition has been logged, and
    // cleared on the next successful mount, so repeated retries don't
    // spam wisp::log the way they used to (see file comment).
    bool logged_absent_ = false;

    std::atomic<bool> worker_running_{true};
    std::unique_ptr<wisp::os::Task> worker_;
};

}  // namespace wisp::port::esp
