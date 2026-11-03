#pragma once

// core::FirmwareUpdater on ESP-IDF's app_update (connectivity spec 6.5):
// an upload is written with esp_ota_* to the OTA slot the device is not
// running from, erasing as it goes (OTA_WITH_SEQUENTIAL_WRITES), so a
// large image never stalls the HTTP task on one long erase. finish() has
// esp_ota_end() verify the whole image (checksum and SHA-256, as the
// bootloader would) and reads its head back from flash; activate() makes
// it the boot slot. The bootloader's app rollback
// (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) boots a fresh image in a
// pending state: mark_valid() keeps it, and a crash or reset before that
// returns to the previous slot.
//
// create() returns null when the partition table has no second OTA slot;
// the App then reports firmware update as unsupported.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "wisp/core/firmware_update.hpp"

namespace wisp::port::esp {

class EspFirmwareUpdater final : public core::FirmwareUpdater {
public:
    // `board`: the name compiled into this image's board descriptor.
    static std::unique_ptr<EspFirmwareUpdater> create(std::string board);
    ~EspFirmwareUpdater() override;

    EspFirmwareUpdater(const EspFirmwareUpdater&) = delete;
    EspFirmwareUpdater& operator=(const EspFirmwareUpdater&) = delete;

    core::FirmwareTarget target() const override;
    std::string running_slot() const override;
    size_t capacity() const override;
    Result<void> begin(size_t size) override;
    Result<void> write(std::span<const uint8_t> chunk) override;
    Result<core::FirmwareImageInfo> finish() override;
    void abort() override;
    Result<void> activate() override;
    void mark_valid() override;

    struct Impl;

private:
    explicit EspFirmwareUpdater(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace wisp::port::esp
