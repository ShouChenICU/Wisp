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

Result<void> fail(std::string message) {
    return Result<void>::err(ErrorCode::InvalidArgument, "font image: " + std::move(message));
}

// The tables Tiny TTF (stb_truetype) needs to draw glyphs from a TrueType
// outline font.
constexpr const char* kRequiredTables[] = {"cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp"};
enum { kCmap, kGlyf, kHead, kHhea, kHmtx, kLoca, kMaxp };

// Why these checks exist: with LV_TINY_TTF_FILE_SUPPORT off, stb_truetype
// indexes the font bytes directly and never checks a bound. Everything it
// reads while opening the font and while looking up, measuring and
// locating a glyph is checked here, so that those reads stay inside the
// image:
//   - the directory and every table lie in the TTF, after the directory;
//   - head, hhea and maxp are long enough for the fields stb reads, and
//     indexToLocFormat is one stb understands;
//   - every cmap encoding record lies in cmap, and the subtable stb picks
//     (the last Unicode one; format 4 or 12) lies in cmap with all of its
//     arrays, has the binary-search fields stb trusts, and maps every
//     character to a glyph below numGlyphs;
//   - hmtx holds numberOfHMetrics long metrics plus a side bearing for
//     each remaining glyph;
//   - loca has exactly numGlyphs + 1 entries, non-decreasing, none past
//     the end of glyf, and every non-empty glyph has room for its header.
// Residual risk, not checked: the inside of each glyph (contours, points,
// composite components) and the GPOS/kern data. Checking those means
// parsing every glyph, which costs far more than the rest. The CRC still
// rejects accidental damage, and the fonts partition is written only over
// USB (spec 3.5), so reaching these reads takes a deliberately crafted
// image with a matching CRC.
struct Table {
    const uint8_t* data = nullptr;
    uint64_t length = 0;
};

constexpr uint64_t kGlyphHeaderBytes = 10;

// The checks below read each header field once and report the value they
// checked through `length` and `count`, so what FontImage keeps is what
// was checked.
Result<void> check_cmap_format4(const uint8_t* sub, uint64_t avail, uint32_t num_glyphs,
                                uint64_t& length, uint32_t& count) {
    if (avail < 14) return fail("cmap format 4 subtable out of bounds");
    length = be16(sub + 2);
    if (length > avail) return fail("cmap format 4 subtable out of bounds");
    const uint32_t seg_x2 = be16(sub + 6);
    const uint32_t segs = seg_x2 / 2;
    if (seg_x2 == 0 || seg_x2 % 2 != 0) return fail("cmap format 4 segment count invalid");
    const uint64_t ro_at = 16 + 6 * uint64_t{segs};
    if (16 + 8 * uint64_t{segs} > length) return fail("cmap format 4 segments out of bounds");
    // stb_truetype's binary search steps by these fields without checking
    // them; the canonical values keep it inside endCode[].
    uint32_t pow2 = 1;
    uint32_t log2 = 0;
    while (pow2 * 2 <= segs) {
        pow2 *= 2;
        ++log2;
    }
    if (be16(sub + 8) != 2 * pow2 || be16(sub + 10) != log2 ||
        be16(sub + 12) != seg_x2 - 2 * pow2) {
        return fail("cmap format 4 search fields invalid");
    }
    // Sorted, disjoint segments cover at most 65536 characters, which
    // bounds the per-character loops below.
    int64_t prev_end = -1;
    for (uint32_t i = 0; i < segs; ++i) {
        const uint32_t end = be16(sub + 14 + 2 * i);
        const uint32_t start = be16(sub + 16 + 2 * uint64_t{segs} + 2 * i);
        const uint32_t delta = be16(sub + 16 + 4 * uint64_t{segs} + 2 * i);
        const uint64_t ro_pos = ro_at + 2 * i;
        const uint32_t ro = be16(sub + ro_pos);
        if (start > end || static_cast<int64_t>(start) <= prev_end) {
            return fail("cmap format 4 segments unsorted");
        }
        prev_end = end;
        if (ro == 0) {
            for (uint32_t c = start; c <= end; ++c) {
                if (((c + delta) & 0xFFFF) >= num_glyphs) {
                    return fail("cmap maps a character past the last glyph");
                }
            }
            continue;
        }
        // stb_truetype returns glyphIdArray entries as they are.
        const uint64_t first = ro_pos + ro;
        if (first + 2 * uint64_t{end - start + 1} > length) {
            return fail("cmap format 4 glyph array out of bounds");
        }
        for (uint32_t k = 0; k <= end - start; ++k) {
            if (be16(sub + first + 2 * k) >= num_glyphs) {
                return fail("cmap maps a character past the last glyph");
            }
        }
    }
    count = segs;
    return Result<void>::ok();
}

Result<void> check_cmap_format12(const uint8_t* sub, uint64_t avail, uint32_t num_glyphs,
                                 uint64_t& length, uint32_t& count) {
    if (avail < 16) return fail("cmap format 12 subtable out of bounds");
    length = be32(sub + 4);
    if (length > avail) return fail("cmap format 12 subtable out of bounds");
    const uint64_t groups = be32(sub + 12);
    if (16 + 12 * groups > length) return fail("cmap format 12 groups out of bounds");
    for (uint64_t i = 0; i < groups; ++i) {
        const uint8_t* g = sub + 16 + 12 * i;
        const uint64_t start = be32(g);
        const uint64_t end = be32(g + 4);
        const uint64_t glyph = be32(g + 8);
        if (start > end) return fail("cmap format 12 group invalid");
        if (glyph + (end - start) >= num_glyphs) {
            return fail("cmap maps a character past the last glyph");
        }
    }
    count = static_cast<uint32_t>(groups);
    return Result<void>::ok();
}

// The checked cmap subtable and the header fields lookups steer by,
// copied into FontImage.
struct CmapChoice {
    std::span<const uint8_t> subtable;
    uint32_t format = 0;
    uint32_t count = 0;
};

// Mirrors the subtable choice in stb_truetype's InitFont: the last
// Microsoft Unicode (BMP or full) or Unicode-platform record wins.
Result<void> check_cmap(const Table& cmap, uint32_t num_glyphs, CmapChoice& choice) {
    if (cmap.length < 4) return fail("'cmap' table too short");
    const uint64_t records = be16(cmap.data + 2);
    if (4 + 8 * records > cmap.length) return fail("cmap encoding records out of bounds");
    const uint8_t* chosen = nullptr;
    for (uint64_t i = 0; i < records; ++i) {
        const uint8_t* rec = cmap.data + 4 + 8 * i;
        const uint32_t platform = be16(rec);
        const uint32_t encoding = be16(rec + 2);
        if (platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10))) chosen = rec;
    }
    if (chosen == nullptr) return fail("no Unicode cmap subtable");
    const uint64_t offset = be32(chosen + 4);
    if (offset + 2 > cmap.length) return fail("cmap subtable out of bounds");
    const uint8_t* sub = cmap.data + offset;
    const uint64_t avail = cmap.length - offset;
    const uint32_t format = be16(sub);
    uint64_t length = 0;
    uint32_t count = 0;
    if (format == 4) {
        WISP_TRY(check_cmap_format4(sub, avail, num_glyphs, length, count));
    } else if (format == 12) {
        WISP_TRY(check_cmap_format12(sub, avail, num_glyphs, length, count));
    }
    if (format == 4 || format == 12) {
        choice = {{sub, static_cast<size_t>(length)}, format, count};
        return Result<void>::ok();
    }
    return fail("cmap subtable format " + std::to_string(format) + " is not supported");
}

Result<void> check_metrics(const Table& hhea, const Table& hmtx, uint32_t num_glyphs) {
    const uint64_t long_metrics = be16(hhea.data + 34);
    // stb_truetype reads the last long metric for the glyphs after them.
    if (long_metrics == 0) return fail("numberOfHMetrics is 0");
    uint64_t need = 4 * long_metrics;
    if (num_glyphs > long_metrics) need += 2 * (num_glyphs - long_metrics);
    if (hmtx.length < need) {
        return fail("'hmtx' table is " + std::to_string(hmtx.length) + " bytes, needs " +
                    std::to_string(need));
    }
    return Result<void>::ok();
}

Result<void> check_loca(const Table& loca, const Table& glyf, bool long_format,
                        uint32_t num_glyphs) {
    const uint64_t width = long_format ? 4 : 2;
    const uint64_t need = width * (uint64_t{num_glyphs} + 1);
    if (loca.length != need) {
        return fail("'loca' table is " + std::to_string(loca.length) + " bytes, expected " +
                    std::to_string(need));
    }
    uint64_t prev = 0;
    for (uint64_t i = 0; i <= num_glyphs; ++i) {
        const uint64_t at =
            long_format ? be32(loca.data + 4 * i) : 2 * uint64_t{be16(loca.data + 2 * i)};
        if (at < prev) return fail("'loca' entries decrease");
        if (at > glyf.length) return fail("'loca' entry past the end of 'glyf'");
        if (at != prev && at - prev < kGlyphHeaderBytes) {
            return fail("'loca' glyph shorter than its header");
        }
        prev = at;
    }
    return Result<void>::ok();
}

Result<void> check_min_length(const Table& t, const char* tag, uint64_t min) {
    if (t.length >= min) return Result<void>::ok();
    return fail(std::string("'") + tag + "' table is " + std::to_string(t.length) +
                " bytes, needs " + std::to_string(min));
}

// Spec 3.6 step 6. Offsets and lengths are summed in 64 bits so a hostile
// pair can't wrap around.
Result<void> check_sfnt(std::span<const uint8_t> ttf, CmapChoice& cmap) {
    constexpr size_t kOffsetTable = 12;
    constexpr size_t kRecord = 16;
    if (ttf.size() < kOffsetTable) return fail("TTF shorter than its offset table");
    const uint8_t* p = ttf.data();
    if (be32(p) != 0x00010000u) {
        char msg[48];
        std::snprintf(msg, sizeof msg, "sfnt version %08x is not TrueType",
                      static_cast<unsigned>(be32(p)));
        return fail(msg);
    }
    const uint32_t num_tables = be16(p + 4);
    if (num_tables == 0) return fail("no tables");
    const uint64_t dir_end = kOffsetTable + uint64_t{kRecord} * num_tables;
    if (dir_end > ttf.size()) return fail("table directory out of bounds");
    Table tables[std::size(kRequiredTables)];
    for (uint32_t i = 0; i < num_tables; ++i) {
        const uint8_t* rec = p + kOffsetTable + kRecord * i;
        const std::string tag(reinterpret_cast<const char*>(rec), 4);
        const uint64_t offset = be32(rec + 8);
        const uint64_t length = be32(rec + 12);
        if (offset < dir_end) return fail("table '" + tag + "' overlaps the table directory");
        if (offset + length > ttf.size()) return fail("table '" + tag + "' out of bounds");
        for (size_t t = 0; t < std::size(kRequiredTables); ++t) {
            // stb_truetype uses the first record with a tag.
            if (tables[t].data == nullptr && std::memcmp(rec, kRequiredTables[t], 4) == 0) {
                tables[t] = {p + offset, length};
            }
        }
    }
    for (size_t t = 0; t < std::size(kRequiredTables); ++t) {
        if (tables[t].data == nullptr) {
            return fail(std::string("no '") + kRequiredTables[t] + "' table");
        }
    }
    WISP_TRY(check_min_length(tables[kHead], "head", 54));
    WISP_TRY(check_min_length(tables[kHhea], "hhea", 36));
    WISP_TRY(check_min_length(tables[kMaxp], "maxp", 6));
    const uint32_t index_to_loc = be16(tables[kHead].data + 50);
    if (index_to_loc > 1) {
        return fail("indexToLocFormat " + std::to_string(index_to_loc) + " is not 0 or 1");
    }
    const uint32_t num_glyphs = be16(tables[kMaxp].data + 4);
    if (num_glyphs == 0) return fail("'maxp' lists no glyphs");
    WISP_TRY(check_cmap(tables[kCmap], num_glyphs, cmap));
    WISP_TRY(check_metrics(tables[kHhea], tables[kHmtx], num_glyphs));
    WISP_TRY(check_loca(tables[kLoca], tables[kGlyf], index_to_loc == 1, num_glyphs));
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
    CmapChoice cmap;
    WISP_TRY(check_sfnt(ttf, cmap));
    return Result<FontImage>::ok(FontImage{ttf, crc, cmap.subtable, cmap.format, cmap.count});
}

bool font_image_has_glyph(const FontImage& image, uint32_t codepoint) {
    // The format and counts come from the copies made when open_font_image()
    // checked the subtable, never from `cmap` itself (see FontImage). With
    // them every endCode/startCode/idDelta/idRangeOffset/group read below
    // stays inside the subtable whatever bytes it returns. The only offset
    // built from a value read here, the glyphIdArray position, is checked
    // against the subtable's length.
    const uint8_t* sub = image.cmap.data();
    if (image.cmap_format == 4) {
        if (codepoint > 0xFFFF) return false;
        const uint32_t segs = image.cmap_count;
        // The first segment whose endCode is at or above the character.
        uint32_t lo = 0;
        uint32_t hi = segs;
        while (lo < hi) {
            const uint32_t mid = (lo + hi) / 2;
            if (be16(sub + 14 + 2 * mid) < codepoint) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo == segs) return false;
        const uint32_t start = be16(sub + 16 + 2 * segs + 2 * lo);
        if (codepoint < start) return false;
        const uint32_t delta = be16(sub + 16 + 4 * segs + 2 * lo);
        const size_t ro_pos = 16 + 6 * size_t{segs} + 2 * lo;
        const uint32_t ro = be16(sub + ro_pos);
        if (ro == 0) return ((codepoint + delta) & 0xFFFF) != 0;
        const size_t at = ro_pos + ro + 2 * size_t{codepoint - start};
        if (at + 2 > image.cmap.size()) return false;
        return be16(sub + at) != 0;
    }
    if (image.cmap_format == 12) {
        uint32_t lo = 0;
        uint32_t hi = image.cmap_count;
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2;
            const uint8_t* g = sub + 16 + 12 * size_t{mid};
            if (codepoint < be32(g)) {
                hi = mid;
            } else if (codepoint > be32(g + 4)) {
                lo = mid + 1;
            } else {
                return be32(g + 8) + (codepoint - be32(g)) != 0;
            }
        }
    }
    return false;
}

}  // namespace wisp::ui
