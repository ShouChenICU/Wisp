#include "wisp/ui/font_image.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "support/font_image.hpp"
#include "wisp/base/crc32.hpp"
#include "wisp/base/error.hpp"

namespace {

using wisp::ErrorCode;
using wisp::ui::open_font_image;

void put_be16(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    b[at] = static_cast<uint8_t>(v >> 8);
    b[at + 1] = static_cast<uint8_t>(v);
}

void put_be32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}

void put_le16(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    b[at] = static_cast<uint8_t>(v);
    b[at + 1] = static_cast<uint8_t>(v >> 8);
}

void put_le32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

const std::vector<std::string> kRequiredTags = {"cmap", "glyf", "head", "hhea",
                                                "hmtx", "loca", "maxp"};

// A minimal TrueType sfnt: the offset table, one directory record per tag
// and 4 bytes of content per table, right after the directory.
std::vector<uint8_t> make_sfnt(const std::vector<std::string>& tags = kRequiredTags) {
    const size_t dir_end = 12 + 16 * tags.size();
    std::vector<uint8_t> b(dir_end + 4 * tags.size(), 0);
    put_be32(b, 0, 0x00010000);
    put_be16(b, 4, static_cast<uint32_t>(tags.size()));
    for (size_t i = 0; i < tags.size(); ++i) {
        const size_t rec = 12 + 16 * i;
        for (size_t c = 0; c < 4; ++c) b[rec + c] = static_cast<uint8_t>(tags[i][c]);
        put_be32(b, rec + 8, static_cast<uint32_t>(dir_end + 4 * i));
        put_be32(b, rec + 12, 4);
        for (size_t c = 0; c < 4; ++c) b[dir_end + 4 * i + c] = static_cast<uint8_t>(i + 1);
    }
    return b;
}

// The fonts partition image (vector-font spec 3.2) around `ttf`, with a
// correct length and CRC.
std::vector<uint8_t> make_image(const std::vector<uint8_t>& ttf, uint32_t version = 1,
                                uint32_t header_bytes = 32) {
    std::vector<uint8_t> raw(32, 0);
    raw[0] = 'W';
    raw[1] = 'F';
    raw[2] = 'N';
    raw[3] = 'T';
    put_le16(raw, 4, version);
    put_le16(raw, 6, header_bytes);
    put_le32(raw, 8, static_cast<uint32_t>(ttf.size()));
    put_le32(raw, 12, wisp::crc32(ttf));
    raw.insert(raw.end(), ttf.begin(), ttf.end());
    return raw;
}

ErrorCode code_of(const std::vector<uint8_t>& raw) {
    auto r = open_font_image(raw);
    return r ? ErrorCode::Ok : r.error().code;
}

}  // namespace

TEST(FontImage, AcceptsWellFormedImage) {
    const auto ttf = make_sfnt();
    const auto raw = make_image(ttf);
    auto r = open_font_image(raw);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r.value().ttf.data(), raw.data() + 32);
    EXPECT_EQ(r.value().ttf.size(), ttf.size());
    EXPECT_EQ(r.value().crc32, wisp::crc32(ttf));
}

TEST(FontImage, EmptyIsNotFound) {
    auto r = open_font_image({});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
    EXPECT_EQ(r.error().message, "no font image");
}

TEST(FontImage, ErasedPartitionIsNotFound) {
    const std::vector<uint8_t> erased(4096, 0xFF);
    auto r = open_font_image(erased);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
    EXPECT_EQ(r.error().message, "fonts partition holds no font image");
}

TEST(FontImage, OtherVersionIsUnsupported) {
    auto r = open_font_image(make_image(make_sfnt(), /*version=*/2));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
    EXPECT_EQ(r.error().message, "font image format 2, firmware reads 1");
}

TEST(FontImage, ShortHeaderIsInvalid) {
    EXPECT_EQ(code_of(make_image(make_sfnt(), 1, /*header_bytes=*/16)), ErrorCode::InvalidArgument);
}

TEST(FontImage, TruncatedHeaderIsInvalid) {
    auto raw = make_image(make_sfnt());
    raw.resize(20);
    EXPECT_EQ(code_of(raw), ErrorCode::InvalidArgument);
}

TEST(FontImage, LengthBeyondDataIsInvalid) {
    auto raw = make_image(make_sfnt());
    put_le32(raw, 8, static_cast<uint32_t>(raw.size()));
    EXPECT_EQ(code_of(raw), ErrorCode::InvalidArgument);
}

TEST(FontImage, ZeroLengthIsInvalid) {
    EXPECT_EQ(code_of(make_image({})), ErrorCode::InvalidArgument);
}

TEST(FontImage, CrcMismatchIsInvalid) {
    auto raw = make_image(make_sfnt());
    raw.back() ^= 0x01;
    auto r = open_font_image(raw);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(r.error().message, "font image CRC mismatch");
}

TEST(FontImage, CffIsInvalid) {
    auto ttf = make_sfnt();
    ttf[0] = 'O';
    ttf[1] = 'T';
    ttf[2] = 'T';
    ttf[3] = 'O';
    EXPECT_EQ(code_of(make_image(ttf)), ErrorCode::InvalidArgument);
}

TEST(FontImage, TableOutOfBoundsIsInvalid) {
    auto ttf = make_sfnt();
    // The last table's length runs past the payload by one byte.
    const size_t rec = 12 + 16 * (kRequiredTags.size() - 1);
    put_be32(ttf, rec + 12, 5);
    EXPECT_EQ(code_of(make_image(ttf)), ErrorCode::InvalidArgument);

    // offset + length wraps around 32 bits.
    auto wrap = make_sfnt();
    put_be32(wrap, rec + 8, 0xFFFFFFF0u);
    put_be32(wrap, rec + 12, 0x20);
    EXPECT_EQ(code_of(make_image(wrap)), ErrorCode::InvalidArgument);
}

TEST(FontImage, TableDirectoryOutOfBoundsIsInvalid) {
    auto ttf = make_sfnt();
    put_be16(ttf, 4, 1000);
    EXPECT_EQ(code_of(make_image(ttf)), ErrorCode::InvalidArgument);

    auto none = make_sfnt();
    put_be16(none, 4, 0);
    EXPECT_EQ(code_of(make_image(none)), ErrorCode::InvalidArgument);

    // Shorter than the sfnt offset table itself.
    EXPECT_EQ(code_of(make_image({0x00, 0x01, 0x00, 0x00})), ErrorCode::InvalidArgument);
}

TEST(FontImage, MissingGlyfIsInvalid) {
    std::vector<std::string> tags;
    for (const auto& t : kRequiredTags) {
        if (t != "glyf") tags.push_back(t);
    }
    auto r = open_font_image(make_image(make_sfnt(tags)));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_NE(r.error().message.find("glyf"), std::string::npos) << r.error().message;
}

TEST(FontImage, BuiltImageIsValid) {
    const auto raw = wisp::test::test_font_image();
    ASSERT_FALSE(raw.empty());
    auto r = open_font_image(raw);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_GT(r.value().ttf.size(), 2'000'000u);
}
