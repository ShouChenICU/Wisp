#include "wisp/port/esp/esp_storage.hpp"

#include <cstddef>
#include <mutex>
#include <string>
#include <utility>

#include "dev_fs_fat.h"
#include "esp_board_manager.h"
#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wisp/base/error.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"
#include "wisp/os/os.hpp"

namespace wisp::port::esp {

namespace {

constexpr const char* kTag = "esp_storage";

// Must match firmware/partitions.csv's "storage" partition label. Unlike
// EspSdStorage's bmgr device name/model (passed in by board.cpp -- see
// esp_storage.hpp's comment), this is not a board-specific fact: every
// board using this component mounts the same internal LittleFS partition
// the same way, so it is not something boards/ needs to vary.
constexpr const char* kInternalPartitionLabel = "storage";

// Tags esp_board_manager/esp_driver_sdmmc/fatfs log to at ESP_LOG_ERROR or
// above every time a mount attempt finds no card -- without suppression,
// a card-less board would spam these at kRetryIntervalMs (2s) forever.
// ScopedMountLogSuppress below silences exactly these, and only for the
// duration of one mount attempt, so logs from the card's actual mounted
// use (and the *wisp::log*-side WLOG_* lines this file logs on its own,
// routed through wisp::log, not esp_log -- see log_esp.hpp -- which are
// unaffected either way) are not touched.
//   - "DEV_FS_FAT_SUB_SDMMC": esp_board_manager's fs_fat/sdmmc device
//     (managed_components/espressif__esp_board_manager/devices/dev_fs_fat/
//     dev_fs_fat_sub_sdmmc.c)
//   - "BOARD_DEVICE": esp_board_manager_init_device_by_name()'s own
//     "Failed to init device: %s" (src/esp_board_device.c) -- fired on
//     every failed try_mount() alongside the sub-device's own log line,
//     one layer up.
//   - "BOARD_MANAGER": esp_board_manager_init_device_by_name()'s
//     "Failed to initialize device %s" (src/esp_board_manager.c), the
//     outermost layer.
//   - "DEV_FS_FAT": the fs_fat device type's own "Failed to initialize
//     sub device: %s" (devices/dev_fs_fat/dev_fs_fat.c), one layer below
//     DEV_FS_FAT_SUB_SDMMC.
//   - "sdmmc_cmd", "sdmmc_init", "sdmmc_common", "sdmmc_sd": components/
//     sdmmc/{sdmmc_cmd,sdmmc_init,sdmmc_common,sdmmc_sd}.c -- the ACMD41/
//     CMD8 probes that come back empty when no card is present log
//     ESP_LOGE from sdmmc_common.c and sdmmc_sd.c, not just sdmmc_cmd.c/
//     sdmmc_init.c.
//   - "SD_TRANS", "SD_HOST": components/esp_driver_sdmmc/src/
//     {sd_trans_sdmmc,sd_host_sdmmc}.c
//   - "vfs_fat_sdmmc": components/fatfs/vfs/vfs_fat_sdmmc.c
constexpr const char* kChattyMountTags[] = {
    "DEV_FS_FAT_SUB_SDMMC", "BOARD_MANAGER", "BOARD_DEVICE", "DEV_FS_FAT", "sdmmc_cmd",
    "sdmmc_init",           "sdmmc_common",  "sdmmc_sd",     "SD_TRANS",   "SD_HOST",
    "vfs_fat_sdmmc",
};
constexpr size_t kChattyMountTagCount = sizeof(kChattyMountTags) / sizeof(kChattyMountTags[0]);

// RAII: lowers kChattyMountTags to ESP_LOG_NONE for its lifetime, saving
// each tag's previous level and restoring it on destruction -- so a
// *failed* mount attempt doesn't spam the log, while any level a user
// explicitly configured (via menuconfig, or at runtime) is left alone
// everywhere else, including during a successful mount or ordinary
// mounted-card use.
class ScopedMountLogSuppress {
public:
    ScopedMountLogSuppress() {
        for (size_t i = 0; i < kChattyMountTagCount; ++i) {
            saved_[i] = esp_log_level_get(kChattyMountTags[i]);
            esp_log_level_set(kChattyMountTags[i], ESP_LOG_NONE);
        }
    }
    ~ScopedMountLogSuppress() {
        for (size_t i = 0; i < kChattyMountTagCount; ++i) {
            esp_log_level_set(kChattyMountTags[i], saved_[i]);
        }
    }
    ScopedMountLogSuppress(const ScopedMountLogSuppress&) = delete;
    ScopedMountLogSuppress& operator=(const ScopedMountLogSuppress&) = delete;

private:
    esp_log_level_t saved_[kChattyMountTagCount];
};

}  // namespace

// ---------------------------------------------------------------------
// EspInternalStorage
// ---------------------------------------------------------------------

Result<std::shared_ptr<EspInternalStorage>> EspInternalStorage::create() {
    using R = Result<std::shared_ptr<EspInternalStorage>>;

    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = "/int";
    conf.partition_label = kInternalPartitionLabel;
    conf.format_if_mount_failed = true;

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        return R::err(ErrorCode::DeviceFailure,
                      std::string("esp_vfs_littlefs_register: ") + esp_err_to_name(err));
    }
    return R::ok(std::shared_ptr<EspInternalStorage>(new EspInternalStorage()));
}

EspInternalStorage::EspInternalStorage() {
    info_.id = "storage.int";
    info_.cls = kClass;
    info_.model = "littlefs";
    info_.caps =
        wisp::Properties{{"removable", false}, {"fs", wisp::Value(std::string("littlefs"))}};
}

Result<wisp::hal::Capacity> EspInternalStorage::capacity() {
    size_t total = 0;
    size_t used = 0;
    esp_err_t err = esp_littlefs_info(kInternalPartitionLabel, &total, &used);
    if (err != ESP_OK) {
        return Result<wisp::hal::Capacity>::err(
            ErrorCode::IoError, std::string("esp_littlefs_info: ") + esp_err_to_name(err));
    }
    wisp::hal::Capacity cap;
    cap.total_bytes = total;
    cap.free_bytes = used >= total ? 0 : total - used;
    return Result<wisp::hal::Capacity>::ok(cap);
}

// ---------------------------------------------------------------------
// EspSdStorage
// ---------------------------------------------------------------------

EspSdStorage::EspSdStorage(std::string mount_point, std::string bmgr_device_name, std::string model,
                           bool already_mounted, sdmmc_card_t* card)
    : mount_point_(std::move(mount_point)),
      bmgr_device_name_(std::move(bmgr_device_name)),
      retry_(kRetryIntervalMs) {
    info_.id = "storage.sd";
    info_.cls = kClass;
    info_.model = std::move(model);
    info_.caps = wisp::Properties{{"removable", true}, {"fs", wisp::Value(std::string("fat"))}};

    if (already_mounted && card != nullptr) {
        card_ = card;
        retry_.mark_mounted();
    } else {
        // Counts bmgr's own boot-time mount attempt (made by
        // esp_board_manager_init(), before this object exists) as "last
        // attempt now" so the worker's first retry check doesn't
        // immediately fire -- see sd_retry.hpp.
        retry_.mark_unmounted(wisp::os::now_ms());
    }

    wisp::os::TaskConfig config;
    config.name = "wisp.sd";
    // Default stack_bytes (4096): bmgr/sdmmc/fatfs call chains (mount,
    // CMD13, f_getfree) are deep enough that a smaller custom stack isn't
    // worth the risk for what is otherwise meant to be a "modest"/low
    // -priority worker.
    config.priority = 2;  // low: this never blocks anything else on it
    EspSdStorage* self = this;
    worker_ = wisp::os::Task::spawn(config, [self] { self->worker_main(); });
    if (!worker_) {
        // Without this log line, a failed spawn leaves online()/
        // capacity() permanently reporting "no card" (retry_ stuck at
        // Unmounted, nothing ever calls try_mount() again) with no
        // indication why -- hot-plug (and the very first mount attempt)
        // would just never happen.
        WLOG_E(kTag, "storage.sd: failed to spawn worker task; hot-plug detection disabled");
    }
}

EspSdStorage::~EspSdStorage() {
    worker_running_.store(false, std::memory_order_release);
    // The worker may currently be blocked on standby_wake_ (if
    // set_standby(true) was called and never undone before shutdown) --
    // wake it now so this join() below notices worker_running_
    // promptly, rather than waiting out standby_wake_.take()'s own
    // bounded timeout in worker_main().
    standby_wake_.give();
    if (worker_) {
        worker_->join();
    }
}

bool EspSdStorage::online() {
    std::lock_guard<wisp::os::Mutex> lock(state_mutex_);
    if (retry_.state() == SdCardState::Removed) {
        // Safe to do this slow-ish-but-not-card-I/O call right here: the
        // only caller of online() (core::StorageManager::status()/
        // refresh()) always holds StorageManager's own mutex while doing
        // so, which guarantees no FATFS I/O against this mount point is
        // in flight right now -- see esp_storage.hpp's file comment.
        esp_err_t err = esp_board_manager_deinit_device_by_name(bmgr_device_name_.c_str());
        if (err != ESP_OK) {
            WLOG_W(kTag, "storage.sd: deinit after card removal failed: %s", esp_err_to_name(err));
        }
        card_ = nullptr;
        capacity_valid_ = false;
        retry_.mark_unmounted(wisp::os::now_ms());
        return false;
    }
    return retry_.mounted();
}

Result<wisp::hal::Capacity> EspSdStorage::capacity() {
    std::lock_guard<wisp::os::Mutex> lock(state_mutex_);
    if (!retry_.mounted() || !capacity_valid_) {
        return Result<wisp::hal::Capacity>::err(ErrorCode::StorageUnavailable,
                                                "sd card not mounted");
    }
    return Result<wisp::hal::Capacity>::ok(cached_capacity_);
}

void EspSdStorage::set_standby(bool standby) {
    standby_.store(standby, std::memory_order_release);
    if (!standby) {
        standby_wake_.give();
    }
}

bool EspSdStorage::try_mount(uint64_t now) {
    esp_err_t err;
    {
        // Only the attempt itself needs the chatty tags suppressed; a
        // success logs its own clear WLOG_I below regardless.
        ScopedMountLogSuppress suppress;
        err = esp_board_manager_init_device_by_name(bmgr_device_name_.c_str());
    }
    if (err != ESP_OK) {
        std::lock_guard<wisp::os::Mutex> lock(state_mutex_);
        retry_.mark_unmounted(now);
        if (!logged_absent_) {
            WLOG_I(kTag, "storage.sd: no card (bmgr init: %s)", esp_err_to_name(err));
            logged_absent_ = true;
        }
        return false;
    }

    void* handle = nullptr;
    err = esp_board_manager_get_device_handle(bmgr_device_name_.c_str(), &handle);
    if (err != ESP_OK || handle == nullptr) {
        std::lock_guard<wisp::os::Mutex> lock(state_mutex_);
        retry_.mark_unmounted(now);
        return false;
    }

    sdmmc_card_t* card = static_cast<dev_fs_fat_handle_t*>(handle)->card;
    {
        std::lock_guard<wisp::os::Mutex> lock(state_mutex_);
        card_ = card;
        retry_.mark_mounted();
        logged_absent_ = false;
    }
    WLOG_I(kTag, "storage.sd: card mounted at %s", mount_point_.c_str());
    refresh_capacity_cache();
    return true;
}

void EspSdStorage::refresh_capacity_cache() {
    uint64_t total = 0;
    uint64_t free = 0;
    esp_err_t err = esp_vfs_fat_info(mount_point_.c_str(), &total, &free);

    std::lock_guard<wisp::os::Mutex> lock(state_mutex_);
    // The card may have been flagged Removed (or already unmounted by
    // online()) while this ran unlocked; don't resurrect a stale cache
    // for a card that's no longer considered mounted.
    if (!retry_.mounted()) {
        return;
    }
    if (err == ESP_OK) {
        cached_capacity_.total_bytes = total;
        cached_capacity_.free_bytes = free;
        capacity_valid_ = true;
    } else {
        WLOG_W(kTag, "storage.sd: esp_vfs_fat_info failed: %s", esp_err_to_name(err));
        capacity_valid_ = false;
    }
}

void EspSdStorage::worker_main() {
    while (worker_running_.load(std::memory_order_acquire)) {
        // Skip presence probing and mount retry entirely while in
        // standby -- block on standby_wake_ instead of falling through
        // into the switch below at the usual
        // kWorkerPollMs/kProbeIntervalMs/kRetryIntervalMs cadence.
        // set_standby(false) gives standby_wake_ to resume immediately;
        // the destructor also gives it unconditionally. Both of those
        // give() paths are unconditional, so unlike kProbeIntervalMs
        // -style loops elsewhere in this function (which do need a
        // bounded poll, to notice worker_running_ going false without
        // anyone giving anything), there's no missed-wake-up scenario
        // here to guard against with a bounded "safety net" wait --
        // wait indefinitely instead of waking once a second for nothing
        // while standby holds.
        if (standby_.load(std::memory_order_acquire)) {
            standby_wake_.take(wisp::os::kForever);
            continue;
        }

        SdCardState state;
        sdmmc_card_t* card_snapshot;
        {
            std::lock_guard<wisp::os::Mutex> lock(state_mutex_);
            state = retry_.state();
            card_snapshot = card_;
        }

        switch (state) {
            case SdCardState::Mounted: {
                // Also bail out of this wait the moment standby begins,
                // not just when worker_running_ goes false -- without
                // this, standby starting partway
                // through the wait still let one more CMD13 probe (and
                // capacity refresh) through below before the standby_
                // check at the top of the outer loop got another chance
                // to run.
                for (uint32_t waited = 0;
                     waited < kProbeIntervalMs && worker_running_.load(std::memory_order_acquire) &&
                     !standby_.load(std::memory_order_acquire);
                     waited += kWorkerPollMs) {
                    wisp::os::sleep_ms(kWorkerPollMs);
                }
                if (!worker_running_.load(std::memory_order_acquire)) {
                    break;
                }
                if (standby_.load(std::memory_order_acquire)) {
                    // Standby began during the wait above -- skip the
                    // probe entirely and loop back around to the
                    // standby_wake_ wait at the top of worker_main().
                    break;
                }
                // CMD13; safe to run concurrently with FATFS I/O on
                // another task -- see esp_storage.hpp's file comment for
                // the IDF source backing that.
                esp_err_t err = sdmmc_get_status(card_snapshot);
                if (err != ESP_OK) {
                    std::lock_guard<wisp::os::Mutex> lock(state_mutex_);
                    if (retry_.state() == SdCardState::Mounted) {
                        retry_.mark_removed();
                        WLOG_W(kTag, "storage.sd: card lost (status probe: %s)",
                               esp_err_to_name(err));
                    }
                } else {
                    refresh_capacity_cache();
                }
                break;
            }
            case SdCardState::Removed:
                // online() owns the Removed -> Unmounted transition (see
                // sd_retry.hpp's mark_removed() comment); just wait for
                // it to happen.
                wisp::os::sleep_ms(kWorkerPollMs);
                break;
            case SdCardState::Unmounted: {
                uint64_t now = wisp::os::now_ms();
                bool should_try;
                {
                    std::lock_guard<wisp::os::Mutex> lock(state_mutex_);
                    should_try = retry_.should_retry(now);
                }
                if (should_try) {
                    try_mount(now);
                }
                wisp::os::sleep_ms(kWorkerPollMs);
                break;
            }
        }
    }
}

}  // namespace wisp::port::esp
