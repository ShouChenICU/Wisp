// Page drags (wisp/shell/shell.hpp, mvp spec 6.3), driven through LVGL's
// own input path one touch read at a time, on a clock the test moves: the
// pages follow the finger, the release completes or snaps back, and what
// must not start a drag does not.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "shell/page_fixture.hpp"
#include "wisp/shell/home.hpp"
#include "wisp/shell/limits.hpp"
#include "wisp/shell/settings_page.hpp"
#include "wisp/shell/shell.hpp"
#include "wisp/ui/wisp_view.hpp"

using wisp::shell::Page;
using wisp::test::box_visible;
using wisp::test::coords_of;

namespace {

class Drags {
public:
    explicit Drags(const std::string& shape = "rect", int32_t w = 480, int32_t h = 480)
        : h_(shape, w, h) {
        if (!h_.started()) return;
        h_.deps().now_ms = [this] { return clock_.load(); };
        h_.boot();
    }

    bool started() { return h_.started(); }
    wisp::test::PageHarness& h() { return h_; }
    wisp::shell::Shell& shell() { return h_.shell(); }
    int32_t width() const { return h_.shape().width; }
    int32_t height() const { return h_.shape().height; }

    // The finger: down, moved (after `ms` on the clock) and up.
    void down(lv_point_t p) {
        h_.feed({{static_cast<int16_t>(p.x), static_cast<int16_t>(p.y), 0}});
    }
    void to(lv_point_t p, uint32_t ms = 100) {
        clock_ += ms;
        h_.feed({{static_cast<int16_t>(p.x), static_cast<int16_t>(p.y), 0}});
    }
    void up(uint32_t ms = 100) {
        clock_ += ms;
        h_.feed({});
    }
    // From `from` by (dx, dy) in steps of at most 10 px, 100 ms apart
    // (0.1 px/ms, far from a fling); the finger stays down.
    void slow(lv_point_t from, int32_t dx, int32_t dy) {
        down(from);
        const int32_t n = std::max(std::abs(dx), std::abs(dy)) / 10 + 1;
        for (int32_t i = 1; i <= n; ++i) to({from.x + dx * i / n, from.y + dy * i / n});
    }

    bool settled() {
        return h_.ui_until([&] { return !shell().sliding(); });
    }
    lv_area_t home_root() {
        lv_area_t a{};
        h_.on_ui([&] {
            lv_obj_update_layout(shell().home()->root());
            a = coords_of(shell().home()->root());
        });
        return a;
    }
    lv_area_t page_root() {
        lv_area_t a{};
        h_.on_ui([&] {
            ASSERT_NE(shell().page_root(), nullptr);
            lv_obj_update_layout(shell().page_root());
            a = coords_of(shell().page_root());
        });
        return a;
    }
    // On Home, at rest: no sub-page, the home screen in front with its
    // content in place.
    void expect_home() {
        h_.on_ui([&] {
            EXPECT_EQ(shell().page(), Page::Home);
            EXPECT_EQ(shell().page_screen(), nullptr);
            EXPECT_EQ(shell().page_root(), nullptr);
            EXPECT_EQ(lv_screen_active(), shell().home_screen());
            lv_obj_t* root = shell().home()->root();
            lv_obj_update_layout(root);
            EXPECT_EQ(lv_obj_get_parent(root), shell().home_screen());
            EXPECT_EQ(coords_of(root).x1, 0);
            EXPECT_EQ(coords_of(root).y1, 0);
        });
    }
    void expect_page(Page p) {
        h_.on_ui([&] {
            EXPECT_EQ(shell().page(), p);
            ASSERT_NE(shell().page_screen(), nullptr);
            EXPECT_EQ(lv_screen_active(), shell().page_screen());
            EXPECT_EQ(lv_obj_get_parent(shell().page_root()), shell().page_screen());
            lv_obj_update_layout(shell().page_root());
            lv_obj_update_layout(shell().home()->root());
            EXPECT_EQ(coords_of(shell().page_root()).x1, 0);
            EXPECT_EQ(coords_of(shell().page_root()).y1, 0);
            // Home's content is back on its own screen too.
            lv_obj_t* root = shell().home()->root();
            EXPECT_EQ(lv_obj_get_parent(root), shell().home_screen());
            EXPECT_EQ(coords_of(root).x1, 0);
        });
    }

private:
    wisp::test::PageHarness h_;
    std::atomic<uint64_t> clock_{1000};
};

// Clear of the wisp on every shape: below it, left of centre.
constexpr lv_point_t kHomeLeft{60, 420};
constexpr lv_point_t kHomeRight{420, 420};

}  // namespace

TEST(PageDrag, DragBelowThirdSnapsBack) {
    Drags d;
    ASSERT_TRUE(d.started());
    d.slow(kHomeLeft, d.width() / 3 - 10, 0);
    d.h().on_ui([&] {
        EXPECT_TRUE(d.shell().sliding());
        EXPECT_NE(d.shell().settings_page(), nullptr);  // built at the drag's start
    });
    d.up();
    ASSERT_TRUE(d.settled());
    d.expect_home();
    d.h().on_ui([&] { EXPECT_EQ(d.shell().settings_page(), nullptr); });
}

TEST(PageDrag, DragPastThirdCompletes) {
    Drags d;
    ASSERT_TRUE(d.started());
    d.slow(kHomeLeft, d.width() / 3 + 10, 0);
    d.up();
    ASSERT_TRUE(d.settled());
    d.expect_page(Page::Settings);
    // And back the other way.
    d.slow({400, 30}, -(d.width() / 3 + 10), 0);
    d.up();
    ASSERT_TRUE(d.settled());
    d.expect_home();
}

TEST(PageDrag, FlingShortButFastCompletes) {
    Drags d;
    ASSERT_TRUE(d.started());
    d.down(kHomeRight);
    for (int i = 1; i <= 3; ++i) d.to({kHomeRight.x - 20 * i, kHomeRight.y}, 10);  // 2 px/ms
    d.up(5);
    ASSERT_TRUE(d.settled());
    d.expect_page(Page::Scenes);
}

// Mid-drag, both pages sit where the finger has taken them: the current
// one moved by the finger's travel, the target one screen behind it.
TEST(PageDrag, PagesFollowTheFinger) {
    struct Case {
        lv_point_t from;
        int32_t ux, uy;
        Page to;
    };
    for (const Case c :
         {Case{kHomeLeft, 1, 0, Page::Settings}, Case{kHomeRight, -1, 0, Page::Scenes},
          Case{{60, 60}, 0, 1, Page::History}}) {
        SCOPED_TRACE(wisp::shell::to_string(c.to));
        Drags d;
        ASSERT_TRUE(d.started());
        const int32_t extent = c.ux != 0 ? d.width() : d.height();
        d.down(c.from);
        for (const int32_t travel : {30, 100, 250, 20}) {
            d.to({c.from.x + c.ux * travel, c.from.y + c.uy * travel});
            const lv_area_t home = d.home_root();
            const lv_area_t page = d.page_root();
            EXPECT_EQ(home.x1, c.ux * travel);
            EXPECT_EQ(home.y1, c.uy * travel);
            EXPECT_EQ(page.x1, c.ux * (travel - extent));
            EXPECT_EQ(page.y1, c.uy * (travel - extent));
            d.h().on_ui([&] {
                EXPECT_EQ(d.shell().page(), Page::Home);
                EXPECT_EQ(lv_screen_active(), d.shell().home_screen());
                EXPECT_EQ(lv_obj_get_screen(d.shell().page_root()), d.shell().home_screen());
            });
        }
        // Back to 20 px, slowly: snaps back.
        d.up();
        ASSERT_TRUE(d.settled());
        d.expect_home();
    }
}

TEST(PageDrag, OffAxisMotionIgnored) {
    Drags d;
    ASSERT_TRUE(d.started());
    d.down(kHomeLeft);
    d.to({kHomeLeft.x + 30, kHomeLeft.y});
    int i = 0;
    for (const int32_t dy : {-80, 40, -30, 70}) {
        const int32_t dx = 30 + 25 * ++i;
        d.to({kHomeLeft.x + dx, kHomeLeft.y + dy});
        EXPECT_EQ(d.home_root().x1, dx);
        EXPECT_EQ(d.home_root().y1, 0);
        EXPECT_EQ(d.page_root().y1, 0);
    }
    d.up();
    ASSERT_TRUE(d.settled());
    d.expect_home();
}

TEST(PageDrag, SliderDragDoesNotNavigate) {
    Drags d;
    ASSERT_TRUE(d.started());
    d.h().show(Page::Settings);
    lv_point_t at{};
    d.h().on_ui([&] {
        const lv_area_t a = coords_of(d.shell().settings_page()->brightness());
        at = {a.x2 - 4, (a.y1 + a.y2) / 2};
    });
    // Left is Settings' way back.
    d.slow(at, -(d.width() / 2), 0);
    d.h().on_ui([&] { EXPECT_FALSE(d.shell().sliding()); });
    d.up();
    ASSERT_TRUE(d.settled());
    d.expect_page(Page::Settings);
}

// A press the system layer takes away (here: LVGL told to wait for the
// release, which then reports PRESS_LOST) snaps back, however far it went.
TEST(PageDrag, PressLostSnapsBack) {
    Drags d;
    ASSERT_TRUE(d.started());
    d.slow(kHomeLeft, d.width() / 2, 0);
    d.h().on_ui([&] {
        ASSERT_TRUE(d.shell().sliding());
        lv_indev_wait_release(lv_indev_get_next(nullptr));
    });
    d.up();
    ASSERT_TRUE(d.settled());
    d.expect_home();
}

// No drag from the top zone downwards (the panel's), nor the way a page
// has no target.
TEST(PageDrag, NoTargetNoDrag) {
    Drags d;
    ASSERT_TRUE(d.started());
    d.slow({240, 2}, 0, 200);
    d.h().on_ui([&] { EXPECT_FALSE(d.shell().sliding()); });
    d.up();
    d.slow({60, 400}, 0, -200);  // up on Home
    d.h().on_ui([&] { EXPECT_FALSE(d.shell().sliding()); });
    d.up();
    d.expect_home();
}

// A programmatic navigation in the middle of a drag ends it first.
TEST(PageDrag, ShowEndsADrag) {
    Drags d;
    ASSERT_TRUE(d.started());
    d.slow(kHomeLeft, 200, 0);
    d.h().show(Page::History);
    d.h().on_ui([&] { EXPECT_FALSE(d.shell().sliding()); });
    d.up();
    d.expect_page(Page::History);
}

// --- every shape ------------------------------------------------------------

namespace {

struct ShapeCase {
    const char* shape;
    int32_t width;
    int32_t height;
};

class PageDragShapes : public ::testing::TestWithParam<ShapeCase> {};

}  // namespace

// Home -> scene list -> Home; mid-drag the pages sit side by side, and at
// rest the page's title is where the page puts it.
TEST_P(PageDragShapes, ToScenesAndBack) {
    const ShapeCase& c = GetParam();
    Drags d(c.shape, c.width, c.height);
    ASSERT_TRUE(d.started());
    const lv_point_t from{c.width - 40, c.height - 60};
    d.slow(from, -(c.width / 2), 0);
    EXPECT_EQ(d.home_root().x1, -(c.width / 2));
    EXPECT_EQ(d.page_root().x1, c.width - c.width / 2);
    d.up();
    ASSERT_TRUE(d.settled());
    d.expect_page(Page::Scenes);
    d.h().on_ui([&] {
        lv_obj_t* title = lv_obj_get_child(d.shell().page_root(), 0);
        EXPECT_TRUE(box_visible(d.h().shape(), coords_of(title)));
    });
    d.slow({40, c.height - 60}, c.width / 2, 0);
    d.up();
    ASSERT_TRUE(d.settled());
    d.expect_home();
    d.h().on_ui([&] {
        lv_obj_update_layout(d.shell().home_screen());
        EXPECT_TRUE(box_visible(
            d.h().shape(),
            coords_of(d.shell().wisp() ? d.shell().wisp()->obj() : d.shell().home_screen())));
    });
}

INSTANTIATE_TEST_SUITE_P(
    Shapes, PageDragShapes,
    ::testing::Values(ShapeCase{"rect", 480, 480}, ShapeCase{"rounded_rect", 480, 480},
                      ShapeCase{"circle", 480, 480}, ShapeCase{"circle", 360, 480}),
    [](const ::testing::TestParamInfo<ShapeCase>& info) {
        return std::string(info.param.shape) + "_" + std::to_string(info.param.width);
    });
