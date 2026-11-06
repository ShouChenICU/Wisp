// The A8 glyph cache fonts (components/wisp_ui/src/a8_font.hpp): every
// glyph reaches LVGL with the same metrics and the same 8-bit coverage as
// the base font's, cached or not, and the UI renders identically.

#include "a8_font.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/lvgl_probe.hpp"
#include "wisp/ui/ui_runtime.hpp"

extern "C" {
extern const lv_font_t lv_font_cjk_16;
}

namespace {

using wisp::test::FakeDisplay;

// Every code point a lv_font_fmt_txt font maps.
std::vector<uint32_t> code_points(const lv_font_t* font) {
    const auto* d = static_cast<const lv_font_fmt_txt_dsc_t*>(font->dsc);
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < d->cmap_num; ++i) {
        const lv_font_fmt_txt_cmap_t& c = d->cmaps[i];
        if (c.unicode_list == nullptr) {
            for (uint32_t k = 0; k < c.range_length; ++k) out.push_back(c.range_start + k);
        } else {
            for (uint32_t k = 0; k < c.list_length; ++k)
                out.push_back(c.range_start + c.unicode_list[k]);
        }
    }
    return out;
}

// The 8-bit coverage LVGL gets for `letter` from `font`, as rows of
// box_w bytes, via the path a straight letter takes (the static bitmap
// when the glyph is A8) or, with `via_draw_buf`, the one a rotated
// letter takes. Empty if the font has no such glyph.
std::vector<uint8_t> coverage(const lv_font_t* font, uint32_t letter, bool via_draw_buf,
                              lv_font_glyph_dsc_t* g) {
    std::vector<uint8_t> out;
    if (!lv_font_get_glyph_dsc(font, g, letter, 0) || g->box_w == 0 || g->box_h == 0) return out;
    const bool raw = !via_draw_buf && lv_font_has_static_bitmap(g->resolved_font) &&
                     g->format == LV_FONT_GLYPH_FORMAT_A8;
    if (raw) {
        const auto* bitmap = static_cast<const uint8_t*>(lv_font_get_glyph_static_bitmap(g));
        for (uint32_t y = 0; y < g->box_h; ++y) {
            out.insert(out.end(), bitmap + y * g->stride, bitmap + y * g->stride + g->box_w);
        }
        return out;
    }
    lv_draw_buf_t* buf = lv_draw_buf_create(g->box_w, g->box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    const auto* got = static_cast<const lv_draw_buf_t*>(lv_font_get_glyph_bitmap(g, buf));
    if (got != nullptr) {
        for (uint32_t y = 0; y < g->box_h; ++y) {
            const uint8_t* row = got->data + y * got->header.stride;
            out.insert(out.end(), row, row + g->box_w);
        }
    }
    lv_draw_buf_destroy(buf);
    return out;
}

class A8FontTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Each test starts from an empty cache, whatever ran before.
        wisp::ui::reset_a8_cache_for_test();
        disp_ = std::make_shared<FakeDisplay>("display.main", 480, 480);
        ui_ = std::make_unique<wisp::ui::UiRuntime>(disp_, nullptr);
        ASSERT_TRUE(ui_->start());
    }
    void TearDown() override { ui_->stop(); }

    // Compares every glyph of `base` (and its fallbacks) with the wrapper;
    // returns the first difference.
    std::string compare_all(const lv_font_t* base) {
        std::string problem;
        wisp::test::run_on_ui_blocking(ui_.get(), [&] {
            const lv_font_t* wrapped = wisp::ui::a8_font(base);
            if (wrapped == base) {
                problem = "not wrapped";
                return;
            }
            for (const lv_font_t* f = base; f != nullptr && problem.empty(); f = f->fallback) {
                for (uint32_t cp : code_points(f)) {
                    for (bool via_draw_buf : {false, true}) {
                        lv_font_glyph_dsc_t gb{};
                        lv_font_glyph_dsc_t gw{};
                        const auto want = coverage(base, cp, false, &gb);
                        const auto got = coverage(wrapped, cp, via_draw_buf, &gw);
                        if (gb.adv_w != gw.adv_w || gb.box_w != gw.box_w || gb.box_h != gw.box_h ||
                            gb.ofs_x != gw.ofs_x || gb.ofs_y != gw.ofs_y || want != got) {
                            problem =
                                "U+" + std::to_string(cp) + (via_draw_buf ? " (draw buf)" : "");
                            return;
                        }
                    }
                }
            }
        });
        return problem;
    }

    std::shared_ptr<FakeDisplay> disp_;
    std::unique_ptr<wisp::ui::UiRuntime> ui_;
};

TEST_F(A8FontTest, TheDefaultFontHandsOutA8Glyphs) {
    std::string problem;
    wisp::test::run_on_ui_blocking(ui_.get(), [&] {
        const lv_font_t* font = wisp::ui::default_font();
        if (!lv_font_has_static_bitmap(font)) problem = "not a static-bitmap font";
        for (uint32_t cp : {static_cast<uint32_t>('W'), 0x4E2Du /* 中 */, 0x3042u /* あ */}) {
            lv_font_glyph_dsc_t g{};
            if (!lv_font_get_glyph_dsc(font, &g, cp, 0) || g.format != LV_FONT_GLYPH_FORMAT_A8) {
                problem = "U+" + std::to_string(cp) + " is not A8";
            }
        }
    });
    EXPECT_EQ(problem, "");
}

// Montserrat's few hundred glyphs all fit the cache; the CJK font's
// thousands do not, so its later glyphs go the uncached way.
TEST_F(A8FontTest, EveryGlyphMatchesTheBaseFont) {
    static lv_font_t base = [] {
        lv_font_t f = lv_font_montserrat_16;
        f.fallback = &lv_font_cjk_16;
        return f;
    }();
    EXPECT_EQ(compare_all(&base), "");
}

TEST_F(A8FontTest, TextRendersIdentically) {
    static lv_font_t base = [] {
        lv_font_t f = lv_font_montserrat_16;
        f.fallback = &lv_font_cjk_16;
        return f;
    }();
    lv_obj_t* label = nullptr;
    wisp::test::run_on_ui_blocking(ui_.get(), [&] {
        label = lv_label_create(lv_screen_active());
        lv_obj_set_width(label, 460);
        lv_label_set_text(label,
                          "Wisp 0123456789 AVAWA Tg,;\tjy\n自检 网络 设置 你好世界\n"
                          "セットアップ ネットワーク ひらがな");
        lv_obj_set_style_text_font(label, &base, 0);
    });
    auto want = ui_->snapshot_rgb888(5000);
    ASSERT_TRUE(want);
    wisp::test::run_on_ui_blocking(
        ui_.get(), [&] { lv_obj_set_style_text_font(label, wisp::ui::a8_font(&base), 0); });
    auto got = ui_->snapshot_rgb888(5000);
    ASSERT_TRUE(got);
    EXPECT_TRUE(want.value() == got.value());
}

// The body font's CJK fallback is chosen when a UiRuntime starts
// (fonts.cpp), after default_font() has handed its wrapper out, so the
// wrapper's fallback is rewired in place and wrapped like a8_font()'s.
TEST_F(A8FontTest, SetFallbackRewires) {
    static lv_font_t base = [] {
        lv_font_t f = lv_font_montserrat_16;
        f.fallback = nullptr;
        return f;
    }();
    bool with_cjk = false;
    const lv_font_t* resolved = nullptr;
    const lv_font_t* cjk_wrapper = nullptr;
    bool without_cjk = true;
    bool latin = false;
    wisp::test::run_on_ui_blocking(ui_.get(), [&] {
        const lv_font_t* w = wisp::ui::a8_font(&base);
        cjk_wrapper = wisp::ui::a8_font(&lv_font_cjk_16);
        lv_font_glyph_dsc_t g{};
        wisp::ui::a8_set_fallback(w, &lv_font_cjk_16);
        with_cjk = lv_font_get_glyph_dsc(w, &g, 0x4F60 /* 你 */, 0);
        resolved = g.resolved_font;
        wisp::ui::a8_set_fallback(w, nullptr);
        without_cjk = lv_font_get_glyph_dsc(w, &g, 0x4F60, 0);
        latin = lv_font_get_glyph_dsc(w, &g, 'A', 0);
    });
    EXPECT_TRUE(with_cjk);
    EXPECT_EQ(resolved, cjk_wrapper);
    EXPECT_NE(cjk_wrapper, &lv_font_cjk_16);
    EXPECT_FALSE(without_cjk);
    EXPECT_TRUE(latin);
}

}  // namespace
