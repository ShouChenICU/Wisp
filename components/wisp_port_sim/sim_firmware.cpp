#include "wisp/port/sim/sim_firmware.hpp"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "wisp/core/firmware_image.hpp"
#include "wisp/core/log.hpp"
#include "wisp/os/flash_guard.hpp"

namespace wisp::port::sim {

namespace {

constexpr const char* kTag = "firmware";

Result<void> io_error(const char* what) { return Result<void>::err(ErrorCode::IoError, what); }

}  // namespace

SimFirmwareUpdater::SimFirmwareUpdater(std::string data_dir)
    : path_(std::move(data_dir) + "/firmware.bin") {}

SimFirmwareUpdater::~SimFirmwareUpdater() { close_file(); }

core::FirmwareTarget SimFirmwareUpdater::target() const {
    core::FirmwareTarget t;
    t.board = kBoard;
    return t;
}

void SimFirmwareUpdater::close_file() {
    if (file_ != nullptr) std::fclose(file_);
    file_ = nullptr;
}

Result<void> SimFirmwareUpdater::begin(size_t size) {
    WISP_ASSERT_FLASH_THREAD("SimFirmwareUpdater::begin");
    close_file();
    file_ = std::fopen(path_.c_str(), "wb");
    if (file_ == nullptr) return io_error("cannot create the firmware file");
    expected_ = size;
    written_ = 0;
    return Result<void>::ok();
}

Result<void> SimFirmwareUpdater::write(std::span<const uint8_t> chunk) {
    WISP_ASSERT_FLASH_THREAD("SimFirmwareUpdater::write");
    if (file_ == nullptr) return io_error("no firmware upload in progress");
    if (std::fwrite(chunk.data(), 1, chunk.size(), file_) != chunk.size()) {
        return io_error("cannot write the firmware file");
    }
    written_ += chunk.size();
    return Result<void>::ok();
}

Result<core::FirmwareImageInfo> SimFirmwareUpdater::finish() {
    WISP_ASSERT_FLASH_THREAD("SimFirmwareUpdater::finish");
    using R = Result<core::FirmwareImageInfo>;
    if (file_ == nullptr) return R::err(ErrorCode::IoError, "no firmware upload in progress");
    const bool flushed = std::fflush(file_) == 0;
    close_file();
    if (!flushed) return R::err(ErrorCode::IoError, "cannot write the firmware file");
    if (written_ != expected_) return R::err(ErrorCode::InvalidArgument, "image is incomplete");

    // Read back what actually landed, as the device re-reads its slot.
    std::vector<uint8_t> image(written_);
    std::FILE* in = std::fopen(path_.c_str(), "rb");
    if (in == nullptr) return R::err(ErrorCode::IoError, "cannot read the firmware file back");
    const size_t got = std::fread(image.data(), 1, image.size(), in);
    std::fclose(in);
    if (got != image.size()) return R::err(ErrorCode::IoError, "firmware file is short");
    if (auto v = core::verify_firmware_image(image); !v) {
        return R::err(v.error().code, v.error().message);
    }
    return core::parse_firmware_head(image);
}

void SimFirmwareUpdater::abort() {
    WISP_ASSERT_FLASH_THREAD("SimFirmwareUpdater::abort");
    close_file();
    std::remove(path_.c_str());
    expected_ = 0;
    written_ = 0;
}

Result<void> SimFirmwareUpdater::activate() {
    WISP_ASSERT_FLASH_THREAD("SimFirmwareUpdater::activate");
    WLOG_I(kTag, "the simulator cannot boot an ESP image; firmware.bin is kept");
    return Result<void>::ok();
}

}  // namespace wisp::port::sim
