// Pure-geometry tests for wisp::hal's display-shape/safe-area
// model (spec 5.6). No LVGL, no App -- just screen_shape_from_caps(),
// visible(), safe_rect() and row_span() against literal caps Properties.

#include "wisp/hal/display_geometry.hpp"

#include <gtest/gtest.h>

#include <string>

#include "wisp/base/value.hpp"

using namespace wisp::hal;

static ScreenShape shape(const char* s, int w = 480, int h = 480, int r = 40) {
    return screen_shape_from_caps(
               wisp::Properties{
                   {"width", w}, {"height", h}, {"shape", wisp::Value(s)}, {"corner_radius", r}})
        .value();
}

TEST(DisplayGeometry, ParsesCaps) {
    EXPECT_EQ(
        screen_shape_from_caps(wisp::Properties{{"width", 320}, {"height", 240}}).value().kind,
        ScreenShapeKind::Rect);
    EXPECT_EQ(shape("circle").kind, ScreenShapeKind::Circle);
    auto bad = screen_shape_from_caps(
        wisp::Properties{{"width", 480}, {"height", 480}, {"shape", wisp::Value("triangle")}});
    EXPECT_NE(bad.error().message.find("triangle"), std::string::npos);
    EXPECT_FALSE(screen_shape_from_caps(
        wisp::Properties{{"width", 480}, {"height", 480}, {"shape", wisp::Value("rounded_rect")}}));
    EXPECT_FALSE(screen_shape_from_caps(wisp::Properties{{"height", 480}}));
    EXPECT_EQ(shape("rounded_rect", 480, 480, 1000).corner_radius, 240);
}

TEST(DisplayGeometry, RoundedRect) {
    auto s = shape("rounded_rect");
    EXPECT_FALSE(visible(s, 0, 0));
    EXPECT_FALSE(visible(s, 11, 11));
    EXPECT_TRUE(visible(s, 12, 12));
    EXPECT_TRUE(visible(s, 240, 0));
    EXPECT_TRUE(visible(s, 0, 240));
    EXPECT_FALSE(visible(s, 480, 240));
    auto a = safe_rect(s);
    // r=40, corner margin m=4: ceil(40 - 36/sqrt(2)) == 15.
    EXPECT_EQ(a.x1, 15);
    EXPECT_EQ(a.y1, 15);
    EXPECT_EQ(a.x2, 464);
    EXPECT_EQ(a.y2, 464);
    auto r = row_span(s, 0);
    EXPECT_EQ(r.x1, 34);
    EXPECT_EQ(r.x2, 445);
}

// The real waveshare_esp32_s3_touch_amoled_2_16 panel's measured corner
// radius (docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md): arcs of
// known radius drawn on the real panel showed the visible boundary
// crossing the diagonal at about 20 px from the corner, i.e. r=70.
TEST(DisplayGeometry, RoundedRectMeasuredRadius) {
    auto s = shape("rounded_rect", 480, 480, 70);
    auto a = safe_rect(s);
    // ceil(70 - 66/sqrt(2)) == 24.
    EXPECT_EQ(a.x1, 24);
    EXPECT_EQ(a.y1, 24);
    EXPECT_EQ(a.x2, 455);
    EXPECT_EQ(a.y2, 455);

    // Each safe-rect corner carries at least the 4 px corner margin: from
    // the top-left corner, stepping 4 px further outward along either
    // edge (toward the actual screen corner) must still land on a
    // visible pixel. (Stepping outward along *both* axes at once, i.e.
    // the Euclidean diagonal, moves 4*sqrt(2) px -- more than the 4 px
    // margin -- so that combined offset is not part of the guarantee.)
    EXPECT_TRUE(visible(s, a.x1 - 4, a.y1));
    EXPECT_TRUE(visible(s, a.x1, a.y1 - 4));
    EXPECT_TRUE(visible(s, a.x2 + 4, a.y2));
    EXPECT_TRUE(visible(s, a.x2, a.y2 + 4));
}

TEST(DisplayGeometry, Circle) {
    auto s = shape("circle");
    EXPECT_FALSE(visible(s, 0, 0));
    EXPECT_TRUE(visible(s, 240, 0));
    EXPECT_TRUE(visible(s, 240, 240));
    auto a = safe_rect(s);
    // R=240, corner margin m=4: ceil(240 - 236/sqrt(2)) == 74.
    EXPECT_EQ(a.x1, 74);
    EXPECT_EQ(a.y1, 74);
    EXPECT_EQ(a.x2, 405);
    EXPECT_EQ(a.y2, 405);
    auto r = row_span(s, 0);
    EXPECT_EQ(r.x1, 225);
    EXPECT_EQ(r.x2, 254);
    EXPECT_TRUE(row_span(s, -1).empty());
    EXPECT_TRUE(row_span(s, 480).empty());

    // Same corner-margin check as RoundedRectMeasuredRadius, for Circle.
    EXPECT_TRUE(visible(s, a.x1 - 4, a.y1));
    EXPECT_TRUE(visible(s, a.x1, a.y1 - 4));
    EXPECT_TRUE(visible(s, a.x2 + 4, a.y2));
    EXPECT_TRUE(visible(s, a.x2, a.y2 + 4));
}

TEST(DisplayGeometry, RectIsFullScreen) {
    auto s = shape("rect", 320, 240);
    auto a = safe_rect(s);
    EXPECT_EQ(a.x1, 0);
    EXPECT_EQ(a.y1, 0);
    EXPECT_EQ(a.x2, 319);
    EXPECT_EQ(a.y2, 239);
    EXPECT_TRUE(visible(s, 0, 0));
    EXPECT_EQ(row_span(s, 10).x2, 319);
}

TEST(DisplayGeometry, ConsistentWithVisible) {  // cross-check all three shapes pixel by pixel
    for (const char* k : {"rect", "rounded_rect", "circle"}) {
        auto s = shape(k);
        auto a = safe_rect(s);
        for (int32_t y = 0; y < 480; ++y) {
            auto sp = row_span(s, y);
            for (int32_t x = 0; x < 480; ++x) {
                bool in_span = !sp.empty() && x >= sp.x1 && x <= sp.x2;
                ASSERT_EQ(visible(s, x, y), in_span) << k << " " << x << "," << y;
                if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) {
                    ASSERT_TRUE(visible(s, x, y)) << k;
                }
            }
        }
    }
}

// row_span() searches for each row's ends instead of scanning every
// pixel (scenes call it per row from Lua, on a chip without a double FPU):
// it must still agree with visible() pixel for pixel on odd sizes, wide
// and tall screens, and radii from 0 to the clamped maximum.
TEST(DisplayGeometry, RowSpanMatchesVisibleOnOddShapes) {
    const struct {
        const char* kind;
        int w, h, r;
    } cases[] = {{"rounded_rect", 101, 57, 28},
                 {"rounded_rect", 101, 57, 500},
                 {"rounded_rect", 300, 200, 100},
                 {"rounded_rect", 300, 200, 0},
                 {"rounded_rect", 466, 466, 70},
                 {"circle", 101, 57, 0},
                 {"circle", 57, 101, 0},
                 {"circle", 2, 2, 0},
                 {"circle", 1, 1, 0},
                 {"circle", 466, 466, 0},
                 {"rounded_rect", 1, 1, 1},
                 {"rect", 7, 3, 0}};
    for (const auto& c : cases) {
        auto s = shape(c.kind, c.w, c.h, c.r);
        for (int32_t y = -1; y <= c.h; ++y) {
            auto sp = row_span(s, y);
            for (int32_t x = -1; x <= c.w; ++x) {
                bool in_span = !sp.empty() && x >= sp.x1 && x <= sp.x2;
                ASSERT_EQ(visible(s, x, y), in_span)
                    << c.kind << " " << c.w << "x" << c.h << " r" << c.r << " at " << x << "," << y;
            }
        }
    }
}
