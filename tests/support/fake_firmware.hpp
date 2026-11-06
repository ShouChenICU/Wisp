#pragma once

// Test doubles for the firmware upload (wisp/core/firmware_update.hpp):
//   - make_firmware_image() builds a small but complete ESP application
//     image (header, segments, checksum, appended SHA-256) carrying an
//     esp_app_desc_t and a Wisp board descriptor, laid out as
//     wisp/core/firmware_image.hpp describes;
//   - FakeFirmwareUpdater records what the service asks of the platform.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/base/sha256.hpp"
#include "wisp/core/firmware_image.hpp"
#include "wisp/core/firmware_update.hpp"
#include "wisp/os/flash_guard.hpp"

namespace wisp::test {

// ESP32-S3's esp_image_header_t::chip_id.
inline constexpr uint16_t kChipEsp32s3 = 0x0009;

struct ImageSpec {
    std::string project = "wisp";
    std::string version = "0.1.0-5-gabcdef0";
    std::string board = "test_board";
    uint16_t chip_id = kChipEsp32s3;
    bool board_desc = true;
    bool hash_appended = true;
    // Bytes of filler data in a second segment (a multiple of 4).
    size_t extra_bytes = 4096;
};

inline void put_u16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
    b[at] = static_cast<uint8_t>(v);
    b[at + 1] = static_cast<uint8_t>(v >> 8);
}

inline void put_u32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

inline void put_str(std::vector<uint8_t>& b, size_t at, const std::string& s, size_t field) {
    std::memcpy(b.data() + at, s.data(), std::min(s.size(), field - 1));
}

inline std::vector<uint8_t> make_firmware_image(const ImageSpec& spec = {}) {
    using namespace wisp::core;
    // Segment 0: esp_app_desc_t + board descriptor (or zeros).
    std::vector<uint8_t> seg0(kAppDescBytes + kBoardDescBytes, 0);
    put_u32(seg0, 0, kAppDescMagic);
    put_str(seg0, 16, spec.version, 32);
    put_str(seg0, 48, spec.project, 32);
    if (spec.board_desc) {
        put_u32(seg0, kAppDescBytes, kBoardDescMagic);
        put_u16(seg0, kAppDescBytes + 4, kBoardDescVersion);
        put_str(seg0, kAppDescBytes + 8, spec.board, kBoardNameBytes);
    }
    std::vector<uint8_t> seg1(spec.extra_bytes);
    for (size_t i = 0; i < seg1.size(); ++i) seg1[i] = static_cast<uint8_t>(i * 7 + 3);

    std::vector<uint8_t> img(kImageHeaderBytes, 0);
    img[0] = kImageMagic;
    img[1] = 2;     // segment count
    img[2] = 2;     // DIO
    img[3] = 0x4F;  // 80 MHz, 16 MB
    put_u32(img, 4, 0x40370000);
    put_u16(img, 12, spec.chip_id);
    img[23] = spec.hash_appended ? 1 : 0;

    uint8_t checksum = 0xEF;
    uint32_t load = 0x3C000020;
    for (const auto* seg : {&seg0, &seg1}) {
        std::vector<uint8_t> h(kSegmentHeaderBytes, 0);
        put_u32(h, 0, load);
        put_u32(h, 4, static_cast<uint32_t>(seg->size()));
        img.insert(img.end(), h.begin(), h.end());
        img.insert(img.end(), seg->begin(), seg->end());
        for (uint8_t byte : *seg) checksum ^= byte;
        load += static_cast<uint32_t>(seg->size());
    }
    // Pad so the checksum is the last byte of a 16-byte block.
    while ((img.size() + 1) % 16 != 0) img.push_back(0);
    img.push_back(checksum);
    if (spec.hash_appended) {
        auto digest = wisp::sha256(img);
        img.insert(img.end(), digest.begin(), digest.end());
    }
    return img;
}

// Records every call; failures are injected per method.
class FakeFirmwareUpdater : public wisp::core::FirmwareUpdater {
public:
    wisp::core::FirmwareTarget target() const override { return target_value; }
    std::string running_slot() const override { return "ota_0"; }
    size_t capacity() const override { return capacity_bytes; }

    Result<void> begin(size_t size) override {
        // Outside the lock, so a test can hold the flash thread in here.
        if (on_begin) on_begin();
        std::lock_guard<std::mutex> lock(mutex);
        ++begins;
        note_thread();
        written.clear();
        began_size = size;
        if (fail_begin) return Result<void>::err(ErrorCode::IoError, "fake begin failure");
        return Result<void>::ok();
    }
    Result<void> write(std::span<const uint8_t> chunk) override {
        if (on_write) on_write();
        std::lock_guard<std::mutex> lock(mutex);
        ++writes;
        note_thread();
        write_sizes.push_back(chunk.size());
        if (fail_write) return Result<void>::err(ErrorCode::IoError, "fake write failure");
        written.insert(written.end(), chunk.begin(), chunk.end());
        return Result<void>::ok();
    }
    Result<wisp::core::FirmwareImageInfo> finish() override {
        std::lock_guard<std::mutex> lock(mutex);
        ++finishes;
        note_thread();
        finish_on_flash_thread = wisp::os::on_flash_thread();
        if (auto v = wisp::core::verify_firmware_image(written); !v) {
            return Result<wisp::core::FirmwareImageInfo>::err(v.error().code, v.error().message);
        }
        return wisp::core::parse_firmware_head(written);
    }
    void abort() override {
        std::lock_guard<std::mutex> lock(mutex);
        ++aborts;
        note_thread();
    }
    Result<void> activate() override {
        std::lock_guard<std::mutex> lock(mutex);
        ++activations;
        note_thread();
        if (fail_activate) return Result<void>::err(ErrorCode::IoError, "fake activate failure");
        return Result<void>::ok();
    }
    void mark_valid() override {
        std::lock_guard<std::mutex> lock(mutex);
        ++mark_valid_calls;
        note_thread();
        if (on_mark_valid) on_mark_valid();
    }

    wisp::core::FirmwareTarget target_value{"wisp", "test_board", kChipEsp32s3};
    size_t capacity_bytes = 5 * 1024 * 1024;
    bool fail_begin = false;
    bool fail_write = false;
    bool fail_activate = false;
    std::function<void()> on_mark_valid;
    // Run at the top of begin()/write(), before the lock.
    std::function<void()> on_begin;
    std::function<void()> on_write;

    mutable std::mutex mutex;
    int begins = 0;
    int writes = 0;
    int finishes = 0;
    int aborts = 0;
    int activations = 0;
    int mark_valid_calls = 0;
    // Calls that touch flash (all but target()/running_slot()/capacity())
    // made off the flash thread while one is registered.
    std::atomic<int> off_flash_thread{0};
    bool finish_on_flash_thread = false;
    size_t began_size = 0;
    std::vector<size_t> write_sizes;
    std::vector<uint8_t> written;

private:
    void note_thread() {
        if (!wisp::os::on_flash_thread()) off_flash_thread.fetch_add(1);
    }
};

}  // namespace wisp::test
