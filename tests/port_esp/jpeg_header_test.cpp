// read_jpeg_header(): what the firmware's JPEG decoder accepts before it
// hands the bytes to esp_new_jpeg (scene-runtime-core spec 4.3).

#include "wisp/port/esp/jpeg_header.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using wisp::port::esp::JpegKind;
using wisp::port::esp::read_jpeg_header;

// SOI, an APP0 segment, then a frame header with marker `sof` for a
// `w` x `h` image, then the start of scan.
std::vector<uint8_t> jpeg(uint8_t sof, uint16_t w, uint16_t h) {
    std::vector<uint8_t> d = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 'J', 'F'};
    std::vector<uint8_t> frame = {0xFF,
                                  sof,
                                  0x00,
                                  0x11,
                                  0x08,
                                  static_cast<uint8_t>(h >> 8),
                                  static_cast<uint8_t>(h),
                                  static_cast<uint8_t>(w >> 8),
                                  static_cast<uint8_t>(w),
                                  0x03,
                                  0x01,
                                  0x22,
                                  0x00,
                                  0x02,
                                  0x11,
                                  0x01,
                                  0x03,
                                  0x11,
                                  0x01};
    d.insert(d.end(), frame.begin(), frame.end());
    d.insert(d.end(), {0xFF, 0xDA, 0x00, 0x02});
    return d;
}

TEST(JpegHeader, BaselineGivesItsSize) {
    for (uint8_t sof : {0xC0, 0xC1}) {
        auto d = jpeg(sof, 320, 240);
        auto h = read_jpeg_header(d.data(), d.size());
        EXPECT_EQ(h.kind, JpegKind::Baseline);
        EXPECT_EQ(h.width, 320);
        EXPECT_EQ(h.height, 240);
    }
    // Fill bytes before a marker are allowed.
    auto d = jpeg(0xC0, 20, 12);
    d.insert(d.begin() + 2, {0xFF, 0xFF});
    EXPECT_EQ(read_jpeg_header(d.data(), d.size()).kind, JpegKind::Baseline);
}

TEST(JpegHeader, ProgressiveAndOtherCodingsAreUnsupported) {
    for (uint8_t sof : {0xC2, 0xC3, 0xC5, 0xC9, 0xCA, 0xCF}) {
        auto d = jpeg(sof, 20, 12);
        EXPECT_EQ(read_jpeg_header(d.data(), d.size()).kind, JpegKind::Unsupported) << int(sof);
    }
    auto empty = jpeg(0xC0, 0, 12);
    EXPECT_EQ(read_jpeg_header(empty.data(), empty.size()).kind, JpegKind::Unsupported);
}

TEST(JpegHeader, AnythingElseIsNotAJpeg) {
    const uint8_t png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    EXPECT_EQ(read_jpeg_header(png, sizeof(png)).kind, JpegKind::NotJpeg);
    EXPECT_EQ(read_jpeg_header(nullptr, 0).kind, JpegKind::NotJpeg);
    auto d = jpeg(0xC0, 20, 12);
    // Cut anywhere before the frame header's size.
    for (size_t n = 0; n < 19; ++n)
        EXPECT_EQ(read_jpeg_header(d.data(), n).kind, JpegKind::NotJpeg) << n;
    // A scan before any frame header.
    const uint8_t scan_first[] = {0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x02, 0x00, 0x00};
    EXPECT_EQ(read_jpeg_header(scan_first, sizeof(scan_first)).kind, JpegKind::NotJpeg);
    // A segment length that points past the end, or below its own size.
    const uint8_t long_seg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0xFF, 0xF0, 0x00, 0x00};
    EXPECT_EQ(read_jpeg_header(long_seg, sizeof(long_seg)).kind, JpegKind::NotJpeg);
    const uint8_t short_seg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x01, 0x00, 0x00};
    EXPECT_EQ(read_jpeg_header(short_seg, sizeof(short_seg)).kind, JpegKind::NotJpeg);
}

}  // namespace
