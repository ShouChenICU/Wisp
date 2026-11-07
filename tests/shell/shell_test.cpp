// Shell (wisp/shell/shell.hpp) on a live UiRuntime: the home screen it boots
// into (the wisp centred in the safe area on every screen shape), the pages'
// names, and the diagnostics hooks it drives.

#include "wisp/shell/shell.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "support/fake_devices.hpp"
#include "support/lvgl_probe.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/shell/limits.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"
#include "wisp/ui/wisp_view.hpp"

namespace {

using wisp::shell::Page;
using wisp::shell::Shell;
using wisp::shell::ShellDeps;

class ShellFixture {
public:
    explicit ShellFixture(const std::string& shape, int32_t width = 480, int32_t height = 480) {
        display_ =
            std::make_shared<wisp::test::FakeDisplay>("display.main", width, height, 1, shape);
        shape_ = wisp::hal::screen_shape_from_caps(display_->info().caps).value();
        ui_ = std::make_unique<wisp::ui::UiRuntime>(display_, nullptr);
        started_ = static_cast<bool>(ui_->start());
        if (!started_) return;
        (void)bus_.start();
        on_ui([&] { layer_ = std::make_unique<wisp::ui::SystemLayer>(shape_); });
        shell_ = std::make_unique<Shell>(ShellDeps{
            .ui = *ui_,
            .layer = *layer_,
            .shape = shape_,
            .bus = bus_,
            .conversation = nullptr,
            .open_diagnostics =
                [this] {
                    ++opens;
                    diag = lv_obj_create(nullptr);
                    lv_screen_load(diag);
                },
            .close_diagnostics =
                [this] {
                    ++closes;
                    lv_obj_delete(diag);
                    diag = nullptr;
                },
            // A stand-in for the scene runtime: a scene is "in front" while
            // `scene` is set; sending it back only queues (the screen stays
            // until the runtime's UI side drops it).
            .leave_scene = [this]() -> wisp::Result<bool> {
                ++leaves;
                if (scene == nullptr) return wisp::Result<bool>::ok(false);
                if (refuse) return wisp::Result<bool>::err(wisp::ErrorCode::Busy, "queue full");
                leaving = true;
                return wisp::Result<bool>::ok(true);
            },
        });
    }

    ~ShellFixture() {
        if (!started_) return;
        on_ui([&] {
            shell_->stop();
            layer_.reset();
        });
        shell_.reset();
        bus_.stop();
        ui_->stop();
    }

    bool started() const { return started_; }
    void on_ui(const std::function<void()>& fn) { wisp::test::run_on_ui_blocking(ui_.get(), fn); }
    Shell& shell() { return *shell_; }
    const wisp::hal::ScreenShape& shape() const { return shape_; }

    // A scene's view put in front, as the scene runtime does.
    void open_scene() {
        scene = lv_obj_create(nullptr);
        lv_screen_load(scene);
        layer_->set_back_handler([] {});
    }
    // The runtime's UI side dropping the view after the scene went back.
    void drop_scene() {
        if (lv_screen_active() == scene) lv_screen_load(shell_->home_screen());
        lv_obj_delete(scene);
        scene = nullptr;
    }
    wisp::ui::SystemLayer& layer() { return *layer_; }

    int opens = 0;
    int closes = 0;
    int leaves = 0;
    bool refuse = false;
    bool leaving = false;
    lv_obj_t* diag = nullptr;
    lv_obj_t* scene = nullptr;

private:
    std::shared_ptr<wisp::test::FakeDisplay> display_;
    wisp::hal::ScreenShape shape_;
    std::unique_ptr<wisp::ui::UiRuntime> ui_;
    wisp::core::EventBus bus_;
    std::unique_ptr<wisp::ui::SystemLayer> layer_;
    std::unique_ptr<Shell> shell_;
    bool started_ = false;
};

struct ShapeCase {
    const char* shape;
    int32_t width;
    int32_t height;
};

// Square panels plus portrait ones, where the wisp's size follows the
// safe area's short side and its vertical centre differs from its
// horizontal one.
class ShellShapes : public ::testing::TestWithParam<ShapeCase> {};

}  // namespace

TEST(Shell, PageNames) {
    EXPECT_STREQ(wisp::shell::to_string(Page::Home), "home");
    EXPECT_STREQ(wisp::shell::to_string(Page::Settings), "settings");
    EXPECT_STREQ(wisp::shell::to_string(Page::Scenes), "scenes");
    EXPECT_STREQ(wisp::shell::to_string(Page::History), "history");
}

// mvp spec 6.2: the wisp centred in the safe area, kWispIdlePercent of its
// short side, on a black screen of its own that is the active one.
TEST_P(ShellShapes, HomeWispCentredInSafeArea) {
    const ShapeCase& c = GetParam();
    ShellFixture f(c.shape, c.width, c.height);
    ASSERT_TRUE(f.started());
    f.on_ui([&] {
        f.shell().start();
        ASSERT_NE(f.shell().home_screen(), nullptr);
        EXPECT_EQ(lv_screen_active(), f.shell().home_screen());
        EXPECT_EQ(f.shell().page(), Page::Home);
        lv_color_t bg = lv_obj_get_style_bg_color(f.shell().home_screen(), LV_PART_MAIN);
        EXPECT_EQ(lv_color_to_u32(bg) & 0xFFFFFF, 0u);
        EXPECT_EQ(lv_obj_get_style_bg_opa(f.shell().home_screen(), LV_PART_MAIN), LV_OPA_COVER);

        wisp::ui::WispView* wisp = f.shell().wisp();
        ASSERT_NE(wisp, nullptr);
        lv_obj_update_layout(f.shell().home_screen());
        lv_area_t a;
        lv_obj_get_coords(wisp->obj(), &a);
        const wisp::hal::Area safe = wisp::hal::safe_rect(f.shape());
        EXPECT_EQ(lv_area_get_width(&a),
                  wisp::ui::wisp_size_px(safe, wisp::shell::kWispIdlePercent));
        EXPECT_NEAR((a.x1 + a.x2) / 2.0, (safe.x1 + safe.x2) / 2.0, 1.0);
        EXPECT_NEAR((a.y1 + a.y2) / 2.0, (safe.y1 + safe.y2) / 2.0, 1.0);
        EXPECT_GE(a.x1, safe.x1);
        EXPECT_GE(a.y1, safe.y1);
        EXPECT_LE(a.x2, safe.x2);
        EXPECT_LE(a.y2, safe.y2);
    });
}

INSTANTIATE_TEST_SUITE_P(
    Shapes, ShellShapes,
    ::testing::Values(ShapeCase{"rect", 480, 480}, ShapeCase{"rounded_rect", 480, 480},
                      ShapeCase{"circle", 480, 480}, ShapeCase{"rounded_rect", 368, 448},
                      ShapeCase{"rect", 410, 502}, ShapeCase{"circle", 360, 480}),
    [](const ::testing::TestParamInfo<ShapeCase>& info) {
        return std::string(info.param.shape) + "_" + std::to_string(info.param.width) + "x" +
               std::to_string(info.param.height);
    });

TEST(Shell, DiagnosticsOpenOverHomeAndCloseBackToIt) {
    ShellFixture f("rounded_rect");
    ASSERT_TRUE(f.started());
    f.on_ui([&] {
        f.shell().start();
        f.shell().open_diagnostics();
        EXPECT_EQ(f.opens, 1);
        EXPECT_TRUE(f.shell().diagnostics_open());
        EXPECT_EQ(lv_screen_active(), f.diag);
        // Already open: nothing more is built.
        f.shell().open_diagnostics();
        EXPECT_EQ(f.opens, 1);

        f.shell().show(Page::Home);
        EXPECT_EQ(f.closes, 1);
        EXPECT_FALSE(f.shell().diagnostics_open());
        EXPECT_EQ(lv_screen_active(), f.shell().home_screen());
    });
}

TEST(Shell, StopClosesTheDiagnosticsAndLeavesTheHomeScreen) {
    ShellFixture f("rect");
    ASSERT_TRUE(f.started());
    lv_obj_t* before = nullptr;
    f.on_ui([&] {
        before = lv_screen_active();
        f.shell().start();
        f.shell().open_diagnostics();
        f.shell().stop();
        EXPECT_EQ(f.closes, 1);
        EXPECT_EQ(f.shell().home_screen(), nullptr);
        EXPECT_EQ(lv_screen_active(), before);
        // Idempotent.
        f.shell().stop();
        EXPECT_EQ(f.closes, 1);
    });
}

// A scene in front when the shell navigates: it is sent back and the
// shell's own screen loads at once, so the scene's view is never left
// running off-screen. One case per path.
TEST(Shell, HomeSendsTheSceneInFrontBack) {
    ShellFixture f("rect");
    ASSERT_TRUE(f.started());
    f.on_ui([&] {
        f.shell().start();
        f.open_scene();
        f.shell().show(Page::Home);
        EXPECT_TRUE(f.leaving);
        EXPECT_EQ(lv_screen_active(), f.shell().home_screen());
        f.drop_scene();
        EXPECT_EQ(lv_screen_active(), f.shell().home_screen());
    });
}

TEST(Shell, PageSendsTheSceneInFrontBack) {
    ShellFixture f("rect");
    ASSERT_TRUE(f.started());
    f.on_ui([&] {
        f.shell().start();
        f.open_scene();
        f.shell().show(Page::Settings);
        EXPECT_TRUE(f.leaving);
        ASSERT_NE(f.shell().page_screen(), nullptr);
        EXPECT_EQ(lv_screen_active(), f.shell().page_screen());
        EXPECT_EQ(f.shell().page(), Page::Settings);
        f.drop_scene();
        EXPECT_EQ(lv_screen_active(), f.shell().page_screen());
    });
}

TEST(Shell, DiagnosticsSendTheSceneInFrontBack) {
    ShellFixture f("rect");
    ASSERT_TRUE(f.started());
    f.on_ui([&] {
        f.shell().start();
        f.open_scene();
        f.shell().open_diagnostics();
        EXPECT_TRUE(f.leaving);
        EXPECT_TRUE(f.shell().diagnostics_open());
        EXPECT_EQ(lv_screen_active(), f.diag);
        f.drop_scene();
        EXPECT_EQ(lv_screen_active(), f.diag);
        // Back from the diagnostics goes home.
        f.shell().show(Page::Home);
        EXPECT_EQ(lv_screen_active(), f.shell().home_screen());
    });
}

// When the scene cannot be sent back, the navigation is refused: the scene
// stays in front, its view on the screen.
TEST(Shell, RefusedLeaveKeepsTheSceneInFront) {
    ShellFixture f("rect");
    ASSERT_TRUE(f.started());
    f.on_ui([&] {
        f.shell().start();
        f.open_scene();
        f.refuse = true;
        f.shell().show(Page::Home);
        EXPECT_EQ(lv_screen_active(), f.scene);
        f.shell().show(Page::Scenes);
        EXPECT_EQ(lv_screen_active(), f.scene);
        EXPECT_EQ(f.shell().page_screen(), nullptr);
        f.shell().open_diagnostics();
        EXPECT_EQ(f.opens, 0);
        EXPECT_EQ(lv_screen_active(), f.scene);
        f.refuse = false;
        f.shell().show(Page::Home);
        EXPECT_EQ(lv_screen_active(), f.shell().home_screen());
        f.drop_scene();
    });
}

// The sub-pages: a screen of their own, deleted once the home screen is
// back.
TEST(Shell, SubPagesComeAndGo) {
    ShellFixture f("circle");
    ASSERT_TRUE(f.started());
    f.on_ui([&] {
        f.shell().start();
        for (Page p : {Page::Settings, Page::Scenes, Page::History}) {
            f.shell().show(p);
            EXPECT_EQ(f.shell().page(), p);
            ASSERT_NE(f.shell().page_screen(), nullptr);
            EXPECT_EQ(lv_screen_active(), f.shell().page_screen());
            f.shell().show(Page::Home);
            EXPECT_EQ(f.shell().page_screen(), nullptr);
            EXPECT_EQ(lv_screen_active(), f.shell().home_screen());
        }
        // Stopped on a sub-page: everything goes.
        f.shell().show(Page::History);
        f.shell().stop();
        EXPECT_EQ(f.shell().page_screen(), nullptr);
        EXPECT_EQ(f.shell().home_screen(), nullptr);
    });
}
