#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace wisp::port::sim {

// One-shot SHA-1 (FIPS 180-4), used only to compute the WebSocket
// handshake's Sec-WebSocket-Accept (RFC 6455 section 4.2.2). SHA-1 is not
// collision resistant: never use it for anything security-relevant.
std::array<uint8_t, 20> sha1(std::span<const uint8_t> data);

}  // namespace wisp::port::sim
