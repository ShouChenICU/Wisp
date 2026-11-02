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
//   - EspSdStorage ("storage.sd"/"sdcard-sdmmc", caps removable=true,
//     fs="fat"): wraps bmgr's "fs_sdcard" device (board_devices.yaml,
//     SDMMC 1-bit). Always registered, even when no card is present at
//     boot (board.cpp then records DeviceState::Missing/"no card" --
//     task-18-brief.md). online() is where the hot-plug behavior lives:
//     see sd_retry.hpp's SdRetryState for the pure retry/backoff policy
//     this class drives with real sdmmc_get_status()/
//     esp_board_manager_{,de}init_device_by_name() calls. Driven
//     periodically by core::StorageManager::refresh()'s 2s timer (see
//     components/wisp_app/src/app.cpp's kStorageRefreshMs) via
//     StorageManager::status()/refresh() calling online() -- both already
//     serialize on StorageManager's own mutex, so online() itself needs
//     no locking of its own.

#include <cstdint>
#include <memory>
#include <string>

#include "sdmmc_cmd.h"
#include "wisp/base/result.hpp"
#include "wisp/hal/storage.hpp"
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
    // process's lifetime, like every other ESP HAL device Wisp
    // registers.
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
    // ("/sd" -- board_devices.yaml). `already_mounted`/`card` reflect
    // whether esp_board_manager_init() (called by board.cpp before
    // constructing this device) already succeeded in mounting fs_sdcard
    // at boot; `card` must be non-null whenever `already_mounted` is
    // true, and is ignored otherwise.
    EspSdStorage(std::string mount_point, bool already_mounted, sdmmc_card_t* card);

    const wisp::hal::DeviceInfo& info() const override { return info_; }
    std::string mount_point() const override { return mount_point_; }
    // Already mounted: sdmmc_get_status(card) -- a failure means the
    // card is gone, so this unmounts it (esp_board_manager_deinit_
    // device_by_name("fs_sdcard")) and returns false. Not mounted: tries
    // esp_board_manager_init_device_by_name("fs_sdcard") again only if
    // SdRetryState::should_retry() says at least 2s have elapsed since
    // the last attempt (sd_retry.hpp).
    bool online() override;
    // esp_vfs_fat_info(mount_point(), &total, &free). Fails with
    // ErrorCode::StorageUnavailable if not currently mounted.
    Result<wisp::hal::Capacity> capacity() override;

private:
    static constexpr uint32_t kRetryIntervalMs = 2000;

    wisp::hal::DeviceInfo info_;
    std::string mount_point_;
    SdRetryState retry_;
    sdmmc_card_t* card_ = nullptr;
};

}  // namespace wisp::port::esp
