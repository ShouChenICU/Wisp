// CRC-32 (ISO 3309 / ITU-T V.42, the zip and gzip checksum), table-driven
// over the reflected polynomial 0xEDB88320.
#include "wisp/base/crc32.hpp"

#include <array>

namespace wisp {

namespace {

constexpr std::array<uint32_t, 256> make_table() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
        }
        t[i] = c;
    }
    return t;
}

// 1 KB of flash-resident constants, computed at compile time.
constexpr std::array<uint32_t, 256> kTable = make_table();

}  // namespace

uint32_t crc32(std::span<const uint8_t> data, uint32_t crc) {
    uint32_t c = ~crc;
    for (uint8_t b : data) {
        c = kTable[(c ^ b) & 0xFFU] ^ (c >> 8);
    }
    return ~c;
}

}  // namespace wisp
