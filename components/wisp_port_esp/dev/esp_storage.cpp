#include "wisp/port/esp/esp_storage.hpp"

#include <string>
#include <utility>

#include "dev_fs_fat.h"
#include "esp_board_manager.h"
#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_vfs_fat.h"
#include "wisp/base/error.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"
#include "wisp/os/os.hpp"

namespace wisp::port::esp {

namespace {

constexpr const char* kTag = "esp_storage";

// Must match board_devices.yaml's fs_sdcard device name.
constexpr const char* kBmgrSdcard = "fs_sdcard";

// Must match firmware/partitions.csv's "storage" partition label.
constexpr const char* kInternalPartitionLabel = "storage";

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

EspSdStorage::EspSdStorage(std::string mount_point, bool already_mounted, sdmmc_card_t* card)
    : mount_point_(std::move(mount_point)), retry_(kRetryIntervalMs) {
    info_.id = "storage.sd";
    info_.cls = kClass;
    info_.model = "sdcard-sdmmc";
    info_.caps = wisp::Properties{{"removable", true}, {"fs", wisp::Value(std::string("fat"))}};

    if (already_mounted && card != nullptr) {
        card_ = card;
        retry_.mark_mounted();
    } else {
        // Counts bmgr's own boot-time mount attempt (made by
        // esp_board_manager_init(), before this object exists) as "last
        // attempt now" so the first periodic online() poll doesn't
        // immediately retry -- see sd_retry.hpp.
        retry_.mark_unmounted(wisp::os::now_ms());
    }
}

bool EspSdStorage::online() {
    uint64_t now = wisp::os::now_ms();

    if (retry_.mounted()) {
        if (card_ != nullptr && sdmmc_get_status(card_) == ESP_OK) {
            return true;
        }
        WLOG_W(kTag, "storage.sd: card lost, unmounting");
        esp_board_manager_deinit_device_by_name(kBmgrSdcard);
        card_ = nullptr;
        retry_.mark_unmounted(now);
        return false;
    }

    if (!retry_.should_retry(now)) {
        return false;
    }

    esp_err_t err = esp_board_manager_init_device_by_name(kBmgrSdcard);
    if (err != ESP_OK) {
        retry_.mark_unmounted(now);
        return false;
    }

    void* handle = nullptr;
    err = esp_board_manager_get_device_handle(kBmgrSdcard, &handle);
    if (err != ESP_OK || handle == nullptr) {
        retry_.mark_unmounted(now);
        return false;
    }

    card_ = static_cast<dev_fs_fat_handle_t*>(handle)->card;
    retry_.mark_mounted();
    WLOG_I(kTag, "storage.sd: card mounted at %s", mount_point_.c_str());
    return true;
}

Result<wisp::hal::Capacity> EspSdStorage::capacity() {
    if (!retry_.mounted()) {
        return Result<wisp::hal::Capacity>::err(ErrorCode::StorageUnavailable,
                                                "sd card not mounted");
    }
    uint64_t total = 0;
    uint64_t free = 0;
    esp_err_t err = esp_vfs_fat_info(mount_point_.c_str(), &total, &free);
    if (err != ESP_OK) {
        return Result<wisp::hal::Capacity>::err(
            ErrorCode::IoError, std::string("esp_vfs_fat_info: ") + esp_err_to_name(err));
    }
    wisp::hal::Capacity cap;
    cap.total_bytes = total;
    cap.free_bytes = free;
    return Result<wisp::hal::Capacity>::ok(cap);
}

}  // namespace wisp::port::esp
