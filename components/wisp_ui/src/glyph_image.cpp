#include "wisp/ui/glyph_image.hpp"

#include <cstring>
#include <new>
#include <string>
#include <utility>

#include "wisp/base/crc32.hpp"
#include "wisp/base/error.hpp"

// Why only the bitmap stays in the caller's bytes (spec 4.1): on the
// device they are a flash mapping, and a read that races a flash erase or
// write can return wrong bytes. Bitmap bytes read wrongly only draw wrong
// pixels. A wrong glyph index, list length or offset would send LVGL's
// lookups outside the font, so every value LVGL steers by is copied into
// Mem::Large memory and checked as copied (read once, so a second read
// cannot differ from the checked one), and LVGL reads only the copies.
// The format itself is described in glyph_image.hpp.

namespace wisp::ui {

namespace {

constexpr size_t kMetricsBytes = 32;
constexpr size_t kGlyphBytes = 10;
constexpr size_t kCmapBytes = 20;
constexpr size_t kKernPairBytes = 5;  // u16 left, u16 right, i8 value
constexpr uint32_t kAbsent = 0xFFFFFFFFu;
constexpr uint32_t kBpp = 4;

// Limits of the LVGL fields the values are stored in
// (lv_font_fmt_txt.h, LV_FONT_FMT_TXT_LARGE 0): a value past them would
// be silently truncated into a different, unchecked one.
constexpr uint32_t kMaxBitmapIndex = (1u << 20) - 1;
constexpr uint32_t kMaxAdvW = (1u << 12) - 1;
constexpr uint32_t kMaxCmaps = (1u << 9) - 1;
constexpr uint32_t kMaxKernPairs = (1u << 30) - 1;

uint32_t le16(const uint8_t* p) { return p[0] | (uint32_t{p[1]} << 8); }
uint32_t le32(const uint8_t* p) { return le16(p) | (le16(p + 2) << 16); }

using Opened = Result<std::unique_ptr<GlyphFont>>;

Opened invalid(std::string message) {
    return Opened::err(ErrorCode::InvalidArgument, "glyph image: " + std::move(message));
}

size_t align_up(size_t n) {
    constexpr size_t a = alignof(std::max_align_t);
    return (n + a - 1) / a * a;
}

struct Metrics {
    uint32_t line_height;
    int32_t base_line;
    int32_t underline_position;
    int32_t underline_thickness;
    uint32_t bpp;
    uint32_t kern_scale;
    uint32_t cmap_count;
    uint32_t glyph_count;
    uint32_t list_bytes;
    uint32_t kern_pairs;
    uint32_t bitmap_bytes;
};

Metrics read_metrics(const uint8_t* m) {
    return {le16(m),
            static_cast<int16_t>(le16(m + 2)),
            static_cast<int8_t>(m[4]),
            static_cast<int8_t>(m[5]),
            m[6],
            le16(m + 8),
            le16(m + 10),
            le32(m + 12),
            le32(m + 16),
            le32(m + 20),
            le32(m + 24)};
}

// Bytes per entry of a cmap type's list; the type has been checked first.
size_t list_entry_bytes(uint32_t type) { return type == LV_FONT_FMT_TXT_CMAP_SPARSE_TINY ? 2 : 1; }

std::string cmap_error(uint32_t index, const std::string& what) {
    return "glyph image: cmap " + std::to_string(index) + " " + what;
}

// A cmap record read once from the image. Sizing the tables and copying
// them both go through read_cmap(), so each structural bound lives in one
// place, and the copy uses the very values the copy pass checked.
struct CmapRecord {
    uint32_t range_start;
    uint32_t range_length;
    uint32_t glyph_id_start;
    uint32_t type;
    uint32_t list_length;
    uint32_t list_at;  // payload offset of its list, kAbsent for none
    size_t list_bytes;
};

// Reads cmap `index` (the 20 bytes at `c`) and checks it against the
// lists section, which `lists_at` and `list_bytes` locate by payload
// offset. Whether it maps past the last glyph is checked on the copied
// lists instead (cmap_glyph_end()).
Result<CmapRecord> read_cmap(const uint8_t* c, uint32_t index, uint64_t lists_at,
                             uint64_t list_bytes) {
    using R = Result<CmapRecord>;
    CmapRecord r{le32(c), le16(c + 4), le16(c + 6), c[8], le16(c + 10), kAbsent, 0};
    const uint32_t unicode_at = le32(c + 12);
    const uint32_t ofs_at = le32(c + 16);
    // SPARSE_FULL is not part of the format (tools/pack_glyph_image.py
    // rejects it), so it is refused here with every unknown value.
    switch (r.type) {
        case LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY:
            if (unicode_at != kAbsent || ofs_at != kAbsent) {
                return R::err(ErrorCode::InvalidArgument, cmap_error(index, "has a list"));
            }
            break;
        case LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL:
            if (unicode_at != kAbsent || ofs_at == kAbsent) {
                return R::err(ErrorCode::InvalidArgument,
                              cmap_error(index, "lists do not match type"));
            }
            if (r.list_length < r.range_length) {
                return R::err(ErrorCode::InvalidArgument,
                              cmap_error(index, "offset list shorter than its range"));
            }
            r.list_at = ofs_at;
            break;
        case LV_FONT_FMT_TXT_CMAP_SPARSE_TINY:
            if (unicode_at == kAbsent || ofs_at != kAbsent) {
                return R::err(ErrorCode::InvalidArgument,
                              cmap_error(index, "lists do not match type"));
            }
            r.list_at = unicode_at;
            break;
        default:
            return R::err(
                ErrorCode::InvalidArgument,
                cmap_error(index, "type " + std::to_string(r.type) + " is not supported"));
    }
    if (r.list_at != kAbsent) {
        const uint64_t bytes = uint64_t{r.list_length} * list_entry_bytes(r.type);
        if (r.list_at < lists_at || r.list_at + bytes > lists_at + list_bytes) {
            return R::err(ErrorCode::InvalidArgument,
                          cmap_error(index, "list out of the lists section"));
        }
        r.list_bytes = static_cast<size_t>(bytes);
    }
    return R::ok(r);
}

// One past the highest glyph id the copied cmap `c` maps to.
uint32_t cmap_glyph_end(const lv_font_fmt_txt_cmap_t& c) {
    if (c.type == LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY) return c.glyph_id_start + c.range_length;
    if (c.type == LV_FONT_FMT_TXT_CMAP_SPARSE_TINY) return c.glyph_id_start + c.list_length;
    // FORMAT0_FULL: LVGL indexes the offsets by code point - range_start,
    // below range_length.
    const auto* ofs = static_cast<const uint8_t*>(c.glyph_id_ofs_list);
    uint32_t top = 0;
    for (uint32_t i = 0; i < c.range_length; ++i) {
        if (ofs[i] + 1u > top) top = ofs[i] + 1u;
    }
    return c.glyph_id_start + top;
}

}  // namespace

Opened open_glyph_image(std::span<const uint8_t> raw) {
    if (raw.empty()) return Opened::err(ErrorCode::NotFound, "no glyph image");
    const uint8_t* p = raw.data();
    if (raw.size() < 4 || std::memcmp(p, "WGLY", 4) != 0) {
        // A freshly erased partition reads as 0xFF throughout.
        return Opened::err(ErrorCode::NotFound, "glyphs partition holds no glyph image");
    }
    if (raw.size() < kGlyphImageHeaderBytes) return invalid("header truncated");
    const uint32_t version = le16(p + 4);
    if (version != kGlyphImageVersion) {
        return Opened::err(ErrorCode::Unsupported, "glyph image format " + std::to_string(version) +
                                                       ", firmware reads " +
                                                       std::to_string(kGlyphImageVersion));
    }
    const uint32_t header_bytes = le16(p + 6);
    if (header_bytes != kGlyphImageHeaderBytes) {
        return invalid("header is " + std::to_string(header_bytes) + " bytes, expected " +
                       std::to_string(kGlyphImageHeaderBytes));
    }
    // The partition is larger than the image: the payload need only fit.
    const uint64_t length = le32(p + 8);
    if (header_bytes + length > raw.size()) {
        return invalid("payload length " + std::to_string(length) + " runs past the image");
    }
    const auto payload_span = raw.subspan(header_bytes, static_cast<size_t>(length));
    const uint32_t crc = le32(p + 12);
    if (crc32(payload_span) != crc) return invalid("CRC mismatch");
    const uint8_t* payload = payload_span.data();
    if (length < kMetricsBytes) return invalid("payload shorter than its metrics");

    const Metrics m = read_metrics(payload);
    if (m.bpp != kBpp) return invalid("bpp " + std::to_string(m.bpp) + ", expected 4");
    if (m.cmap_count > kMaxCmaps) return invalid("too many cmaps");
    if (m.kern_pairs > kMaxKernPairs) return invalid("too many kern pairs");
    if (m.glyph_count == 0) return invalid("no glyphs");

    // Section offsets in the payload, summed in 64 bits so that no count
    // can wrap them around.
    const uint64_t glyphs_at = kMetricsBytes;
    const uint64_t cmaps_at = glyphs_at + kGlyphBytes * uint64_t{m.glyph_count};
    const uint64_t lists_at = cmaps_at + kCmapBytes * uint64_t{m.cmap_count};
    const uint64_t kern_at = lists_at + m.list_bytes;
    const uint64_t bitmap_at = kern_at + kKernPairBytes * uint64_t{m.kern_pairs};
    if (bitmap_at + m.bitmap_bytes != length) {
        return invalid("sections add up to " + std::to_string(bitmap_at + m.bitmap_bytes) +
                       " bytes, payload is " + std::to_string(length));
    }

    // Only the list sizes are needed before allocating; every other check
    // runs below on the copies, after the last read of each value. Two
    // cmaps may name the same list, so the copies are sized by this sum,
    // not by list_bytes.
    size_t list_copy_bytes = 0;
    for (uint32_t i = 0; i < m.cmap_count; ++i) {
        auto r = read_cmap(payload + cmaps_at + kCmapBytes * i, i, lists_at, m.list_bytes);
        if (!r.is_ok()) return Opened::err(r.error().code, r.error().message);
        list_copy_bytes += r.value().list_bytes;
    }

    // One Mem::Large block holds every table: the descriptors first, then
    // each cmap's list on its own 2-byte boundary (the lists section packs
    // u8 and u16 arrays back to back, so a u16 list there can be odd).
    const size_t dsc_off = 0;
    const size_t kern_dsc_off = align_up(dsc_off + sizeof(lv_font_fmt_txt_dsc_t));
    const size_t glyph_off = align_up(kern_dsc_off + sizeof(lv_font_fmt_txt_kern_pair_t));
    const size_t cmap_off =
        align_up(glyph_off + sizeof(lv_font_fmt_txt_glyph_dsc_t) * size_t{m.glyph_count});
    const size_t list_off = align_up(cmap_off + sizeof(lv_font_fmt_txt_cmap_t) * m.cmap_count);
    const size_t kern_ids_off = align_up(list_off + list_copy_bytes + m.cmap_count);
    const size_t kern_values_off = kern_ids_off + 4 * size_t{m.kern_pairs};
    const size_t total = kern_values_off + m.kern_pairs;

    std::unique_ptr<GlyphFont> out(new (std::nothrow) GlyphFont());
    if (out == nullptr) return Opened::err(ErrorCode::NoMemory, "glyph image: no memory");
    out->tables = os::LargeBuffer::allocate(total);
    if (!out->tables.ok()) {
        return Opened::err(ErrorCode::NoMemory, "glyph image: no memory for " +
                                                    std::to_string(total) + " bytes of tables");
    }
    uint8_t* base = out->tables.data();
    std::memset(base, 0, total);

    // The memset above leaves every table a valid zeroed C struct; each
    // value below is read from the image once, checked, and stored as read.
    auto* glyphs = reinterpret_cast<lv_font_fmt_txt_glyph_dsc_t*>(base + glyph_off);
    for (uint32_t i = 0; i < m.glyph_count; ++i) {
        const uint8_t* g = payload + glyphs_at + kGlyphBytes * i;
        const uint32_t index = le32(g);
        const uint32_t adv_w = le16(g + 4);
        const uint32_t box_w = g[6];
        const uint32_t box_h = g[7];
        const uint64_t bytes = (uint64_t{box_w} * box_h * kBpp + 7) / 8;
        // Every index names a byte of the section, even an empty glyph's:
        // LVGL hands out &glyph_bitmap[bitmap_index] to a caller asking for
        // the raw bitmap.
        if (index > kMaxBitmapIndex || index >= m.bitmap_bytes || index + bytes > m.bitmap_bytes) {
            return invalid("glyph " + std::to_string(i) + "'s bitmap runs past the bitmap section");
        }
        if (adv_w > kMaxAdvW) return invalid("glyph " + std::to_string(i) + " too wide");
        glyphs[i].bitmap_index = index;
        glyphs[i].adv_w = adv_w;
        glyphs[i].box_w = static_cast<uint8_t>(box_w);
        glyphs[i].box_h = static_cast<uint8_t>(box_h);
        glyphs[i].ofs_x = static_cast<int8_t>(g[8]);
        glyphs[i].ofs_y = static_cast<int8_t>(g[9]);
    }

    auto* cmaps = reinterpret_cast<lv_font_fmt_txt_cmap_t*>(base + cmap_off);
    const size_t list_end = list_off + list_copy_bytes + m.cmap_count;
    size_t list_pos = list_off;
    for (uint32_t i = 0; i < m.cmap_count; ++i) {
        auto read = read_cmap(payload + cmaps_at + kCmapBytes * i, i, lists_at, m.list_bytes);
        if (!read.is_ok()) return Opened::err(read.error().code, read.error().message);
        const CmapRecord& r = read.value();
        if (r.type == LV_FONT_FMT_TXT_CMAP_SPARSE_TINY) list_pos += list_pos % 2;
        // Only a record that changed since the sizing pass can overrun the
        // space that pass reserved.
        if (list_pos + r.list_bytes > list_end) {
            return Opened::err(ErrorCode::InvalidArgument,
                               cmap_error(i, "changed while being read"));
        }
        lv_font_fmt_txt_cmap_t& dst = cmaps[i];
        dst.range_start = r.range_start;
        dst.range_length = static_cast<uint16_t>(r.range_length);
        dst.glyph_id_start = static_cast<uint16_t>(r.glyph_id_start);
        dst.type = static_cast<lv_font_fmt_txt_cmap_type_t>(r.type);
        dst.list_length = static_cast<uint16_t>(r.list_length);
        if (r.type == LV_FONT_FMT_TXT_CMAP_SPARSE_TINY) {
            auto* list = reinterpret_cast<uint16_t*>(base + list_pos);
            for (uint32_t k = 0; k < r.list_length; ++k) {
                list[k] = static_cast<uint16_t>(le16(payload + r.list_at + 2 * k));
            }
            dst.unicode_list = list;
        } else if (r.type == LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL) {
            std::memcpy(base + list_pos, payload + r.list_at, r.list_bytes);
            dst.glyph_id_ofs_list = base + list_pos;
        }
        list_pos += r.list_bytes;
        if (cmap_glyph_end(dst) > m.glyph_count) {
            return Opened::err(ErrorCode::InvalidArgument,
                               cmap_error(i, "maps past the last glyph"));
        }
    }

    const uint8_t* kern_ids = payload + kern_at;
    auto* kern_ids_out = reinterpret_cast<uint16_t*>(base + kern_ids_off);
    for (size_t i = 0; i < 2 * size_t{m.kern_pairs}; ++i) {
        kern_ids_out[i] = static_cast<uint16_t>(le16(kern_ids + 2 * i));
        if (kern_ids_out[i] >= m.glyph_count) {
            return invalid("kern pair " + std::to_string(i / 2) + " names a glyph past the last");
        }
    }
    auto* kern_values = reinterpret_cast<int8_t*>(base + kern_values_off);
    std::memcpy(kern_values, kern_ids + 4 * size_t{m.kern_pairs}, m.kern_pairs);
    auto* kern = reinterpret_cast<lv_font_fmt_txt_kern_pair_t*>(base + kern_dsc_off);
    kern->glyph_ids = kern_ids_out;
    kern->values = kern_values;
    kern->pair_cnt = m.kern_pairs;
    kern->glyph_ids_size = 1;  // 16-bit glyph ids

    auto* dsc = reinterpret_cast<lv_font_fmt_txt_dsc_t*>(base + dsc_off);
    dsc->glyph_bitmap = payload + bitmap_at;
    dsc->glyph_dsc = glyphs;
    dsc->cmaps = cmaps;
    dsc->kern_dsc = kern;
    dsc->kern_scale = static_cast<uint16_t>(m.kern_scale);
    dsc->cmap_num = static_cast<uint16_t>(m.cmap_count);
    dsc->bpp = kBpp;
    dsc->kern_classes = 0;
    dsc->bitmap_format = LV_FONT_FMT_TXT_PLAIN;
    dsc->stride = 0;

    lv_font_t& f = out->font;
    f.get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt;
    f.get_glyph_bitmap = lv_font_get_bitmap_fmt_txt;
    f.line_height = static_cast<int32_t>(m.line_height);
    f.base_line = m.base_line;
    f.subpx = LV_FONT_SUBPX_NONE;
    f.underline_position = static_cast<int8_t>(m.underline_position);
    f.underline_thickness = static_cast<int8_t>(m.underline_thickness);
    f.dsc = dsc;
    f.fallback = nullptr;
    f.user_data = nullptr;
    out->crc32 = crc;
    return Opened::ok(std::move(out));
}

}  // namespace wisp::ui
