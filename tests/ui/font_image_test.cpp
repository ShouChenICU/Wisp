#include "wisp/ui/font_image.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <ios>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "support/font_image.hpp"
#include "wisp/base/crc32.hpp"
#include "wisp/base/error.hpp"

namespace {

using wisp::ErrorCode;
using wisp::ui::font_image_has_glyph;
using wisp::ui::FontImage;
using wisp::ui::open_font_image;
using Bytes = std::vector<uint8_t>;

void put_be16(Bytes& b, size_t at, uint32_t v) {
    b[at] = static_cast<uint8_t>(v >> 8);
    b[at + 1] = static_cast<uint8_t>(v);
}

void put_be32(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}

void put_le16(Bytes& b, size_t at, uint32_t v) {
    b[at] = static_cast<uint8_t>(v);
    b[at + 1] = static_cast<uint8_t>(v >> 8);
}

void put_le32(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

void append_be16(Bytes& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}

void append_be32(Bytes& b, uint32_t v) {
    append_be16(b, v >> 16);
    append_be16(b, v & 0xFFFF);
}

// One cmap format 4 segment; `glyphs` is its slice of glyphIdArray when
// `use_array` is set (idRangeOffset is then computed to point at it).
struct Segment {
    uint16_t start;
    uint16_t end;
    uint16_t delta = 0;
    bool use_array = false;
    std::vector<uint16_t> glyphs;
};

// A cmap format 4 subtable with the canonical binary-search fields.
Bytes cmap_format4(const std::vector<Segment>& segs) {
    const uint32_t n = static_cast<uint32_t>(segs.size());
    uint32_t pow2 = 1;
    uint32_t log2 = 0;
    while (pow2 * 2 <= n) {
        pow2 *= 2;
        ++log2;
    }
    Bytes b;
    append_be16(b, 4);
    append_be16(b, 0);  // length, patched below
    append_be16(b, 0);  // language
    append_be16(b, 2 * n);
    append_be16(b, 2 * pow2);
    append_be16(b, log2);
    append_be16(b, 2 * n - 2 * pow2);
    for (const auto& s : segs) append_be16(b, s.end);
    append_be16(b, 0);  // reservedPad
    for (const auto& s : segs) append_be16(b, s.start);
    for (const auto& s : segs) append_be16(b, s.delta);
    // idRangeOffset[i] is relative to its own position; glyphIdArray
    // follows the idRangeOffset array.
    const size_t ro_base = 16 + 6 * n;
    size_t array_at = 16 + 8 * n;
    std::vector<uint16_t> array;
    for (uint32_t i = 0; i < n; ++i) {
        if (segs[i].use_array) {
            append_be16(b, static_cast<uint32_t>(array_at + 2 * array.size() - (ro_base + 2 * i)));
            array.insert(array.end(), segs[i].glyphs.begin(), segs[i].glyphs.end());
        } else {
            append_be16(b, 0);
        }
    }
    for (uint16_t g : array) append_be16(b, g);
    put_be16(b, 2, static_cast<uint32_t>(b.size()));
    return b;
}

struct Group {
    uint32_t start;
    uint32_t end;
    uint32_t glyph;
};

Bytes cmap_format12(const std::vector<Group>& groups) {
    Bytes b;
    append_be16(b, 12);
    append_be16(b, 0);
    append_be32(b, 16 + 12 * static_cast<uint32_t>(groups.size()));
    append_be32(b, 0);  // language
    append_be32(b, static_cast<uint32_t>(groups.size()));
    for (const auto& g : groups) {
        append_be32(b, g.start);
        append_be32(b, g.end);
        append_be32(b, g.glyph);
    }
    return b;
}

// A cmap table with one encoding record (`platform`, `encoding`) for
// `subtable`.
Bytes cmap_table(const Bytes& subtable, uint32_t platform = 3, uint32_t encoding = 1) {
    Bytes b;
    append_be16(b, 0);
    append_be16(b, 1);
    append_be16(b, platform);
    append_be16(b, encoding);
    append_be32(b, 12);
    b.insert(b.end(), subtable.begin(), subtable.end());
    return b;
}

// 'A' -> glyph 1, U+FFFF -> glyph 0, as a real font ends its segments.
Bytes default_cmap() {
    return cmap_table(cmap_format4({{0x41, 0x41, (1 - 0x41) & 0xFFFF}, {0xFFFF, 0xFFFF, 1}}));
}

// The smallest TrueType font the validator (and stb_truetype) accepts:
// two glyphs, the second 12 bytes of glyf, one long horizontal metric.
struct Font {
    std::vector<std::pair<std::string, Bytes>> tables;

    Bytes& operator[](const std::string& tag) {
        for (auto& t : tables) {
            if (t.first == tag) return t.second;
        }
        tables.emplace_back(tag, Bytes{});
        return tables.back().second;
    }

    void drop(const std::string& tag) {
        std::erase_if(tables, [&](const auto& t) { return t.first == tag; });
    }
};

constexpr uint32_t kNumGlyphs = 2;

Font make_font() {
    Font f;
    f["cmap"] = default_cmap();
    f["glyf"] = Bytes(12, 0);
    Bytes head(54, 0);
    put_be32(head, 0, 0x00010000);
    put_be16(head, 18, 1000);  // unitsPerEm
    f["head"] = head;
    Bytes hhea(36, 0);
    put_be16(hhea, 34, 1);  // numberOfHMetrics
    f["hhea"] = hhea;
    f["hmtx"] = Bytes(4 * 1 + 2 * (kNumGlyphs - 1), 0);
    Bytes loca;  // short format, in 2-byte units
    append_be16(loca, 0);
    append_be16(loca, 0);
    append_be16(loca, 6);
    f["loca"] = loca;
    Bytes maxp(6, 0);
    put_be32(maxp, 0, 0x00005000);
    put_be16(maxp, 4, kNumGlyphs);
    f["maxp"] = maxp;
    return f;
}

// The sfnt: offset table, directory, then the tables back to back in
// order.
Bytes build(const Font& f) {
    const size_t n = f.tables.size();
    const size_t dir_end = 12 + 16 * n;
    Bytes b(dir_end, 0);
    put_be32(b, 0, 0x00010000);
    put_be16(b, 4, static_cast<uint32_t>(n));
    for (size_t i = 0; i < n; ++i) {
        const size_t rec = 12 + 16 * i;
        for (size_t c = 0; c < 4; ++c) b[rec + c] = static_cast<uint8_t>(f.tables[i].first[c]);
        put_be32(b, rec + 8, static_cast<uint32_t>(b.size()));
        put_be32(b, rec + 12, static_cast<uint32_t>(f.tables[i].second.size()));
        b.insert(b.end(), f.tables[i].second.begin(), f.tables[i].second.end());
    }
    return b;
}

Bytes make_sfnt() { return build(make_font()); }

// The directory record of `tag` in `sfnt`.
size_t record_of(const Bytes& sfnt, const std::string& tag) {
    const size_t n = (sfnt[4] << 8) | sfnt[5];
    for (size_t i = 0; i < n; ++i) {
        if (std::string(reinterpret_cast<const char*>(&sfnt[12 + 16 * i]), 4) == tag) {
            return 12 + 16 * i;
        }
    }
    ADD_FAILURE() << "no " << tag << " record";
    return 0;
}

// The fonts partition image (vector-font spec 3.2) around `ttf`, with a
// correct length and CRC.
Bytes make_image(const Bytes& ttf, uint32_t version = 1, uint32_t header_bytes = 32) {
    Bytes raw(32, 0);
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

ErrorCode code_of(const Bytes& raw) {
    auto r = open_font_image(raw);
    return r ? ErrorCode::Ok : r.error().code;
}

// The validation message for `f`, or "ok" when it passes.
std::string message_of(const Font& f) {
    auto r = open_font_image(make_image(build(f)));
    return r ? "ok" : r.error().message;
}

#define EXPECT_REJECTED(font, needle)                                                        \
    do {                                                                                     \
        auto r_ = open_font_image(make_image(build(font)));                                  \
        ASSERT_FALSE(r_) << "accepted";                                                      \
        EXPECT_EQ(r_.error().code, ErrorCode::InvalidArgument);                              \
        EXPECT_NE(r_.error().message.find(needle), std::string::npos) << r_.error().message; \
    } while (0)

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
    const Bytes erased(4096, 0xFF);
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
    // maxp is the last table: its length runs past the payload by one byte.
    auto ttf = make_sfnt();
    const size_t rec = record_of(ttf, "maxp");
    put_be32(ttf, rec + 12, 7);
    EXPECT_EQ(code_of(make_image(ttf)), ErrorCode::InvalidArgument);

    // offset + length wraps around 32 bits.
    auto wrap = make_sfnt();
    put_be32(wrap, rec + 8, 0xFFFFFFF0u);
    put_be32(wrap, rec + 12, 0x20);
    EXPECT_EQ(code_of(make_image(wrap)), ErrorCode::InvalidArgument);
}

TEST(FontImage, TableOverlappingDirectoryIsInvalid) {
    auto ttf = make_sfnt();
    put_be32(ttf, record_of(ttf, "hhea") + 8, 0);
    auto r = open_font_image(make_image(ttf));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(r.error().message, "font image: table 'hhea' overlaps the table directory");
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
    Font f = make_font();
    f.drop("glyf");
    EXPECT_REJECTED(f, "glyf");
}

// --- the structures stb_truetype reads through (spec 3.6 step 6) ---

TEST(FontImage, ShortFixedTablesAreInvalid) {
    Font head = make_font();
    head["head"].resize(53);
    EXPECT_REJECTED(head, "'head'");

    Font hhea = make_font();
    hhea["hhea"].resize(35);
    EXPECT_REJECTED(hhea, "'hhea'");

    Font maxp = make_font();
    maxp["maxp"].resize(5);
    EXPECT_REJECTED(maxp, "'maxp'");
}

TEST(FontImage, UnknownIndexToLocFormatIsInvalid) {
    Font f = make_font();
    put_be16(f["head"], 50, 2);
    EXPECT_REJECTED(f, "indexToLocFormat");
}

TEST(FontImage, NoGlyphsIsInvalid) {
    Font f = make_font();
    put_be16(f["maxp"], 4, 0);
    EXPECT_REJECTED(f, "glyphs");
}

TEST(FontImage, CmapRecordsOutOfBoundsIsInvalid) {
    Font f = make_font();
    put_be16(f["cmap"], 2, 100);
    EXPECT_REJECTED(f, "cmap");
}

TEST(FontImage, CmapWithoutUnicodeSubtableIsInvalid) {
    Font f = make_font();
    f["cmap"] = cmap_table(cmap_format4({{0xFFFF, 0xFFFF, 1}}), /*platform=*/1, /*encoding=*/0);
    EXPECT_REJECTED(f, "Unicode");
}

TEST(FontImage, CmapSubtableOutOfBoundsIsInvalid) {
    Font f = make_font();
    put_be32(f["cmap"], 8, static_cast<uint32_t>(f["cmap"].size()));
    EXPECT_REJECTED(f, "cmap");

    // The subtable's own length runs past the cmap table.
    Font len = make_font();
    put_be16(len["cmap"], 12 + 2, static_cast<uint32_t>(len["cmap"].size()));
    EXPECT_REJECTED(len, "cmap");
}

TEST(FontImage, CmapUnsupportedFormatIsInvalid) {
    Font f = make_font();
    put_be16(f["cmap"], 12, 6);
    EXPECT_REJECTED(f, "format 6");
}

TEST(FontImage, CmapFormat4SegmentsOutOfBoundsIsInvalid) {
    Font f = make_font();
    // segCountX2 claims 100 segments in a 2-segment subtable.
    put_be16(f["cmap"], 12 + 6, 200);
    EXPECT_REJECTED(f, "cmap");
}

TEST(FontImage, CmapFormat4SearchFieldsMustBeCanonical) {
    // stb_truetype trusts searchRange/entrySelector/rangeShift to keep its
    // binary search inside endCode[].
    Font f = make_font();
    put_be16(f["cmap"], 12 + 12, 0x7FFE);  // rangeShift
    EXPECT_REJECTED(f, "cmap");
}

TEST(FontImage, CmapFormat4UnsortedSegmentsAreInvalid) {
    Font f = make_font();
    f["cmap"] = cmap_table(cmap_format4({{0x50, 0x50, 0}, {0x41, 0x41, 0}, {0xFFFF, 0xFFFF, 1}}));
    EXPECT_REJECTED(f, "cmap");
}

TEST(FontImage, CmapFormat4GlyphArray) {
    Font ok = make_font();
    ok["cmap"] = cmap_table(cmap_format4({{0x41, 0x42, 0, true, {1, 0}}, {0xFFFF, 0xFFFF, 1}}));
    EXPECT_EQ(message_of(ok), "ok");

    // idRangeOffset points past the end of the subtable.
    Font out = make_font();
    out["cmap"] = cmap_table(cmap_format4({{0x41, 0x42, 0, true, {1, 0}}, {0xFFFF, 0xFFFF, 1}}));
    const size_t ro0 = 12 + 16 + 6 * 2;  // idRangeOffset[0] inside cmap
    put_be16(out["cmap"], ro0, 0x100);
    EXPECT_REJECTED(out, "cmap");
}

TEST(FontImage, CmapGlyphBeyondNumGlyphsIsInvalid) {
    // hmtx and loca are indexed by the glyph the cmap returns.
    Font delta = make_font();
    delta["cmap"] =
        cmap_table(cmap_format4({{0x41, 0x41, (5 - 0x41) & 0xFFFF}, {0xFFFF, 0xFFFF, 1}}));
    EXPECT_REJECTED(delta, "glyph");

    Font array = make_font();
    array["cmap"] =
        cmap_table(cmap_format4({{0x41, 0x42, 0, true, {1, kNumGlyphs}}, {0xFFFF, 0xFFFF, 1}}));
    EXPECT_REJECTED(array, "glyph");

    Font groups = make_font();
    groups["cmap"] = cmap_table(cmap_format12({{0x41, 0x42, 1}}), 3, 10);
    EXPECT_REJECTED(groups, "glyph");
}

TEST(FontImage, CmapFormat12) {
    Font ok = make_font();
    ok["cmap"] = cmap_table(cmap_format12({{0x41, 0x41, 1}, {0x20000, 0x20000, 1}}), 3, 10);
    EXPECT_EQ(message_of(ok), "ok");

    Font out = make_font();
    out["cmap"] = cmap_table(cmap_format12({{0x41, 0x41, 1}}), 3, 10);
    put_be32(out["cmap"], 12 + 12, 1000);  // numGroups
    EXPECT_REJECTED(out, "cmap");
}

TEST(FontImage, HmtxTooShortIsInvalid) {
    Font f = make_font();
    f["hmtx"].resize(5);
    EXPECT_REJECTED(f, "'hmtx'");

    Font zero = make_font();
    put_be16(zero["hhea"], 34, 0);
    EXPECT_REJECTED(zero, "numberOfHMetrics");
}

TEST(FontImage, LocaSizeMustMatchNumGlyphs) {
    Font f = make_font();
    f["loca"].resize(4);
    EXPECT_REJECTED(f, "'loca'");

    // The long format needs 4 bytes per entry.
    Font lng = make_font();
    put_be16(lng["head"], 50, 1);
    EXPECT_REJECTED(lng, "'loca'");
}

TEST(FontImage, AcceptsLongLoca) {
    Font f = make_font();
    put_be16(f["head"], 50, 1);
    Bytes loca;
    append_be32(loca, 0);
    append_be32(loca, 0);
    append_be32(loca, 12);
    f["loca"] = loca;
    EXPECT_EQ(message_of(f), "ok");
}

TEST(FontImage, LocaMustBeMonotonic) {
    Font f = make_font();
    Bytes& loca = f["loca"];
    put_be16(loca, 2, 6);
    put_be16(loca, 4, 0);
    EXPECT_REJECTED(f, "'loca'");
}

TEST(FontImage, LocaBeyondGlyfIsInvalid) {
    Font f = make_font();
    put_be16(f["loca"], 4, 7);  // 14 bytes into a 12-byte glyf
    EXPECT_REJECTED(f, "'loca'");
}

TEST(FontImage, BuiltImageIsValid) {
    const auto raw = wisp::test::test_font_image();
    ASSERT_FALSE(raw.empty());
    auto r = open_font_image(raw);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_GT(r.value().ttf.size(), 2'000'000u);
}

TEST(FontImage, HasGlyphFollowsTheCmapFormat4) {
    // 'A' -> glyph 1 through idDelta; U+FFFF maps to glyph 0.
    auto r = open_font_image(make_image(make_sfnt()));
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_TRUE(font_image_has_glyph(r.value(), 'A'));
    EXPECT_FALSE(font_image_has_glyph(r.value(), 'B'));
    EXPECT_FALSE(font_image_has_glyph(r.value(), 0x40));
    EXPECT_FALSE(font_image_has_glyph(r.value(), 0xFFFF));
    EXPECT_FALSE(font_image_has_glyph(r.value(), 0x10041));

    // glyphIdArray entries are taken as they are; 0 means no glyph.
    Font array = make_font();
    array["cmap"] = cmap_table(cmap_format4({{0x41, 0x42, 0, true, {1, 0}}, {0xFFFF, 0xFFFF, 1}}));
    const auto raw = make_image(build(array));
    auto a = open_font_image(raw);
    ASSERT_TRUE(a) << a.error().message;
    EXPECT_TRUE(font_image_has_glyph(a.value(), 'A'));
    EXPECT_FALSE(font_image_has_glyph(a.value(), 'B'));
}

TEST(FontImage, HasGlyphFollowsTheCmapFormat12) {
    Font f = make_font();
    f["cmap"] = cmap_table(cmap_format12({{0x41, 0x42, 0}, {0x20000, 0x20000, 1}}), 3, 10);
    const auto raw = make_image(build(f));
    auto r = open_font_image(raw);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_FALSE(font_image_has_glyph(r.value(), 'A'));  // glyph 0
    EXPECT_TRUE(font_image_has_glyph(r.value(), 'B'));
    EXPECT_FALSE(font_image_has_glyph(r.value(), 'C'));
    EXPECT_FALSE(font_image_has_glyph(r.value(), 0x1FFFF));
    EXPECT_TRUE(font_image_has_glyph(r.value(), 0x20000));
    EXPECT_FALSE(font_image_has_glyph(r.value(), 0x20001));
}

TEST(FontImage, HasGlyphOnTheBuiltImage) {
    auto r = open_font_image(wisp::test::test_font_image());
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_TRUE(font_image_has_glyph(r.value(), 'A'));
    EXPECT_TRUE(font_image_has_glyph(r.value(), 0x81EA));   // 自
    EXPECT_TRUE(font_image_has_glyph(r.value(), 0x3042));   // あ
    EXPECT_FALSE(font_image_has_glyph(r.value(), 0xD55C));  // 한
    EXPECT_FALSE(font_image_has_glyph(r.value(), 0xF00C));  // LV_SYMBOL_OK
    EXPECT_FALSE(font_image_has_glyph(FontImage{}, 'A'));
}

// On the device the image is a flash mapping, and a read that races a
// flash erase or write can see 0xFF in place of the real bytes. The
// subtable's format and counts are copied at open, so a lookup never
// steers by bytes read later.
TEST(FontImage, HasGlyphUsesTheCmapHeaderCheckedAtOpen) {
    auto raw = make_image(make_sfnt());
    auto r = open_font_image(raw);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r.value().cmap_format, 4u);
    EXPECT_EQ(r.value().cmap_count, 2u);
    // format, length, language, segCountX2, searchRange, entrySelector,
    // rangeShift, as an erased flash page reads.
    const size_t sub = static_cast<size_t>(r.value().cmap.data() - raw.data());
    for (size_t i = 0; i < 14; ++i) raw[sub + i] = 0xFF;
    EXPECT_TRUE(font_image_has_glyph(r.value(), 'A'));
    EXPECT_FALSE(font_image_has_glyph(r.value(), 'B'));

    Font f = make_font();
    f["cmap"] = cmap_table(cmap_format12({{0x41, 0x42, 0}, {0x20000, 0x20000, 1}}), 3, 10);
    auto raw12 = make_image(build(f));
    auto r12 = open_font_image(raw12);
    ASSERT_TRUE(r12) << r12.error().message;
    EXPECT_EQ(r12.value().cmap_format, 12u);
    EXPECT_EQ(r12.value().cmap_count, 2u);
    const size_t sub12 = static_cast<size_t>(r12.value().cmap.data() - raw12.data());
    for (size_t i = 0; i < 16; ++i) raw12[sub12 + i] = 0xFF;
    EXPECT_TRUE(font_image_has_glyph(r12.value(), 'B'));
    EXPECT_TRUE(font_image_has_glyph(r12.value(), 0x20000));
    EXPECT_FALSE(font_image_has_glyph(r12.value(), 'C'));
}

// When the whole subtable reads as 0xFF, a lookup answers "no glyph"
// without reading past the subtable. The bytes after the image are
// nonzero, so a glyphIdArray read that strayed there would answer "yes".
TEST(FontImage, HasGlyphStaysInsideTheCmapWhenItReadsGarbage) {
    auto raw = make_image(make_sfnt());
    raw.resize(raw.size() + 0x30000, 0x01);
    auto r = open_font_image(raw);
    ASSERT_TRUE(r) << r.error().message;
    const size_t sub = static_cast<size_t>(r.value().cmap.data() - raw.data());
    for (size_t i = 0; i < r.value().cmap.size(); ++i) raw[sub + i] = 0xFF;
    for (uint32_t c : {0x0u, 0x41u, 0x4E00u, 0xFFFEu, 0xFFFFu, 0x10000u, 0x10FFFFu}) {
        EXPECT_FALSE(font_image_has_glyph(r.value(), c)) << std::hex << c;
    }
}

TEST(FontImage, ErasedCmapSubtableIsInvalid) {
    Font f = make_font();
    Bytes& cmap = f["cmap"];
    for (size_t i = 12; i < cmap.size(); ++i) cmap[i] = 0xFF;
    EXPECT_REJECTED(f, "cmap");
}
