#include "wisp/port/esp/esp_firmware.hpp"

#include <array>
#include <string>
#include <utility>

#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "sdkconfig.h"
#include "wisp/core/firmware_image.hpp"
#include "wisp/core/log.hpp"

namespace wisp::port::esp {

namespace {

constexpr const char* kTag = "firmware";

Result<void> esp_failure(const char* what, esp_err_t err) {
    return Result<void>::err(ErrorCode::IoError, std::string(what) + ": " + esp_err_to_name(err));
}

}  // namespace

struct EspFirmwareUpdater::Impl {
    std::string board;
    const esp_partition_t* running = nullptr;
    // The slot uploads go to: the next one after the running image.
    const esp_partition_t* target = nullptr;
    esp_ota_handle_t handle = 0;
    bool writing = false;   // handle is open
    bool finished = false;  // target holds a verified image
};

std::unique_ptr<EspFirmwareUpdater> EspFirmwareUpdater::create(std::string board) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (running == nullptr || next == nullptr || next == running) {
        WLOG_W(kTag, "no second OTA slot: firmware upload is disabled");
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->board = std::move(board);
    impl->running = running;
    impl->target = next;
    return std::unique_ptr<EspFirmwareUpdater>(new EspFirmwareUpdater(std::move(impl)));
}

EspFirmwareUpdater::EspFirmwareUpdater(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

EspFirmwareUpdater::~EspFirmwareUpdater() { abort(); }

core::FirmwareTarget EspFirmwareUpdater::target() const {
    core::FirmwareTarget t;
    t.board = impl_->board;
    t.chip_id = static_cast<uint16_t>(CONFIG_IDF_FIRMWARE_CHIP_ID);
    return t;
}

std::string EspFirmwareUpdater::running_slot() const { return impl_->running->label; }

size_t EspFirmwareUpdater::capacity() const { return impl_->target->size; }

Result<void> EspFirmwareUpdater::begin(size_t size) {
    abort();
    if (size > impl_->target->size) {
        return Result<void>::err(ErrorCode::InvalidArgument, "image larger than the slot");
    }
    esp_ota_handle_t h = 0;
    // Erases sector by sector as the writes arrive.
    const esp_err_t err = esp_ota_begin(impl_->target, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) return esp_failure("esp_ota_begin", err);
    impl_->handle = h;
    impl_->writing = true;
    return Result<void>::ok();
}

Result<void> EspFirmwareUpdater::write(std::span<const uint8_t> chunk) {
    if (!impl_->writing) {
        return Result<void>::err(ErrorCode::IoError, "no firmware upload in progress");
    }
    const esp_err_t err = esp_ota_write(impl_->handle, chunk.data(), chunk.size());
    if (err != ESP_OK) return esp_failure("esp_ota_write", err);
    return Result<void>::ok();
}

Result<core::FirmwareImageInfo> EspFirmwareUpdater::finish() {
    using R = Result<core::FirmwareImageInfo>;
    if (!impl_->writing) return R::err(ErrorCode::IoError, "no firmware upload in progress");
    impl_->writing = false;
    // Verifies the image in the slot (and frees the handle either way).
    const esp_err_t err = esp_ota_end(impl_->handle);
    if (err != ESP_OK) {
        return R::err(
            err == ESP_ERR_OTA_VALIDATE_FAILED ? ErrorCode::InvalidArgument : ErrorCode::IoError,
            std::string("esp_ota_end: ") + esp_err_to_name(err));
    }
    // What the bootloader will see, not what the client claimed.
    std::array<uint8_t, core::kFirmwareHeadBytes> head{};
    const esp_err_t rerr = esp_partition_read(impl_->target, 0, head.data(), head.size());
    if (rerr != ESP_OK) {
        return R::err(ErrorCode::IoError,
                      std::string("esp_partition_read: ") + esp_err_to_name(rerr));
    }
    auto info = core::parse_firmware_head(head);
    if (info) impl_->finished = true;
    return info;
}

void EspFirmwareUpdater::abort() {
    if (impl_->writing) esp_ota_abort(impl_->handle);
    impl_->writing = false;
    // A verified image left in the slot is harmless: the boot slot was
    // never switched to it, and the next begin() erases it.
    impl_->finished = false;
}

Result<void> EspFirmwareUpdater::activate() {
    if (!impl_->finished) {
        return Result<void>::err(ErrorCode::IoError, "no verified image to activate");
    }
    // Verifies the image once more before switching otadata.
    const esp_err_t err = esp_ota_set_boot_partition(impl_->target);
    if (err != ESP_OK) return esp_failure("esp_ota_set_boot_partition", err);
    WLOG_I(kTag, "next boot from %s", impl_->target->label);
    return Result<void>::ok();
}

void EspFirmwareUpdater::mark_valid() {
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(impl_->running, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return;
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        WLOG_I(kTag, "%s started properly; rollback cancelled", impl_->running->label);
    } else {
        WLOG_E(kTag, "could not mark %s valid: %s", impl_->running->label, esp_err_to_name(err));
    }
}

}  // namespace wisp::port::esp
