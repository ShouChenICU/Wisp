// SHA-1 written from FIPS 180-4 (section 6.1) and RFC 3174; no third-party
// code was copied. Only the WebSocket handshake uses it (see sha1.hpp).

#include "sha1.hpp"

#include <cstring>

namespace wisp::port::sim {

namespace {

uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

void compress(std::array<uint32_t, 5>& h, const uint8_t* block) {
    uint32_t w[80];
    for (int t = 0; t < 16; ++t) {
        w[t] = (uint32_t{block[4 * t]} << 24) | (uint32_t{block[4 * t + 1]} << 16) |
               (uint32_t{block[4 * t + 2]} << 8) | uint32_t{block[4 * t + 3]};
    }
    for (int t = 16; t < 80; ++t) w[t] = rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int t = 0; t < 80; ++t) {
        uint32_t f;
        uint32_t k;
        if (t < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (t < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (t < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        const uint32_t temp = rotl(a, 5) + f + e + k + w[t];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = temp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

}  // namespace

std::array<uint8_t, 20> sha1(std::span<const uint8_t> data) {
    std::array<uint32_t, 5> h = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};

    const size_t full_blocks = data.size() / 64;
    for (size_t i = 0; i < full_blocks; ++i) compress(h, data.data() + 64 * i);

    // Padding: 0x80, zeros, then the 64-bit big-endian bit length; it
    // spills into a second block when fewer than 9 bytes remain.
    uint8_t tail[128] = {};
    const size_t rest = data.size() % 64;
    if (rest > 0) std::memcpy(tail, data.data() + 64 * full_blocks, rest);
    tail[rest] = 0x80;
    const size_t tail_size = rest < 56 ? 64 : 128;
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    for (int i = 0; i < 8; ++i) {
        tail[tail_size - 1 - i] = static_cast<uint8_t>(bits >> (8 * i));
    }
    compress(h, tail);
    if (tail_size == 128) compress(h, tail + 64);

    std::array<uint8_t, 20> out{};
    for (size_t i = 0; i < 5; ++i) {
        out[4 * i] = static_cast<uint8_t>(h[i] >> 24);
        out[4 * i + 1] = static_cast<uint8_t>(h[i] >> 16);
        out[4 * i + 2] = static_cast<uint8_t>(h[i] >> 8);
        out[4 * i + 3] = static_cast<uint8_t>(h[i]);
    }
    return out;
}

}  // namespace wisp::port::sim
