// Task 12b: pure-geometry tests for wisp::hal's display-shape/safe-area
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
    EXPECT_EQ(a.x1, 12);
    EXPECT_EQ(a.y1, 12);
    EXPECT_EQ(a.x2, 467);
    EXPECT_EQ(a.y2, 467);
    auto r = row_span(s, 0);
    EXPECT_EQ(r.x1, 34);
    EXPECT_EQ(r.x2, 445);
}

TEST(DisplayGeometry, Circle) {
    auto s = shape("circle");
    EXPECT_FALSE(visible(s, 0, 0));
    EXPECT_TRUE(visible(s, 240, 0));
    EXPECT_TRUE(visible(s, 240, 240));
    auto a = safe_rect(s);
    EXPECT_EQ(a.x1, 71);
    EXPECT_EQ(a.y1, 71);
    EXPECT_EQ(a.x2, 408);
    EXPECT_EQ(a.y2, 408);
    auto r = row_span(s, 0);
    EXPECT_EQ(r.x1, 225);
    EXPECT_EQ(r.x2, 254);
    EXPECT_TRUE(row_span(s, -1).empty());
    EXPECT_TRUE(row_span(s, 480).empty());
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
