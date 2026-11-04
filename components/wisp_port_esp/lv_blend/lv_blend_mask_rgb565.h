// A faster LVGL software-renderer hook for the commonest blend of a text
// screen: a solid colour through an 8-bit coverage mask (every glyph, and
// the anti-aliased edges of rounded boxes) at full opacity into an
// RGB565_SWAPPED buffer. Plain C, so any target can use it; a chip's hook
// header (lv_blend/<chip>/) includes it.
//
// LVGL's own loop calls the out-of-line lv_color_16_16_mix() for every
// partly covered pixel and looks at the mask two bytes at a time. This
// one inlines the same arithmetic (results are bit-identical, which
// tests/port_esp/lv_blend_mask_test.cpp checks against LVGL itself) and
// skips or fills four uncovered or fully covered pixels at once.

#pragma once

#include <stdint.h>

#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"

#ifdef __cplusplus
extern "C" {
#endif

// Four mask bytes or two pixels read or written as one aligned word.
// Plain loads and stores rather than memcpy(), which some toolchains
// (ESP-IDF's among them) leave as a call into ROM.
typedef uint32_t __attribute__((may_alias, aligned(4))) wisp_lv_word_t;

#define WISP_LV_INLINE static inline __attribute__((always_inline))

// lv_color_16_16_mix(c1, c2, mix) for 0 < mix < 255 and c1 != c2, with
// c1 already spread as (c1 | c1 << 16) & 0x07E0F81F.
WISP_LV_INLINE uint16_t wisp_lv_mix_565(uint32_t fg, uint16_t c2, uint8_t mix) {
    const uint32_t m = ((uint32_t)mix + 4) >> 3;
    const uint32_t bg = ((uint32_t)c2 | ((uint32_t)c2 << 16)) & 0x07E0F81Fu;
    const uint32_t r = ((((fg - bg) * m) >> 5) + bg) & 0x07E0F81Fu;
    return (uint16_t)((r >> 16) | r);
}

WISP_LV_INLINE void wisp_lv_mask_px_swapped(uint16_t* d, uint8_t m, uint16_t c, uint16_t c_swapped,
                                            uint32_t fg) {
    if (m == 0) return;
    if (m == 255) {
        *d = c_swapped;
        return;
    }
    const uint16_t px = (uint16_t)((*d >> 8) | (*d << 8));
    if (px == c) return;
    const uint16_t out = wisp_lv_mix_565(fg, px, m);
    *d = (uint16_t)((out >> 8) | (out << 8));
}

static inline lv_result_t wisp_lv_fill_mask_rgb565_swapped(lv_draw_sw_blend_fill_dsc_t* dsc) {
    const int32_t w = dsc->dest_w;
    const int32_t h = dsc->dest_h;
    const uint16_t c = lv_color_to_u16(dsc->color);
    const uint16_t cs = (uint16_t)((c >> 8) | (c << 8));
    const uint32_t fg = ((uint32_t)c | ((uint32_t)c << 16)) & 0x07E0F81Fu;
    const uint32_t cs2 = (uint32_t)cs | ((uint32_t)cs << 16);
    uint8_t* row = (uint8_t*)dsc->dest_buf;
    const lv_opa_t* mask = dsc->mask_buf;

    for (int32_t y = 0; y < h; ++y) {
        uint16_t* d = (uint16_t*)row;
        int32_t x = 0;
        // Up to the first mask byte on a 4-byte boundary, one at a time.
        while (x < w && ((uintptr_t)(mask + x) & 3u) != 0) {
            wisp_lv_mask_px_swapped(&d[x], mask[x], c, cs, fg);
            ++x;
        }
        for (; x + 4 <= w; x += 4) {
            const uint32_t m4 = *(const wisp_lv_word_t*)(mask + x);
            if (m4 == 0) continue;
            if (m4 == 0xFFFFFFFFu) {
                if (((uintptr_t)&d[x] & 3u) == 0) {
                    *(wisp_lv_word_t*)&d[x] = cs2;
                    *(wisp_lv_word_t*)&d[x + 2] = cs2;
                } else {
                    d[x] = d[x + 1] = d[x + 2] = d[x + 3] = cs;
                }
                continue;
            }
            wisp_lv_mask_px_swapped(&d[x], mask[x], c, cs, fg);
            wisp_lv_mask_px_swapped(&d[x + 1], mask[x + 1], c, cs, fg);
            wisp_lv_mask_px_swapped(&d[x + 2], mask[x + 2], c, cs, fg);
            wisp_lv_mask_px_swapped(&d[x + 3], mask[x + 3], c, cs, fg);
        }
        for (; x < w; ++x) wisp_lv_mask_px_swapped(&d[x], mask[x], c, cs, fg);
        row += dsc->dest_stride;
        mask += dsc->mask_stride;
    }
    return LV_RESULT_OK;
}

#ifdef __cplusplus
} /*extern "C"*/
#endif
