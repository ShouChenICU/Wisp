// Tests for the simulator's screenshot PNG encoder.
// Verifies the file-level framing (signature, IHDR width/height, IHDR
// CRC, trailing IEND) that write_png_rgb888() must produce; it does not
// re-verify every chunk's CRC/Adler-32 bit for bit (that's exercised
// indirectly: a wrong CRC/Adler-32 would make any real PNG decoder
// reject the file, which is what this encoder exists to avoid).

#include "wisp/port/sim/png_writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
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

// Independent Adler-32 (RFC 1950), kept separate from png_writer.cpp's
// own implementation for the same reason as crc32_of() above -- so this
// test doesn't just compare the production code against itself.
uint32_t adler32_of(const uint8_t* data, size_t len) {
    uint32_t a = 1;
    uint32_t b = 0;
    constexpr uint32_t kMod = 65521;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % kMod;
        b = (b + a) % kMod;
    }
    return (b << 16) | a;
}

struct Chunk {
    std::string type;
    std::vector<uint8_t> data;
};

// Walks every chunk after the 8-byte PNG signature, verifying each
// chunk's CRC-32 as it goes (so a caller doesn't have to separately
// re-check IDAT's/IEND's CRCs the way WritesValidHeader checks IHDR's).
std::vector<Chunk> parse_chunks(const std::vector<uint8_t>& file) {
    std::vector<Chunk> chunks;
    size_t pos = 8;
    while (pos + 12 <= file.size()) {
        uint32_t length = be32(&file[pos]);
        std::string type(reinterpret_cast<const char*>(&file[pos + 4]), 4);
        size_t data_start = pos + 8;
        EXPECT_LE(data_start + length + 4, file.size());
        EXPECT_EQ(crc32_of(&file[pos + 4], 4 + length), be32(&file[data_start + length]));
        chunks.push_back({type, std::vector<uint8_t>(
                                    file.begin() + static_cast<ptrdiff_t>(data_start),
                                    file.begin() + static_cast<ptrdiff_t>(data_start + length))});
        pos = data_start + length + 4;
        if (type == "IEND") {
            break;
        }
    }
    return chunks;
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
    // 200x200x3 = 120000 bytes of pixel data, plus one filter byte per
    // row (200 rows) = 120200 raw bytes -- over the 65535-byte limit a
    // single deflate "stored" block can hold (see png_writer.cpp's file
    // header), so this requires exactly two stored blocks. A smaller
    // image (e.g. 128x128, 49280 raw bytes) would stay under 65535 and
    // so never exercise this path at all.
    constexpr uint32_t w = 200, h = 200;
    constexpr size_t kExpectedRawBytes = static_cast<size_t>(h) * (1 + static_cast<size_t>(w) * 3);
    ASSERT_GT(kExpectedRawBytes, 65535u);

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

    auto chunks = parse_chunks(b);
    ASSERT_FALSE(chunks.empty());
    EXPECT_EQ(chunks.back().type, "IEND");
    auto idat_it =
        std::find_if(chunks.begin(), chunks.end(), [](const Chunk& c) { return c.type == "IDAT"; });
    ASSERT_NE(idat_it, chunks.end());
    const std::vector<uint8_t>& idat = idat_it->data;

    // zlib header (RFC 1950): CMF/FLG, (CMF<<8|FLG) % 31 == 0.
    ASSERT_GE(idat.size(), 2u);
    EXPECT_EQ((static_cast<uint32_t>(idat[0]) << 8 | idat[1]) % 31, 0u);

    // Walk the deflate "stored" blocks (RFC 1951 section 3.2.4): each is
    // a 1-byte header (bit 0 = BFINAL, bits 1-2 = BTYPE, 0 for stored),
    // then a little-endian LEN/NLEN pair, then LEN raw bytes.
    size_t pos = 2;
    std::vector<uint8_t> reassembled;
    std::vector<uint16_t> block_lengths;
    bool saw_final = false;
    while (pos < idat.size() && !saw_final) {
        uint8_t header = idat[pos];
        EXPECT_EQ((header >> 1) & 0x3, 0) << "expected a stored (uncompressed) block";
        bool is_final = (header & 1) != 0;
        pos += 1;
        ASSERT_LE(pos + 4, idat.size());
        uint16_t len = static_cast<uint16_t>(idat[pos] | (idat[pos + 1] << 8));
        uint16_t nlen = static_cast<uint16_t>(idat[pos + 2] | (idat[pos + 3] << 8));
        EXPECT_EQ(nlen, static_cast<uint16_t>(~len));
        pos += 4;
        ASSERT_LE(pos + len, idat.size());
        reassembled.insert(reassembled.end(), idat.begin() + static_cast<ptrdiff_t>(pos),
                           idat.begin() + static_cast<ptrdiff_t>(pos + len));
        block_lengths.push_back(len);
        pos += len;
        saw_final = is_final;
    }
    EXPECT_TRUE(saw_final);

    // Exactly two blocks, the first exactly at the 65535-byte cap.
    ASSERT_EQ(block_lengths.size(), 2u);
    EXPECT_EQ(block_lengths[0], 65535u);
    EXPECT_EQ(static_cast<size_t>(block_lengths[0]) + block_lengths[1], kExpectedRawBytes);

    // The zlib trailer (RFC 1950): Adler-32 of the *uncompressed* data,
    // big-endian, immediately after the last block, with nothing after
    // it.
    ASSERT_EQ(pos + 4, idat.size());
    uint32_t trailer_adler = be32(&idat[pos]);
    EXPECT_EQ(trailer_adler, adler32_of(reassembled.data(), reassembled.size()));

    // And the reassembled raw stream really is what write_png_rgb888()
    // was asked to encode: a filter-type-0 byte then w*3 original pixel
    // bytes, per row.
    ASSERT_EQ(reassembled.size(), kExpectedRawBytes);
    for (uint32_t y = 0; y < h; ++y) {
        size_t row_start = static_cast<size_t>(y) * (1 + static_cast<size_t>(w) * 3);
        EXPECT_EQ(reassembled[row_start], 0) << "row " << y << " filter byte";
        EXPECT_EQ(memcmp(&reassembled[row_start + 1], &px[static_cast<size_t>(y) * w * 3], w * 3),
                  0)
            << "row " << y << " pixel data";
    }
}

TEST(PngWriter, FailsForUnwritablePath) {
    auto result = wisp::port::sim::write_png_rgb888("/nonexistent-dir/x.png", nullptr, 0, 0);
    EXPECT_FALSE(result);
}
