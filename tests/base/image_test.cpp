#include "wisp/base/image.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace {

std::vector<uint8_t> gradient(uint32_t w, uint32_t h) {
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &rgb[(size_t(y) * w + x) * 3];
            p[0] = uint8_t(x * 255 / (w - 1));
            p[1] = uint8_t(y * 255 / (h - 1));
            p[2] = 128;
        }
    }
    return rgb;
}

// A fixed pseudo-random image (LCG), which JPEG compresses poorly, so the
// quality setting shows in the size.
std::vector<uint8_t> noise(uint32_t w, uint32_t h) {
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    uint32_t s = 12345;
    for (auto& b : rgb) {
        s = s * 1103515245u + 12345u;
        b = uint8_t(s >> 16);
    }
    return rgb;
}

std::span<const uint8_t> bytes(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

std::vector<uint8_t> encode(std::span<const uint8_t> rgb, uint32_t w, uint32_t h, int q,
                            size_t cap = 65536) {
    std::vector<uint8_t> out(cap);
    auto r = wisp::encode_jpeg(rgb, w, h, q, out);
    if (!r) {
        ADD_FAILURE() << r.error().message;
        return {};
    }
    out.resize(r.value());
    return out;
}

}  // namespace

TEST(Jpeg, EncodesMarkersAndSize) {
    const auto rgb = gradient(16, 8);
    std::array<uint8_t, 4096> out{};
    auto r = wisp::encode_jpeg(rgb, 16, 8, 80, out);
    ASSERT_TRUE(r) << r.error().message;
    const size_t n = r.value();
    ASSERT_GE(n, 4u);
    ASSERT_LE(n, out.size());
    EXPECT_EQ(out[0], 0xFF);
    EXPECT_EQ(out[1], 0xD8);
    EXPECT_EQ(out[n - 2], 0xFF);
    EXPECT_EQ(out[n - 1], 0xD9);

    auto info = wisp::sniff_image(std::span<const uint8_t>(out.data(), n));
    ASSERT_TRUE(info.has_value());
    EXPECT_STREQ(info->mime, "image/jpeg");
    EXPECT_EQ(info->width, 16u);
    EXPECT_EQ(info->height, 8u);
}

TEST(Jpeg, SameInputSameBytes) {
    const auto rgb = gradient(16, 8);
    const auto a = encode(rgb, 16, 8, 75);
    const auto b = encode(rgb, 16, 8, 75);
    ASSERT_FALSE(a.empty());
    EXPECT_EQ(a, b);
}

TEST(Jpeg, QualityChangesTheSize) {
    const auto rgb = noise(64, 64);
    const auto low = encode(rgb, 64, 64, 30);
    const auto high = encode(rgb, 64, 64, 90);
    ASSERT_FALSE(low.empty());
    ASSERT_FALSE(high.empty());
    EXPECT_LT(low.size(), high.size());
}

TEST(Jpeg, RefusesBadArguments) {
    const auto rgb = gradient(16, 8);
    std::array<uint8_t, 4096> out;
    out.fill(0xAA);
    const auto untouched = out;

    auto expect_invalid = [&](std::span<const uint8_t> in, uint32_t w, uint32_t h, int q) {
        auto r = wisp::encode_jpeg(in, w, h, q, out);
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, wisp::ErrorCode::InvalidArgument);
        EXPECT_EQ(out, untouched);
    };
    expect_invalid(std::span<const uint8_t>(), 0, 8, 80);
    expect_invalid(std::span<const uint8_t>(), 16, 0, 80);
    expect_invalid(rgb, 16, 8, 0);
    expect_invalid(rgb, 16, 8, 101);
    expect_invalid(std::span<const uint8_t>(rgb).first(rgb.size() - 1), 16, 8, 80);
    expect_invalid(std::span<const uint8_t>(), 65536, 1, 80);
}

TEST(Jpeg, TooSmallAnOutputIsQuotaExceeded) {
    const auto rgb = noise(64, 64);
    std::array<uint8_t, 100> out{};
    auto r = wisp::encode_jpeg(rgb, 64, 64, 80, out);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, wisp::ErrorCode::QuotaExceeded);
}

TEST(SniffImage, Png) {
    // Signature, then the IHDR chunk: length 13, "IHDR", width, height, ...
    const uint8_t png[] = {0x89, 'P',  'N', 'G', '\r', '\n', 0x1A, '\n', 0,    0, 0,
                           13,   'I',  'H', 'D', 'R',  0,    0,    0x01, 0x2C, 0, 0,
                           0x00, 0xC8, 8,   2,   0,    0,    0,    0,    0,    0, 0};
    auto info = wisp::sniff_image(png);
    ASSERT_TRUE(info.has_value());
    EXPECT_STREQ(info->mime, "image/png");
    EXPECT_EQ(info->width, 300u);
    EXPECT_EQ(info->height, 200u);
}

TEST(SniffImage, JpegWithExifBeforeSof) {
    std::vector<uint8_t> jpeg = {0xFF, 0xD8};
    // APP1 (Exif) with a 2-byte length covering itself and 8 bytes of payload,
    // the payload holding byte patterns that look like markers.
    const uint8_t app1[] = {0xFF, 0xE1, 0x00, 0x0A, 'E', 'x', 'i', 'f', 0, 0, 0xFF, 0xC0};
    jpeg.insert(jpeg.end(), std::begin(app1), std::end(app1));
    // A Huffman table (DHT) comes before the frame header in many files and
    // must be skipped.
    const uint8_t dht[] = {0xFF, 0xC4, 0x00, 0x04, 0x00, 0x00};
    jpeg.insert(jpeg.end(), std::begin(dht), std::end(dht));
    // SOF2 (progressive): length 11, precision, height 480, width 640, one
    // component (id, sampling, table).
    const uint8_t sof2[] = {0xFF, 0xC2, 0x00, 0x0B, 8, 0x01, 0xE0, 0x02, 0x80, 1, 1, 0x11, 0};
    jpeg.insert(jpeg.end(), std::begin(sof2), std::end(sof2));
    auto info = wisp::sniff_image(jpeg);
    ASSERT_TRUE(info.has_value());
    EXPECT_STREQ(info->mime, "image/jpeg");
    EXPECT_EQ(info->width, 640u);
    EXPECT_EQ(info->height, 480u);
}

TEST(SniffImage, NeitherFormat) {
    EXPECT_FALSE(wisp::sniff_image(bytes("GIF89a\x01\x00\x01\x00")).has_value());
    EXPECT_FALSE(wisp::sniff_image(bytes("<html><body>hi</body></html>")).has_value());
    EXPECT_FALSE(wisp::sniff_image(bytes("")).has_value());
    EXPECT_FALSE(wisp::sniff_image(bytes("plain text, not an image")).has_value());

    // Only the magic: recognised, size unknown.
    auto truncated = wisp::sniff_image(bytes("\xFF\xD8\xFF"));
    ASSERT_TRUE(truncated.has_value());
    EXPECT_STREQ(truncated->mime, "image/jpeg");
    EXPECT_EQ(truncated->width, 0u);
    EXPECT_EQ(truncated->height, 0u);

    // A segment length running past the end is not followed.
    auto cut =
        wisp::sniff_image(bytes("\xFF\xD8\xFF\xE1\x40\x00"
                                "Exif"));
    ASSERT_TRUE(cut.has_value());
    EXPECT_EQ(cut->width, 0u);
}
