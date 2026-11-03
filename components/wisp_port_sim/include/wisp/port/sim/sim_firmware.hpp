#pragma once

// The simulator's core::FirmwareUpdater (connectivity spec 6.5): an
// uploaded image goes to <data-dir>/firmware.bin, standing in for the
// inactive OTA slot. finish() reads the file back and checks it with the
// same portable code the device relies on (core::verify_firmware_image),
// so the smoke test covers the whole flow. Images must be built for board
// "simulator"; any chip is accepted. activate() only logs: the simulator
// cannot boot an ESP image, and the App restarts (WISP_REBOOT) as on the
// device.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>

#include "wisp/core/firmware_update.hpp"

namespace wisp::port::sim {

class SimFirmwareUpdater final : public core::FirmwareUpdater {
public:
    static constexpr const char* kBoard = "simulator";
    // The reference board's ota_0 (5 MiB).
    static constexpr size_t kCapacity = 5 * 1024 * 1024;

    explicit SimFirmwareUpdater(std::string data_dir);
    ~SimFirmwareUpdater() override;

    SimFirmwareUpdater(const SimFirmwareUpdater&) = delete;
    SimFirmwareUpdater& operator=(const SimFirmwareUpdater&) = delete;

    core::FirmwareTarget target() const override;
    std::string running_slot() const override { return "sim"; }
    size_t capacity() const override { return kCapacity; }
    Result<void> begin(size_t size) override;
    Result<void> write(std::span<const uint8_t> chunk) override;
    Result<core::FirmwareImageInfo> finish() override;
    void abort() override;
    Result<void> activate() override;
    void mark_valid() override {}

    const std::string& image_path() const { return path_; }

private:
    void close_file();

    const std::string path_;
    std::FILE* file_ = nullptr;
    size_t expected_ = 0;
    size_t written_ = 0;
};

}  // namespace wisp::port::sim
