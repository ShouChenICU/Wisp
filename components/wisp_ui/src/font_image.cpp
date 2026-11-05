#include "wisp/ui/font_image.hpp"

#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

#include "wisp/base/crc32.hpp"
#include "wisp/base/error.hpp"

namespace wisp::ui {

namespace {

// The image header is little-endian (spec 3.2); the sfnt inside is
// big-endian, as TrueType always is.
uint32_t le16(const uint8_t* p) { return p[0] | (uint32_t{p[1]} << 8); }
uint32_t le32(const uint8_t* p) { return le16(p) | (le16(p + 2) << 16); }
uint32_t be16(const uint8_t* p) { return (uint32_t{p[0]} << 8) | p[1]; }
uint32_t be32(const uint8_t* p) { return (be16(p) << 16) | be16(p + 2); }

Result<FontImage> invalid(std::string message) {
    return Result<FontImage>::err(ErrorCode::InvalidArgument, std::move(message));
}

// The tables Tiny TTF (stb_truetype) needs to draw glyphs from a TrueType
// outline font.
constexpr const char* kRequiredTables[] = {"cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp"};

// Spec 3.6 step 6: everything stb_truetype will index through must lie in
// `ttf`. Offsets and lengths are summed in 64 bits so a hostile pair can't
// wrap around.
Result<void> check_sfnt(std::span<const uint8_t> ttf) {
    auto fail = [](std::string message) {
        return Result<void>::err(ErrorCode::InvalidArgument, std::move(message));
    };
    constexpr size_t kOffsetTable = 12;
    constexpr size_t kRecord = 16;
    if (ttf.size() < kOffsetTable) return fail("font image: TTF shorter than its offset table");
    const uint8_t* p = ttf.data();
    if (be32(p) != 0x00010000u) {
        char msg[64];
        std::snprintf(msg, sizeof msg, "font image: sfnt version %08x is not TrueType",
                      static_cast<unsigned>(be32(p)));
        return fail(msg);
    }
    const uint32_t num_tables = be16(p + 4);
    if (num_tables == 0) return fail("font image: no tables");
    if (kOffsetTable + uint64_t{kRecord} * num_tables > ttf.size()) {
        return fail("font image: table directory out of bounds");
    }
    bool found[std::size(kRequiredTables)] = {};
    for (uint32_t i = 0; i < num_tables; ++i) {
        const uint8_t* rec = p + kOffsetTable + kRecord * i;
        const uint64_t offset = be32(rec + 8);
        const uint64_t length = be32(rec + 12);
        if (offset + length > ttf.size()) {
            return fail("font image: table '" + std::string(reinterpret_cast<const char*>(rec), 4) +
                        "' out of bounds");
        }
        for (size_t t = 0; t < std::size(kRequiredTables); ++t) {
            if (std::memcmp(rec, kRequiredTables[t], 4) == 0) found[t] = true;
        }
    }
    for (size_t t = 0; t < std::size(kRequiredTables); ++t) {
        if (!found[t])
            return fail(std::string("font image: no '") + kRequiredTables[t] + "' table");
    }
    return Result<void>::ok();
}

}  // namespace

Result<FontImage> open_font_image(std::span<const uint8_t> raw) {
    // Spec 3.6, checked in this order.
    if (raw.empty()) return Result<FontImage>::err(ErrorCode::NotFound, "no font image");
    const uint8_t* p = raw.data();
    if (raw.size() < 4 || std::memcmp(p, "WFNT", 4) != 0) {
        // A freshly erased partition reads as 0xFF throughout.
        return Result<FontImage>::err(ErrorCode::NotFound, "fonts partition holds no font image");
    }
    if (raw.size() < kFontImageHeaderBytes) return invalid("font image header truncated");
    const uint32_t version = le16(p + 4);
    if (version != kFontImageVersion) {
        return Result<FontImage>::err(ErrorCode::Unsupported,
                                      "font image format " + std::to_string(version) +
                                          ", firmware reads " + std::to_string(kFontImageVersion));
    }
    const uint32_t header_bytes = le16(p + 6);
    if (header_bytes < kFontImageHeaderBytes) {
        return invalid("font image header is " + std::to_string(header_bytes) +
                       " bytes, expected " + std::to_string(kFontImageHeaderBytes));
    }
    const uint64_t length = le32(p + 8);
    if (length == 0) return invalid("font image holds an empty TTF");
    if (header_bytes + length > raw.size()) {
        return invalid("font image TTF length " + std::to_string(length) + " runs past the image");
    }
    const auto ttf = raw.subspan(header_bytes, static_cast<size_t>(length));
    const uint32_t crc = le32(p + 12);
    if (crc32(ttf) != crc) return invalid("font image CRC mismatch");
    WISP_TRY(check_sfnt(ttf));
    return Result<FontImage>::ok(FontImage{ttf, crc});
}

}  // namespace wisp::ui
