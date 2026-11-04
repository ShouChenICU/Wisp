// Virtualized ui.list: only the rows around the viewport exist, on the
// scene task (scopes, nodes) and in the view (objects) alike
// (scene-runtime-core spec 4.3).

#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include "list_window.hpp"
#include "support/scene_harness.hpp"
#include "wisp/core/log.hpp"

namespace {

using wisp::scene::ListReset;
using wisp::scene::ListRows;
using wisp::scene::ListWindow;
using wisp::scene::NodeId;
using wisp::scene::Prop;
using wisp::scene::push_node_prop;
using wisp::scene::UiEvent;
using wisp::scene::visible_rows;
using wisp::test::describe;
using wisp::test::RecordedRange;

class SceneViewList : public wisp::test::SceneHarness {
protected:
    static constexpr NodeId kList = 2;  // screen 1, list 2

    lv_obj_t* list_obj() const { return view->object(kList); }

    // The list's rows (its children but the filler), on the UI thread.
    size_t rows_built() {
        size_t n = 0;
        on_ui([&] { n = lv_obj_get_child_count(list_obj()) - 1; });
        return n;
    }

    int32_t viewport() {
        int32_t h = 0;
        on_ui([&] {
            lv_obj_update_layout(view->screen());
            h = lv_obj_get_content_height(list_obj());
        });
        return h;
    }

    // The node of the row object at `index`, or 0.
    NodeId row_node(uint32_t index) {
        NodeId found = 0;
        on_ui([&] {
            lv_obj_update_layout(view->screen());
            for (NodeId id = kList + 1; id < next_id && found == 0; ++id) {
                lv_obj_t* obj = view->object(id);
                if (obj != nullptr && lv_obj_get_parent(obj) == list_obj() &&
                    lv_obj_get_y(obj) == static_cast<int32_t>(index) * 56)
                    found = id;
            }
        });
        return found;
    }

    std::string label_of(NodeId row) {
        std::string text;
        on_ui([&] {
            lv_obj_t* label = wisp::test::find_obj_of_class(view->object(row), &lv_label_class);
            if (label) text = lv_label_get_text(label);
        });
        return text;
    }

    bool has_click_handler(NodeId node) {
        lua_State* L = vm->L();
        bool found = push_node_prop(L, node, Prop::OnClick);
        lua_pop(L, 1);
        return found;
    }
};

constexpr const char* kTwoHundredRows = R"(
    local t = {}
    for i = 1, 200 do t[i] = "item " .. i end
    _G.items = state(t)
    return ui.screen {
      ui.list { items = _G.items, row_h = 56, row = function(item, i)
        return ui.row { ui.label { text = item }, ui.button { text = i, on_click = function() end } }
      end },
    }
)";

TEST_F(SceneViewList, OnlyVisibleRowsMaterialized) {
    start("rect");
    auto err = open(kTwoHundredRows);
    ASSERT_FALSE(err) << describe(err);
    const int32_t h = viewport();
    ASSERT_EQ(h, 480);

    ListWindow w = visible_rows(0, h, 56, 200);
    ASSERT_FALSE(all_ranges.empty());
    EXPECT_EQ(all_ranges.back(), (RecordedRange{kList, w.first, w.count}));
    EXPECT_EQ(rows_built(), w.count);
    // screen and list, then three nodes per row.
    EXPECT_EQ(view->object_count(), 2 + w.count * 3);
    on_ui([&] {
        lv_obj_update_layout(view->screen());
        // The content is as tall as every row together.
        EXPECT_EQ(lv_obj_get_scroll_bottom(list_obj()), 200 * 56 - h);
    });
    NodeId first = row_node(0);
    ASSERT_NE(first, 0u);
    EXPECT_EQ(label_of(first), "item 1");
    on_ui([&] {
        EXPECT_EQ(lv_obj_get_width(view->object(first)), lv_obj_get_content_width(list_obj()));
        EXPECT_EQ(lv_obj_get_height(view->object(first)), 56);
    });

    // Scrolling builds the rows that come into view and drops the rest.
    all_ranges.clear();
    on_ui([&] { lv_obj_scroll_to_y(list_obj(), 100 * 56, LV_ANIM_OFF); });
    err = pump();
    ASSERT_FALSE(err) << describe(err);
    w = visible_rows(100 * 56, h, 56, 200);
    ASSERT_FALSE(all_ranges.empty());
    EXPECT_EQ(all_ranges.back(), (RecordedRange{kList, w.first, w.count}));
    EXPECT_EQ(rows_built(), w.count);
    EXPECT_EQ(view->object_count(), 2 + w.count * 3);
    EXPECT_EQ(view->object(first), nullptr);
    EXPECT_FALSE(has_click_handler(first + 2));
    NodeId hundredth = row_node(100);
    ASSERT_NE(hundredth, 0u);
    EXPECT_EQ(label_of(hundredth), "item 101");
    EXPECT_TRUE(has_click_handler(hundredth + 2));
}

TEST_F(SceneViewList, ItemsReplacedWhileScrolledClampsScroll) {
    start("rect");
    auto err = open(kTwoHundredRows);
    ASSERT_FALSE(err) << describe(err);
    const int32_t h = viewport();
    on_ui([&] { lv_obj_scroll_to_y(list_obj(), 200 * 56, LV_ANIM_OFF); });
    err = pump();
    ASSERT_FALSE(err) << describe(err);
    on_ui([&] { EXPECT_EQ(lv_obj_get_scroll_y(list_obj()), 200 * 56 - h); });
    NodeId last = row_node(199);
    ASSERT_NE(last, 0u);
    const NodeId button = last + 2;
    EXPECT_TRUE(has_click_handler(button));

    all_ranges.clear();
    err = message("_G.items:set({'a', 'b', 'c', 'd', 'e'})");
    ASSERT_FALSE(err) << describe(err);
    on_ui([&] {
        lv_obj_update_layout(view->screen());
        EXPECT_EQ(lv_obj_get_scroll_y(list_obj()), 0);
    });
    ASSERT_FALSE(all_ranges.empty());
    EXPECT_EQ(all_ranges.back(), (RecordedRange{kList, 0, 5}));
    EXPECT_EQ(rows_built(), 5u);
    EXPECT_EQ(view->object_count(), 2u + 5 * 3);
    EXPECT_EQ(label_of(row_node(4)), "e");
    // The rows built for the old items are gone on both sides: a click
    // the UI reported before the reset finds no handler and is dropped.
    EXPECT_EQ(view->object(button), nullptr);
    EXPECT_FALSE(has_click_handler(button));
    EXPECT_TRUE(has_click_handler(row_node(0) + 2));
}

TEST_F(SceneViewList, RowEventsCarryTheRowNode) {
    start("rect");
    auto err = open(kTwoHundredRows);
    ASSERT_FALSE(err) << describe(err);
    NodeId row = row_node(2);
    ASSERT_NE(row, 0u);
    on_ui([&] { lv_obj_send_event(view->object(row + 2), LV_EVENT_CLICKED, nullptr); });
    auto events = take_events();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].node, row + 2);
    EXPECT_EQ(events[0].ev, UiEvent::Click);
}

// After the patch queue dropped ops, refresh_all() resets every list, so
// the view asks for its rows again and gets fresh ones.
TEST_F(SceneViewList, RefreshRebuildsRows) {
    start("rect");
    auto err = open(kTwoHundredRows);
    ASSERT_FALSE(err) << describe(err);
    NodeId before = row_node(0);
    const size_t built = rows_built();
    all_ranges.clear();
    err = message("local W = ... W.refresh_all()");
    ASSERT_FALSE(err) << describe(err);
    EXPECT_FALSE(all_ranges.empty());
    EXPECT_EQ(rows_built(), built);
    NodeId after = row_node(0);
    EXPECT_NE(after, 0u);
    EXPECT_NE(after, before);
    EXPECT_EQ(label_of(after), "item 1");
}

// Rows that arrive for indexes the view no longer shows are not built.
TEST_F(SceneViewList, StaleRowsOutsideTheWindowAreDropped) {
    start("rect");
    auto err = open(kTwoHundredRows);
    ASSERT_FALSE(err) << describe(err);
    const size_t before = view->object_count();
    ListRows stale;
    stale.list = kList;
    stale.first = 150;
    stale.rows.emplace_back();
    stale.rows.back().push_back(wisp::test::node(5000, kList, wisp::scene::Widget::Label,
                                                 {{Prop::Text, wisp::test::str("x")}}));
    on_ui([&] { view->apply(stale); });
    EXPECT_EQ(view->object_count(), before);
    EXPECT_EQ(view->object(5000), nullptr);
}

TEST_F(SceneViewList, RowErrorsPointAtTheScene) {
    start("rect");
    auto err = open(R"(
        return ui.screen {
          ui.list { items = {1}, row = function() return 42 end },
        }
    )");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->text(), "main.lua:3: ui.list: row must return a widget (got number)");

    vm.reset();
    batch.clear();
    err = open(R"(
        return ui.screen {
          ui.list { items = {1}, row = function()
            return ui.label { ref = "x" }
          end },
        }
    )");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->text(), "main.lua:4: ui.label: ref cannot be used inside a list row");
}

// Rows of canvases and charts: every row built and dropped while the list
// scrolls takes its widget effects with it, so neither the effects the
// prelude keeps nor the VM's memory grow with the number of scrolls.
TEST_F(SceneViewList, RecycledRowsReleaseTheirWidgetEffects) {
    start("rect");
    auto err = open(R"(
        local t = {}
        for i = 1, 400 do t[i] = i end
        return ui.screen {
          ui.list { items = t, row_h = 40, row = function(item)
            return ui.row {
              ui.canvas { w = 30, h = 30, draw = function(g) g:circle(15, 15, item % 15) end },
              ui.chart { w = 60, h = 30, series = { { item, item + 1, item + 2 } } },
            }
          end },
        }
    )");
    ASSERT_FALSE(err) << describe(err);
    const char* count = R"(
        local W = ...
        _G.effects = W.widget_effects()
        _G.kb = math.floor(collectgarbage("count"))
    )";
    auto scroll = [&](int32_t y) {
        on_ui([&] { lv_obj_scroll_to_y(list_obj(), y, LV_ANIM_OFF); });
        auto e = pump();
        ASSERT_FALSE(e) << describe(e);
    };
    scroll(200 * 40);
    scroll(0);
    lua_gc(vm->L(), LUA_GCCOLLECT);
    ASSERT_FALSE(message(count));
    const int64_t effects = global_int("effects");
    const int64_t kb = global_int("kb");
    // One effect per canvas and chart in the window.
    EXPECT_GT(effects, 0);
    EXPECT_LE(effects, 2 * static_cast<int64_t>(rows_built()) + 1);  // and the list's
    for (int i = 0; i < 30; ++i) {
        scroll(((i * 7) % 10) * 40 * 40);
        scroll(0);
    }
    lua_gc(vm->L(), LUA_GCCOLLECT);
    ASSERT_FALSE(message(count));
    EXPECT_EQ(global_int("effects"), effects);
    EXPECT_LE(global_int("kb"), kb + 16);
}

// The 2000-widget limit (spec 4.5) covers list rows too, across every
// request for rows: rows beyond it are not built, and the scene's log
// says so.
TEST_F(SceneViewList, RowsBeyondTheWidgetLimitAreNotBuilt) {
    struct Sink : wisp::LogSink {
        std::mutex m;
        std::vector<std::string> lines;
        void write(const wisp::LogRecord& r) override {
            std::lock_guard<std::mutex> lock(m);
            if (r.level == wisp::LogLevel::Warn) lines.emplace_back(r.msg);
        }
    } sink;
    wisp::log::add_sink(&sink);
    start("rect");
    // screen, stack, 1970 labels and the list: 1973 nodes, room for 27
    // more, nine rows of three.
    auto err = open(R"(
        local labels = {}
        for i = 1, 1970 do labels[i] = ui.label { text = "x" } end
        local t = {}
        for i = 1, 100 do t[i] = i end
        return ui.screen {
          ui.stack(labels),
          ui.list { items = t, row_h = 30, row = function(item)
            return ui.row { ui.label { text = item }, ui.button { text = "b" } }
          end },
        }
    )");
    wisp::log::remove_sink(&sink);
    ASSERT_FALSE(err) << describe(err);
    constexpr NodeId kBigList = 1973;
    auto rows = [&] {
        size_t n = 0;
        on_ui([&] { n = lv_obj_get_child_count(view->object(kBigList)) - 1; });
        return n;
    };
    EXPECT_EQ(rows(), 9u);
    EXPECT_LE(view->object_count(), wisp::scene::kMaxNodes);
    bool warned = false;
    for (const auto& line : sink.lines)
        if (line.find("ui.list: widget limit 2000 reached") != std::string::npos) warned = true;
    EXPECT_TRUE(warned);

    // Scrolling keeps the total within the limit.
    on_ui([&] { lv_obj_scroll_to_y(view->object(kBigList), 50 * 30, LV_ANIM_OFF); });
    err = pump();
    ASSERT_FALSE(err) << describe(err);
    EXPECT_EQ(rows(), 9u);
    EXPECT_LE(view->object_count(), wisp::scene::kMaxNodes);
}

}  // namespace
