// SceneView swipes (on_swipe): touches fed through a fake touch panel and
// LVGL's real input path, with the system layer's back and pull-down
// gestures watching the same pointer (scene_view.hpp).

#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

#include "support/scene_view_fixture.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/scene/scene_view.hpp"
#include "wisp/ui/system_layer.hpp"

namespace {

using wisp::scene::classify_swipe;
using wisp::scene::kEventClick;
using wisp::scene::kEventSwipe;
using wisp::scene::NodeId;
using wisp::scene::Prop;
using wisp::scene::PropValue;
using wisp::scene::SwipeDir;
using wisp::scene::UiEvent;
using wisp::scene::Widget;
using wisp::test::build;
using wisp::test::node;
using wisp::test::num;
using wisp::test::RecordedEvent;
using wisp::test::str;
using wisp::ui::SystemLayer;

// Ids of the tree build_board() sends.
enum : NodeId {
    kRoot = 1,
    kBoard = 2,    // column, on_swipe
    kTitle = 3,    // label
    kButton = 4,   // button, on_click
    kPlain = 5,    // stack without handlers ...
    kInner = 6,    // ... holding a label
    kNested = 7,   // stack, on_swipe of its own
    kScroll = 8,   // vertical scroll with more rows than fit
    kSlider = 30,  // slider
    kCheck = 31,   // checkbox
};

std::string dir_of(const RecordedEvent& e) {
    const auto* s = std::get_if<wisp::os::LargeString>(&e.value);
    return s ? std::string(s->data(), s->size()) : std::string("(not a string)");
}

class SceneViewSwipe : public wisp::test::SceneViewTest,
                       public ::testing::WithParamInterface<const char*> {
protected:
    void start_touch(const char* shape_name) {
        with_touch = true;
        start(shape_name);
        // The scene host loads the view's screen, and another one before
        // the view goes; here the test does.
        on_ui([&] {
            home = lv_screen_active();
            lv_screen_load(view->screen());
        });
    }

    void TearDown() override {
        if (ui && home != nullptr) on_ui([&] { lv_screen_load(home); });
        SceneViewTest::TearDown();
    }

    void build_board() {
        wisp::scene::Build b;
        auto add = [&](wisp::scene::NodeDesc d) { b.nodes.push_back(std::move(d)); };
        add(node(kRoot, 0, Widget::Screen));
        add(node(kBoard, kRoot, Widget::Column,
                 {{Prop::W, str("fill")}, {Prop::H, str("fill")}, {Prop::Align, str("center")}},
                 kEventSwipe));
        add(node(kTitle, kBoard, Widget::Label, {{Prop::Text, str("board")}}));
        add(node(kButton, kBoard, Widget::Button,
                 {{Prop::Text, str("go")}, {Prop::W, num(300)}, {Prop::H, num(80)}}, kEventClick));
        add(node(kPlain, kBoard, Widget::Stack, {{Prop::W, num(300)}, {Prop::H, num(80)}}));
        add(node(kInner, kPlain, Widget::Label, {{Prop::Text, str("inner")}}));
        add(node(kNested, kBoard, Widget::Stack, {{Prop::W, num(300)}, {Prop::H, num(80)}},
                 kEventSwipe));
        add(node(kScroll, kBoard, Widget::Scroll, {{Prop::W, num(300)}, {Prop::H, num(80)}}));
        for (NodeId i = 0; i < 20; ++i)
            add(node(kScroll + 1 + i, kScroll, Widget::Label, {{Prop::Text, str("row")}}));
        add(node(kSlider, kBoard, Widget::Slider, {{Prop::W, num(300)}}));
        add(node(kCheck, kBoard, Widget::Checkbox, {{Prop::Text, str("check")}}));
        apply(b);
        on_ui([&] {
            lv_obj_update_layout(view->screen());
            lv_obj_update_layout(lv_layer_top());
        });
    }

    lv_point_t center(NodeId id) {
        lv_area_t a{};
        on_ui([&] { lv_obj_get_coords(view->object(id), &a); });
        return {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
    }

    static wisp::hal::TouchPoint at(int32_t x, int32_t y) {
        return {static_cast<int16_t>(x), static_cast<int16_t>(y), 0};
    }

    // Down at `from`, through `steps` points to `to`, up.
    void drag(lv_point_t from, lv_point_t to, int steps = 4) {
        feed({at(from.x, from.y)});
        for (int i = 1; i <= steps; ++i)
            feed({at(from.x + (to.x - from.x) * i / steps, from.y + (to.y - from.y) * i / steps)});
        feed({});
    }

    // A swipe of `d` pixels from the middle of `id`.
    void swipe_over(NodeId id, int32_t dx, int32_t dy) {
        const lv_point_t c = center(id);
        drag({c.x - dx / 2, c.y - dy / 2}, {c.x + dx / 2, c.y + dy / 2});
    }

    lv_obj_t* home = nullptr;

    std::vector<RecordedEvent> swipes() {
        std::vector<RecordedEvent> out;
        for (auto& e : take_events())
            if (e.ev == UiEvent::Swipe) out.push_back(std::move(e));
        return out;
    }
};

INSTANTIATE_TEST_SUITE_P(Shapes, SceneViewSwipe,
                         ::testing::Values("rect", "rounded_rect", "circle"));

TEST(SwipeRule, ThresholdAndDominantAxis) {
    wisp::hal::ScreenShape s{wisp::hal::ScreenShapeKind::Circle, 480, 480, 240};
    EXPECT_EQ(wisp::scene::swipe_min_px(s), 30);
    s.width = s.height = 800;
    EXPECT_EQ(wisp::scene::swipe_min_px(s), 50);
    s.width = 240;
    EXPECT_EQ(wisp::scene::swipe_min_px(s), 30);

    EXPECT_EQ(classify_swipe({100, 100}, {130, 100}, 30), SwipeDir::Right);
    EXPECT_EQ(classify_swipe({100, 100}, {129, 100}, 30), SwipeDir::None);
    EXPECT_EQ(classify_swipe({100, 100}, {40, 140}, 30), SwipeDir::Left);  // 60 vs 40: 1.5x
    EXPECT_EQ(classify_swipe({100, 100}, {40, 141}, 30), SwipeDir::None);
    EXPECT_EQ(classify_swipe({100, 100}, {100, 40}, 30), SwipeDir::Up);
    EXPECT_EQ(classify_swipe({100, 100}, {110, 200}, 30), SwipeDir::Down);
    EXPECT_EQ(classify_swipe({100, 100}, {160, 160}, 30), SwipeDir::None);

    EXPECT_STREQ(wisp::scene::swipe_name(SwipeDir::Down), "down");
    EXPECT_EQ(wisp::scene::swipe_from_name("left"), SwipeDir::Left);
    EXPECT_EQ(wisp::scene::swipe_from_name("Left"), SwipeDir::None);
}

// A swipe over a child without handlers (a stack holding a label) reaches
// the board, once and in the direction the finger went.
TEST_P(SceneViewSwipe, EachDirectionReachesTheNearestOnSwipe) {
    start_touch(GetParam());
    build_board();
    on_ui([&] { EXPECT_TRUE(lv_obj_has_flag(view->object(kBoard), LV_OBJ_FLAG_CLICKABLE)); });
    const struct {
        int32_t dx, dy;
        const char* dir;
    } cases[] = {{-100, 0, "left"}, {100, 0, "right"}, {0, -70, "up"}, {0, 70, "down"}};
    for (const auto& c : cases) {
        SCOPED_TRACE(c.dir);
        swipe_over(kPlain, c.dx, c.dy);
        auto got = take_events();
        ASSERT_EQ(got.size(), 1u);
        EXPECT_EQ(got[0].node, kBoard);
        EXPECT_EQ(got[0].ev, UiEvent::Swipe);
        EXPECT_EQ(dir_of(got[0]), c.dir);
    }
    // Over the title label, straight on the board.
    swipe_over(kTitle, -80, 0);
    auto got = swipes();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].node, kBoard);
    EXPECT_EQ(dir_of(got[0]), "left");

    // A nested stack with on_swipe of its own takes it; the board does not.
    swipe_over(kNested, -100, 0);
    got = take_events();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].node, kNested);
    EXPECT_EQ(dir_of(got[0]), "left");
}

TEST_P(SceneViewSwipe, ASwipeIsNotAClickAndATapIsNotASwipe) {
    start_touch(GetParam());
    build_board();
    // A swipe that starts on the button: the board's, and no click.
    swipe_over(kButton, 100, 0);
    auto got = take_events();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].node, kBoard);
    EXPECT_EQ(dir_of(got[0]), "right");

    // A tap that wobbles below the threshold clicks.
    const lv_point_t c = center(kButton);
    drag(c, {c.x + 12, c.y - 8});
    got = take_events();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].node, kButton);
    EXPECT_EQ(got[0].ev, UiEvent::Click);

    // The same over a stack: nothing at all.
    swipe_over(kPlain, 20, 0);
    EXPECT_TRUE(take_events().empty());

    // A drag across the checkbox swipes and does not toggle it.
    swipe_over(kCheck, 60, 0);
    got = take_events();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(dir_of(got[0]), "right");
    on_ui([&] { EXPECT_FALSE(lv_obj_has_state(view->object(kCheck), LV_STATE_CHECKED)); });
}

// Neither axis 1.5 times the other: no swipe, and, having moved past the
// threshold, no click either.
TEST_P(SceneViewSwipe, DiagonalIsNeitherSwipeNorClick) {
    start_touch(GetParam());
    build_board();
    swipe_over(kPlain, 60, 50);
    swipe_over(kButton, -50, 50);
    EXPECT_TRUE(take_events().empty());
}

// Along a scroll's direction a drag scrolls it; across it, it swipes.
TEST_P(SceneViewSwipe, ScrollKeepsItsAxis) {
    start_touch(GetParam());
    build_board();
    swipe_over(kScroll, 0, -60);
    on_ui([&] { EXPECT_GT(lv_obj_get_scroll_y(view->object(kScroll)), 0); });
    swipe_over(kScroll, 0, 60);
    EXPECT_TRUE(swipes().empty());
    swipe_over(kScroll, -100, 0);
    auto got = swipes();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].node, kBoard);
    EXPECT_EQ(dir_of(got[0]), "left");
}

// A scroll without enough rows to scroll still keeps its axis.
TEST_P(SceneViewSwipe, ScrollWithNothingToScrollKeepsItsAxisToo) {
    start_touch(GetParam());
    apply(build({node(1, 0, Widget::Screen, {}, kEventSwipe),
                 node(2, 1, Widget::Scroll, {{Prop::W, num(300)}, {Prop::H, num(300)}}),
                 node(3, 2, Widget::Label, {{Prop::Text, str("one")}})}));
    on_ui([&] { lv_obj_update_layout(view->screen()); });
    swipe_over(2, 0, 100);
    EXPECT_TRUE(swipes().empty());
    swipe_over(2, 100, 0);
    auto got = swipes();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].node, 1u);
    EXPECT_EQ(dir_of(got[0]), "right");
}

TEST_P(SceneViewSwipe, SliderDragIsNoSwipe) {
    start_touch(GetParam());
    build_board();
    swipe_over(kSlider, 120, 0);
    EXPECT_TRUE(swipes().empty());
}

// The canvas takes on_swipe itself, and an on_swipe root gets the rest.
TEST_P(SceneViewSwipe, CanvasAndRoot) {
    start_touch(GetParam());
    apply(
        build({node(1, 0, Widget::Screen, {{Prop::Align, str("center")}}, kEventSwipe),
               node(2, 1, Widget::Canvas, {{Prop::W, num(200)}, {Prop::H, num(160)}}, kEventSwipe),
               node(3, 1, Widget::Label, {{Prop::Text, str("score")}})}));
    on_ui([&] { lv_obj_update_layout(view->screen()); });
    swipe_over(2, 0, -100);
    swipe_over(3, 100, 0);
    auto got = take_events();
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].node, 2u);
    EXPECT_EQ(dir_of(got[0]), "up");
    EXPECT_EQ(got[1].node, 1u);
    EXPECT_EQ(dir_of(got[1]), "right");
}

// The back gesture takes a rightward swipe from the left edge first.
TEST_P(SceneViewSwipe, BackGestureKeepsPriority) {
    start_touch(GetParam());
    build_board();
    int backs = 0;
    on_ui([&] { layer->set_back_handler([&] { ++backs; }); });
    const int32_t y = center(kNested).y;
    int32_t x = 0;
    while (!SystemLayer::in_back_zone(shape, x, y)) ++x;
    drag({x + 2, y}, {x + 160, y}, 8);
    EXPECT_EQ(backs, 1);
    EXPECT_TRUE(take_events().empty());

    // A rightward swipe from outside the zone is the scene's.
    const int32_t from = center(kNested).x - 70;
    drag({from, y}, {from + 140, y});
    EXPECT_EQ(backs, 1);
    auto got = swipes();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].node, kNested);
    EXPECT_EQ(dir_of(got[0]), "right");
}

// The pull-down takes a downward swipe from the top edge first; one from
// lower down is the scene's.
TEST_P(SceneViewSwipe, PullDownKeepsPriority) {
    start_touch(GetParam());
    build_board();
    on_ui([&] { layer->set_panel_handlers({.on_open = [] {}}); });
    const int32_t x = 240;
    int32_t y = 0;
    while (!SystemLayer::in_top_zone(shape, x, y)) ++y;
    drag({x, y + 2}, {x, y + 160}, 8);
    on_ui([&] {
        EXPECT_TRUE(layer->panel_open());
        layer->close_panel();
    });
    EXPECT_TRUE(take_events().empty());

    swipe_over(kButton, 0, 70);
    auto got = take_events();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].node, kBoard);
    EXPECT_EQ(dir_of(got[0]), "down");
}

// swipe() (the scripts' way in) goes to the same node a finger's would.
TEST_P(SceneViewSwipe, ProgrammaticSwipeFindsTheNearestOnSwipe) {
    start_touch(GetParam());
    build_board();
    on_ui([&] {
        EXPECT_TRUE(view->swipe(kInner, SwipeDir::Up));
        EXPECT_TRUE(view->swipe(kNested, SwipeDir::Left));
        EXPECT_FALSE(view->swipe(kRoot, SwipeDir::Down));
        EXPECT_FALSE(view->swipe(kInner, SwipeDir::None));
        EXPECT_FALSE(view->swipe(999, SwipeDir::Down));
    });
    auto got = take_events();
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].node, kBoard);
    EXPECT_EQ(dir_of(got[0]), "up");
    EXPECT_EQ(got[1].node, kNested);
    EXPECT_EQ(dir_of(got[1]), "left");
}

}  // namespace
