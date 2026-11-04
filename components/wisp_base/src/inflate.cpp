// Raw DEFLATE decoder written from RFC 1951 (sections 3.2.1-3.2.7).
//
// Huffman codes are kept in canonical form (RFC 1951 3.2.2): per code
// length, how many codes have that length, plus the symbols sorted by
// code. Decoding walks the code one bit at a time, comparing it with the
// first code of each length. That needs no lookup tables, so the whole
// decoder state (the Decoder struct below, about 1.5 KB) fits on a small
// task stack; the price is speed, which does not matter for the few
// hundred KB a scene package holds.
#include "wisp/base/inflate.hpp"

#include <cstring>
#include <string>

namespace wisp {

namespace {

constexpr int kMaxBits = 15;          // longest Huffman code
constexpr int kMaxLitLen = 288;       // literal/length alphabet incl. the two unused codes
constexpr int kMaxDist = 32;          // distance alphabet incl. the two unused codes
constexpr int kNumLitLenCodes = 286;  // valid literal/length symbols
constexpr int kNumDistCodes = 30;     // valid distance symbols
constexpr int kEndOfBlock = 256;

// RFC 1951 3.2.5: base lengths and extra bits for length symbols 257..285.
constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
// Base distances and extra bits for distance symbols 0..29.
constexpr uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                    33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
// RFC 1951 3.2.7: the order in which code-length code lengths are sent.
constexpr uint8_t kCodeLenOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                       11, 4,  12, 3, 13, 2, 14, 1, 15};

template <int N>
struct Huffman {
    uint16_t count[kMaxBits + 1];  // count[len]: number of codes of that length
    uint16_t symbol[N];            // symbols ordered by (length, code)
};

// Builds the canonical code for `lengths[0..n)`. Rejects an
// over-subscribed set of lengths. An incomplete code is accepted (RFC 1951
// allows a single distance code, and zlib emits such streams); a bit
// pattern that maps to no symbol fails later in decode().
template <int N>
bool build(Huffman<N>& h, const uint8_t* lengths, int n) {
    std::memset(h.count, 0, sizeof(h.count));
    for (int s = 0; s < n; ++s) {
        ++h.count[lengths[s]];
    }
    if (h.count[0] == n) {
        return true;  // no codes at all; any decode attempt fails
    }
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left = (left << 1) - h.count[len];
        if (left < 0) {
            return false;
        }
    }
    uint16_t offset[kMaxBits + 2];
    offset[1] = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
        offset[len + 1] = static_cast<uint16_t>(offset[len] + h.count[len]);
    }
    for (int s = 0; s < n; ++s) {
        if (lengths[s] != 0) {
            h.symbol[offset[lengths[s]]++] = static_cast<uint16_t>(s);
        }
    }
    return true;
}

struct Decoder {
    std::span<const uint8_t> in;
    size_t in_pos = 0;
    uint32_t bit_buf = 0;
    int bit_count = 0;

    std::span<uint8_t> out;
    size_t out_pos = 0;

    Huffman<kMaxLitLen> litlen;
    Huffman<kMaxDist> dist;

    // Reads `n` (<= 16) bits, least significant first (RFC 1951 3.1.1).
    bool bits(int n, uint32_t& v) {
        while (bit_count < n) {
            if (in_pos >= in.size()) {
                return false;
            }
            bit_buf |= static_cast<uint32_t>(in[in_pos++]) << bit_count;
            bit_count += 8;
        }
        v = bit_buf & ((1U << n) - 1U);
        bit_buf >>= n;
        bit_count -= n;
        return true;
    }

    // Huffman codes are packed most significant bit first, so the code is
    // assembled one bit at a time. `first` is the first code of the
    // current length and `index` the position of its symbol.
    template <int N>
    bool decode(const Huffman<N>& h, int& sym) {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            uint32_t b = 0;
            if (!bits(1, b)) {
                return false;
            }
            code |= static_cast<int>(b);
            const int count = h.count[len];
            if (code - first < count) {
                sym = h.symbol[index + (code - first)];
                return true;
            }
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return false;
    }

    bool stored() {
        // The block starts at the next byte boundary; whole bytes already
        // pulled into bit_buf cannot exist because bits() only loads what
        // it needs.
        bit_buf = 0;
        bit_count = 0;
        if (in.size() - in_pos < 4) {
            return false;
        }
        const uint32_t len = in[in_pos] | (static_cast<uint32_t>(in[in_pos + 1]) << 8);
        const uint32_t nlen = in[in_pos + 2] | (static_cast<uint32_t>(in[in_pos + 3]) << 8);
        in_pos += 4;
        if ((len ^ 0xFFFFU) != nlen) {
            return false;
        }
        if (in.size() - in_pos < len || out.size() - out_pos < len) {
            return false;
        }
        // An empty `out` may have a null data(), which memcpy rejects even
        // for zero bytes.
        if (len != 0) {
            std::memcpy(out.data() + out_pos, in.data() + in_pos, len);
        }
        in_pos += len;
        out_pos += len;
        return true;
    }

    bool codes() {
        for (;;) {
            int sym = 0;
            if (!decode(litlen, sym)) {
                return false;
            }
            if (sym < kEndOfBlock) {
                if (out_pos >= out.size()) {
                    return false;
                }
                out[out_pos++] = static_cast<uint8_t>(sym);
                continue;
            }
            if (sym == kEndOfBlock) {
                return true;
            }
            sym -= 257;
            if (sym >= 29) {
                return false;  // 286 and 287 never occur in valid data
            }
            uint32_t extra = 0;
            if (!bits(kLenExtra[sym], extra)) {
                return false;
            }
            const size_t len = kLenBase[sym] + extra;

            int dsym = 0;
            if (!decode(dist, dsym) || dsym >= kNumDistCodes) {
                return false;
            }
            if (!bits(kDistExtra[dsym], extra)) {
                return false;
            }
            const size_t distance = kDistBase[dsym] + extra;
            if (distance > out_pos || out.size() - out_pos < len) {
                return false;
            }
            // Byte by byte: when distance < len the copy reads bytes it
            // has just written, which is how DEFLATE encodes runs.
            uint8_t* dst = out.data() + out_pos;
            const uint8_t* src = dst - distance;
            for (size_t i = 0; i < len; ++i) {
                dst[i] = src[i];
            }
            out_pos += len;
        }
    }

    bool fixed() {
        uint8_t lengths[kMaxLitLen];
        // RFC 1951 3.2.6.
        int s = 0;
        for (; s < 144; ++s) lengths[s] = 8;
        for (; s < 256; ++s) lengths[s] = 9;
        for (; s < 280; ++s) lengths[s] = 7;
        for (; s < kMaxLitLen; ++s) lengths[s] = 8;
        build(litlen, lengths, kMaxLitLen);
        for (s = 0; s < kMaxDist; ++s) lengths[s] = 5;
        build(dist, lengths, kMaxDist);
        return codes();
    }

    bool dynamic() {
        uint32_t hlit = 0;
        uint32_t hdist = 0;
        uint32_t hclen = 0;
        if (!bits(5, hlit) || !bits(5, hdist) || !bits(4, hclen)) {
            return false;
        }
        const int nlen = static_cast<int>(hlit) + 257;
        const int ndist = static_cast<int>(hdist) + 1;
        const int ncode = static_cast<int>(hclen) + 4;
        if (nlen > kNumLitLenCodes || ndist > kNumDistCodes) {
            return false;
        }

        uint8_t lengths[kNumLitLenCodes + kNumDistCodes] = {};
        for (int i = 0; i < ncode; ++i) {
            uint32_t v = 0;
            if (!bits(3, v)) {
                return false;
            }
            lengths[kCodeLenOrder[i]] = static_cast<uint8_t>(v);
        }
        // The code-length code borrows the literal/length table, which is
        // rebuilt from the decoded lengths right after.
        if (!build(litlen, lengths, 19)) {
            return false;
        }

        int i = 0;
        while (i < nlen + ndist) {
            int sym = 0;
            if (!decode(litlen, sym)) {
                return false;
            }
            if (sym < 16) {
                lengths[i++] = static_cast<uint8_t>(sym);
                continue;
            }
            uint8_t value = 0;
            uint32_t repeat = 0;
            if (sym == 16) {
                if (i == 0 || !bits(2, repeat)) {
                    return false;
                }
                value = lengths[i - 1];
                repeat += 3;
            } else if (sym == 17) {
                if (!bits(3, repeat)) {
                    return false;
                }
                repeat += 3;
            } else {
                if (!bits(7, repeat)) {
                    return false;
                }
                repeat += 11;
            }
            if (i + static_cast<int>(repeat) > nlen + ndist) {
                return false;
            }
            while (repeat-- > 0) {
                lengths[i++] = value;
            }
        }
        if (lengths[kEndOfBlock] == 0) {
            return false;  // a block that cannot end
        }
        if (!build(litlen, lengths, nlen) || !build(dist, lengths + nlen, ndist)) {
            return false;
        }
        return codes();
    }

    bool run() {
        for (;;) {
            uint32_t last = 0;
            uint32_t type = 0;
            if (!bits(1, last) || !bits(2, type)) {
                return false;
            }
            bool ok = false;
            switch (type) {
                case 0:
                    ok = stored();
                    break;
                case 1:
                    ok = fixed();
                    break;
                case 2:
                    ok = dynamic();
                    break;
                default:
                    return false;  // reserved block type
            }
            if (!ok) {
                return false;
            }
            if (last != 0) {
                return true;
            }
        }
    }
};

}  // namespace

Result<size_t> inflate_raw(std::span<const uint8_t> in, std::span<uint8_t> out) {
    Decoder d;
    d.in = in;
    d.out = out;
    if (!d.run()) {
        return Result<size_t>::err(ErrorCode::InvalidArgument,
                                   "deflate: malformed or truncated stream, or output exceeds " +
                                       std::to_string(out.size()) + " bytes");
    }
    return Result<size_t>::ok(d.out_pos);
}

}  // namespace wisp
