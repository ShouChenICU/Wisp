#pragma once

// Reads ESP-IDF application images as plain bytes (connectivity spec 6.5),
// so the firmware upload checks an image the same way on the device and in
// the simulator. Nothing here includes an ESP-IDF header: the layouts below
// are restated from ESP-IDF's esp_app_format.h (esp_image_header_t,
// esp_image_segment_header_t) and esp_app_desc.h (esp_app_desc_t), whose
// sizes and offsets are fixed by the image format itself.
//
// Every Wisp image starts the same way:
//
//   offset   0  esp_image_header_t (24 bytes): magic 0xE9, segment count,
//               ..., chip id (u16 at 12), ..., hash_appended (u8 at 23)
//   offset  24  the first segment's header (8 bytes: load address, length)
//   offset  32  esp_app_desc_t (256 bytes): magic 0xABCD5432, ...,
//               version[32] at 16, project_name[32] at 48
//   offset 288  Wisp's board descriptor (kBoardDesc*), linked right after
//               esp_app_desc_t through the ".rodata_custom_desc" section
//               (firmware/main/board_desc.c)
//
// After the last segment comes one checksum byte (0xEF XOR every segment
// data byte) at the end of a 16-byte block and, when hash_appended is set,
// the SHA-256 of everything before it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "wisp/base/result.hpp"

namespace wisp::core {

inline constexpr size_t kImageHeaderBytes = 24;
inline constexpr size_t kSegmentHeaderBytes = 8;
inline constexpr size_t kAppDescOffset = kImageHeaderBytes + kSegmentHeaderBytes;
inline constexpr size_t kAppDescBytes = 256;
inline constexpr size_t kBoardDescOffset = kAppDescOffset + kAppDescBytes;
// Board descriptor: u32 magic, u16 version, u16 reserved, char board[40]
// (NUL-terminated, so at most 39 characters).
inline constexpr size_t kBoardDescBytes = 48;
inline constexpr size_t kBoardNameBytes = 40;
// The bytes an upload must have received before it can be identified.
inline constexpr size_t kFirmwareHeadBytes = kBoardDescOffset + kBoardDescBytes;

inline constexpr uint8_t kImageMagic = 0xE9;
inline constexpr size_t kImageMaxSegments = 16;
inline constexpr uint32_t kAppDescMagic = 0xABCD5432;
// "WSPB" as a little-endian u32.
inline constexpr uint32_t kBoardDescMagic = 0x42505357;
inline constexpr uint16_t kBoardDescVersion = 1;

struct FirmwareImageInfo {
    uint16_t chip_id = 0;
    std::string project;
    std::string version;
    // Empty when the image carries no Wisp board descriptor.
    std::string board;
};

// What an uploaded image must be built for.
struct FirmwareTarget {
    std::string project = "wisp";
    std::string board;
    // nullopt accepts any chip (the simulator is none).
    std::optional<uint16_t> chip_id;
};

// Identifies an image from its first kFirmwareHeadBytes bytes. Fails with
// InvalidArgument when they are not an ESP application image: too few
// bytes, a wrong header magic, a segment count of 0 or over 16, or no
// esp_app_desc_t where it belongs; or when its version, project or board
// name holds anything but printable ASCII (they end up in logs, events and
// on the screen). The fields' sizes bound their lengths (31 and 39). A
// missing board descriptor is not an error here (`board` stays empty):
// matches_target() refuses it.
Result<FirmwareImageInfo> parse_firmware_head(std::span<const uint8_t> head);

// Same project, same board name and (when the target names one) same chip.
bool matches_target(const FirmwareImageInfo& info, const FirmwareTarget& target);

// Checks a whole image the way the bootloader does: every segment fits and
// has a length that is a multiple of 4, the checksum byte matches and, when
// the header says one is appended, so does the SHA-256. Bytes after the
// image are ignored (an OTA slot is larger than the image it holds).
// InvalidArgument naming the first problem.
Result<void> verify_firmware_image(std::span<const uint8_t> image);

// Orders two versions of the form [v]MAJOR.MINOR.PATCH[-COMMITS-gHASH]
// [-dirty] (git describe on a release tag, and what firmware/CMakeLists.txt
// builds without one): by the three numbers, then by the commit count
// (absent counts as 0). Negative, zero or positive like strcmp; nullopt
// when either does not have that form.
std::optional<int> compare_firmware_versions(std::string_view a, std::string_view b);

}  // namespace wisp::core
