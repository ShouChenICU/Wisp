#pragma once

// The widget whitelist (scene-runtime-core spec 4.3-4.5) and the `ui`
// table scenes declare their UI with.
//
// Scenes build nodes with constructors: ui.label { text = "hi" }.
// String keys are properties, checked against the schema when the
// constructor runs, so an unknown property or a value of the wrong type
// is reported at the line that built the node. Array entries are
// children, for the widgets that take them. A constructor returns a
// node: a userdata with a hidden metatable whose properties and children
// live in a record only C code can reach, so a scene can neither read
// nor change a node after building it, nor make something that passes
// for one. A node can be a child of only one widget, and ui.screen only
// the root, so what collect_tree() walks is always a tree.
//
// Values (PropType):
//   Size     integer pixels, "fill" or "content"
//   Int      integer (an integral float is taken as an integer)
//   Number   integer or float
//   String   string or number (a number is passed on as it is: the UI
//            shows it as text)
//   Bool     boolean
//   Color    "#RRGGBB" or a theme token (accent text muted surface bg
//            danger ok warn)
//   Font     xs sm md lg xl xxl
//   Align    start center end
//   Justify  start center end between around evenly
//   Pad      integer or "safe" (the safe area, spec 5.6/AGENTS.md
//            section 3; ui.screen always gets at least that)
//   Enum     one of PropSpec::choices
//   Fn       a function
//   Items    a table, or a state/computed/bind holding one
//   Series   up to 4 tables of up to 128 numbers each
// A bindable property also takes a state, computed or bind; the tree
// carries its current value and a watch keeps the widget up to date.
// Only the first value is checked against the schema: later values reach
// the UI as nil, a boolean, a number or a string, and the UI ignores
// what it cannot show. Items and Series hold tables, which a watch
// cannot carry; for them the node keeps the Lua value (push_node_prop())
// and the list and chart code runs an effect over it.
//
// What collect_tree() leaves in the internal table (push_wisp_internal()),
// all keyed by NodeId and replaced by every collect_tree():
//   nodes[id]   the node, for push_node_prop()
//   refs[name]  the id of the node with ref = name
//   values[id]  the current value of an input (text), slider (value),
//               checkbox or switch (checked); set_value(id, v) updates it
//               (the scene task calls it before dispatching a Change
//               event)
//
// ui.show_if(s, node) sets node's hidden to bind(s, not v) (or to not s
// for a plain boolean) and returns node: the widget is only shown and
// hidden, never rebuilt.
// ui.ref(name) returns a handle at once; its methods look the name up in
// the tree collected last, when they are called:
//   h:value()        values[id] (input, slider, checkbox, switch)
//   h:scroll_to(i)   ScrollTo{id, i - 1} (list, scroll)
//   h:focus()        Focus{id} (input)

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "lua.hpp"
#include "wisp/scene/ui_ops.hpp"
#include "wisp/scene/vm.hpp"

namespace wisp::scene {

enum class Widget : WidgetId {
    Screen,
    Column,
    Row,
    Stack,
    Grid,
    Spacer,
    Scroll,
    Label,
    Icon,
    Image,
    Button,
    Checkbox,
    Switch,
    Slider,
    Input,
    Bar,
    Arc,
    Chart,
    List,
    Canvas
};

enum class Prop : PropId {
    W,
    H,
    Grow,
    Pad,
    Gap,
    Align,
    Justify,
    Bg,
    Fg,
    Radius,
    Border,
    Font,
    Hidden,
    Opa,
    Ref,
    Text,
    Placeholder,
    Password,
    MaxLen,
    Name,
    Src,
    Value,
    Min,
    Max,
    Checked,
    Cols,
    Direction,
    Type,
    Series,
    Items,
    Row,
    RowH,
    Draw,
    Width,
    Wrap,
    OnClick,
    OnLongPress,
    OnChange
};

inline constexpr size_t kWidgetCount = static_cast<size_t>(Widget::Canvas) + 1;
inline constexpr size_t kPropCount = static_cast<size_t>(Prop::OnChange) + 1;

enum class PropType {
    Size,
    Int,
    Number,
    String,
    Bool,
    Color,
    Font,
    Align,
    Justify,
    Pad,
    Enum,
    Fn,
    Items,
    Series
};

struct PropSpec {
    Prop prop;
    std::string_view name;
    PropType type;
    bool bindable;
    // The strings the property accepts (for Size and Pad, the ones it
    // accepts besides integers; for Color, besides "#RRGGBB").
    std::span<const std::string_view> choices;
};

struct WidgetSpec {
    Widget widget;
    std::string_view name;  // without "ui."
    bool children;
    std::span<const Prop> props;
};

inline constexpr size_t kMaxNodes = 2000;

// The widget called `name` ("label", without "ui."), or nullptr.
const WidgetSpec* find_widget(std::string_view name);
const WidgetSpec& widget_spec(Widget w);
const PropSpec& prop_spec(Prop p);
// What a value of `p` must be, for messages and the generated doc
// ("number", "integer >= 1", ...).
std::string_view prop_expects(Prop p);

// The candidate closest to `wrong` by Levenshtein distance; on a tie the
// lexicographically first. Empty if there are no candidates. Only the
// first 64 bytes of each name count.
std::string nearest_name(std::string_view wrong, std::span<const std::string_view> candidates);

// docs/scene-api.md: the schema as Markdown.
std::string schema_markdown();

// Sets the global `ui` (the constructors, ui.show_if, ui.ref) and adds
// set_value, nodes, refs and values to the internal table. Needs
// install_prelude() first; call once, before any scene code runs.
std::optional<ScriptError> install_ui(Vm& vm);

// Turns the node at stack `index` (the entry file's result) into `out`,
// which it replaces: nodes in pre-order, the root first, with ids from
// next_id on (next_id ends one past the last). The root must be a
// ui.screen. Registers a watch for every bindable property given a
// state, so it runs as scene code under Vm::call() (continuing the
// current message's time budget), and rebuilds the internal nodes, refs
// and values tables. Leaves the stack as it was.
//
// Errors carry the file and line of the constructor call that built the
// node at fault ("main.lua:20: entry must return ui.screen (got
// ui.column)", "main.lua:9: too many widgets (limit 2000)"); a string
// property over kMaxPropStringBytes is "value too large", and a tree that
// would take the PatchBatch past kMaxBatchBytes is "ui update too large",
// refused before the rest is copied. On an error `out` is empty and
// next_id unchanged; watches made so far stay, since the VM is destroyed
// after an error anyway (spec 3.3).
std::optional<ScriptError> collect_tree(Vm& vm, int index, NodeId& next_id, Build& out);

// Pushes the value a node of the tree collected last holds for `prop`
// (a handler, row, draw, items, series, or any other property as the
// scene gave it), or nil, and returns whether it was there. For the scene
// task and the list and chart code. Never raises; pushes nothing and
// returns false if the stack cannot grow.
bool push_node_prop(lua_State* L, NodeId node, Prop prop);

}  // namespace wisp::scene
