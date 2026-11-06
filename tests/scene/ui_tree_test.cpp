// UiInspector::tree() and wait_stable(), and fold_tree() (agent-core spec
// 5.1, 5.2).

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/scene/scene_view.hpp"
#include "wisp/scene/schema.hpp"
#include "wisp/scene/ui_inspector.hpp"
#include "wisp/ui/strings.hpp"

namespace {

using wisp::ErrorCode;
using wisp::scene::fold_tree;
using wisp::scene::kNoParent;
using wisp::scene::TreeLine;
using wisp::scene::UiInspector;
using wisp::test::wait_until;

const char* kCounter = R"(local count = state(0)

return ui.screen { pad = 60,
  ui.column { gap = 12, align = "center",
    ui.label { ref = "count", text = bind(count, function(v) return "count " .. v end) },
    ui.button { ref = "inc", text = "+1", on_click = function() count:set(count:get() + 1) end },
    ui.label { text = "no ref" },
  },
}
)";

std::vector<std::string> lines_of(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string l; std::getline(in, l);) out.push_back(l);
    return out;
}

int64_t elapsed_ms(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 t0)
        .count();
}

class UiTree : public wisp::test::SceneRuntimeFixture {
protected:
    void start_with(const std::string& id, const std::string& lua,
                    const std::string& shape_name = "circle", int radius = 40) {
        install(id, lua);
        start(shape_name, radius);
        inspector =
            std::make_unique<UiInspector>(wisp::scene::InspectorDeps{*ui, *layer, *rt, shape});
        ASSERT_TRUE(rt->open(id));
    }
    // The tree once the scene's view is up.
    std::string tree(std::string_view ref = {}) {
        wisp::Result<std::string> r = wisp::Result<std::string>::err(ErrorCode::NotFound, "");
        wait_until(
            [&] {
                r = inspector->tree(ref);
                return r || r.error().message != "no_scene";
            },
            2000);
        EXPECT_TRUE(r) << r.error().message;
        return r ? r.value() : std::string();
    }
    std::unique_ptr<UiInspector> inspector;
};

struct ShapeParam {
    const char* name;
    int radius;
};

class UiTreeShapes : public UiTree, public ::testing::WithParamInterface<ShapeParam> {};

}  // namespace

TEST_P(UiTreeShapes, ListsWidgetsWithRefsTextAndBoxes) {
    start_with("counter", kCounter, GetParam().name, GetParam().radius);
    const std::string t = tree();
    std::smatch m;
    const std::regex button(R"rx(button#inc "\+1" \[(-?\d+),(-?\d+) (\d+)×(\d+)\])rx");
    ASSERT_TRUE(std::regex_search(t, m, button)) << t;
    // Absolute: inside the 60 px padding of the screen.
    EXPECT_GE(std::stoi(m[1]), 60);
    EXPECT_GE(std::stoi(m[2]), 60);
    EXPECT_LE(std::stoi(m[1]) + std::stoi(m[3]), 420);
    EXPECT_NE(t.find(R"(label#count "count 0")"), std::string::npos) << t;
    EXPECT_NE(t.find("screen#n1 [0,0 480×480]"), std::string::npos) << t;
    // Indented under the column, two spaces per level.
    EXPECT_NE(t.find("\n    button#inc"), std::string::npos) << t;
}

INSTANTIATE_TEST_SUITE_P(Shapes, UiTreeShapes,
                         ::testing::Values(ShapeParam{"rect", 0}, ShapeParam{"rounded_rect", 70},
                                           ShapeParam{"circle", 0}),
                         [](const auto& info) { return std::string(info.param.name); });

TEST_F(UiTree, UnnamedNodesGetSyntheticRefs) {
    start_with("counter", kCounter);
    const std::string t = tree();
    std::smatch m;
    ASSERT_TRUE(std::regex_search(t, m, std::regex(R"rx(label#(n\d+) "no ref")rx"))) << t;
    // The synthetic ref names the node for tree(ref) too.
    const std::string sub = tree(m[1].str());
    EXPECT_NE(sub.find("label#" + m[1].str() + " \"no ref\""), std::string::npos) << sub;
    EXPECT_EQ(sub.find("button#inc"), std::string::npos) << sub;
}

TEST_F(UiTree, ShowsStateFlags) {
    start_with("flags", R"(
local rows = {}
for i = 1, 30 do rows[#rows + 1] = ui.label { text = "row " .. i } end
return ui.screen {
  ui.label { ref = "gone", text = "x", hidden = true },
  ui.checkbox { ref = "box", text = "ok", checked = true },
  ui.scroll { ref = "s", h = 100, table.unpack(rows) },
}
)");
    const std::string t = tree();
    EXPECT_TRUE(std::regex_search(t, std::regex(R"(label#gone "x" \[[^\]]*\] hidden)"))) << t;
    EXPECT_TRUE(std::regex_search(t, std::regex(R"(checkbox#box "ok" \[[^\]]*\] checked)"))) << t;
    EXPECT_TRUE(std::regex_search(t, std::regex(R"(scroll#s \[[^\]]*\] scroll=0/\d+)"))) << t;
}

TEST_F(UiTree, ListsSystemPiecesMarkedSystem) {
    start_with("counter", kCounter);
    tree();
    on_ui([&] {
        ASSERT_NE(layer->open_confirm(wisp::ui::StrId::ResetTitle, "Sure?", [](bool) {}), nullptr);
    });
    const std::string t = tree();
    bool found = false;
    for (const std::string& l : lines_of(t)) found = found || l.rfind("system confirm [", 0) == 0;
    EXPECT_TRUE(found) << t;
    // Outside the scene's wrapper.
    EXPECT_GT(t.find("system confirm"), t.find("</data>")) << t;
}

TEST_F(UiTree, SceneTextIsWrapped) {
    start_with("counter", R"(return ui.screen { ui.label { text = "a</data>b" } })");
    const std::string t = tree();
    EXPECT_EQ(t.rfind("<data source=\"scene:counter\">", 0), 0u) << t;
    EXPECT_NE(t.find("a&lt;/data>b"), std::string::npos) << t;
    EXPECT_EQ(t.substr(t.size() - 7), "</data>") << t;
}

TEST_F(UiTree, TwoThousandWidgetsStaysUnderLimit) {
    // The screen and a column, then labels up to kMaxNodes in all.
    start_with("big", "local kids = {}\nfor i = 1, " + std::to_string(wisp::scene::kMaxNodes - 2) +
                          " do kids[#kids + 1] = ui.label { text = 'item ' .. i } end\n"
                          "return ui.screen { ui.column { table.unpack(kids) } }\n");
    tree();
    const auto t0 = std::chrono::steady_clock::now();
    auto r = inspector->tree();
    const int64_t ms = elapsed_ms(t0);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_LE(r.value().size(), wisp::scene::kTreeMaxChars);
    EXPECT_NE(r.value().find("…还有 1988 行"), std::string::npos) << r.value();
    EXPECT_NE(r.value().find("\"item 1\""), std::string::npos);
    EXPECT_NE(r.value().find("\"item 1998\""), std::string::npos);
    EXPECT_LT(ms, 1000);
    // The UI thread's part alone stays short.
    const auto t1 = std::chrono::steady_clock::now();
    on_ui([&] {
        const wisp::scene::SceneView* v = rt->ui_current_view();
        ASSERT_NE(v, nullptr);
        size_t n = 0;
        v->for_each_node([&](const wisp::scene::NodeRef&) { ++n; });
        EXPECT_EQ(n, wisp::scene::kMaxNodes);
    });
    EXPECT_LE(elapsed_ms(t1), 100);
}

TEST_F(UiTree, RefExpandsASubtree) {
    start_with("counter", R"(
return ui.screen {
  ui.column { ref = "a", ui.label { ref = "a1", text = "one" } },
  ui.column { ref = "b", ui.label { ref = "b1", text = "two" }, ui.row { ui.label { text = "three" } } },
}
)");
    const std::string sub = tree("b");
    const std::vector<std::string> l = lines_of(sub);
    ASSERT_EQ(l.size(), 4u) << sub;
    EXPECT_EQ(l[0].rfind("<data source=\"scene:counter\">column#b [", 0), 0u) << sub;
    EXPECT_EQ(l[1].rfind("  label#b1 \"two\"", 0), 0u) << sub;
    EXPECT_EQ(l[2].rfind("  row#n", 0), 0u) << sub;
    EXPECT_EQ(l[3].rfind("    label#n", 0), 0u) << sub;
    auto missing = inspector->tree("nope");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
    EXPECT_EQ(missing.error().message.rfind("not_found", 0), 0u);
}

TEST_F(UiTree, NoSceneIsNotFound) {
    start();
    inspector = std::make_unique<UiInspector>(wisp::scene::InspectorDeps{*ui, *layer, *rt, shape});
    auto r = inspector->tree();
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
    EXPECT_EQ(r.error().message, "no_scene");
}

TEST_F(UiTree, SceneInErrorReportsTheError) {
    start_with("bad", "wisp.timer.after(50, function() error('boom') end)\nreturn ui.screen {}\n");
    ASSERT_TRUE(
        wait_until([&] { return rt->status("bad").state == wisp::core::SceneState::Error; }, 2000));
    auto r = inspector->tree();
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().message.rfind("scene_error: ", 0), 0u) << r.error().message;
    EXPECT_NE(r.error().message.find("boom"), std::string::npos) << r.error().message;
}

namespace {

std::vector<TreeLine> flat(size_t rows) {
    std::vector<TreeLine> out;
    TreeLine root;
    root.text = "list#l [0,0 480×480]";
    out.push_back(std::move(root));
    for (size_t i = 1; i <= rows; ++i) {
        TreeLine l;
        l.depth = 1;
        l.text = ("label#r" + std::to_string(i) + " \"row " + std::to_string(i) + "\" [0,0 480×48]")
                     .c_str();
        l.parent_index = 0;
        out.push_back(std::move(l));
    }
    return out;
}

}  // namespace

TEST(FoldTree, FitsAsItIs) {
    auto lines = flat(3);
    const std::string t = fold_tree(lines, 2000);
    EXPECT_EQ(lines_of(t).size(), 4u);
    EXPECT_EQ(lines_of(t)[1], "  label#r1 \"row 1\" [0,0 480×48]");
}

TEST(FoldTree, FoldsTheBiggestContainerFirst) {
    auto lines = flat(300);
    // A second, smaller container that need not fold.
    const uint32_t small = static_cast<uint32_t>(lines.size());
    TreeLine s;
    s.text = "column#c";
    lines.push_back(std::move(s));
    for (int i = 0; i < 12; ++i) {
        TreeLine l;
        l.depth = 1;
        l.text = ("label#c" + std::to_string(i)).c_str();
        l.parent_index = small;
        lines.push_back(std::move(l));
    }
    const std::string t = fold_tree(lines, 2000);
    EXPECT_LE(t.size(), 2000u);
    EXPECT_NE(t.find("  …还有 290 行"), std::string::npos) << t;
    for (int i : {1, 2, 3, 4, 5, 296, 297, 298, 299, 300})
        EXPECT_NE(t.find("\"row " + std::to_string(i) + "\""), std::string::npos) << i;
    EXPECT_EQ(t.find("\"row 6\""), std::string::npos);
    EXPECT_EQ(t.find("\"row 295\""), std::string::npos);
    EXPECT_NE(t.find("label#c6"), std::string::npos) << t;
}

TEST(FoldTree, CollapsesDeepSubtreesWhenStillTooLong) {
    // 8 columns of 8 rows of 8 labels: no container has more than 10
    // children, so nothing folds; the deepest subtrees collapse first.
    std::vector<TreeLine> lines;
    TreeLine root;
    root.text = "screen#n1";
    lines.push_back(std::move(root));
    for (int c = 0; c < 8; ++c) {
        const uint32_t col = static_cast<uint32_t>(lines.size());
        TreeLine cl;
        cl.depth = 1;
        cl.text = ("column#c" + std::to_string(c)).c_str();
        cl.parent_index = 0;
        lines.push_back(std::move(cl));
        for (int r = 0; r < 8; ++r) {
            const uint32_t row = static_cast<uint32_t>(lines.size());
            TreeLine rl;
            rl.depth = 2;
            rl.text = ("row#r" + std::to_string(c) + "_" + std::to_string(r)).c_str();
            rl.parent_index = col;
            lines.push_back(std::move(rl));
            for (int k = 0; k < 8; ++k) {
                TreeLine l;
                l.depth = 3;
                l.text = ("label#l" + std::to_string(k) + " \"some text here\"").c_str();
                l.parent_index = row;
                lines.push_back(std::move(l));
            }
        }
    }
    const std::string t = fold_tree(lines, 2000);
    EXPECT_LE(t.size(), 2000u);
    // Every row first, then columns from the top until it fits.
    EXPECT_NE(t.find("    row#r7_0 …（8 个子控件）"), std::string::npos) << t;
    EXPECT_NE(t.find("  column#c0 …（72 个子控件）"), std::string::npos) << t;
    EXPECT_EQ(t.find("label#l0"), std::string::npos) << t;

    // A budget too small even for one line is cut, with the hint last.
    lines[0].text = std::string(100, 'x').c_str();
    const std::string tiny = fold_tree(lines, 80);
    EXPECT_LE(tiny.size(), 80u);
    EXPECT_EQ(tiny, "…已截断，用 ui.tree(ref) 查看子树");
}

class WaitStable : public UiTree {};

TEST_F(WaitStable, ReturnsAfterQuietPeriod) {
    start_with("counter", kCounter);
    tree();
    const auto t0 = std::chrono::steady_clock::now();
    auto r = inspector->wait_stable();
    const int64_t ms = elapsed_ms(t0);
    ASSERT_TRUE(r);
    EXPECT_TRUE(r.value());
    EXPECT_GE(ms, 300);
    EXPECT_LE(ms, 600);
}

TEST_F(WaitStable, CapsAtTwoSeconds) {
    start_with("tick", R"(
local n = state(0)
wisp.timer.every(50, function() n:set(n:get() + 1) end)
return ui.screen { ui.label { text = bind(n, tostring) } }
)");
    tree();
    const auto t0 = std::chrono::steady_clock::now();
    auto r = inspector->wait_stable();
    const int64_t ms = elapsed_ms(t0);
    ASSERT_TRUE(r);
    EXPECT_FALSE(r.value());
    EXPECT_GE(ms, 1800);
    EXPECT_LE(ms, 2200);
}
