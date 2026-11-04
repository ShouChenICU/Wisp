#pragma once

#include <cstdint>
#include <span>

namespace wisp {

// CRC-32 as used by zip and gzip (ISO 3309 / ITU-T V.42: reflected
// polynomial 0xEDB88320, initial value and final XOR 0xFFFFFFFF). `crc` is
// the result of a previous call, so a long input can be fed in pieces:
// crc32(b, crc32(a)) == crc32(a ++ b).
uint32_t crc32(std::span<const uint8_t> data, uint32_t crc = 0);

}  // namespace wisp
