// on_swipe end to end: the swipe_demo fixture (tests/scene/fixtures) on a
// SceneRuntime, swiped through the fake touch panel and LVGL's input path
// and through SceneRuntime::swipe_ref(), on every screen shape.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/scene/scene_view.hpp"

namespace {

namespace fs = std::filesystem;
using wisp::ErrorCode;
using wisp::test::wait_until;

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

class SceneSwipe : public wisp::test::SceneRuntimeFixture,
                   public ::testing::WithParamInterface<const char*> {
protected:
    void open_demo() {
        start(GetParam());
        const fs::path from = fs::path(WISP_TEST_DIR) / "scene" / "fixtures" / "swipe_demo";
        install("swipe_demo", read_file(from / "main.lua"),
                {{"scene.json", read_file(from / "scene.json")}});
        auto opened = rt->open("swipe_demo");
        ASSERT_TRUE(opened) << opened.error().message;
        ASSERT_TRUE(wait_label("swipe none 0"));
        on_ui([&] {
            lv_obj_update_layout(lv_screen_active());
            lv_obj_update_layout(lv_layer_top());
        });
    }

    lv_area_t area(const char* ref) {
        lv_area_t a{};
        on_ui([&] {
            const wisp::scene::SceneView* v = rt->ui_current_view();
            ASSERT_NE(v, nullptr);
            auto id = v->find_ref(ref);
            ASSERT_TRUE(id);
            lv_obj_get_coords(v->object(*id), &a);
        });
        return a;
    }

    static lv_point_t middle(const lv_area_t& a) { return {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2}; }

    static wisp::hal::TouchPoint at(int32_t x, int32_t y) {
        return {static_cast<int16_t>(x), static_cast<int16_t>(y), 0};
    }

    void drag(lv_point_t from, int32_t dx, int32_t dy, int steps = 5) {
        feed({at(from.x, from.y)});
        for (int i = 1; i <= steps; ++i)
            feed({at(from.x + dx * i / steps, from.y + dy * i / steps)});
        feed({});
    }
};

INSTANTIATE_TEST_SUITE_P(Shapes, SceneSwipe, ::testing::Values("rect", "rounded_rect", "circle"));

TEST_P(SceneSwipe, SwipesMoveTheBoardAndTapsStillClick) {
    open_demo();
    const lv_area_t board = area("grid");
    const lv_point_t c = middle(board);

    // Over the canvas, above the widgets drawn on it.
    drag({c.x + 50, board.y1 + 40}, -100, 0);
    EXPECT_TRUE(wait_label("swipe left 1")) << label("swipe");
    // Over the label on top of the canvas.
    const lv_point_t status = middle(area("status"));
    drag({status.x, status.y + 40}, 0, -80);
    EXPECT_TRUE(wait_label("swipe up 2")) << label("swipe");
    // Over the button: a swipe, not a tap.
    const lv_point_t tap = middle(area("tap"));
    drag({tap.x - 50, tap.y}, 100, 0);
    EXPECT_TRUE(wait_label("swipe right 3")) << label("swipe");
    // Down is reachable from anywhere below the top edge.
    drag({c.x, c.y - 40}, 0, 90);
    EXPECT_TRUE(wait_label("swipe down 4")) << label("swipe");
    EXPECT_EQ(label("taps"), "taps 0");

    // A tap on the button clicks.
    drag(tap, 4, 3, 1);
    EXPECT_TRUE(wait_label("taps 1")) << label("taps");
    EXPECT_EQ(label("swipe"), "swipe down 4");
}

TEST_P(SceneSwipe, SwipeRefReachesTheNearestOnSwipe) {
    open_demo();
    ASSERT_TRUE(rt->swipe_ref("tap", "down"));
    EXPECT_TRUE(wait_label("swipe down 1")) << label("swipe");
    ASSERT_TRUE(rt->swipe_ref("board", "left"));
    EXPECT_TRUE(wait_label("swipe left 2")) << label("swipe");
    EXPECT_EQ(rt->swipe_ref("tap", "sideways").error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(rt->swipe_ref("nope", "up").error().code, ErrorCode::NotFound);
    EXPECT_EQ(label("taps"), "taps 0");
}

// The back gesture still closes the scene; it is no swipe on the way.
TEST_P(SceneSwipe, BackGestureStillGoesBack) {
    open_demo();
    const int32_t y = middle(area("grid")).y;
    int32_t x = 0;
    while (!wisp::ui::SystemLayer::in_back_zone(shape, x, y)) ++x;
    feed({at(x + 2, y)});
    for (int32_t dx : {20, 60, 120, 180}) feed({at(x + 2 + dx, y)});
    EXPECT_EQ(label("swipe"), "swipe none 0");
    feed({});
    EXPECT_TRUE(wait_until([&] { return !rt->foreground(); }, 1000));
    EXPECT_TRUE(wait_until([&] { return active_screen() == home; }, 1000));
}

}  // namespace
