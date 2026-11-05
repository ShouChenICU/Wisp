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
//   (Size, Pad and Int integers must fall in prop_range(): LVGL reads
//   some high bits of a coordinate as flags, so the UI can use any value
//   that passes as it is.)
//   Number   integer or float
//   String   string or number (a number is passed on as it is: the UI
//            shows it as text)
//   Bool     boolean
//   Color    "#RRGGBB" or a theme token (accent text muted surface bg
//            danger ok warn)
//   Font     xs sm md lg xl xxl
//   Align    start center end
//   Justify  start center end between around evenly
//   Pad      integer, or {top, right, bottom, left} of integers (exactly
//            those four entries); the UI gets the table as one
//            encode_pad_sides() integer (ui_ops.hpp)
//   Enum     one of PropSpec::choices
//   Fn       a function
//   Items    a table, or a state/computed/bind holding one
//   Series   up to 4 tables of up to 128 numbers each
// A bindable property also takes a state, computed or bind; the tree
// carries its current value and a watch keeps the widget up to date.
// Every value is checked against the schema: the first by
// collect_tree(), later ones by the watch's emit() (bound_value_ok()),
// so the UI only ever gets values a constructor would have taken. Items
// and Series hold tables, which a watch cannot carry; for them the node
// keeps the Lua value (push_node_prop()) and the list and chart code runs
// an effect over it, which must check each value it reads with
// prop_value_ok(): collect_tree() checks only static ones. `src` and the
// other structural properties (ref, cols, direction, type) are not
// bindable, so an image only ever shows the asset its constructor named.
//
// What collect_tree() leaves in the internal table (push_wisp_internal()),
// all keyed by NodeId and replaced by every collect_tree():
//   nodes[id]   the node, for push_node_prop()
//   refs[name]  the id of the node with ref = name
//   values[id]  the current value of an input (text), slider (value),
//               checkbox or switch (checked); set_value(id, v) updates it
//               (the scene task calls it before dispatching a Change
//               event), and so does a watch that emits a new bound value
//               for that property (nil: the widget's default)
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
#include <cstdint>
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

// The integers a Size, Pad or Int property takes (both ends included).
struct IntRange {
    int32_t min;
    int32_t max;
};
IntRange prop_range(Prop p);

// Room for any message prop_value_ok() and bound_value_ok() write.
inline constexpr size_t kPropErrorBytes = 320;

// Checks a value for `prop` (not a state, computed or bind) at stack
// `idx` the way the constructors do; nil passes (an unset bound value).
// On failure writes the reason to `why` ("property 'text' expects string,
// got boolean"). Never raises.
bool prop_value_ok(lua_State* L, int idx, Prop prop, char* why, size_t why_size);

// prop_value_ok() for a value a watch emits for `node`. The message is
// the constructors' ("main.lua:9: ui.label: property 'text' expects
// string, got boolean", at the line that built the node), or the bare
// reason when the node is not in the tree collected last. Never raises.
bool bound_value_ok(lua_State* L, NodeId node, Prop prop, int idx, char* why, size_t why_size);

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
// and values tables. The watches and widget effects of the tree collected
// before are stopped (they share one scope per tree). Leaves the stack as it was. While it runs,
// ui.ref handle methods refuse to work, so nothing the watches evaluate can push ops ahead of the
// Build (or past the size it was checked against).
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

// Scene task: answers SceneView's on_list_range(list, first, count) for
// a list of the tree collected last. Makes rows [first, first + count)
// (0-based, cut at the length of the items the list shows) exist and
// drops every other row of that list: its scope is disposed and its nodes
// leave the internal nodes and values tables, so an event that arrives
// for one later finds no handler and is dropped. Each new row is
// row(item, index) (index 1-based), collected like a tree whose root is
// that node (any widget but ui.screen and ui.list, and no `ref` in it),
// with ids from next_id on and its watches in the row's scope; the rows
// go to the PatchBatch as ListRows, one per run of consecutive rows. A
// list that is not in the tree, or whose items have not been read yet (the
// flush after the Build reads them), builds nothing. The tree and every
// row built, of all lists, share kMaxNodes (spec 4.5): the row that would
// pass it is not built, nor any after it in this call, and the scene's
// log gets a warning ("ui.list: widget limit 2000 reached, rows from 31
// on are not built"); a single row of more than kMaxNodes is still an
// error. Runs under
// Vm::call() as a message of its own (Budget::Fresh); the caller flushes
// afterwards as after any message. Errors as collect_tree(), positioned at
// the row's constructor (or at the list for a row() that returns no
// widget).
//
// Widgets fed by tables: collect_tree() starts an effect for every list,
// canvas and chart, which first runs at the next flush (after the Build is
// in the batch) and again whenever what it read changes, and also on
// __wisp.refresh_all() (the patch queue may have dropped what it sent):
//   list    reads items (checked like a constructor's value), sends
//           ListReset{list, #items} and drops every row.
//   canvas  runs draw(g); g:line, g:circle, g:arc, g:rect, g:fill and
//           g:text record at most 512 commands (a 513th raises "canvas:
//           more than 512 draw commands"), sent as one CanvasDraw. Its
//           size is counted against kMaxBatchBytes as each text is
//           recorded, so the g:text that would pass it raises "ui update
//           too large".
//   chart   reads series (checked like a constructor's value) and sends
//           it as SetProp{chart, series, <encoded points>}.
std::optional<ScriptError> build_list_rows(Vm& vm, NodeId list, uint32_t first, uint32_t count,
                                           NodeId& next_id);

// Pushes the value a node of the tree collected last holds for `prop`
// (a handler, row, draw, items, series, or any other property as the
// scene gave it), or nil, and returns whether it was there. For the scene
// task and the list and chart code. Never raises; pushes nothing and
// returns false if the stack cannot grow.
bool push_node_prop(lua_State* L, NodeId node, Prop prop);

}  // namespace wisp::scene
