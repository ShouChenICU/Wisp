#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/font_image.hpp"
#include "wisp/core/log.hpp"
#include "wisp/ui/font_image.hpp"
#include "wisp/ui/fonts.hpp"
#include "wisp/ui/lv_heap.hpp"
#include "wisp/ui/ui_runtime.hpp"

using wisp::test::FakeDisplay;
using wisp::test::test_font_image;
using wisp::ui::default_font;
using wisp::ui::font;
using wisp::ui::font_cache_entries;
using wisp::ui::font_step;
using wisp::ui::kFontSteps;
using wisp::ui::UiConfig;
using wisp::ui::UiRuntime;
using wisp::ui::vector_fonts_ready;

namespace {

std::unique_ptr<UiRuntime> start_ui(std::span<const uint8_t> image) {
    auto disp = std::make_shared<FakeDisplay>("display.main", 480, 480, 2);
    UiConfig config;
    config.font_image = image;
    auto ui = std::make_unique<UiRuntime>(disp, nullptr, config);
    EXPECT_TRUE(ui->start());
    return ui;
}

lv_font_glyph_dsc_t glyph(const lv_font_t* f, uint32_t letter) {
    lv_font_glyph_dsc_t dsc{};
    lv_font_get_glyph_dsc(f, &dsc, letter, 0);
    return dsc;
}

double estimated_entry_bytes(int step) { return 0.6 * step * step + 250; }

}  // namespace

TEST(VectorFont, StepsAndThreshold) {
    EXPECT_EQ(font_step(0), 16);
    EXPECT_EQ(font_step(19), 16);
    EXPECT_EQ(font_step(20), 20);
    EXPECT_EQ(font_step(23), 20);
    EXPECT_EQ(font_step(31), 28);
    EXPECT_EQ(font_step(47), 40);
    EXPECT_EQ(font_step(63), 48);
    EXPECT_EQ(font_step(128), 128);
    EXPECT_EQ(font_step(500), 128);
    for (int step : kFontSteps) EXPECT_EQ(font_step(step), step);
}

TEST(VectorFont, CacheEntriesPerStep) {
    EXPECT_EQ(font_cache_entries(20), 256u);
    EXPECT_EQ(font_cache_entries(24), 220u);
    EXPECT_EQ(font_cache_entries(32), 151u);
    EXPECT_EQ(font_cache_entries(48), 80u);
    EXPECT_EQ(font_cache_entries(64), 48u);
    EXPECT_EQ(font_cache_entries(128), 13u);
    for (int step : kFontSteps) {
        const size_t n = font_cache_entries(step);
        EXPECT_GE(n, 8u) << step;
        EXPECT_LE(n, 256u) << step;
        // Each step's cache stays within 128 KiB, give or take one entry.
        EXPECT_LE(n * estimated_entry_bytes(step), 131072 + estimated_entry_bytes(step)) << step;
    }
}

TEST(VectorFont, LargeSizesAreVectorWithCjk) {
    auto ui = start_ui(test_font_image());
    ui->run_blocking([] {
        EXPECT_TRUE(vector_fonts_ready());
        const lv_font_t* f = font(32);
        ASSERT_NE(f, nullptr);
        const auto zi = glyph(f, 0x81EA);  // 自
        EXPECT_EQ(zi.resolved_font, f);
        EXPECT_GE(zi.box_h, 24);
        EXPECT_EQ(glyph(f, 'A').resolved_font, f);
        EXPECT_EQ(font(33), f);
        EXPECT_EQ(font(16), default_font());
        EXPECT_EQ(font(19), default_font());
    });
    ui->stop();
}

TEST(VectorFont, LineHeightAndBaseline) {
    auto ui = start_ui(test_font_image());
    ui->run_blocking([] {
        EXPECT_EQ(font(24)->base_line, 6);
        EXPECT_EQ(font(24)->line_height, 31);
        EXPECT_EQ(font(48)->base_line, 13);
        EXPECT_EQ(font(48)->line_height, 62);
        EXPECT_EQ(font(128)->base_line, 35);
        EXPECT_EQ(font(128)->line_height, 163);
    });
    ui->stop();
}

TEST(VectorFont, GlyphsFitLineBox) {
    auto image = wisp::ui::open_font_image(test_font_image());
    ASSERT_TRUE(image) << image.error().message;
    struct Range {
        uint32_t first;
        uint32_t last;
        size_t min_present;  // guards against a filter that drops everything
    };
    const Range ranges[] = {
        {0x21, 0x7E, 94},     {0xA1, 0xFF, 95},       {0x3041, 0x3096, 83},
        {0x30A1, 0x30FA, 86}, {0x4E00, 0x9FFF, 6000},
    };
    std::vector<uint32_t> letters;
    for (const Range& r : ranges) {
        size_t present = 0;
        for (uint32_t c = r.first; c <= r.last; ++c) {
            if (!wisp::ui::font_image_has_glyph(image.value(), c)) continue;
            letters.push_back(c);
            ++present;
        }
        EXPECT_GE(present, r.min_present) << std::hex << r.first;
    }

    auto ui = start_ui(test_font_image());
    ui->run_blocking([&] {
        for (int px : {20, 48, 128}) {
            const lv_font_t* f = font(px);
            int failures = 0;
            std::string examples;
            for (uint32_t c : letters) {
                const auto dsc = glyph(f, c);
                ASSERT_EQ(dsc.resolved_font, f) << std::hex << c;
                // Rows of the bitmap within the line box; the bitmap's last
                // row is the blank one Tiny TTF adds, so the ink ends one
                // row before top + box_h.
                const int top = f->line_height - f->base_line - dsc.box_h - dsc.ofs_y;
                if (top < 0 || top + dsc.box_h - 1 > f->line_height) {
                    if (++failures <= 5) {
                        char buf[64];
                        std::snprintf(buf, sizeof buf, " U+%04X(top %d, h %d)",
                                      static_cast<unsigned>(c), top, dsc.box_h);
                        examples += buf;
                    }
                }
            }
            EXPECT_EQ(failures, 0) << px << " px:" << examples;
        }
    });
    ui->stop();
}

TEST(VectorFont, OutsideCharsetFallsBackToBody) {
    auto ui = start_ui(test_font_image());
    ui->run_blocking([] {
        // Hangul is in no Wisp font: nothing resolves it, and LVGL draws
        // its placeholder box.
        lv_font_glyph_dsc_t han{};
        EXPECT_FALSE(lv_font_get_glyph_dsc(font(48), &han, 0xD55C, 0));  // 한
        EXPECT_EQ(han.resolved_font, nullptr);
        // Icons are not in the vector font; at step 20 they come from the
        // 16 px body font.
        const auto ok = glyph(font(20), 0xF00C);  // LV_SYMBOL_OK
        ASSERT_NE(ok.resolved_font, nullptr);
        EXPECT_NE(ok.resolved_font, font(20));
        EXPECT_LE(ok.box_h, 16);
    });
    ui->stop();
}

TEST(VectorFont, MissingGlyphsFallBackQuietly) {
    // Without a lookup ahead of it, Tiny TTF logs an LVGL error for every
    // character the font lacks, each time it is measured or drawn.
    wisp::log::reset_for_test();
    wisp::log::init(64 * 1024);
    auto ui = start_ui(test_font_image());
    ui->run_blocking([] {
        for (int px : {20, 48}) {
            lv_obj_t* label = lv_label_create(lv_screen_active());
            lv_obj_set_style_text_font(label, font(px), 0);
            lv_label_set_text(label, "\xED\x95\x9C " LV_SYMBOL_OK " A");  // 한 OK A
            lv_obj_update_layout(label);
            lv_refr_now(nullptr);
        }
    });
    ui->stop();
    for (const auto& e : wisp::log::ring_entries()) {
        EXPECT_NE(e.tag, "lvgl") << e.msg;
    }
    wisp::log::reset_for_test();
}

namespace {

void expect_degraded() {
    EXPECT_FALSE(vector_fonts_ready());
    const auto zi = glyph(font(48), 0x81EA);  // 自
    EXPECT_NE(zi.resolved_font, nullptr);
    EXPECT_LE(zi.box_h, 16);
    EXPECT_EQ(font(48), font(64));
    EXPECT_EQ(font(16), default_font());
}

}  // namespace

TEST(VectorFont, CorruptImageDegrades) {
    std::vector<uint8_t> corrupt(test_font_image().begin(), test_font_image().end());
    ASSERT_GT(corrupt.size(), 1000u);
    corrupt[1000] ^= 0x01;
    auto ui = start_ui(corrupt);
    ui->run_blocking([] { expect_degraded(); });
    ui->stop();
}

TEST(VectorFont, NoImageDegrades) {
    auto ui = start_ui({});
    ui->run_blocking([] { expect_degraded(); });
    ui->stop();
}

TEST(VectorFont, RestartRecreatesFonts) {
    auto ui = start_ui(test_font_image());
    ui->run_blocking([] { ASSERT_EQ(glyph(font(32), 0x81EA).resolved_font, font(32)); });
    ui->stop();
    ASSERT_TRUE(ui->start());
    ui->run_blocking([] {
        EXPECT_TRUE(vector_fonts_ready());
        const lv_font_t* f = font(32);
        auto dsc = glyph(f, 0x81EA);  // 自
        ASSERT_EQ(dsc.resolved_font, f);
        EXPECT_NE(lv_font_get_glyph_bitmap(&dsc, nullptr), nullptr);
        lv_font_glyph_release_draw_data(&dsc);
    });
    ui->stop();
}

TEST(VectorFont, StopReleasesLvglHeap) {
    auto ui = start_ui(test_font_image());
    size_t first = 0;
    ui->run_blocking([&] {
        first = wisp::ui::lv_heap_bytes();
        for (int px : {20, 32, 48, 128}) {
            const lv_font_t* f = font(px);
            for (uint32_t c = 0x4E00; c < 0x4E00 + 20; ++c) {
                auto dsc = glyph(f, c);
                if (dsc.resolved_font != f) continue;
                EXPECT_NE(lv_font_get_glyph_bitmap(&dsc, nullptr), nullptr);
                lv_font_glyph_release_draw_data(&dsc);
            }
        }
        EXPECT_GT(wisp::ui::lv_heap_bytes(), first + 4096);
    });
    ui->stop();
    ASSERT_TRUE(ui->start());
    size_t second = 0;
    ui->run_blocking([&] { second = wisp::ui::lv_heap_bytes(); });
    ui->stop();
    const long diff = static_cast<long>(second) - static_cast<long>(first);
    EXPECT_LT(diff < 0 ? -diff : diff, 1024) << first << " -> " << second;
}
