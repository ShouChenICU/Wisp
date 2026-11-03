#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace wisp {

// One-shot SHA-256 (FIPS 180-4). Portable layers cannot reach mbedTLS, and
// the only callers hash short inputs (session tokens), so a small streaming-
// free implementation is enough.
std::array<uint8_t, 32> sha256(std::span<const uint8_t> data);

// Lowercase hexadecimal rendering of `data`, two characters per byte.
std::string to_hex(std::span<const uint8_t> data);

}  // namespace wisp
