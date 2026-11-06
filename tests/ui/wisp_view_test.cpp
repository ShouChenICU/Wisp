// WispView (wisp/ui/wisp_view.hpp) on a live UiRuntime: it redraws only
// its own box, stops drawing while paused, rebuilds on set_size(), and is
// sized and centred from the safe area on every screen shape.

#include "wisp/ui/wisp_view.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <mutex>
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

// FakeDisplay that also keeps every flushed pixel, like a panel's memory.
class FramebufferDisplay : public wisp::test::FakeDisplay {
public:
    explicit FramebufferDisplay(const std::string& shape)
        : FakeDisplay("display.main", 480, 480, 1, shape), fb_(480u * 480u, 0) {}

    wisp::Result<void> flush(const wisp::hal::Area& area, const uint16_t* px) override {
        {
            std::lock_guard<std::mutex> lock(fb_mutex_);
            const int w = area.x2 - area.x1 + 1;
            for (int y = area.y1; y <= area.y2; ++y) {
                for (int x = area.x1; x <= area.x2; ++x) {
                    fb_[static_cast<size_t>(y) * 480 + x] = px[(y - area.y1) * w + (x - area.x1)];
                }
            }
        }
        return FakeDisplay::flush(area, px);
    }

    std::vector<uint16_t> framebuffer() const {
        std::lock_guard<std::mutex> lock(fb_mutex_);
        return fb_;
    }

private:
    mutable std::mutex fb_mutex_;
    std::vector<uint16_t> fb_;
};

class WispViewTest : public ::testing::Test {
protected:
    void start(const std::string& shape = "rect", int corner_radius = 40,
               const std::string& panel = "amoled") {
        display_ = std::make_shared<FramebufferDisplay>(shape);
        display_->set_cap("corner_radius", wisp::Value(corner_radius));
        display_->set_cap("panel", wisp::Value(panel));
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

    std::shared_ptr<FramebufferDisplay> display_;
    std::unique_ptr<wisp::ui::UiRuntime> ui_;
    WispView* view_ = nullptr;
};

// Each frame invalidates the wisp's bounds, this frame's joined with the
// last one's, not its whole object: every flush lies inside the union of
// the bounds the frames had, which is narrower than the object.
TEST_F(WispViewTest, InvalidatesOnlyItsBox) {
    start();
    ui([&] { view_ = WispView::create(lv_screen_active(), 216); });
    ASSERT_NE(view_, nullptr);
    // Let the first frames (and the screen's first full draw) go out.
    wisp::os::sleep_ms(300);
    const lv_area_t box = coords();

    // Samples the model's bounds, in screen pixels, more often than it
    // makes frames, on the UI thread.
    struct Probe {
        WispView* view;
        lv_area_t box;
        lv_area_t seen;
        static void sample(lv_timer_t* t) {
            auto* p = static_cast<Probe*>(lv_timer_get_user_data(t));
            const wisp::hal::Area b = p->view->model().bounds();
            const lv_area_t a{p->box.x1 + b.x1, p->box.y1 + b.y1, p->box.x1 + b.x2,
                              p->box.y1 + b.y2};
            p->seen = {std::min(p->seen.x1, a.x1), std::min(p->seen.y1, a.y1),
                       std::max(p->seen.x2, a.x2), std::max(p->seen.y2, a.y2)};
        }
    } probe{view_, box, {}};
    lv_timer_t* timer = nullptr;
    size_t seen = 0;
    ui([&] {
        const wisp::hal::Area b = view_->model().bounds();
        probe.seen = {box.x1 + b.x1, box.y1 + b.y1, box.x1 + b.x2, box.y1 + b.y2};
        timer = lv_timer_create(&Probe::sample, 1, &probe);
        seen = display_->areas().size();
        view_->set_mood(WispMood::Listening);
    });
    wisp::os::sleep_ms(400);
    lv_area_t u{};
    ui([&] {
        lv_timer_delete(timer);
        u = probe.seen;
    });
    const std::vector<wisp::hal::Area> areas = display_->areas();
    ASSERT_GT(areas.size(), seen);
    for (size_t i = seen; i < areas.size(); ++i) {
        const wisp::hal::Area& a = areas[i];
        EXPECT_GE(a.x1, u.x1) << i;
        EXPECT_GE(a.y1, u.y1) << i;
        EXPECT_LE(a.x2, u.x2) << i;
        EXPECT_LE(a.y2, u.y2) << i;
    }
    EXPECT_LT(lv_area_get_width(&u), lv_area_get_width(&box));
}

// Hidden, or on a screen that is not shown, the wisp does not step or
// draw, without the caller pausing it.
TEST_F(WispViewTest, HiddenViewMakesNoFrames) {
    start();
    ui([&] { view_ = WispView::create(lv_screen_active(), 216); });
    ASSERT_NE(view_, nullptr);
    wisp::os::sleep_ms(200);
    ui([&] { lv_obj_add_flag(view_->obj(), LV_OBJ_FLAG_HIDDEN); });
    const uint32_t hidden_at = frames();
    wisp::os::sleep_ms(400);
    EXPECT_EQ(frames(), hidden_at);
    ui([&] { lv_obj_remove_flag(view_->obj(), LV_OBJ_FLAG_HIDDEN); });
    wisp::os::sleep_ms(300);
    EXPECT_GT(frames(), hidden_at);

    lv_obj_t* other = nullptr;
    lv_obj_t* home = nullptr;
    ui([&] {
        home = lv_screen_active();
        other = lv_obj_create(nullptr);
        lv_screen_load(other);
    });
    const uint32_t away_at = frames();
    wisp::os::sleep_ms(400);
    EXPECT_EQ(frames(), away_at);
    ui([&] {
        lv_screen_load(home);
        lv_obj_delete(other);
    });
    wisp::os::sleep_ms(300);
    EXPECT_GT(frames(), away_at);
}

// A display UiRuntime did not make reads as a plain LCD, whatever its
// user data holds.
TEST_F(WispViewTest, ForeignDisplayReadsAsLcd) {
    start("rect", 40, "mono");
    wisp::hal::PanelKind ours = wisp::hal::PanelKind::Lcd, theirs = wisp::hal::PanelKind::Mono;
    ui([&] {
        ours = wisp::ui::display_panel(lv_display_get_default());
        static int junk = 0x5a5a5a5a;
        lv_display_t* other = lv_display_create(32, 32);
        lv_display_set_user_data(other, &junk);
        theirs = wisp::ui::display_panel(other);
        lv_display_delete(other);
    });
    EXPECT_EQ(ours, wisp::hal::PanelKind::Mono);
    EXPECT_EQ(theirs, wisp::hal::PanelKind::Lcd);
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

// E-paper shows still pictures: one when created, one per mood change,
// nothing for the level, busy or a repeated mood (spec 6.1).
TEST_F(WispViewTest, EPaperRedrawsOnlyOnMoodChange) {
    start("rect", 40, "epaper");
    ui([&] {
        lv_obj_set_style_bg_color(lv_screen_active(), lv_color_white(), 0);
        view_ = WispView::create(lv_screen_active(), 216);
    });
    ASSERT_NE(view_, nullptr);
    wisp::os::sleep_ms(400);
    EXPECT_EQ(frames(), 1u);
    ui([&] {
        view_->set_level(80);
        view_->set_busy(true);
        view_->set_mood(WispMood::Idle);
    });
    wisp::os::sleep_ms(300);
    EXPECT_EQ(frames(), 1u);
    const size_t flushes = display_->flush_count();
    ui([&] { view_->set_mood(WispMood::Error); });
    wisp::os::sleep_ms(400);
    EXPECT_EQ(frames(), 2u);
    EXPECT_GT(display_->flush_count(), flushes);
    bool chevrons = false;
    ui([&] { chevrons = view_->model().shape().chevrons; });
    EXPECT_TRUE(chevrons);
    // Black on white.
    const std::vector<uint16_t> fb = display_->framebuffer();
    EXPECT_NE(std::find(fb.begin(), fb.end(), 0x0000), fb.end());
}

// Mono: no anti-aliasing and no sparks; the wisp is white on black and
// the panel gets exactly two colours.
TEST_F(WispViewTest, MonoDrawsTwoColoursWithoutSparks) {
    start("rect", 40, "mono");
    ui([&] {
        view_ = WispView::create(lv_screen_active(), 216);
        view_->set_mood(WispMood::Thinking);
    });
    ASSERT_NE(view_, nullptr);
    bool aa = true;
    ui([&] { aa = lv_display_get_antialiasing(lv_obj_get_display(view_->obj())); });
    EXPECT_FALSE(aa);
    wisp::os::sleep_ms(500);
    size_t sparks = 1;
    uint32_t interval = 0;
    ui([&] {
        sparks = view_->model().live_particles();
        interval = view_->model().frame_interval_ms();
    });
    EXPECT_EQ(sparks, 0u);
    EXPECT_EQ(interval, 100u);
    const std::vector<uint16_t> fb = display_->framebuffer();
    size_t white = 0;
    for (uint16_t px : fb) {
        ASSERT_TRUE(px == 0x0000 || px == 0xffff) << std::hex << px;
        white += px == 0xffff;
    }
    EXPECT_GT(white, 500u);
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
