#include "a8_font.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "wisp/os/os.hpp"

namespace wisp::ui {

namespace {

constexpr size_t kArenaChunk = 32 * 1024;

struct A8Font {
    lv_font_t font;  // first, so the lv_font_t* LVGL hands back is the A8Font*
    const lv_font_t* base = nullptr;
    const lv_font_fmt_txt_dsc_t* dsc = nullptr;
    uint32_t glyph_count = 0;
    uint8_t** slots = nullptr;  // per glyph id: its A8 bitmap, or nullptr
    bool slots_failed = false;
};

// Bump allocator over Mem::Large chunks shared by every wrapper; the
// cached bitmaps live as long as the program.
struct Arena {
    uint8_t* chunk = nullptr;
    size_t used = kArenaChunk;
    size_t total = 0;
    std::vector<uint8_t*> chunks;  // every chunk taken, for reset()

    uint8_t* take(size_t bytes) {
        if (bytes > kArenaChunk || total + bytes > kA8CacheBytes) return nullptr;
        if (used + bytes > kArenaChunk) {
            chunk = static_cast<uint8_t*>(os::alloc(kArenaChunk, os::Mem::Large));
            if (chunk == nullptr) return nullptr;
            chunks.push_back(chunk);
            used = 0;
        }
        uint8_t* p = chunk + used;
        used += bytes;
        total += bytes;
        return p;
    }

    void reset() {
        for (uint8_t* c : chunks) os::dealloc(c);
        chunks.clear();
        chunk = nullptr;
        used = kArenaChunk;
        total = 0;
    }
};

Arena& arena() {
    static Arena a;
    return a;
}

bool supported(const lv_font_t* f) {
    if (f->get_glyph_dsc != &lv_font_get_glyph_dsc_fmt_txt ||
        f->get_glyph_bitmap != &lv_font_get_bitmap_fmt_txt) {
        return false;
    }
    const auto* d = static_cast<const lv_font_fmt_txt_dsc_t*>(f->dsc);
    return d->bitmap_format == LV_FONT_FMT_TXT_PLAIN && d->stride == 0 &&
           (d->bpp == 1 || d->bpp == 2 || d->bpp == 4);
}

// One past the largest glyph id any of the font's character maps uses.
uint32_t glyph_count(const lv_font_fmt_txt_dsc_t* d) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->cmap_num; ++i) {
        const lv_font_fmt_txt_cmap_t& c = d->cmaps[i];
        uint32_t top = 0;
        switch (c.type) {
            case LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY:
                top = c.range_length;
                break;
            case LV_FONT_FMT_TXT_CMAP_SPARSE_TINY:
                top = c.list_length;
                break;
            case LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL: {
                const auto* ofs = static_cast<const uint8_t*>(c.glyph_id_ofs_list);
                for (uint32_t k = 0; k < c.range_length; ++k)
                    top = std::max<uint32_t>(top, ofs[k] + 1);
                break;
            }
            case LV_FONT_FMT_TXT_CMAP_SPARSE_FULL: {
                const auto* ofs = static_cast<const uint16_t*>(c.glyph_id_ofs_list);
                for (uint32_t k = 0; k < c.list_length; ++k)
                    top = std::max<uint32_t>(top, ofs[k] + 1);
                break;
            }
        }
        n = std::max<uint32_t>(n, c.glyph_id_start + top);
    }
    return n;
}

// Expands glyph `gid` into `out` (box_w * box_h bytes, rows unpadded)
// with the same values lv_font_get_bitmap_fmt_txt() produces: the
// font's pixels are packed most significant bits first and run on from
// one row into the next.
void expand(const lv_font_fmt_txt_dsc_t* d, uint32_t gid, uint8_t* out) {
    const lv_font_fmt_txt_glyph_dsc_t& g = d->glyph_dsc[gid];
    const uint8_t* in = &d->glyph_bitmap[g.bitmap_index];
    const uint32_t bpp = d->bpp;
    const uint32_t mask = (1u << bpp) - 1;
    const uint32_t scale = 255 / mask;  // 1 bpp: 255, 2 bpp: 85, 4 bpp: 17
    const uint32_t n = static_cast<uint32_t>(g.box_w) * g.box_h;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t bit = i * bpp;
        const uint32_t v = (in[bit / 8] >> (8 - bpp - bit % 8)) & mask;
        out[i] = static_cast<uint8_t>(v * scale);
    }
}

// The cached A8 bitmap of glyph `gid`, expanding it on first use;
// nullptr if it cannot be cached.
const uint8_t* cached(A8Font* self, uint32_t gid) {
    if (gid == 0 || gid >= self->glyph_count) return nullptr;
    if (self->slots == nullptr) {
        if (self->slots_failed) return nullptr;
        const size_t bytes = sizeof(uint8_t*) * self->glyph_count;
        self->slots = static_cast<uint8_t**>(os::alloc(bytes, os::Mem::Large));
        if (self->slots == nullptr) {
            self->slots_failed = true;
            return nullptr;
        }
        std::memset(self->slots, 0, bytes);
    }
    if (self->slots[gid] != nullptr) return self->slots[gid];
    const lv_font_fmt_txt_glyph_dsc_t& g = self->dsc->glyph_dsc[gid];
    const size_t bytes = static_cast<size_t>(g.box_w) * g.box_h;
    if (bytes == 0) return nullptr;
    uint8_t* out = arena().take(bytes);
    if (out == nullptr) return nullptr;
    expand(self->dsc, gid, out);
    self->slots[gid] = out;
    return out;
}

bool get_glyph_dsc(const lv_font_t* font, lv_font_glyph_dsc_t* g, uint32_t letter, uint32_t next) {
    auto* self = reinterpret_cast<A8Font*>(const_cast<lv_font_t*>(font));
    if (!lv_font_get_glyph_dsc_fmt_txt(self->base, g, letter, next)) return false;
    // A tab reports a doubled box over the space glyph's bitmap; such a
    // glyph keeps the base format and is expanded per draw as usual.
    const lv_font_fmt_txt_glyph_dsc_t& gd = self->dsc->glyph_dsc[g->gid.index];
    if (g->box_w != gd.box_w || g->box_h != gd.box_h) return true;
    if (cached(self, g->gid.index) != nullptr) {
        g->format = LV_FONT_GLYPH_FORMAT_A8;
        g->stride = g->box_w;
    }
    return true;
}

const void* get_glyph_bitmap(lv_font_glyph_dsc_t* g, lv_draw_buf_t* draw_buf) {
    auto* self = reinterpret_cast<A8Font*>(const_cast<lv_font_t*>(g->resolved_font));
    const uint8_t* bitmap =
        g->format == LV_FONT_GLYPH_FORMAT_A8 ? cached(self, g->gid.index) : nullptr;
    if (bitmap == nullptr) {
        // Not cached: the base font expands it as it always does, reading
        // its own unpadded rows.
        lv_font_glyph_dsc_t base_g = *g;
        base_g.resolved_font = self->base;
        base_g.format = static_cast<lv_font_glyph_format_t>(self->dsc->bpp);
        base_g.stride = 0;
        return lv_font_get_bitmap_fmt_txt(&base_g, draw_buf);
    }
    if (g->req_raw_bitmap) return bitmap;
    // A rotated letter is drawn as an image from `draw_buf`.
    const uint32_t w = g->box_w;
    for (uint32_t y = 0; y < g->box_h; ++y) {
        std::memcpy(draw_buf->data + y * draw_buf->header.stride, bitmap + y * w, w);
    }
    return draw_buf;
}

std::vector<std::unique_ptr<A8Font>>& wrappers() {
    static std::vector<std::unique_ptr<A8Font>> list;
    return list;
}

}  // namespace

const lv_font_t* a8_font(const lv_font_t* base) {
    if (base == nullptr || !supported(base)) return base;
    for (const auto& w : wrappers()) {
        if (w->base == base) return &w->font;
    }
    auto w = std::make_unique<A8Font>();
    w->font = *base;
    w->base = base;
    w->dsc = static_cast<const lv_font_fmt_txt_dsc_t*>(base->dsc);
    w->glyph_count = glyph_count(w->dsc);
    w->font.get_glyph_dsc = &get_glyph_dsc;
    w->font.get_glyph_bitmap = &get_glyph_bitmap;
    w->font.static_bitmap = 1;
    w->font.fallback = a8_font(base->fallback);
    wrappers().push_back(std::move(w));
    return &wrappers().back()->font;
}

void a8_set_fallback(const lv_font_t* wrapper, const lv_font_t* fallback) {
    // Wrapped before the search: a8_font() may add to the list.
    const lv_font_t* wrapped = a8_font(fallback);
    for (const auto& w : wrappers()) {
        if (&w->font == wrapper) {
            w->font.fallback = wrapped;
            return;
        }
    }
}

void reset_a8_cache_for_test() {
    for (const auto& w : wrappers()) {
        os::dealloc(w->slots);
        w->slots = nullptr;
        w->slots_failed = false;
    }
    arena().reset();
}

}  // namespace wisp::ui
