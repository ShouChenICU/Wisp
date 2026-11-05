// SceneView: widget trees and their updates mapped to LVGL on the UI
// thread (scene-runtime-core spec 4.3-4.4).

#include "wisp/scene/scene_view.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <variant>
#include <vector>

#include "support/scene_view_fixture.hpp"
#include "wisp/scene/patch_batch.hpp"
#include "wisp/scene/theme.hpp"

namespace {

using wisp::scene::Focus;
using wisp::scene::kEventChange;
using wisp::scene::kEventClick;
using wisp::scene::ListReset;
using wisp::scene::Prop;
using wisp::scene::PropValue;
using wisp::scene::ScrollTo;
using wisp::scene::UiEvent;
using wisp::scene::Widget;
using wisp::test::build;
using wisp::test::node;
using wisp::test::num;
using wisp::test::set;
using wisp::test::str;

class SceneView : public wisp::test::SceneViewTest {};

std::string label_text(lv_obj_t* label) { return lv_label_get_text(label); }

struct Pads {
    int32_t left, top, right, bottom;
    bool operator==(const Pads&) const = default;
};

Pads pads(lv_obj_t* obj) {
    return {lv_obj_get_style_pad_left(obj, LV_PART_MAIN),
            lv_obj_get_style_pad_top(obj, LV_PART_MAIN),
            lv_obj_get_style_pad_right(obj, LV_PART_MAIN),
            lv_obj_get_style_pad_bottom(obj, LV_PART_MAIN)};
}

TEST_F(SceneView, RootForcesSafePadding) {
    start("circle");
    const wisp::hal::Area safe = wisp::hal::safe_rect(shape);
    ASSERT_EQ(safe.x1, 74);
    ASSERT_EQ(safe.y1, 74);
    ASSERT_EQ(safe.x2, 405);
    ASSERT_EQ(safe.y2, 405);

    // A smaller pad than the safe area's is raised to it.
    apply(build({node(1, 0, Widget::Screen, {{Prop::Pad, num(4)}}),
                 node(2, 1, Widget::Column, {{Prop::Pad, str("safe")}}),
                 node(3, 1, Widget::Column, {{Prop::Pad, num(8)}})}));
    on_ui([&] {
        lv_obj_t* screen = view->screen();
        ASSERT_EQ(view->object(1), screen);
        lv_obj_update_layout(screen);
        EXPECT_EQ(lv_obj_get_width(screen), 480);
        EXPECT_EQ(lv_obj_get_height(screen), 480);
        EXPECT_EQ(pads(screen), (Pads{74, 74, 74, 74}));
        // Inside the root, "safe" adds nothing: the root already keeps
        // its content in the safe area.
        EXPECT_EQ(pads(view->object(2)), (Pads{0, 0, 0, 0}));
        EXPECT_EQ(pads(view->object(3)), (Pads{8, 8, 8, 8}));
    });

    // A larger one is kept; "safe" and a later SetProp that tries to
    // shrink it are held to the safe area.
    apply(set(1, Prop::Pad, num(100)));
    on_ui([&] { EXPECT_EQ(pads(view->screen()), (Pads{100, 100, 100, 100})); });
    apply(set(1, Prop::Pad, num(0)));
    on_ui([&] { EXPECT_EQ(pads(view->screen()), (Pads{74, 74, 74, 74})); });
    apply(set(1, Prop::Pad, str("safe")));
    on_ui([&] { EXPECT_EQ(pads(view->screen()), (Pads{74, 74, 74, 74})); });
    // A wrong type is ignored rather than dropping the safe padding.
    apply(set(1, Prop::Pad, PropValue{true}));
    on_ui([&] { EXPECT_EQ(pads(view->screen()), (Pads{74, 74, 74, 74})); });
}

// The spec's counter example: screen > column { pad = "safe" }.
TEST_F(SceneView, SafeColumnGetsTheWholeSafeArea) {
    start("circle");
    apply(build({node(1, 0, Widget::Screen),
                 node(2, 1, Widget::Column,
                      {{Prop::Pad, str("safe")}, {Prop::W, str("fill")}, {Prop::H, str("fill")}}),
                 node(3, 2, Widget::Label, {{Prop::Text, str("hi")}})}));
    on_ui([&] {
        lv_obj_update_layout(view->screen());
        lv_obj_t* col = view->object(2);
        EXPECT_EQ(lv_obj_get_content_width(col), 332);
        EXPECT_EQ(lv_obj_get_content_height(col), 332);
        lv_area_t a;
        lv_obj_get_coords(col, &a);
        EXPECT_EQ(a.x1, 74);
        EXPECT_EQ(a.y1, 74);
        lv_obj_get_coords(view->object(3), &a);
        EXPECT_EQ(a.x1, 74);
        EXPECT_EQ(a.y1, 74);
    });

    // As the spec writes it, content-sized and centred.
    apply(
        build({node(10, 0, Widget::Screen),
               node(11, 10, Widget::Column,
                    {{Prop::Pad, str("safe")}, {Prop::Gap, num(12)}, {Prop::Align, str("center")}}),
               node(12, 11, Widget::Label, {{Prop::Text, str("hi")}})}));
    on_ui([&] {
        lv_obj_update_layout(view->screen());
        EXPECT_EQ(lv_obj_get_content_width(view->screen()), 332);
        EXPECT_EQ(lv_obj_get_content_height(view->screen()), 332);
        EXPECT_EQ(pads(view->object(11)), (Pads{0, 0, 0, 0}));
        lv_area_t a;
        lv_obj_get_coords(view->object(12), &a);
        EXPECT_EQ(a.x1, 74);
        EXPECT_EQ(a.y1, 74);
    });
}

TEST_F(SceneView, UnsetSizeRestoresTheWidgetDefault) {
    start();
    apply(build({node(1, 0, Widget::Screen),
                 node(2, 1, Widget::Label, {{Prop::W, num(100)}, {Prop::Text, str("x")}}),
                 node(3, 1, Widget::Slider, {{Prop::W, str("fill")}})}));
    int32_t slider_default = 0;
    on_ui([&] {
        lv_obj_t* probe = lv_slider_create(view->screen());
        slider_default = lv_obj_get_style_width(probe, LV_PART_MAIN);
        lv_obj_delete(probe);
        EXPECT_EQ(lv_obj_get_style_width(view->object(2), LV_PART_MAIN), 100);
    });
    apply(set(2, Prop::W, PropValue{}));
    apply(set(3, Prop::W, PropValue{}));
    on_ui([&] {
        EXPECT_EQ(lv_obj_get_style_width(view->object(2), LV_PART_MAIN), LV_SIZE_CONTENT);
        EXPECT_EQ(lv_obj_get_style_width(view->object(3), LV_PART_MAIN), slider_default);
    });
}

TEST_F(SceneView, RootOfARectScreenNeedsNoPadding) {
    start("rect");
    apply(build({node(1, 0, Widget::Screen)}));
    on_ui([&] { EXPECT_EQ(pads(view->screen()), (Pads{0, 0, 0, 0})); });
}

TEST_F(SceneView, BuildsCounterTree) {
    start();
    apply(build({
        node(1, 0, Widget::Screen),
        node(2, 1, Widget::Column,
             {{Prop::Pad, str("safe")},
              {Prop::Gap, num(12)},
              {Prop::Align, str("center")},
              {Prop::Ref, str("col")}}),
        node(3, 2, Widget::Label, {{Prop::Text, str("已点击 0 次")}, {Prop::Font, str("xl")}}),
        node(4, 2, Widget::Button, {{Prop::Text, str("+1")}}, kEventClick),
    }));
    on_ui([&] {
        EXPECT_EQ(view->object_count(), 4u);
        lv_obj_t* col = view->object(2);
        ASSERT_NE(col, nullptr);
        EXPECT_EQ(lv_obj_get_parent(col), view->screen());
        EXPECT_EQ(lv_obj_get_style_pad_row(col, LV_PART_MAIN), 12);
        EXPECT_EQ(lv_obj_get_style_pad_column(col, LV_PART_MAIN), 12);
        EXPECT_EQ(lv_obj_get_style_flex_flow(col, LV_PART_MAIN), LV_FLEX_FLOW_COLUMN);
        EXPECT_EQ(lv_obj_get_style_flex_cross_place(col, LV_PART_MAIN), LV_FLEX_ALIGN_CENTER);

        lv_obj_t* label = view->object(3);
        ASSERT_TRUE(lv_obj_check_type(label, &lv_label_class));
        EXPECT_EQ(lv_obj_get_parent(label), col);
        EXPECT_EQ(label_text(label), "已点击 0 次");
        EXPECT_EQ(lv_obj_get_style_text_font(label, LV_PART_MAIN), wisp::ui::font(32));

        lv_obj_t* btn = view->object(4);
        ASSERT_TRUE(lv_obj_check_type(btn, &lv_button_class));
        lv_obj_t* btn_label = wisp::test::find_obj_of_class(btn, &lv_label_class);
        ASSERT_NE(btn_label, nullptr);
        EXPECT_EQ(label_text(btn_label), "+1");

        EXPECT_EQ(view->find_ref("col"), std::optional<wisp::scene::NodeId>(2));
        EXPECT_EQ(view->find_ref("nope"), std::nullopt);
        EXPECT_EQ(view->object(99), nullptr);
    });

    // A second Build replaces the tree.
    apply(build({node(10, 0, Widget::Screen), node(11, 10, Widget::Label)}));
    on_ui([&] {
        EXPECT_EQ(view->object_count(), 2u);
        EXPECT_EQ(view->object(3), nullptr);
        EXPECT_EQ(view->find_ref("col"), std::nullopt);
        EXPECT_EQ(lv_obj_get_child_count(view->screen()), 1u);
    });
}

TEST_F(SceneView, RejectsMalformedBuildKeepingTheTree) {
    start();
    apply(build(
        {node(1, 0, Widget::Screen), node(2, 1, Widget::Label, {{Prop::Text, str("kept")}})}));
    // Root not a screen; a parent that is not built yet; a parent that
    // takes no children; an unknown widget; a duplicate id.
    apply(build({node(5, 0, Widget::Column)}));
    apply(build({node(5, 0, Widget::Screen), node(6, 7, Widget::Label)}));
    apply(
        build({node(5, 0, Widget::Screen), node(6, 5, Widget::Label), node(7, 6, Widget::Label)}));
    {
        auto bad = build({node(5, 0, Widget::Screen)});
        auto& b = std::get<wisp::scene::Build>(bad);
        b.nodes.push_back(node(6, 5, Widget::Label));
        b.nodes.back().widget = 999;
        apply(bad);
    }
    apply(
        build({node(5, 0, Widget::Screen), node(6, 5, Widget::Label), node(6, 5, Widget::Label)}));
    apply(wisp::scene::Build{});
    on_ui([&] {
        EXPECT_EQ(view->object_count(), 2u);
        ASSERT_NE(view->object(2), nullptr);
        EXPECT_EQ(label_text(view->object(2)), "kept");
        EXPECT_EQ(view->object(5), nullptr);
    });
}

TEST_F(SceneView, SetPropUpdatesLabelAndHidden) {
    start();
    apply(build({node(1, 0, Widget::Screen), node(2, 1, Widget::Label, {{Prop::Text, str("a")}}),
                 node(3, 1, Widget::Button, {{Prop::Text, str("b")}})}));
    apply(set(2, Prop::Text, str("hello")));
    apply(set(3, Prop::Text, str("ok")));
    apply(set(2, Prop::Hidden, PropValue{true}));
    on_ui([&] {
        EXPECT_EQ(label_text(view->object(2)), "hello");
        EXPECT_TRUE(lv_obj_has_flag(view->object(2), LV_OBJ_FLAG_HIDDEN));
        EXPECT_EQ(label_text(wisp::test::find_obj_of_class(view->object(3), &lv_label_class)),
                  "ok");
    });
    apply(set(2, Prop::Hidden, PropValue{false}));
    // Numbers show as Lua prints them.
    apply(set(3, Prop::Text, num(42)));
    on_ui([&] {
        EXPECT_FALSE(lv_obj_has_flag(view->object(2), LV_OBJ_FLAG_HIDDEN));
        EXPECT_EQ(label_text(wisp::test::find_obj_of_class(view->object(3), &lv_label_class)),
                  "42");
    });
    apply(set(2, Prop::Text, PropValue{1.5}));
    on_ui([&] { EXPECT_EQ(label_text(view->object(2)), "1.5"); });
    apply(set(2, Prop::Text, PropValue{2.0}));
    on_ui([&] { EXPECT_EQ(label_text(view->object(2)), "2.0"); });

    // Unknown nodes, wrong types and props the widget lacks are ignored.
    apply(set(99, Prop::Text, str("x")));
    apply(set(2, Prop::Text, PropValue{true}));
    apply(set(2, Prop::Hidden, str("yes")));
    apply(set(2, Prop::Checked, PropValue{true}));
    apply(set(2, Prop::Bg, str("nonsense")));
    on_ui([&] {
        EXPECT_EQ(label_text(view->object(2)), "2.0");
        EXPECT_FALSE(lv_obj_has_flag(view->object(2), LV_OBJ_FLAG_HIDDEN));
    });
}

TEST_F(SceneView, NonFiniteAndOutOfRangeNumbersAreClamped) {
    start();
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    apply(build(
        {node(1, 0, Widget::Screen),
         node(2, 1, Widget::Slider,
              {{Prop::Value, PropValue{nan}},
               {Prop::Min, PropValue{-inf}},
               {Prop::Max, PropValue{inf}}}),
         node(3, 1, Widget::Bar, {{Prop::Value, PropValue{1e300}}}),
         node(4, 1, Widget::Arc, {{Prop::Value, num(INT64_MIN)}}),
         node(5, 1, Widget::Label, {{Prop::W, num(INT64_MAX)}, {Prop::Opa, PropValue{nan}}})}));
    on_ui([&] {
        lv_obj_t* slider = view->object(2);
        EXPECT_LE(lv_slider_get_min_value(slider), lv_slider_get_value(slider));
        EXPECT_GE(lv_slider_get_max_value(slider), lv_slider_get_value(slider));
        EXPECT_LT(lv_slider_get_min_value(slider), lv_slider_get_max_value(slider));
        EXPECT_EQ(lv_bar_get_value(view->object(3)), lv_bar_get_max_value(view->object(3)));
        EXPECT_EQ(lv_arc_get_value(view->object(4)), lv_arc_get_min_value(view->object(4)));
        lv_obj_update_layout(view->screen());
        EXPECT_LE(lv_obj_get_width(view->object(5)), 4096);
    });
    // A whole-number range within kDirectLimit reaches LVGL unscaled.
    apply(set(2, Prop::Min, num(0)));
    apply(set(2, Prop::Max, num(100)));
    apply(set(2, Prop::Value, PropValue{30.6}));
    on_ui([&] {
        EXPECT_EQ(lv_slider_get_max_value(view->object(2)), 100);
        EXPECT_EQ(lv_slider_get_value(view->object(2)), 31);
    });
}

TEST_F(SceneView, FractionalSliderRangeIsScaled) {
    start();
    apply(build({node(1, 0, Widget::Screen), node(2, 1, Widget::Slider,
                                                  {{Prop::Min, PropValue{0.0}},
                                                   {Prop::Max, PropValue{0.5}},
                                                   {Prop::Value, PropValue{0.25}}},
                                                  kEventChange)}));
    on_ui([&] {
        lv_obj_t* slider = view->object(2);
        EXPECT_EQ(lv_slider_get_min_value(slider), 0);
        EXPECT_EQ(lv_slider_get_max_value(slider), wisp::scene::SceneView::kScaledSteps);
        EXPECT_EQ(lv_slider_get_value(slider), wisp::scene::SceneView::kScaledSteps / 2);
        lv_slider_set_value(slider, wisp::scene::SceneView::kScaledSteps / 10, LV_ANIM_OFF);
        lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
    });
    auto events = take_events();
    ASSERT_EQ(events.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<double>(events[0].value));
    EXPECT_DOUBLE_EQ(std::get<double>(events[0].value), 0.05);
}

TEST_F(SceneView, ClickAndChangeEventsReachCallback) {
    start();
    apply(build(
        {node(1, 0, Widget::Screen),
         node(2, 1, Widget::Button, {{Prop::Text, str("go")}}, kEventClick),
         node(3, 1, Widget::Slider, {{Prop::Min, num(0)}, {Prop::Max, num(100)}}, kEventChange),
         node(4, 1, Widget::Checkbox, {{Prop::Text, str("c")}}),
         node(5, 1, Widget::Button, {{Prop::Text, str("quiet")}}), node(6, 1, Widget::Switch)}));
    on_ui([&] {
        lv_obj_send_event(view->object(2), LV_EVENT_CLICKED, nullptr);
        lv_obj_send_event(view->object(5), LV_EVENT_CLICKED, nullptr);  // no on_click
        lv_slider_set_value(view->object(3), 30, LV_ANIM_OFF);
        lv_obj_send_event(view->object(3), LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_add_state(view->object(4), LV_STATE_CHECKED);
        lv_obj_send_event(view->object(4), LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_add_state(view->object(6), LV_STATE_CHECKED);
        lv_obj_send_event(view->object(6), LV_EVENT_VALUE_CHANGED, nullptr);
    });
    auto events = take_events();
    ASSERT_EQ(events.size(), 4u);
    EXPECT_EQ(events[0].node, 2u);
    EXPECT_EQ(events[0].ev, UiEvent::Click);
    EXPECT_EQ(events[1].node, 3u);
    EXPECT_EQ(events[1].ev, UiEvent::Change);
    EXPECT_EQ(events[1].value, num(30));
    EXPECT_EQ(events[2].node, 4u);
    EXPECT_EQ(events[2].ev, UiEvent::Change);
    EXPECT_EQ(events[2].value, PropValue{true});
    EXPECT_EQ(events[3].node, 6u);
    EXPECT_EQ(events[3].value, PropValue{true});

    // Updates the scene sends do not echo back as events.
    apply(set(3, Prop::Value, num(70)));
    apply(set(4, Prop::Checked, PropValue{false}));
    on_ui([&] {
        EXPECT_EQ(lv_slider_get_value(view->object(3)), 70);
        EXPECT_FALSE(lv_obj_has_state(view->object(4), LV_STATE_CHECKED));
    });
    EXPECT_TRUE(take_events().empty());
}

TEST_F(SceneView, LongPressIsNotAlsoAClick) {
    start();
    apply(build({node(1, 0, Widget::Screen),
                 node(2, 1, Widget::Column, {}, kEventClick | wisp::scene::kEventLongPress)}));
    on_ui([&] {
        lv_obj_t* col = view->object(2);
        EXPECT_TRUE(lv_obj_has_flag(col, LV_OBJ_FLAG_CLICKABLE));
        lv_obj_send_event(col, LV_EVENT_PRESSED, nullptr);
        lv_obj_send_event(col, LV_EVENT_LONG_PRESSED, nullptr);
        lv_obj_send_event(col, LV_EVENT_CLICKED, nullptr);
        lv_obj_send_event(col, LV_EVENT_PRESSED, nullptr);
        lv_obj_send_event(col, LV_EVENT_CLICKED, nullptr);
    });
    auto events = take_events();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].ev, UiEvent::LongPress);
    EXPECT_EQ(events[1].ev, UiEvent::Click);
}

TEST_F(SceneView, LayoutsMapToFlexGridAndStack) {
    start("rect");
    apply(build({
        node(1, 0, Widget::Screen),
        node(2, 1, Widget::Row,
             {{Prop::Justify, str("between")}, {Prop::Align, str("end")}, {Prop::W, str("fill")}}),
        node(3, 2, Widget::Spacer),
        node(4, 1, Widget::Grid, {{Prop::Cols, num(3)}, {Prop::Align, str("center")}}),
        node(5, 4, Widget::Label),
        node(6, 4, Widget::Label),
        node(7, 4, Widget::Label),
        node(8, 4, Widget::Label, {{Prop::W, str("fill")}}),
        node(9, 1, Widget::Stack, {{Prop::Align, str("center")}}),
        node(10, 9, Widget::Label),
        node(11, 1, Widget::Column, {{Prop::W, num(200)}, {Prop::H, num(300)}}),
        node(12, 11, Widget::Label, {{Prop::W, str("fill")}, {Prop::H, str("fill")}}),
        node(13, 1, Widget::Scroll, {{Prop::Direction, str("h")}}),
    }));
    on_ui([&] {
        lv_obj_update_layout(view->screen());
        lv_obj_t* row = view->object(2);
        EXPECT_EQ(lv_obj_get_style_flex_flow(row, LV_PART_MAIN), LV_FLEX_FLOW_ROW);
        EXPECT_EQ(lv_obj_get_style_flex_main_place(row, LV_PART_MAIN), LV_FLEX_ALIGN_SPACE_BETWEEN);
        EXPECT_EQ(lv_obj_get_style_flex_cross_place(row, LV_PART_MAIN), LV_FLEX_ALIGN_END);
        EXPECT_EQ(lv_obj_get_width(row), 480);
        EXPECT_EQ(lv_obj_get_style_flex_grow(view->object(3), LV_PART_MAIN), 1);

        lv_obj_t* label8 = view->object(8);
        EXPECT_EQ(lv_obj_get_style_grid_cell_column_pos(label8, LV_PART_MAIN), 0);
        EXPECT_EQ(lv_obj_get_style_grid_cell_row_pos(label8, LV_PART_MAIN), 1);
        EXPECT_EQ(lv_obj_get_style_grid_cell_x_align(label8, LV_PART_MAIN), LV_GRID_ALIGN_STRETCH);
        lv_obj_t* label7 = view->object(7);
        EXPECT_EQ(lv_obj_get_style_grid_cell_column_pos(label7, LV_PART_MAIN), 2);
        EXPECT_EQ(lv_obj_get_style_grid_cell_x_align(label7, LV_PART_MAIN), LV_GRID_ALIGN_CENTER);

        EXPECT_EQ(lv_obj_get_style_align(view->object(10), LV_PART_MAIN), LV_ALIGN_CENTER);

        // In a column, "fill" grows down it and spans across it.
        lv_obj_t* filled = view->object(12);
        EXPECT_EQ(lv_obj_get_width(filled), 200);
        EXPECT_EQ(lv_obj_get_height(filled), 300);

        lv_obj_t* scroll = view->object(13);
        EXPECT_EQ(lv_obj_get_style_flex_flow(scroll, LV_PART_MAIN), LV_FLEX_FLOW_ROW);
        EXPECT_EQ(lv_obj_get_scroll_dir(scroll), LV_DIR_HOR);
    });

    // A child changing to "fill" later re-stretches its grid cell.
    apply(set(7, Prop::W, str("fill")));
    on_ui([&] {
        EXPECT_EQ(lv_obj_get_style_grid_cell_x_align(view->object(7), LV_PART_MAIN),
                  LV_GRID_ALIGN_STRETCH);
    });
}

TEST_F(SceneView, InputFocusShowsKeyboard) {
    start();
    apply(build(
        {node(1, 0, Widget::Screen),
         node(2, 1, Widget::Input,
              {{Prop::Text, str("ab")}, {Prop::Placeholder, str("name")}, {Prop::MaxLen, num(8)}},
              kEventChange)}));
    on_ui([&] {
        lv_obj_t* ta = view->object(2);
        ASSERT_TRUE(lv_obj_check_type(ta, &lv_textarea_class));
        EXPECT_STREQ(lv_textarea_get_text(ta), "ab");
        EXPECT_STREQ(lv_textarea_get_placeholder_text(ta), "name");
        EXPECT_EQ(lv_textarea_get_max_length(ta), 8u);
        EXPECT_EQ(layer->keyboard(), nullptr);
        lv_obj_send_event(ta, LV_EVENT_FOCUSED, nullptr);
        ASSERT_NE(layer->keyboard(), nullptr);
        EXPECT_EQ(lv_keyboard_get_textarea(layer->keyboard()), ta);
        lv_textarea_add_text(ta, "c");
    });
    auto events = take_events();
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().node, 2u);
    EXPECT_EQ(events.back().ev, UiEvent::Change);
    EXPECT_EQ(events.back().value, str("abc"));

    // Setting the text from the scene does not echo.
    apply(set(2, Prop::Text, str("xyz")));
    on_ui([&] { EXPECT_STREQ(lv_textarea_get_text(view->object(2)), "xyz"); });
    for (const auto& e : take_events()) EXPECT_NE(e.ev, UiEvent::Change);
}

// The text of a Change event is a string property's value on the scene
// side, so it has the same limit (spec 5.6): past it the event is not
// sent and the scene's log hears why.
TEST_F(SceneView, InputChangeOverTheStringLimitIsNotSent) {
    start("rect");
    apply(build({node(1, 0, Widget::Screen), node(2, 1, Widget::Input, {}, kEventChange)}));
    const std::string at_limit(wisp::scene::kMaxPropStringBytes, 'x');
    on_ui([&] { lv_textarea_set_text(view->object(2), at_limit.c_str()); });
    auto events = take_events();
    ASSERT_EQ(events.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<wisp::os::LargeString>(events[0].value));
    EXPECT_EQ(std::get<wisp::os::LargeString>(events[0].value).size(), at_limit.size());
    EXPECT_TRUE(take_warnings().empty());

    on_ui([&] { lv_textarea_add_text(view->object(2), "y"); });
    EXPECT_TRUE(take_events().empty());
    EXPECT_EQ(take_warnings(), (std::vector<std::string>{"ui.input: value too large"}));
}

TEST_F(SceneView, ScrollToAndFocusOps) {
    start("rect");
    apply(build({node(1, 0, Widget::Screen),
                 node(2, 1, Widget::List,
                      {{Prop::H, num(300)}, {Prop::W, str("fill")}, {Prop::RowH, num(56)}}),
                 node(3, 1, Widget::Scroll, {{Prop::H, num(100)}, {Prop::W, num(200)}}),
                 node(4, 3, Widget::Label, {{Prop::H, num(80)}, {Prop::Text, str("0")}}),
                 node(5, 3, Widget::Label, {{Prop::H, num(80)}, {Prop::Text, str("1")}}),
                 node(6, 3, Widget::Label, {{Prop::H, num(80)}, {Prop::Text, str("2")}}),
                 node(7, 3, Widget::Label, {{Prop::H, num(80)}, {Prop::Text, str("3")}}),
                 node(8, 1, Widget::Input)}));
    apply(ListReset{2, 200});
    apply(ScrollTo{2, 50});
    on_ui([&] {
        lv_obj_update_layout(view->screen());
        EXPECT_EQ(lv_obj_get_scroll_y(view->object(2)), 50 * 56);
    });
    // Past the end: held to the last position the list can scroll to.
    apply(ScrollTo{2, 100000});
    on_ui([&] {
        lv_obj_t* list = view->object(2);
        lv_obj_update_layout(view->screen());
        int32_t viewport = lv_obj_get_content_height(list);
        EXPECT_EQ(lv_obj_get_scroll_y(list), 200 * 56 - viewport);
    });

    apply(ScrollTo{3, 2});
    on_ui([&] {
        lv_obj_update_layout(view->screen());
        lv_area_t box, child;
        lv_obj_get_coords(view->object(3), &box);
        lv_obj_get_coords(view->object(6), &child);
        EXPECT_GE(child.y1, box.y1);
        EXPECT_LE(child.y2, box.y2);
        EXPECT_GT(lv_obj_get_scroll_y(view->object(3)), 0);
    });

    // Gone or not an input: ignored.
    apply(Focus{99});
    apply(Focus{4});
    apply(ScrollTo{99, 3});
    on_ui([&] { EXPECT_EQ(layer->keyboard(), nullptr); });
    apply(Focus{8});
    on_ui([&] {
        ASSERT_NE(layer->keyboard(), nullptr);
        EXPECT_EQ(lv_keyboard_get_textarea(layer->keyboard()), view->object(8));
        EXPECT_TRUE(lv_obj_has_state(view->object(8), LV_STATE_FOCUSED));
    });
}

TEST_F(SceneView, FontsAndColorsMapToTheme) {
    start();
    on_ui([&] {
        using wisp::scene::theme_color;
        using wisp::scene::theme_font;
        EXPECT_EQ(theme_font("xs"), wisp::ui::font(16));
        EXPECT_EQ(theme_font("sm"), wisp::ui::font(16));
        EXPECT_EQ(theme_font("md"), wisp::ui::font(16));
        EXPECT_EQ(theme_font("lg"), wisp::ui::font(24));
        EXPECT_EQ(theme_font("xl"), wisp::ui::font(32));
        EXPECT_EQ(theme_font("xxl"), wisp::ui::font(48));
        EXPECT_TRUE(lv_color_eq(theme_color("accent"), lv_color_hex(0x4F9DFF)));
        EXPECT_TRUE(lv_color_eq(theme_color("text"), lv_color_hex(0xFFFFFF)));
        EXPECT_TRUE(lv_color_eq(theme_color("muted"), lv_color_hex(0x8E8E93)));
        EXPECT_TRUE(lv_color_eq(theme_color("surface"), lv_color_hex(0x1C1C1E)));
        EXPECT_TRUE(lv_color_eq(theme_color("bg"), lv_color_hex(0x000000)));
        EXPECT_TRUE(lv_color_eq(theme_color("danger"), lv_color_hex(0xFF453A)));
        EXPECT_TRUE(lv_color_eq(theme_color("ok"), lv_color_hex(0x32D74B)));
        EXPECT_TRUE(lv_color_eq(theme_color("warn"), lv_color_hex(0xFFD60A)));
        EXPECT_TRUE(lv_color_eq(theme_color("#12AbEf"), lv_color_hex(0x12ABEF)));
    });
    apply(build(
        {node(1, 0, Widget::Screen),
         node(2, 1, Widget::Label,
              {{Prop::Fg, str("danger")}, {Prop::Bg, str("#102030")}, {Prop::Font, str("lg")}})}));
    on_ui([&] {
        lv_obj_t* label = view->object(2);
        EXPECT_TRUE(
            lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN), lv_color_hex(0xFF453A)));
        EXPECT_TRUE(
            lv_color_eq(lv_obj_get_style_bg_color(label, LV_PART_MAIN), lv_color_hex(0x102030)));
        EXPECT_EQ(lv_obj_get_style_bg_opa(label, LV_PART_MAIN), LV_OPA_COVER);
        EXPECT_EQ(lv_obj_get_style_text_font(label, LV_PART_MAIN), wisp::ui::font(24));
    });
}

}  // namespace
