// WispView (wisp/ui/wisp_view.hpp) on a live UiRuntime: it redraws only
// its own box, stops drawing while paused, rebuilds on set_size(), and is
// sized and centred from the safe area on every screen shape.

#include "wisp/ui/wisp_view.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/lvgl_probe.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace {

using wisp::ui::WispMood;
using wisp::ui::WispView;

class WispViewTest : public ::testing::Test {
protected:
    void start(const std::string& shape = "rect", int corner_radius = 40) {
        display_ = std::make_shared<wisp::test::FakeDisplay>("display.main", 480, 480, 1, shape);
        display_->set_cap("corner_radius", wisp::Value(corner_radius));
        ui_ = std::make_unique<wisp::ui::UiRuntime>(display_, nullptr);
        ASSERT_TRUE(ui_->start());
        ui([] {
            lv_obj_t* scr = lv_screen_active();
            lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
            lv_obj_set_style_pad_all(scr, 0, 0);
        });
    }

    void TearDown() override {
        if (ui_) {
            if (view_ != nullptr) ui([this] { lv_obj_delete(view_->obj()); });
            ui_->stop();
        }
    }

    void ui(const std::function<void()>& fn) { wisp::test::run_on_ui_blocking(ui_.get(), fn); }

    uint32_t frames() {
        uint32_t n = 0;
        ui([&] { n = view_->frames(); });
        return n;
    }

    lv_area_t coords() {
        lv_area_t a{};
        ui([&] {
            lv_obj_update_layout(view_->obj());
            lv_obj_get_coords(view_->obj(), &a);
        });
        return a;
    }

    std::shared_ptr<wisp::test::FakeDisplay> display_;
    std::unique_ptr<wisp::ui::UiRuntime> ui_;
    WispView* view_ = nullptr;
};

TEST_F(WispViewTest, InvalidatesOnlyItsBox) {
    start();
    ui([&] { view_ = WispView::create(lv_screen_active(), 216); });
    ASSERT_NE(view_, nullptr);
    // Let the first frames (and the screen's first full draw) go out.
    wisp::os::sleep_ms(300);
    const lv_area_t box = coords();
    const size_t seen = display_->areas().size();
    ui([&] { view_->set_mood(WispMood::Listening); });
    wisp::os::sleep_ms(400);
    const std::vector<wisp::hal::Area> areas = display_->areas();
    ASSERT_GT(areas.size(), seen);
    for (size_t i = seen; i < areas.size(); ++i) {
        const wisp::hal::Area& a = areas[i];
        EXPECT_GE(a.x1, box.x1);
        EXPECT_GE(a.y1, box.y1);
        EXPECT_LE(a.x2, box.x2);
        EXPECT_LE(a.y2, box.y2);
    }
}

TEST_F(WispViewTest, PausedRendersNoFrames) {
    start();
    ui([&] { view_ = WispView::create(lv_screen_active(), 216); });
    ASSERT_NE(view_, nullptr);
    wisp::os::sleep_ms(200);
    EXPECT_GT(frames(), 0u);
    ui([&] { view_->set_paused(true); });
    const uint32_t paused_at = frames();
    wisp::os::sleep_ms(500);
    EXPECT_EQ(frames(), paused_at);
    ui([&] { view_->set_paused(false); });
    wisp::os::sleep_ms(300);
    EXPECT_GT(frames(), paused_at);
}

TEST_F(WispViewTest, SetSizeRebuilds) {
    start();
    ui([&] { view_ = WispView::create(lv_screen_active(), 216); });
    ASSERT_NE(view_, nullptr);
    lv_area_t a = coords();
    EXPECT_EQ(lv_area_get_width(&a), 216);
    EXPECT_EQ(lv_area_get_height(&a), 270);
    const int32_t cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
    ui([&] {
        view_->set_mood(WispMood::Thinking);
        view_->set_size(134);
    });
    a = coords();
    EXPECT_EQ(lv_area_get_width(&a), 134);
    EXPECT_EQ(lv_area_get_height(&a), 167);
    EXPECT_NEAR((a.x1 + a.x2) / 2, cx, 1);
    EXPECT_NEAR((a.y1 + a.y2) / 2, cy, 1);
    int w = 0;
    WispMood mood = WispMood::Idle;
    ui([&] {
        w = view_->model().width();
        mood = view_->model().mood();
    });
    EXPECT_EQ(w, 134);
    EXPECT_EQ(mood, WispMood::Thinking);
    const uint32_t before = frames();
    wisp::os::sleep_ms(300);
    EXPECT_GT(frames(), before);
}

class WispViewShapes : public WispViewTest, public ::testing::WithParamInterface<std::string> {};

TEST_P(WispViewShapes, SizedFromSafeArea) {
    start(GetParam(), 70);
    wisp::hal::Area safe{};
    ui([&] {
        auto shape = wisp::hal::screen_shape_from_caps(display_->info().caps);
        ASSERT_TRUE(shape);
        safe = wisp::hal::safe_rect(shape.value());
        const int size = wisp::ui::wisp_size_px(safe, 45);
        view_ = WispView::create(lv_screen_active(), size);
    });
    ASSERT_NE(view_, nullptr);
    const int short_side = std::min(safe.x2 - safe.x1 + 1, safe.y2 - safe.y1 + 1);
    const lv_area_t a = coords();
    EXPECT_EQ(lv_area_get_width(&a), short_side * 45 / 100);
    EXPECT_NEAR(a.x1 + a.x2, safe.x1 + safe.x2, 1);
    EXPECT_NEAR(a.y1 + a.y2, safe.y1 + safe.y2, 1);
    EXPECT_GE(a.x1, safe.x1);
    EXPECT_GE(a.y1, safe.y1);
    EXPECT_LE(a.x2, safe.x2);
    EXPECT_LE(a.y2, safe.y2);
}

INSTANTIATE_TEST_SUITE_P(Shapes, WispViewShapes,
                         ::testing::Values("rect", "rounded_rect", "circle"));

}  // namespace
