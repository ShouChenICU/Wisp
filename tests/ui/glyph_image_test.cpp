// The glyph image reader (glyph-partition spec 4.1): the font it builds
// from the packed image is identical to the built-in lv_font_cjk_16, and
// damaged or crafted images are rejected before LVGL could follow any of
// their offsets.

#include "wisp/ui/glyph_image.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <vector>

#include "a8_font.hpp"
#include "wisp/base/crc32.hpp"
#include "wisp/base/error.hpp"

extern "C" {
extern const lv_font_t lv_font_cjk_16;
}

namespace {

using wisp::ErrorCode;
using wisp::ui::GlyphFont;
using wisp::ui::open_glyph_image;
using Bytes = std::vector<uint8_t>;

constexpr size_t kHeader = 32;
constexpr size_t kMetrics = 32;
constexpr size_t kGlyph = 10;
constexpr size_t kCmap = 20;

const Bytes& image() {
    static const Bytes bytes = [] {
        std::ifstream in(WISP_GLYPH_IMAGE, std::ios::binary);
        return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }();
    return bytes;
}

uint32_t get_le16(const Bytes& b, size_t at) { return b[at] | (uint32_t{b[at + 1]} << 8); }
uint32_t get_le32(const Bytes& b, size_t at) {
    return get_le16(b, at) | (get_le16(b, at + 2) << 16);
}

void put_le16(Bytes& b, size_t at, uint32_t v) {
    b[at] = static_cast<uint8_t>(v);
    b[at + 1] = static_cast<uint8_t>(v >> 8);
}

void put_le32(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

// Recomputes the payload CRC so that a structural check, not the CRC,
// is what rejects a corrupted copy.
void fix_crc(Bytes& b) {
    const uint32_t length = get_le32(b, 8);
    put_le32(b, 12, wisp::crc32(std::span<const uint8_t>(b).subspan(kHeader, length)));
}

// Section positions in the image (absolute byte offsets).
struct Layout {
    uint32_t cmap_count;
    uint32_t glyph_count;
    uint32_t list_bytes;
    uint32_t kern_pairs;
    uint32_t bitmap_bytes;
    size_t glyphs;
    size_t cmaps;
    size_t lists;
    size_t kern;
    size_t bitmap;
};

Layout layout(const Bytes& b) {
    Layout l{};
    const size_t m = kHeader;
    l.cmap_count = get_le16(b, m + 10);
    l.glyph_count = get_le32(b, m + 12);
    l.list_bytes = get_le32(b, m + 16);
    l.kern_pairs = get_le32(b, m + 20);
    l.bitmap_bytes = get_le32(b, m + 24);
    l.glyphs = m + kMetrics;
    l.cmaps = l.glyphs + kGlyph * l.glyph_count;
    l.lists = l.cmaps + kCmap * l.cmap_count;
    l.kern = l.lists + l.list_bytes;
    l.bitmap = l.kern + 5 * size_t{l.kern_pairs};
    return l;
}

const lv_font_fmt_txt_dsc_t* dsc(const lv_font_t& f) {
    return static_cast<const lv_font_fmt_txt_dsc_t*>(f.dsc);
}

ErrorCode open_error(const Bytes& b) {
    auto r = open_glyph_image(b);
    EXPECT_FALSE(r.is_ok());
    return r.is_ok() ? ErrorCode::Ok : r.error().code;
}

TEST(GlyphImage, OpensBuildImage) {
    ASSERT_FALSE(image().empty()) << "cannot read " << WISP_GLYPH_IMAGE;
    auto r = open_glyph_image(image());
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    const lv_font_t& f = r.value()->font;
    EXPECT_EQ(f.line_height, lv_font_cjk_16.line_height);
    EXPECT_EQ(f.base_line, lv_font_cjk_16.base_line);
    EXPECT_EQ(f.underline_position, lv_font_cjk_16.underline_position);
    EXPECT_EQ(f.underline_thickness, lv_font_cjk_16.underline_thickness);
    EXPECT_EQ(f.get_glyph_dsc, lv_font_cjk_16.get_glyph_dsc);
    EXPECT_EQ(f.get_glyph_bitmap, lv_font_cjk_16.get_glyph_bitmap);
    EXPECT_EQ(f.fallback, nullptr);
    EXPECT_EQ(r.value()->crc32, get_le32(image(), 12));
}

TEST(GlyphImage, MatchesBuiltinExactly) {
    auto r = open_glyph_image(image());
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    const lv_font_fmt_txt_dsc_t* got = dsc(r.value()->font);
    const lv_font_fmt_txt_dsc_t* want = dsc(lv_font_cjk_16);
    EXPECT_EQ(got->cmap_num, want->cmap_num);
    EXPECT_EQ(got->bpp, want->bpp);
    EXPECT_EQ(got->kern_scale, want->kern_scale);
    EXPECT_EQ(got->kern_classes, want->kern_classes);
    EXPECT_EQ(got->bitmap_format, want->bitmap_format);
    EXPECT_EQ(got->stride, want->stride);

    const Layout l = layout(image());
    ASSERT_EQ(l.glyph_count, 8710u);
    ASSERT_EQ(l.bitmap_bytes, 996827u);
    for (uint32_t i = 0; i < l.glyph_count; ++i) {
        const auto& g = got->glyph_dsc[i];
        const auto& w = want->glyph_dsc[i];
        ASSERT_EQ(g.bitmap_index, w.bitmap_index) << "glyph " << i;
        ASSERT_EQ(g.adv_w, w.adv_w) << "glyph " << i;
        ASSERT_EQ(g.box_w, w.box_w) << "glyph " << i;
        ASSERT_EQ(g.box_h, w.box_h) << "glyph " << i;
        ASSERT_EQ(g.ofs_x, w.ofs_x) << "glyph " << i;
        ASSERT_EQ(g.ofs_y, w.ofs_y) << "glyph " << i;
    }
    for (uint32_t i = 0; i < want->cmap_num; ++i) {
        const auto& c = got->cmaps[i];
        const auto& w = want->cmaps[i];
        EXPECT_EQ(c.range_start, w.range_start) << "cmap " << i;
        EXPECT_EQ(c.range_length, w.range_length) << "cmap " << i;
        EXPECT_EQ(c.glyph_id_start, w.glyph_id_start) << "cmap " << i;
        EXPECT_EQ(c.list_length, w.list_length) << "cmap " << i;
        EXPECT_EQ(c.type, w.type) << "cmap " << i;
        ASSERT_EQ(c.unicode_list == nullptr, w.unicode_list == nullptr) << "cmap " << i;
        ASSERT_EQ(c.glyph_id_ofs_list == nullptr, w.glyph_id_ofs_list == nullptr) << "cmap " << i;
        if (w.unicode_list != nullptr) {
            EXPECT_EQ(std::memcmp(c.unicode_list, w.unicode_list, 2 * size_t{w.list_length}), 0)
                << "cmap " << i;
        }
        if (w.glyph_id_ofs_list != nullptr) {
            EXPECT_EQ(std::memcmp(c.glyph_id_ofs_list, w.glyph_id_ofs_list, w.list_length), 0)
                << "cmap " << i;
        }
    }
    const auto* gk = static_cast<const lv_font_fmt_txt_kern_pair_t*>(got->kern_dsc);
    const auto* wk = static_cast<const lv_font_fmt_txt_kern_pair_t*>(want->kern_dsc);
    ASSERT_EQ(gk->pair_cnt, wk->pair_cnt);
    EXPECT_EQ(gk->glyph_ids_size, wk->glyph_ids_size);
    EXPECT_EQ(std::memcmp(gk->glyph_ids, wk->glyph_ids, 4 * size_t{wk->pair_cnt}), 0);
    EXPECT_EQ(std::memcmp(gk->values, wk->values, wk->pair_cnt), 0);
    EXPECT_EQ(std::memcmp(got->glyph_bitmap, want->glyph_bitmap, l.bitmap_bytes), 0);
}

TEST(GlyphImage, LooksUpLikeBuiltin) {
    auto r = open_glyph_image(image());
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    for (uint32_t letter : {uint32_t{'A'}, uint32_t{0x4E2D}, uint32_t{0x3042}, uint32_t{0xD55C}}) {
        lv_font_glyph_dsc_t got{};
        lv_font_glyph_dsc_t want{};
        const bool found = lv_font_get_glyph_dsc_fmt_txt(&r.value()->font, &got, letter, 'V');
        EXPECT_EQ(found, lv_font_get_glyph_dsc_fmt_txt(&lv_font_cjk_16, &want, letter, 'V'))
            << letter;
        if (!found) continue;
        EXPECT_EQ(got.gid.index, want.gid.index) << letter;
        EXPECT_EQ(got.adv_w, want.adv_w) << letter;
    }
}

TEST(GlyphImage, A8WrapperAcceptsIt) {
    // a8_font() keeps its wrappers for the life of the process, keyed by
    // the base font's address, so the base must not be freed afterwards.
    static auto r = open_glyph_image(image());
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    EXPECT_NE(wisp::ui::a8_font(&r.value()->font), &r.value()->font);
}

TEST(GlyphImage, RejectsEmpty) { EXPECT_EQ(open_error({}), ErrorCode::NotFound); }

TEST(GlyphImage, RejectsErasedFlash) {
    EXPECT_EQ(open_error(Bytes(1179648, 0xFF)), ErrorCode::NotFound);
}

TEST(GlyphImage, RejectsOtherVersion) {
    Bytes b = image();
    put_le16(b, 4, 2);
    EXPECT_EQ(open_error(b), ErrorCode::Unsupported);
}

TEST(GlyphImage, RejectsBadCrc) {
    Bytes b = image();
    b[layout(b).bitmap + 100] ^= 0x01;
    EXPECT_EQ(open_error(b), ErrorCode::InvalidArgument);
}

TEST(GlyphImage, RejectsTruncated) {
    Bytes b = image();
    b.pop_back();
    EXPECT_EQ(open_error(b), ErrorCode::InvalidArgument);
}

TEST(GlyphImage, RejectsShortHeader) {
    Bytes b = image();
    put_le16(b, 6, 31);
    EXPECT_EQ(open_error(b), ErrorCode::InvalidArgument);
}

TEST(GlyphImage, RejectsOtherBpp) {
    Bytes b = image();
    b[kHeader + 6] = 2;
    fix_crc(b);
    EXPECT_EQ(open_error(b), ErrorCode::InvalidArgument);
}

TEST(GlyphImage, RejectsGlyphPastBitmap) {
    Bytes b = image();
    const Layout l = layout(b);
    put_le32(b, l.glyphs + kGlyph * 1, l.bitmap_bytes);
    fix_crc(b);
    EXPECT_EQ(open_error(b), ErrorCode::InvalidArgument);
}

TEST(GlyphImage, RejectsGlyphBitmapRunningPastEnd) {
    Bytes b = image();
    const Layout l = layout(b);
    // Glyph 2 has a non-empty box: starting on the last byte, it ends
    // past the section.
    put_le32(b, l.glyphs + kGlyph * 2, l.bitmap_bytes - 1);
    fix_crc(b);
    EXPECT_EQ(open_error(b), ErrorCode::InvalidArgument);
}

TEST(GlyphImage, RejectsGlyphTooWide) {
    Bytes b = image();
    const Layout l = layout(b);
    // adv_w is a 12-bit field in LVGL: 4096 would be stored as 0.
    put_le16(b, l.glyphs + kGlyph * 1 + 4, 4096);
    fix_crc(b);
    EXPECT_EQ(open_error(b), ErrorCode::InvalidArgument);
}

TEST(GlyphImage, RejectsCmapListPastLists) {
    Bytes b = image();
    const Layout l = layout(b);
    // Point the first non-empty list a cmap has at the end of the lists
    // section (cmap 0 of the built font is FORMAT0_TINY, without a list).
    size_t field = 0;
    for (uint32_t i = 0; i < l.cmap_count && field == 0; ++i) {
        const size_t c = l.cmaps + kCmap * i;
        if (get_le16(b, c + 10) == 0) continue;
        if (get_le32(b, c + 12) != 0xFFFFFFFFu) field = c + 12;
        if (get_le32(b, c + 16) != 0xFFFFFFFFu) field = c + 16;
    }
    ASSERT_NE(field, 0u) << "no cmap has a list";
    put_le32(b, field, static_cast<uint32_t>(l.lists - kHeader) + l.list_bytes);
    fix_crc(b);
    EXPECT_EQ(open_error(b), ErrorCode::InvalidArgument);
}

TEST(GlyphImage, RejectsSparseFullCmap) {
    Bytes b = image();
    b[layout(b).cmaps + 8] = 1;  // LV_FONT_FMT_TXT_CMAP_SPARSE_FULL
    fix_crc(b);
    EXPECT_EQ(open_error(b), ErrorCode::InvalidArgument);
}

TEST(GlyphImage, RejectsCmapGlyphPastGlyphs) {
    Bytes b = image();
    const Layout l = layout(b);
    put_le16(b, l.cmaps + 6, 0xFFFF);  // cmap 0's glyph_id_start
    fix_crc(b);
    EXPECT_EQ(open_error(b), ErrorCode::InvalidArgument);
}

TEST(GlyphImage, RejectsKernIdPastGlyphs) {
    Bytes b = image();
    const Layout l = layout(b);
    put_le16(b, l.kern, l.glyph_count);
    fix_crc(b);
    EXPECT_EQ(open_error(b), ErrorCode::InvalidArgument);
}

TEST(GlyphImage, TablesNotInMapping) {
    const Bytes& raw = image();
    auto r = open_glyph_image(raw);
    ASSERT_TRUE(r.is_ok()) << r.error().message;
    const auto lo = reinterpret_cast<uintptr_t>(raw.data());
    const auto hi = lo + raw.size();
    auto outside = [&](const void* p) {
        const auto a = reinterpret_cast<uintptr_t>(p);
        return a < lo || a >= hi;
    };
    const lv_font_fmt_txt_dsc_t* d = dsc(r.value()->font);
    EXPECT_TRUE(outside(d));
    EXPECT_TRUE(outside(d->glyph_dsc));
    EXPECT_TRUE(outside(d->cmaps));
    EXPECT_TRUE(outside(d->kern_dsc));
    const auto* k = static_cast<const lv_font_fmt_txt_kern_pair_t*>(d->kern_dsc);
    EXPECT_TRUE(outside(k->glyph_ids));
    EXPECT_TRUE(outside(k->values));
    for (uint32_t i = 0; i < d->cmap_num; ++i) {
        if (d->cmaps[i].unicode_list != nullptr) EXPECT_TRUE(outside(d->cmaps[i].unicode_list));
        if (d->cmaps[i].glyph_id_ofs_list != nullptr) {
            EXPECT_TRUE(outside(d->cmaps[i].glyph_id_ofs_list));
        }
    }
    EXPECT_FALSE(outside(d->glyph_bitmap));
}

}  // namespace
