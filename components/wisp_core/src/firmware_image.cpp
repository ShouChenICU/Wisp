#include "wisp/core/firmware_image.hpp"

#include <array>
#include <charconv>
#include <cstring>
#include <tuple>

#include "wisp/base/sha256.hpp"

namespace wisp::core {

namespace {

constexpr size_t kChipIdOffset = 12;
constexpr size_t kHashAppendedOffset = 23;
constexpr size_t kAppDescVersionOffset = 16;
constexpr size_t kAppDescProjectOffset = 48;
constexpr size_t kAppDescFieldBytes = 32;
constexpr size_t kBoardDescNameOffset = 8;
constexpr size_t kHashBytes = 32;
constexpr uint8_t kChecksumSeed = 0xEF;

uint16_t u16_at(std::span<const uint8_t> b, size_t at) {
    return static_cast<uint16_t>(b[at] | (b[at + 1] << 8));
}

uint32_t u32_at(std::span<const uint8_t> b, size_t at) {
    return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
           (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}

// A fixed-size, NUL-padded C string field; an unterminated one keeps all
// but its last byte, as esp_app_desc_t's writers truncate.
std::string c_string_at(std::span<const uint8_t> b, size_t at, size_t field) {
    const auto* p = reinterpret_cast<const char*>(b.data() + at);
    size_t n = 0;
    while (n + 1 < field && p[n] != '\0') ++n;
    return std::string(p, n);
}

Result<void> invalid(const char* why) { return Result<void>::err(ErrorCode::InvalidArgument, why); }

struct Version {
    unsigned major = 0, minor = 0, patch = 0, commits = 0;
};

bool parse_uint(std::string_view& s, unsigned& out) {
    const char* end = s.data() + s.size();
    auto [p, ec] = std::from_chars(s.data(), end, out);
    if (ec != std::errc() || p == s.data()) return false;
    s.remove_prefix(static_cast<size_t>(p - s.data()));
    return true;
}

bool eat(std::string_view& s, char c) {
    if (s.empty() || s.front() != c) return false;
    s.remove_prefix(1);
    return true;
}

std::optional<Version> parse_version(std::string_view s) {
    Version v;
    if (!s.empty() && s.front() == 'v') s.remove_prefix(1);
    if (!parse_uint(s, v.major) || !eat(s, '.') || !parse_uint(s, v.minor) || !eat(s, '.') ||
        !parse_uint(s, v.patch)) {
        return std::nullopt;
    }
    if (s.empty() || s == "-dirty") return v;
    // -COMMITS-gHASH[-dirty]
    if (!eat(s, '-') || !parse_uint(s, v.commits) || !eat(s, '-') || !eat(s, 'g')) {
        return std::nullopt;
    }
    return v;
}

}  // namespace

Result<FirmwareImageInfo> parse_firmware_head(std::span<const uint8_t> head) {
    using R = Result<FirmwareImageInfo>;
    if (head.size() < kFirmwareHeadBytes) {
        return R::err(ErrorCode::InvalidArgument, "too short for an application image");
    }
    if (head[0] != kImageMagic) {
        return R::err(ErrorCode::InvalidArgument, "not an ESP application image (magic)");
    }
    if (head[1] == 0 || head[1] > kImageMaxSegments) {
        return R::err(ErrorCode::InvalidArgument, "implausible segment count");
    }
    if (u32_at(head, kAppDescOffset) != kAppDescMagic) {
        return R::err(ErrorCode::InvalidArgument, "no application description");
    }
    FirmwareImageInfo info;
    info.chip_id = u16_at(head, kChipIdOffset);
    info.version = c_string_at(head, kAppDescOffset + kAppDescVersionOffset, kAppDescFieldBytes);
    info.project = c_string_at(head, kAppDescOffset + kAppDescProjectOffset, kAppDescFieldBytes);
    if (u32_at(head, kBoardDescOffset) == kBoardDescMagic &&
        u16_at(head, kBoardDescOffset + 4) == kBoardDescVersion) {
        info.board = c_string_at(head, kBoardDescOffset + kBoardDescNameOffset, kBoardNameBytes);
    }
    return R::ok(std::move(info));
}

bool matches_target(const FirmwareImageInfo& info, const FirmwareTarget& target) {
    if (info.project != target.project) return false;
    if (info.board.empty() || info.board != target.board) return false;
    return !target.chip_id || *target.chip_id == info.chip_id;
}

Result<void> verify_firmware_image(std::span<const uint8_t> image) {
    if (image.size() < kImageHeaderBytes || image[0] != kImageMagic) {
        return invalid("not an ESP application image");
    }
    const size_t segments = image[1];
    if (segments == 0 || segments > kImageMaxSegments) return invalid("implausible segment count");

    uint8_t checksum = kChecksumSeed;
    size_t pos = kImageHeaderBytes;
    for (size_t i = 0; i < segments; ++i) {
        if (image.size() - pos < kSegmentHeaderBytes) return invalid("truncated segment header");
        const uint32_t len = u32_at(image, pos + 4);
        pos += kSegmentHeaderBytes;
        if (len % 4 != 0) return invalid("unaligned segment length");
        if (image.size() - pos < len) return invalid("truncated segment");
        for (uint8_t b : image.subspan(pos, len)) checksum ^= b;
        pos += len;
    }
    // The checksum byte ends a 16-byte block.
    const size_t checksum_at = ((pos + 1 + 15) & ~size_t{15}) - 1;
    if (image.size() <= checksum_at) return invalid("truncated before the checksum");
    if (image[checksum_at] != checksum) return invalid("checksum mismatch");
    if (image[kHashAppendedOffset] == 1) {
        const size_t hashed = checksum_at + 1;
        if (image.size() - hashed < kHashBytes) return invalid("truncated before the hash");
        const std::array<uint8_t, 32> digest = sha256(image.first(hashed));
        if (std::memcmp(digest.data(), image.data() + hashed, kHashBytes) != 0) {
            return invalid("SHA-256 mismatch");
        }
    }
    return Result<void>::ok();
}

std::optional<int> compare_firmware_versions(std::string_view a, std::string_view b) {
    auto va = parse_version(a);
    auto vb = parse_version(b);
    if (!va || !vb) return std::nullopt;
    auto key = [](const Version& v) { return std::tie(v.major, v.minor, v.patch, v.commits); };
    if (key(*va) < key(*vb)) return -1;
    if (key(*vb) < key(*va)) return 1;
    return 0;
}

}  // namespace wisp::core
