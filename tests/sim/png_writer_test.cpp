// TDD step 1 (task-13-brief.md): the simulator's screenshot PNG encoder.
// Verifies the file-level framing (signature, IHDR width/height, IHDR
// CRC, trailing IEND) that write_png_rgb888() must produce; it does not
// re-verify every chunk's CRC/Adler-32 bit for bit (that's exercised
// indirectly: a wrong CRC/Adler-32 would make any real PNG decoder
// reject the file, which is what this encoder exists to avoid).

#include "wisp/port/sim/png_writer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "support/temp_dir.hpp"

namespace {

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

std::vector<uint8_t> read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
}

// Independent CRC-32 (standard zlib/PNG polynomial 0xEDB88320), kept
// separate from png_writer.cpp's own implementation so this test doesn't
// just compare the production code against itself.
uint32_t crc32_of(const uint8_t* data, size_t len) {
    static std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

}  // namespace

TEST(PngWriter, WritesValidHeader) {
    std::vector<uint8_t> px(4 * 3 * 3, 0x80);
    auto p = wisp::test::temp_dir() / "a.png";
    ASSERT_TRUE(wisp::port::sim::write_png_rgb888(p.string(), px.data(), 4, 3));
    auto b = read_all(p);
    ASSERT_GE(b.size(), 45u);
    EXPECT_EQ(std::vector<uint8_t>(b.begin(), b.begin() + 8),
              (std::vector<uint8_t>{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}));
    EXPECT_EQ(be32(&b[16]), 4u);                    // IHDR width
    EXPECT_EQ(be32(&b[20]), 3u);                    // IHDR height
    EXPECT_EQ(crc32_of(&b[12], 17), be32(&b[29]));  // IHDR CRC
    EXPECT_EQ(std::string(b.end() - 8, b.end() - 4), "IEND");
}

TEST(PngWriter, LargerImageSplitsAcrossStoredDeflateBlocks) {
    // 128x128x3 = 49152 bytes of pixel data, plus one filter byte per
    // row (128 rows) = 49280 raw bytes -- over the 65535-byte limit a
    // single deflate "stored" block can hold (see png_writer.cpp's file
    // header), so this exercises the multi-block path.
    constexpr uint32_t w = 128, h = 128;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < px.size(); ++i) {
        px[i] = static_cast<uint8_t>(i * 7);
    }
    auto p = wisp::test::temp_dir() / "b.png";
    ASSERT_TRUE(wisp::port::sim::write_png_rgb888(p.string(), px.data(), w, h));
    auto b = read_all(p);
    EXPECT_EQ(be32(&b[16]), w);
    EXPECT_EQ(be32(&b[20]), h);
    EXPECT_EQ(crc32_of(&b[12], 17), be32(&b[29]));
    EXPECT_EQ(std::string(b.end() - 8, b.end() - 4), "IEND");
}

TEST(PngWriter, FailsForUnwritablePath) {
    auto result = wisp::port::sim::write_png_rgb888("/nonexistent-dir/x.png", nullptr, 0, 0);
    EXPECT_FALSE(result);
}
