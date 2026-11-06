// The body font's CJK fallback (glyph-partition spec 4.2, 4.3): drawn from
// the glyph image when it opens, otherwise from the host's built-in
// lv_font_cjk_16, otherwise from a 16 px vector font, otherwise not at
// all -- and UiRuntime starts in every case.

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <vector>

#include "a8_font.hpp"
#include "support/fake_devices.hpp"
#include "support/font_image.hpp"
#include "wisp/ui/fonts.hpp"
#include "wisp/ui/ui_runtime.hpp"

extern "C" {
extern const lv_font_t lv_font_cjk_16;
}

namespace {

using wisp::test::FakeDisplay;
using wisp::test::test_font_image;
using wisp::ui::a8_font;
using wisp::ui::default_font;
using wisp::ui::UiConfig;
using wisp::ui::UiRuntime;

constexpr uint32_t kNi = 0x4F60;   // 你
constexpr uint32_t kHan = 0xD55C;  // 한, not in the body font

// The packed body font; kept for the whole process, as UiConfig requires
// of the bytes a glyph font was built from.
std::span<const uint8_t> glyph_image() {
    static const std::vector<uint8_t> bytes = [] {
        std::ifstream in(WISP_GLYPH_IMAGE, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>());
    }();
    EXPECT_FALSE(bytes.empty()) << "cannot read " << WISP_GLYPH_IMAGE;
    return bytes;
}

struct Lookup {
    bool found = false;
    lv_font_glyph_dsc_t dsc{};
};

class GlyphFontTest : public ::testing::Test {
protected:
    void start(std::span<const uint8_t> glyphs, bool builtin, std::span<const uint8_t> vector) {
        auto disp = std::make_shared<FakeDisplay>("display.main", 480, 480, 2);
        UiConfig config;
        config.glyph_image = glyphs;
        config.builtin_glyphs = builtin;
        config.font_image = vector;
        ui_ = std::make_unique<UiRuntime>(disp, nullptr, config);
        ASSERT_TRUE(ui_->start());
    }

    void TearDown() override {
        if (ui_) ui_->stop();
    }

    // Looks `letter` up in the body font on the UI task: Tiny TTF fonts
    // use LVGL's caches, which only the UI task touches.
    Lookup lookup(uint32_t letter) {
        Lookup out;
        ui_->run_blocking(
            [&] { out.found = lv_font_get_glyph_dsc(default_font(), &out.dsc, letter, 0); });
        return out;
    }

    std::unique_ptr<UiRuntime> ui_;
};

TEST_F(GlyphFontTest, BuiltinBeforeAnyRuntime) {
    lv_font_glyph_dsc_t dsc{};
    ASSERT_TRUE(lv_font_get_glyph_dsc(default_font(), &dsc, kNi, 0));
    EXPECT_EQ(dsc.resolved_font, a8_font(&lv_font_cjk_16));
}

TEST_F(GlyphFontTest, UsesGlyphImage) {
    start(glyph_image(), true, {});
    const Lookup ni = lookup(kNi);
    ASSERT_TRUE(ni.found);
    const lv_font_t* builtin = a8_font(&lv_font_cjk_16);
    // An A8 wrapper (same lookup function as the built-in's wrapper) whose
    // base, and so whose tables, are the image's rather than the built-in's.
    EXPECT_NE(ni.dsc.resolved_font, builtin);
    EXPECT_EQ(ni.dsc.resolved_font->get_glyph_dsc, builtin->get_glyph_dsc);
    EXPECT_NE(ni.dsc.resolved_font->dsc, lv_font_cjk_16.dsc);

    lv_font_glyph_dsc_t want{};
    ASSERT_TRUE(lv_font_get_glyph_dsc(&lv_font_cjk_16, &want, kNi, 0));
    EXPECT_EQ(ni.dsc.adv_w, want.adv_w);
    EXPECT_EQ(ni.dsc.box_w, want.box_w);
    EXPECT_EQ(ni.dsc.box_h, want.box_h);
    EXPECT_EQ(ni.dsc.ofs_x, want.ofs_x);
    EXPECT_EQ(ni.dsc.ofs_y, want.ofs_y);
}

TEST_F(GlyphFontTest, BuiltinWhenNoImage) {
    start({}, true, {});
    const Lookup ni = lookup(kNi);
    ASSERT_TRUE(ni.found);
    EXPECT_EQ(ni.dsc.resolved_font, a8_font(&lv_font_cjk_16));
}

TEST_F(GlyphFontTest, DegradesToVector16) {
    start({}, false, test_font_image());
    const Lookup ni = lookup(kNi);
    ASSERT_TRUE(ni.found);
    ASSERT_NE(ni.dsc.resolved_font, nullptr);
    EXPECT_NE(ni.dsc.resolved_font->get_glyph_dsc, &lv_font_get_glyph_dsc_fmt_txt);
    EXPECT_NE(ni.dsc.resolved_font->get_glyph_dsc, a8_font(&lv_font_cjk_16)->get_glyph_dsc);
    EXPECT_GT(ni.dsc.box_h, 0);
    EXPECT_LE(ni.dsc.box_h, 16 + 2);
    // Latin text stays on Montserrat.
    EXPECT_EQ(lookup('A').dsc.resolved_font, default_font());
}

TEST_F(GlyphFontTest, NoCjkWithoutAnyImage) {
    start({}, false, {});
    EXPECT_FALSE(lookup(kNi).found);
    const Lookup a = lookup('A');
    EXPECT_TRUE(a.found);
    EXPECT_EQ(a.dsc.resolved_font, default_font());
}

TEST_F(GlyphFontTest, RestartReusesGlyphFont) {
    const lv_font_t* first = nullptr;
    for (int i = 0; i < 3; ++i) {
        start(glyph_image(), true, {});
        const Lookup ni = lookup(kNi);
        ASSERT_TRUE(ni.found);
        ASSERT_NE(ni.dsc.resolved_font, a8_font(&lv_font_cjk_16));
        if (first == nullptr) first = ni.dsc.resolved_font;
        EXPECT_EQ(ni.dsc.resolved_font, first) << "start " << i;
        ui_->stop();
        ui_.reset();
    }
}

TEST_F(GlyphFontTest, MissingCharacterFallsThrough) {
    start(glyph_image(), true, test_font_image());
    EXPECT_FALSE(lookup(kHan).found);
    EXPECT_TRUE(lookup(kNi).found);
}

// After stop() the body font no longer points at the runtime's fallback
// (whose bytes need only outlive the runtime), but at the built-in again.
TEST_F(GlyphFontTest, StopRestoresBuiltin) {
    start(glyph_image(), true, {});
    ui_->stop();
    ui_.reset();
    lv_font_glyph_dsc_t dsc{};
    ASSERT_TRUE(lv_font_get_glyph_dsc(default_font(), &dsc, kNi, 0));
    EXPECT_EQ(dsc.resolved_font, a8_font(&lv_font_cjk_16));
}

}  // namespace
