// The masked fill hook (components/wisp_port_esp/lv_blend/lv_blend_mask_rgb565.h)
// writes exactly what LVGL's own C path writes, for any size, stride,
// alignment, colour and mask. On the host LVGL is built without hooks, so
// lv_draw_sw_blend_color_to_rgb565_swapped() is the reference.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "lv_blend/lv_blend_mask_rgb565.h"
#include "lvgl.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_to_rgb565_swapped.h"

namespace {

// Glyph-like coverage: mostly empty or full, with edges in between.
uint8_t mask_value(std::mt19937& rng) {
    switch (rng() % 6) {
        case 0:
        case 1:
            return 0;
        case 2:
            return 255;
        case 3:
            return static_cast<uint8_t>(1 + rng() % 7);  // barely covered
        default:
            return static_cast<uint8_t>(rng());
    }
}

struct Case {
    int w, h, dest_stride, mask_stride, dest_off, mask_off;
};

TEST(LvBlendMask, MatchesLvglForAnyShape) {
    lv_init();
    std::mt19937 rng(1234);
    for (int i = 0; i < 4000; ++i) {
        Case c{};
        c.w = 1 + static_cast<int>(rng() % (i % 4 == 0 ? 9 : 300));
        c.h = 1 + static_cast<int>(rng() % 12);
        c.dest_off = 2 * static_cast<int>(rng() % 4);
        c.mask_off = static_cast<int>(rng() % 4);
        c.dest_stride = c.w * 2 + 2 * static_cast<int>(rng() % 5);
        c.mask_stride = c.w + static_cast<int>(rng() % 5);

        std::vector<uint8_t> dest(c.dest_off + static_cast<size_t>(c.dest_stride) * c.h + 8);
        std::vector<uint8_t> mask(c.mask_off + static_cast<size_t>(c.mask_stride) * c.h + 8);
        for (auto& b : dest) b = static_cast<uint8_t>(rng());
        // Runs of one value, as glyph rows have.
        for (size_t k = 0; k < mask.size();) {
            const uint8_t v = mask_value(rng);
            for (size_t n = 1 + rng() % 9; n > 0 && k < mask.size(); --n) mask[k++] = v;
        }
        std::vector<uint8_t> want = dest;

        lv_draw_sw_blend_fill_dsc_t d{};
        d.dest_w = c.w;
        d.dest_h = c.h;
        d.dest_stride = c.dest_stride;
        d.mask_stride = c.mask_stride;
        d.opa = LV_OPA_COVER;
        d.color = lv_color_make(static_cast<uint8_t>(rng()), static_cast<uint8_t>(rng()),
                                static_cast<uint8_t>(rng()));
        if (i % 7 == 0) d.color = lv_color_black();
        d.mask_buf = mask.data() + c.mask_off;

        d.dest_buf = want.data() + c.dest_off;
        lv_draw_sw_blend_color_to_rgb565_swapped(&d);

        d.dest_buf = dest.data() + c.dest_off;
        ASSERT_EQ(wisp_lv_fill_mask_rgb565_swapped(&d), LV_RESULT_OK);
        ASSERT_TRUE(dest == want) << "case " << i << ": w=" << c.w << " h=" << c.h
                                  << " dest_off=" << c.dest_off << " mask_off=" << c.mask_off;
    }
    lv_deinit();
}

}  // namespace
