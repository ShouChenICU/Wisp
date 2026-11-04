#include "wisp/scene/schema.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>

#include "wisp/scene/patch_batch.hpp"
#include "wisp/scene/reactive.hpp"
#include "wisp/scene/vm.hpp"

using wisp::os::LargeString;
using wisp::scene::Build;
using wisp::scene::collect_tree;
using wisp::scene::find_widget;
using wisp::scene::flush_reactive;
using wisp::scene::Focus;
using wisp::scene::install_prelude;
using wisp::scene::install_ui;
using wisp::scene::kEventChange;
using wisp::scene::kEventClick;
using wisp::scene::kMaxNodes;
using wisp::scene::nearest_name;
using wisp::scene::NodeDesc;
using wisp::scene::NodeId;
using wisp::scene::PatchBatch;
using wisp::scene::Prop;
using wisp::scene::prop_spec;
using wisp::scene::PropId;
using wisp::scene::PropValue;
using wisp::scene::push_node_prop;
using wisp::scene::push_wisp_internal;
using wisp::scene::schema_markdown;
using wisp::scene::ScriptError;
using wisp::scene::ScriptFault;
using wisp::scene::ScrollTo;
using wisp::scene::SetProp;
using wisp::scene::UiBatch;
using wisp::scene::Vm;
using wisp::scene::Widget;
using wisp::scene::WidgetId;

namespace {

std::string describe(const std::optional<ScriptError>& err) {
    return err ? err->text() : std::string("(no error)");
}

// A scene VM with the prelude and the ui table installed. build() runs
// main.lua the way opening a scene does: call the entry file, collect the
// tree it returns. message() runs one more chunk and flushes, like the
// handling of one message. Both pass the internal table as `...`.
struct Scene {
    PatchBatch batch;
    std::unique_ptr<Vm> vm;
    std::optional<ScriptError> installed;
    NodeId next_id = 1;
    Build tree;

    Scene() {
        vm = Vm::create("t");
        if (!vm) {
            installed = ScriptError{ScriptFault::Memory, "", 0, "no vm"};
            return;
        }
        installed = install_prelude(*vm, batch);
        if (!installed) installed = install_ui(*vm);
    }

    std::optional<ScriptError> build(std::string_view src) {
        if (auto err = vm->load("main.lua", src)) return err;
        if (!push_wisp_internal(vm->L())) lua_pushnil(vm->L());
        if (auto err = vm->call(1, 1)) return err;
        int top = lua_gettop(vm->L());
        auto err = collect_tree(*vm, -1, next_id, tree);
        EXPECT_EQ(lua_gettop(vm->L()), top) << "collect_tree left the stack changed";
        lua_pop(vm->L(), 1);
        return err;
    }

    std::optional<ScriptError> message(std::string_view src) {
        if (auto err = vm->load("main.lua", src)) return err;
        if (!push_wisp_internal(vm->L())) lua_pushnil(vm->L());
        if (auto err = vm->call(1, 0)) return err;
        return flush_reactive(*vm);
    }

    const NodeDesc& node(size_t i) const { return tree.nodes.at(i); }
};

const PropValue* prop_of(const NodeDesc& n, Prop p) {
    for (const auto& [id, value] : n.props)
        if (id == static_cast<PropId>(p)) return &value;
    return nullptr;
}

std::string str_of(const PropValue* v) {
    if (!v) return "(absent)";
    const auto* s = std::get_if<LargeString>(v);
    return s ? std::string(s->data(), s->size()) : std::string("(not a string)");
}

WidgetId wid(Widget w) { return static_cast<WidgetId>(w); }

// Expects `src` to fail while building with exactly `text`.
void expect_build_error(std::string_view src, std::string_view text) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.build(src);
    ASSERT_TRUE(err) << "expected: " << text;
    EXPECT_EQ(err->text(), text);
    EXPECT_TRUE(s.tree.nodes.empty());
}

void expect_build_ok(std::string_view src) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.build(src);
    EXPECT_FALSE(err) << describe(err) << "\nsource: " << src;
}

}  // namespace

TEST(Schema, NearestName) {
    std::string_view names[] = {"text", "wrap", "w", "h"};
    EXPECT_EQ(nearest_name("txt", names), "text");
    // "ab" is one edit away from both; the lexicographically first wins.
    std::string_view tie[] = {"ac", "ab"};
    EXPECT_EQ(nearest_name("aa", tie), "ab");
    EXPECT_EQ(nearest_name("x", {}), "");
    ASSERT_NE(find_widget("label"), nullptr);
    EXPECT_EQ(find_widget("label")->widget, Widget::Label);
    EXPECT_EQ(find_widget("ui.label"), nullptr);
    EXPECT_EQ(prop_spec(Prop::MaxLen).name, "max_len");
    EXPECT_EQ(prop_spec(Prop::OnLongPress).name, "on_long_press");
}

TEST(Schema, UnknownWidgetSuggestsNearest) {
    expect_build_error("\n\n\nreturn ui.lable{}",
                       "main.lua:4: unknown widget 'ui.lable' (did you mean 'ui.label'?)");
}

TEST(Schema, UnknownPropSuggestsNearest) {
    expect_build_error("return ui.screen{\n ui.label{txt = 'a'} }",
                       "main.lua:2: ui.label: unknown property 'txt' (did you mean 'text'?)");
    // Common to every widget, but a property of other widgets is unknown
    // here.
    expect_build_error("return ui.screen{ ui.spacer{text = 'a'} }",
                       "main.lua:1: ui.spacer: unknown property 'text' (did you mean 'font'?)");
}

TEST(Schema, TypeMismatchNamesExpectedType) {
    expect_build_error("\n\n\n\n\n\nreturn ui.screen{ ui.slider{max = 'ten'} }",
                       "main.lua:7: ui.slider: property 'max' expects number, got string");
    expect_build_error("return ui.screen{ ui.label{text = {}} }",
                       "main.lua:1: ui.label: property 'text' expects string, got table");
    expect_build_error(
        "return ui.screen{ ui.label{font = 'huge'} }",
        "main.lua:1: ui.label: property 'font' expects one of xs, sm, md, lg, xl, xxl, got 'huge'");
    expect_build_error("return ui.screen{ ui.grid{cols = 1.5} }",
                       "main.lua:1: ui.grid: property 'cols' expects integer >= 1, got 1.5");
    expect_build_error("return ui.screen{ ui.label{w = 'wide'} }",
                       "main.lua:1: ui.label: property 'w' expects pixels, \"fill\" or "
                       "\"content\", got 'wide'");
    expect_build_error("return ui.screen{ ui.label{1} }",
                       "main.lua:1: ui.label: takes no children");
    expect_build_error("return ui.screen{ ui.column{ {} } }",
                       "main.lua:1: ui.column: child 1 is not a widget (got table)");
    expect_build_error("return ui.screen{ ui.image{src = '../x.png'} }",
                       "main.lua:1: ui.image: property 'src' expects a path assets/<name>.png, "
                       "got '../x.png'");
    expect_build_error("return ui.screen{ ui.icon{name = 'hom'} }",
                       "main.lua:1: ui.icon: property 'name' expects an icon name, got 'hom' "
                       "(did you mean 'home'?)");
    expect_build_ok(
        "return ui.screen{ ui.column{w = 'fill', h = 120.0, grow = 1, align = 'center', "
        "justify = 'between', gap = 8, radius = 4, border = 1, opa = 0.5, hidden = false},"
        " ui.label{text = 12, wrap = true, font = 'xxl'}, ui.icon{name = 'home'},"
        " ui.image{src = 'assets/a/b.png'}, ui.scroll{direction = 'h'},"
        " ui.chart{type = 'bar', series = {{1, 2}, {3}}, min = 0, max = 10},"
        " ui.arc{value = 1, min = 0, max = 2, width = 6}, ui.bar{value = 0.5},"
        " ui.input{text = 'a', placeholder = 'b', password = true, max_len = 8},"
        " ui.canvas{draw = function(g) end}, ui.button{text = 'go', ui.label{}} }");
}

TEST(Schema, ColorAcceptsHexAndTokens) {
    expect_build_ok("return ui.screen{ ui.label{fg = '#12AB34', bg = 'accent'} }");
    expect_build_ok("return ui.screen{ ui.label{fg = '#12ab34', bg = 'warn'} }");
    expect_build_error(
        "return ui.screen{ ui.label{fg = '#12AB3'} }",
        "main.lua:1: ui.label: property 'fg' expects \"#RRGGBB\" or one of accent, text, muted, "
        "surface, bg, danger, ok, warn, got '#12AB3'");
    expect_build_error(
        "return ui.screen{ ui.label{bg = 'blue'} }",
        "main.lua:1: ui.label: property 'bg' expects \"#RRGGBB\" or one of accent, text, muted, "
        "surface, bg, danger, ok, warn, got 'blue'");
    expect_build_error("return ui.screen{ ui.label{bg = '#12AB3G'} }",
                       "main.lua:1: ui.label: property 'bg' expects \"#RRGGBB\" or one of accent, "
                       "text, muted, surface, bg, danger, ok, warn, got '#12AB3G'");
}

TEST(Schema, PadAcceptsSafeAndInts) {
    expect_build_ok("return ui.screen{ pad = 'safe', ui.column{pad = 12}, ui.row{pad = 0} }");
    expect_build_error(
        "return ui.screen{ pad = 'big' }",
        "main.lua:1: ui.screen: property 'pad' expects pixels or \"safe\", got 'big'");
    expect_build_error(
        "return ui.screen{ pad = 1.25 }",
        "main.lua:1: ui.screen: property 'pad' expects pixels or \"safe\", got 1.25");
}

TEST(Schema, StateAndBindAcceptedOnlyWhereBindable) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.build(R"(local count = state(3)
return ui.screen{
    ui.label{text = count, fg = state('ok')},
    ui.label{text = bind(count, function(v) return 'n=' .. v end)},
    ui.label{text = computed(function() return count:get() * 2 end)},
})");
    ASSERT_FALSE(err) << describe(err);
    ASSERT_EQ(s.tree.nodes.size(), 4u);
    // The tree carries the current values.
    EXPECT_EQ(std::get<int64_t>(*prop_of(s.node(1), Prop::Text)), 3);
    EXPECT_EQ(str_of(prop_of(s.node(1), Prop::Fg)), "ok");
    EXPECT_EQ(str_of(prop_of(s.node(2), Prop::Text)), "n=3");
    EXPECT_EQ(std::get<int64_t>(*prop_of(s.node(3), Prop::Text)), 6);

    expect_build_error("local count = state(0)\nreturn ui.screen{ ui.button{on_click = count} }",
                       "main.lua:2: ui.button: property 'on_click' expects function, got state");
    expect_build_error("local c = state(0)\nreturn ui.screen{ ui.grid{cols = c} }",
                       "main.lua:2: ui.grid: property 'cols' expects integer >= 1, got state");
    expect_build_error("return ui.screen{ ui.label{ref = bind(state(1), tostring)} }",
                       "main.lua:1: ui.label: property 'ref' expects a name of 1 to 64 bytes, "
                       "got bind");
    // The first value of a bound property is checked like a plain one, at
    // the line that built the node.
    expect_build_error("local f = state('huge')\n\nreturn ui.screen{\n ui.label{font = f} }",
                       "main.lua:4: ui.label: property 'font' expects one of xs, sm, md, lg, xl, "
                       "xxl, got 'huge'");
}

TEST(Schema, RootMustBeScreen) {
    expect_build_error("return ui.column{}",
                       "main.lua:1: entry must return ui.screen (got ui.column)");
    expect_build_error("return {}", "entry must return ui.screen (got table)");
    expect_build_error("return nil", "entry must return ui.screen (got nil)");
    // ui.screen cannot be nested.
    expect_build_error("return ui.screen{ ui.column{ ui.screen{} } }",
                       "main.lua:1: ui.column: ui.screen can only be the root");
}

TEST(Schema, NodeLimit2000) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.build(R"(local t = {}
for i = 1, 1999 do t[i] = ui.label{text = i} end
return ui.screen(t))");
    ASSERT_FALSE(err) << describe(err);
    EXPECT_EQ(s.tree.nodes.size(), kMaxNodes);
    EXPECT_EQ(s.next_id, kMaxNodes + 1);

    expect_build_error(R"(local t = {}
for i = 1, 2000 do t[i] = ui.label{} end
return ui.screen(t))",
                       "main.lua:2: too many widgets (limit 2000)");
}

TEST(Schema, ListRowFnRequired) {
    expect_build_error("return ui.screen{ ui.list{items = {1, 2}} }",
                       "main.lua:1: ui.list: property 'row' is required");
    expect_build_error("return ui.screen{ ui.list{items = {}, row = 'x'} }",
                       "main.lua:1: ui.list: property 'row' expects function, got string");
    expect_build_ok(
        "return ui.screen{ ui.list{items = state({}), row = function(item, i) end, row_h = 56} }");
}

TEST(Schema, CollectTreeAssignsIdsAndLines) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    s.next_id = 1;
    auto err = s.build(R"(local count = state(0)
local plus = ui.button{text = "+1", on_click = function() count:set(count:get() + 1) end}
return ui.screen{
    ui.column{pad = "safe", gap = 12, align = "center",
        ui.label{text = bind(count, function(v) return "clicked " .. v end), font = "xl"},
        plus,
    },
    ui.slider{value = 3, on_change = function(v) end},
})");
    ASSERT_FALSE(err) << describe(err);
    ASSERT_EQ(s.tree.nodes.size(), 5u);
    EXPECT_EQ(s.next_id, 6u);
    struct Want {
        NodeId id, parent;
        Widget widget;
        int line;
    };
    Want want[] = {{1, 0, Widget::Screen, 3},
                   {2, 1, Widget::Column, 4},
                   {3, 2, Widget::Label, 5},
                   {4, 2, Widget::Button, 2},
                   {5, 1, Widget::Slider, 8}};
    for (size_t i = 0; i < 5; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(s.node(i).id, want[i].id);
        EXPECT_EQ(s.node(i).parent, want[i].parent);
        EXPECT_EQ(s.node(i).widget, wid(want[i].widget));
        EXPECT_EQ(s.node(i).line, want[i].line);
    }
    // Props in schema order; handlers become event bits, not props.
    EXPECT_EQ(str_of(prop_of(s.node(1), Prop::Pad)), "safe");
    EXPECT_EQ(std::get<int64_t>(*prop_of(s.node(1), Prop::Gap)), 12);
    EXPECT_EQ(str_of(prop_of(s.node(1), Prop::Align)), "center");
    EXPECT_EQ(str_of(prop_of(s.node(2), Prop::Text)), "clicked 0");
    EXPECT_EQ(str_of(prop_of(s.node(2), Prop::Font)), "xl");
    EXPECT_EQ(str_of(prop_of(s.node(3), Prop::Text)), "+1");
    EXPECT_EQ(s.node(3).events, kEventClick);
    EXPECT_EQ(prop_of(s.node(3), Prop::OnClick), nullptr);
    EXPECT_EQ(s.node(4).events, kEventChange);
    EXPECT_EQ(s.node(2).events, 0u);

    // The handler stays reachable by node id.
    lua_State* L = s.vm->L();
    ASSERT_TRUE(push_node_prop(L, 4, Prop::OnClick));
    EXPECT_EQ(lua_type(L, -1), LUA_TFUNCTION);
    lua_pop(L, 1);
    EXPECT_FALSE(push_node_prop(L, 4, Prop::OnChange));
    EXPECT_EQ(lua_type(L, -1), LUA_TNIL);
    lua_pop(L, 1);
    EXPECT_FALSE(push_node_prop(L, 99, Prop::OnClick));
    lua_pop(L, 1);

    // The tree fits the patch batch it was sized against.
    EXPECT_TRUE(s.batch.push(std::move(s.tree)).is_ok());
}

TEST(Schema, CollectTreeErrorCarriesFileAndLine) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.build("local a = 1\nlocal b = 2\nlocal root = ui.column{}\nreturn root");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Runtime);
    EXPECT_EQ(err->file, "main.lua");
    EXPECT_EQ(err->line, 3);
    EXPECT_EQ(err->message, "entry must return ui.screen (got ui.column)");

    // The 2001st node in pre-order was built on line 4.
    Scene t;
    ASSERT_FALSE(t.installed) << describe(t.installed);
    err = t.build(R"(local kids = {}
for i = 1, 1999 do kids[i] = ui.label{} end
kids[2000] = ui.label{}
return ui.screen{ ui.column(kids) })");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->file, "main.lua");
    EXPECT_EQ(err->line, 2);  // node 2001: screen, column, then 1999 labels from line 2
    EXPECT_EQ(err->message, "too many widgets (limit 2000)");
    EXPECT_EQ(t.next_id, 1u);

    Scene u;
    ASSERT_FALSE(u.installed) << describe(u.installed);
    err = u.build(R"(local kids = {}
for i = 1, 1998 do kids[i] = ui.label{} end
kids[1999] = ui.label{}
kids[2000] = ui.label{}
return ui.screen(kids))");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->line, 4);
}

TEST(Schema, ShowIfHidesWithoutDestroying) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.build(R"(local flag = state(false)
_G.flag = flag
return ui.screen{ ui.show_if(flag, ui.label{text = "hi"}), ui.show_if(true, ui.label{}) })");
    ASSERT_FALSE(err) << describe(err);
    ASSERT_EQ(s.tree.nodes.size(), 3u);
    const PropValue* hidden = prop_of(s.node(1), Prop::Hidden);
    ASSERT_NE(hidden, nullptr);
    EXPECT_TRUE(std::get<bool>(*hidden));
    EXPECT_FALSE(std::get<bool>(*prop_of(s.node(2), Prop::Hidden)));
    NodeId label = s.node(1).id;

    err = s.message("_G.flag:set(true)");
    ASSERT_FALSE(err) << describe(err);
    UiBatch b = s.batch.take(1);
    ASSERT_EQ(b.ops.size(), 1u);
    const auto* set = std::get_if<SetProp>(&b.ops[0]);
    ASSERT_NE(set, nullptr) << "expected a SetProp, not a rebuild";
    EXPECT_EQ(set->node, label);
    EXPECT_EQ(set->prop, static_cast<PropId>(Prop::Hidden));
    EXPECT_FALSE(std::get<bool>(set->value));

    expect_build_error("return ui.screen{ ui.show_if(state(1), {}) }",
                       "main.lua:1: ui.show_if: expected a widget as the second argument");
    expect_build_error("return ui.screen{ ui.show_if('yes', ui.label{}) }",
                       "main.lua:1: ui.show_if: expected a state, computed, bind or boolean as "
                       "the first argument");
}

TEST(Schema, RefValueTracksChanges) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.build(R"(local h = ui.ref("name")
_G.h = h
local ok, msg = pcall(h.value, h)
assert(not ok and msg:find("ui.ref: no widget with ref 'name'", 1, true), msg)
return ui.screen{
    ui.input{ref = "name", text = "a"},
    ui.label{ref = "title", text = "t"},
    ui.slider{ref = "vol", value = 30},
    ui.switch{ref = "on"},
})");
    ASSERT_FALSE(err) << describe(err);
    ASSERT_EQ(s.tree.nodes.size(), 5u);
    EXPECT_EQ(str_of(prop_of(s.node(1), Prop::Ref)), "name");

    err = s.message(R"(
        assert(_G.h:value() == "a")
        assert(ui.ref("vol"):value() == 30)
        assert(ui.ref("on"):value() == false)
    )");
    ASSERT_FALSE(err) << describe(err);

    err = s.message(R"(local W = ...
        W.set_value(2, "ab")
        assert(_G.h:value() == "ab", tostring(_G.h:value()))
    )");
    ASSERT_FALSE(err) << describe(err);

    err = s.message("local x = ui.ref('nope'):value()");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->text(), "main.lua:1: ui.ref: no widget with ref 'nope'");

    err = s.message("\nlocal x = ui.ref('title'):value()");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->text(), "main.lua:2: ui.ref: 'title' has no value");

    expect_build_error("return ui.screen{ ui.label{ref = 'a'},\n ui.label{ref = 'a'} }",
                       "main.lua:2: duplicate ref 'a'");
}

TEST(Schema, RefScrollToEmitsScrollOp) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.build(R"(return ui.screen{
    ui.list{ref = "l", items = {1, 2, 3}, row = function(item) return ui.label{text = item} end},
    ui.label{ref = "x"},
    ui.scroll{ref = "s"},
})");
    ASSERT_FALSE(err) << describe(err);

    err = s.message("ui.ref('l'):scroll_to(5) ui.ref('s'):scroll_to(1)");
    ASSERT_FALSE(err) << describe(err);
    UiBatch b = s.batch.take(1);
    ASSERT_EQ(b.ops.size(), 2u);
    const auto* op = std::get_if<ScrollTo>(&b.ops[0]);
    ASSERT_NE(op, nullptr);
    EXPECT_EQ(op->node, 2u);
    EXPECT_EQ(op->index, 4u);
    op = std::get_if<ScrollTo>(&b.ops[1]);
    ASSERT_NE(op, nullptr);
    EXPECT_EQ(op->node, 4u);
    EXPECT_EQ(op->index, 0u);

    err = s.message("ui.ref('x'):scroll_to(1)");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->text(), "main.lua:1: ui.ref: 'x' (ui.label) does not support scroll_to");
    err = s.message("ui.ref('l'):scroll_to(0)");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->text(), "main.lua:1: ui.ref: scroll_to expects an index from 1");
}

TEST(Schema, RefFocusEmitsFocusOp) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.build(R"(return ui.screen{ ui.input{ref = "in"}, ui.button{ref = "b"} })");
    ASSERT_FALSE(err) << describe(err);

    err = s.message("ui.ref('in'):focus()");
    ASSERT_FALSE(err) << describe(err);
    UiBatch b = s.batch.take(1);
    ASSERT_EQ(b.ops.size(), 1u);
    const auto* op = std::get_if<Focus>(&b.ops[0]);
    ASSERT_NE(op, nullptr);
    EXPECT_EQ(op->node, 2u);

    err = s.message("ui.ref('b'):focus()");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->text(), "main.lua:1: ui.ref: 'b' (ui.button) does not support focus");
    // The input's value starts empty.
    err = s.message("assert(ui.ref('in'):value() == '')");
    EXPECT_FALSE(err) << describe(err);
}

// Nodes are opaque: a scene can neither forge one, nor read or change one
// after building it, nor reach the metatables behind them.
TEST(Schema, NodesAreSealed) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.build(R"(
local n = ui.label{text = "a"}
assert(type(n) == "userdata")
assert(getmetatable(n) == false)
assert(getmetatable(ui) == false)
assert(getmetatable(ui.ref("x")) == false)
assert(not pcall(function() return n.text end))
assert(not pcall(function() n.text = 5 end))
assert(not pcall(rawset, n, "text", 5))
assert(not pcall(setmetatable, n, {}))
local ok, msg = pcall(function() ui.label = 1 end)
assert(ok, "ui fields are the scene's own business")
ok, msg = pcall(function() ui.nope = 1 end)
assert(not ok and msg:find("ui is read-only", 1, true), msg)
return ui.screen{ n })");
    ASSERT_FALSE(err) << describe(err);
    EXPECT_EQ(str_of(prop_of(s.node(1), Prop::Text)), "a");

    // A table with a node's shape is not a node.
    expect_build_error("return ui.screen{ setmetatable({}, {__index = function() return 1 end}) }",
                       "main.lua:1: ui.screen: child 1 is not a widget (got table)");
    // A node can only be used once, so the tree stays a tree.
    expect_build_error("local l = ui.label{}\nreturn ui.screen{ l,\n l }",
                       "main.lua:2: ui.screen: the same widget appears twice");
    expect_build_error("local l = ui.label{}\nlocal c = ui.column{ l }\nreturn ui.screen{ l, c }",
                       "main.lua:3: ui.screen: child 1 is already inside another widget");
    // A failed constructor leaves its children free for another try.
    expect_build_ok(R"(local l = ui.label{}
assert(not pcall(ui.column, { l, l }))
return ui.screen{ l })");
}

TEST(Schema, DeepNestingIsIterative) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.build(R"(local n = ui.label{text = "leaf"}
for i = 1, 1998 do n = ui.column{ n } end
return ui.screen{ n })");
    ASSERT_FALSE(err) << describe(err);
    ASSERT_EQ(s.tree.nodes.size(), 2000u);
    for (size_t i = 1; i < s.tree.nodes.size(); ++i) ASSERT_EQ(s.node(i).parent, s.node(i - 1).id);
    EXPECT_EQ(s.tree.nodes.back().widget, wid(Widget::Label));

    // Shared subtrees cannot blow the walk up: building one is refused.
    expect_build_error(R"(local n = ui.label{}
for i = 1, 40 do n = ui.column{ n, n } end
return ui.screen{ n })",
                       "main.lua:2: ui.column: the same widget appears twice");
}

TEST(Schema, StringAndBatchCaps) {
    expect_build_error("return ui.screen{ ui.label{text = string.rep('x', 64 * 1024 + 1)} }",
                       "main.lua:1: ui.label: property 'text': value too large");
    expect_build_ok("return ui.screen{ ui.label{text = string.rep('x', 64 * 1024)} }");
    // A bound value is checked when the tree is collected.
    expect_build_error(
        "local s = state(string.rep('x', 64 * 1024 + 1))\nreturn ui.screen{\n ui.label{text = s} "
        "}",
        "main.lua:3: ui.label: property 'text': value too large");
    // One 60 KB string shared by 20 labels is 1.2 MB of copies.
    expect_build_error(R"(local big = string.rep('x', 60 * 1024)
local t = {}
for i = 1, 20 do t[i] = ui.label{text = big} end
return ui.screen(t))",
                       "main.lua:3: ui update too large");
}

TEST(Schema, BatchCapCountsWhatIsAlreadyQueued) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    // Fill the batch to just under the cap with 64 KB SetProps.
    std::string chunk(64 * 1024, 'x');
    for (NodeId n = 1; n <= 15; ++n)
        ASSERT_TRUE(s.batch.set_prop(n, 0, LargeString(chunk.data(), chunk.size())).is_ok());
    auto err = s.build("return ui.screen{ ui.label{text = string.rep('y', 63 * 1024)} }");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->text(), "main.lua:1: ui update too large");
}

// A bound value is read under the message's time limit.
TEST(Schema, CollectRunsUnderTheTimeLimit) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.build(R"(local slow = computed(function() while true do end end)
return ui.screen{ ui.label{text = slow} })");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Timeout);
    EXPECT_TRUE(s.vm->faulted());
}

TEST(Schema, DocIsUpToDate) {
    std::filesystem::path doc =
        (std::filesystem::path(WISP_ROOT) / "docs" / "scene-api.md").lexically_normal();
    std::ifstream in(doc, std::ios::binary);
    std::stringstream have;
    have << in.rdbuf();
    std::string want = schema_markdown();
    if (have.str() != want) {
        std::filesystem::path out = std::filesystem::path(WISP_BUILD_DIR) / "scene-api.md";
        std::ofstream(out, std::ios::binary) << want;
        FAIL() << doc << " is out of date with the schema. The expected text is in " << out
               << "; to accept it:\n  cp " << out << " " << doc;
    }
}
