// Pure-geometry tests for wisp::hal's display-shape/safe-area
// model (spec 5.6). No LVGL, no App -- just screen_shape_from_caps(),
// visible(), safe_rect() and row_span() against literal caps Properties.

#include "wisp/hal/display_geometry.hpp"

#include <gtest/gtest.h>

#include <cmath>
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

// Radial distance from the point (x, y) inward to the arc of radius r
// centred on (cx, cy); negative when the point lies outside the arc.
static double corner_margin(double cx, double cy, double r, double x, double y) {
    return r - std::hypot(x - cx, y - cy);
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

    // The outer corner of each corner pixel lies at least 4 px (radially)
    // inside the corner arc.
    EXPECT_GE(corner_margin(70, 70, 70, a.x1, a.y1), 4.0);
    EXPECT_GE(corner_margin(410, 410, 70, a.x2 + 1, a.y2 + 1), 4.0);
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
    EXPECT_GE(corner_margin(240, 240, 240, a.x1, a.y1), 4.0);
    EXPECT_GE(corner_margin(240, 240, 240, a.x2 + 1, a.y2 + 1), 4.0);
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

TEST(DisplayGeometry, PanelKind) {
    EXPECT_EQ(panel_kind(wisp::Properties{}), PanelKind::Lcd);
    EXPECT_EQ(panel_kind(wisp::Properties{{"panel", wisp::Value("lcd")}}), PanelKind::Lcd);
    EXPECT_EQ(panel_kind(wisp::Properties{{"panel", wisp::Value("amoled")}}), PanelKind::Amoled);
    EXPECT_EQ(panel_kind(wisp::Properties{{"panel", wisp::Value("mono")}}), PanelKind::Mono);
    EXPECT_EQ(panel_kind(wisp::Properties{{"panel", wisp::Value("epaper")}}), PanelKind::EPaper);
    EXPECT_EQ(panel_kind(wisp::Properties{{"panel", wisp::Value("oled?")}}), PanelKind::Lcd);
    EXPECT_TRUE(panel_is_colour(PanelKind::Amoled));
    EXPECT_FALSE(panel_is_colour(PanelKind::EPaper));
}

// Touch distances scale with the screen: a share of its shorter side,
// rounded to the nearest pixel, whatever its shape.
TEST(DisplayGeometry, ShareOfScreen) {
    const ScreenShape sq{ScreenShapeKind::Rect, 480, 480, 0};
    EXPECT_EQ(share_of_screen(sq, 8), 38);  // 38.4
    EXPECT_EQ(share_of_screen(sq, 5), 24);
    EXPECT_EQ(share_of_screen(sq, 2), 10);  // 9.6
    EXPECT_EQ(share_of_screen(sq, 0), 0);
    EXPECT_EQ(share_of_screen(sq, 100), 480);
    const ScreenShape c466{ScreenShapeKind::Circle, 466, 466, 0};
    EXPECT_EQ(share_of_screen(c466, 8), 37);  // 37.28
    EXPECT_EQ(share_of_screen(c466, 5), 23);  // 23.3
    EXPECT_EQ(share_of_screen(c466, 2), 9);   // 9.32
    // The shorter side, either way round.
    EXPECT_EQ(share_of_screen({ScreenShapeKind::Rect, 360, 480, 0}, 8), 29);  // 28.8
    EXPECT_EQ(share_of_screen({ScreenShapeKind::Rect, 480, 360, 0}, 5), 18);
    EXPECT_EQ(share_of_screen({ScreenShapeKind::RoundedRect, 480, 360, 40}, 2), 7);  // 7.2
}
