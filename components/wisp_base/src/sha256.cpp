// SHA-256 written from the FIPS 180-4 specification (sections 4.1.2, 4.2.2,
// 5.1.1, 5.3.3 and 6.2).
#include "wisp/base/sha256.hpp"

#include <cstddef>

namespace wisp {

namespace {

constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr uint32_t kInitialHash[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

constexpr uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void compress(uint32_t h[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int t = 0; t < 16; ++t) {
        w[t] = (uint32_t(block[4 * t]) << 24) | (uint32_t(block[4 * t + 1]) << 16) |
               (uint32_t(block[4 * t + 2]) << 8) | uint32_t(block[4 * t + 3]);
    }
    for (int t = 16; t < 64; ++t) {
        const uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
        const uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int t = 0; t < 64; ++t) {
        const uint32_t big_s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = hh + big_s1 + ch + kRoundConstants[t] + w[t];
        const uint32_t big_s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = big_s0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

}  // namespace

std::array<uint8_t, 32> sha256(std::span<const uint8_t> data) {
    uint32_t h[8];
    for (int i = 0; i < 8; ++i) h[i] = kInitialHash[i];

    const size_t full_blocks = data.size() / 64;
    for (size_t i = 0; i < full_blocks; ++i) compress(h, data.data() + i * 64);

    // Padding (5.1.1): the remainder, a 0x80 byte, zeros, then the 64-bit
    // big-endian bit length. When the remainder leaves fewer than 9 bytes
    // the padding spills into a second block.
    uint8_t tail[128] = {};
    const size_t rem = data.size() - full_blocks * 64;
    for (size_t i = 0; i < rem; ++i) tail[i] = data[full_blocks * 64 + i];
    tail[rem] = 0x80;
    const size_t tail_len = rem + 9 <= 64 ? 64 : 128;
    const uint64_t bit_len = uint64_t(data.size()) * 8;
    for (int i = 0; i < 8; ++i) tail[tail_len - 1 - i] = uint8_t(bit_len >> (8 * i));
    compress(h, tail);
    if (tail_len == 128) compress(h, tail + 64);

    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) {
        out[4 * i] = uint8_t(h[i] >> 24);
        out[4 * i + 1] = uint8_t(h[i] >> 16);
        out[4 * i + 2] = uint8_t(h[i] >> 8);
        out[4 * i + 3] = uint8_t(h[i]);
    }
    return out;
}

std::string to_hex(std::span<const uint8_t> data) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (uint8_t b : data) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0f]);
    }
    return out;
}

}  // namespace wisp
