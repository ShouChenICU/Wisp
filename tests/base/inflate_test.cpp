#include "wisp/base/inflate.hpp"

#include <gtest/gtest.h>
#include <zlib.h>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

// zlib is linked only to produce reference deflate streams; the code under
// test never uses it.

namespace {

std::vector<uint8_t> deflate_raw(const std::vector<uint8_t>& in, int level) {
    z_stream zs{};
    EXPECT_EQ(deflateInit2(&zs, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY), Z_OK);
    std::vector<uint8_t> out(deflateBound(&zs, in.size()) + 16);
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = static_cast<uInt>(in.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    EXPECT_EQ(deflate(&zs, Z_FINISH), Z_STREAM_END);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}

// Random bytes followed by repetitive text, so one stream exercises
// literals, short and long back-references (including overlapping ones)
// and, at level 1/9, dynamic Huffman blocks.
std::vector<uint8_t> sample(size_t n) {
    std::vector<uint8_t> v;
    v.reserve(n);
    std::mt19937 rng(1234);
    const size_t random_part = n / 3;
    for (size_t i = 0; i < random_part; ++i) {
        v.push_back(static_cast<uint8_t>(rng()));
    }
    const std::string text = "scene runtime core, lua and zip packages. aaaaaaaaaaaaaaaaaaaa ";
    size_t k = 0;
    while (v.size() < n) {
        v.push_back(static_cast<uint8_t>(text[k % text.size()]));
        k += 1 + (rng() % 7 == 0 ? 1 : 0);
    }
    return v;
}

// Packs fields least significant bit first, as RFC 1951 3.1.1 lays out
// header fields, so tests can hand-build dynamic block headers.
class BitWriter {
public:
    void put(uint32_t value, int count) {
        for (int i = 0; i < count; ++i) {
            if (bit_ == 0) {
                bytes_.push_back(0);
            }
            bytes_.back() |= static_cast<uint8_t>(((value >> i) & 1U) << bit_);
            bit_ = (bit_ + 1) % 8;
        }
    }

    // Trailing zero bytes, so a rejection is not merely running out of input.
    std::vector<uint8_t> finish() {
        std::vector<uint8_t> out = bytes_;
        out.resize(out.size() + 64, 0);
        return out;
    }

private:
    std::vector<uint8_t> bytes_;
    int bit_ = 0;
};

// BFINAL=1, BTYPE=10 (dynamic) and the HLIT/HDIST/HCLEN counts.
BitWriter dynamic_header(uint32_t hlit, uint32_t hdist, uint32_t hclen) {
    BitWriter w;
    w.put(1, 1);
    w.put(2, 2);
    w.put(hlit, 5);
    w.put(hdist, 5);
    w.put(hclen, 4);
    return w;
}

void expect_invalid(const std::vector<uint8_t>& stream) {
    std::vector<uint8_t> out(1024);
    auto r = wisp::inflate_raw(stream, out);
    ASSERT_FALSE(r.is_ok());
    EXPECT_EQ(r.error().code, wisp::ErrorCode::InvalidArgument);
}

}  // namespace

TEST(Inflate, RoundTripsZlibRawDeflate) {
    for (size_t n : {size_t{0}, size_t{1}, size_t{70000}}) {
        const std::vector<uint8_t> plain = sample(n);
        for (int level : {0, 1, 9}) {
            SCOPED_TRACE("n=" + std::to_string(n) + " level=" + std::to_string(level));
            const std::vector<uint8_t> packed = deflate_raw(plain, level);
            std::vector<uint8_t> out(n);
            auto r = wisp::inflate_raw(packed, out);
            ASSERT_TRUE(r.is_ok()) << r.error().message;
            EXPECT_EQ(r.value(), n);
            EXPECT_EQ(out, plain);
        }
    }
}

TEST(Inflate, OverlappingBackReference) {
    // 100 copies of 'a': one literal then a length-99 match at distance 1.
    const std::vector<uint8_t> plain(100, 'a');
    const std::vector<uint8_t> packed = deflate_raw(plain, 9);
    std::vector<uint8_t> out(plain.size());
    auto r = wisp::inflate_raw(packed, out);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_EQ(out, plain);
}

TEST(Inflate, RejectsCorruptAndOverflow) {
    const std::vector<uint8_t> plain = sample(70000);
    for (int level : {0, 1, 9}) {
        SCOPED_TRACE("level=" + std::to_string(level));
        const std::vector<uint8_t> packed = deflate_raw(plain, level);

        // Truncated input.
        std::vector<uint8_t> out(plain.size());
        std::vector<uint8_t> cut(packed.begin(), packed.begin() + packed.size() / 2);
        auto truncated = wisp::inflate_raw(cut, out);
        ASSERT_FALSE(truncated.is_ok());
        EXPECT_EQ(truncated.error().code, wisp::ErrorCode::InvalidArgument);

        // Output buffer one byte short.
        std::vector<uint8_t> small(plain.size() - 1);
        auto overflow = wisp::inflate_raw(packed, small);
        ASSERT_FALSE(overflow.is_ok());
        EXPECT_EQ(overflow.error().code, wisp::ErrorCode::InvalidArgument);
    }

    std::vector<uint8_t> out(16);
    // Empty input.
    EXPECT_FALSE(wisp::inflate_raw({}, out).is_ok());
    // Block type 3 is reserved.
    const std::vector<uint8_t> reserved{0x07};
    auto r = wisp::inflate_raw(reserved, out);
    ASSERT_FALSE(r.is_ok());
    EXPECT_EQ(r.error().code, wisp::ErrorCode::InvalidArgument);
    // Stored block whose LEN/NLEN do not complement each other.
    const std::vector<uint8_t> bad_len{0x01, 0x02, 0x00, 0x00, 0x00, 'a', 'b'};
    EXPECT_FALSE(wisp::inflate_raw(bad_len, out).is_ok());
    // Fixed block: literal 'a' then distance code 0 (distance 1) is fine,
    // but a match before any output (distance beyond start) is not:
    // BFINAL=1, BTYPE=01, length code 257 (7 bits 0000001), distance code
    // 0 (5 bits) -> back-reference into nothing.
    const std::vector<uint8_t> before_start{0x03, 0x02, 0x00};
    auto bs = wisp::inflate_raw(before_start, out);
    ASSERT_FALSE(bs.is_ok());
    EXPECT_EQ(bs.error().code, wisp::ErrorCode::InvalidArgument);
}

TEST(Inflate, RandomGarbageNeverCrashes) {
    std::mt19937 rng(99);
    std::vector<uint8_t> out(4096);
    for (int i = 0; i < 2000; ++i) {
        std::vector<uint8_t> junk(1 + rng() % 300);
        for (auto& b : junk) {
            b = static_cast<uint8_t>(rng());
        }
        auto r = wisp::inflate_raw(junk, out);
        if (!r.is_ok()) {
            EXPECT_EQ(r.error().code, wisp::ErrorCode::InvalidArgument);
        }
    }
}

TEST(Inflate, CorruptedStreamsNeverOverrun) {
    // Every single-byte corruption of a real dynamic-Huffman stream either
    // decodes into the buffer or fails cleanly; the sanitizer builds catch
    // any read or write outside the spans.
    const std::vector<uint8_t> plain = sample(3000);
    const std::vector<uint8_t> packed = deflate_raw(plain, 9);
    std::vector<uint8_t> out(plain.size());
    for (size_t i = 0; i < packed.size(); ++i) {
        for (uint8_t mask : {uint8_t{0x01}, uint8_t{0x80}, uint8_t{0xFF}}) {
            std::vector<uint8_t> copy = packed;
            copy[i] ^= mask;
            auto r = wisp::inflate_raw(copy, out);
            if (r.is_ok()) {
                EXPECT_LE(r.value(), out.size());
            } else {
                EXPECT_EQ(r.error().code, wisp::ErrorCode::InvalidArgument);
            }
        }
    }
}

TEST(Inflate, RejectsOverSubscribedCodeLengths) {
    // Four code-length symbols (16, 17, 18, 0) all of length 1: four 1-bit
    // codes cannot exist.
    BitWriter w = dynamic_header(0, 0, 0);
    for (int i = 0; i < 4; ++i) {
        w.put(1, 3);
    }
    expect_invalid(w.finish());
}

TEST(Inflate, RejectsTooManyLiteralLengthCodes) {
    // HLIT = 30 -> 287 literal/length codes; RFC 1951 allows at most 286.
    // The code-length code itself is valid (symbols 16 and 0, one bit each).
    BitWriter w = dynamic_header(30, 0, 0);
    w.put(1, 3);  // symbol 16
    w.put(0, 3);  // symbol 17
    w.put(0, 3);  // symbol 18
    w.put(1, 3);  // symbol 0
    expect_invalid(w.finish());
}

TEST(Inflate, RejectsRepeatAsFirstCodeLength) {
    // Code-length code: symbol 0 -> code 0, symbol 16 -> code 1. The first
    // decoded length is "repeat previous" with no previous length.
    BitWriter w = dynamic_header(0, 0, 0);
    w.put(1, 3);  // symbol 16
    w.put(0, 3);  // symbol 17
    w.put(0, 3);  // symbol 18
    w.put(1, 3);  // symbol 0
    w.put(1, 1);  // code 1 = symbol 16
    w.put(0, 2);  // its repeat count
    expect_invalid(w.finish());
}
