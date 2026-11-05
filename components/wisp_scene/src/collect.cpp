// collect_tree() and push_node_prop() (see wisp/scene/schema.hpp; the
// node layout is in node.hpp).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string_view>

#include "node.hpp"
#include "wisp/scene/patch_batch.hpp"
#include "wisp/scene/reactive.hpp"
#include "wisp/scene/schema.hpp"
#include "wisp/scene/vm.hpp"

namespace wisp::scene {

namespace {

using namespace node;

struct Collect {
    Vm* vm;
    os::LargeVector<NodeDesc>* out;
    NodeId next;
    size_t count;
    // The PatchBatch's estimate once `out` is pushed. The batch cannot
    // grow meanwhile: emit() only runs at a flush, and ui.ref handles
    // refuse to push ops while kCollectingKey is set.
    size_t bytes;
    // Stack slots of the tree tables.
    int nodes_slot;
    int refs_slot;
    int values_slot;
    // Collecting a list row (build_list_rows()): its scope and the table
    // its node ids go to.
    bool row = false;
    int scope_slot = 0;
    int ids_slot = 0;
    lua_Integer ids = 0;
};

// How often the walk checks the VM's limits (vm.hpp: C loops that call
// back into Lua check every so often).
constexpr size_t kCheckEvery = 256;

// Registry key of the node count of the tree collected last: list rows
// share kMaxNodes with it.
char kTreeNodesKey;
// Registry key of the scope of the tree collected last: its watches and
// widget effects, stopped when the next tree is collected.
char kTreeScopeKey;

// Stack slots of collect_entry().
constexpr int kCtxSlot = 1;
constexpr int kRootSlot = 2;
constexpr int kNodesSlot = 3;
constexpr int kRefsSlot = 4;
constexpr int kValuesSlot = 5;
constexpr int kScopeSlot = 6;
constexpr int kWalkBase = 6;  // (node, parent id) pairs above this

// Raises at the position the node at `node` was built at.
int raise_at_node(lua_State* L, int node, const char* fmt, const char* arg = "") {
    char file[LUA_IDSIZE];
    int line = node_position(L, node, file);
    return raise(L, file, line, fmt, arg);
}

// The C++ side of a node. Neither raises; false when the batch estimate
// would pass kMaxBatchBytes.
bool begin_node(Collect* c, NodeId id, NodeId parent, Widget w, int line) {
    if (c->bytes + kOpOverheadBytes > kMaxBatchBytes) return false;
    c->bytes += kOpOverheadBytes;
    NodeDesc& n = c->out->emplace_back();
    n.id = id;
    n.parent = parent;
    n.widget = static_cast<WidgetId>(w);
    n.line = line;
    return true;
}

PropValue to_value(lua_State* L, int idx, PropType type) {
    switch (lua_type(L, idx)) {
        case LUA_TBOOLEAN:
            return PropValue{lua_toboolean(L, idx) != 0};
        case LUA_TNUMBER: {
            lua_Integer i = 0;
            if ((type == PropType::Size || type == PropType::Int || type == PropType::Pad ||
                 type == PropType::Font) &&
                integral(L, idx, i))
                return PropValue{static_cast<int64_t>(i)};
            if (lua_isinteger(L, idx))
                return PropValue{static_cast<int64_t>(lua_tointeger(L, idx))};
            return PropValue{static_cast<double>(lua_tonumber(L, idx))};
        }
        case LUA_TSTRING: {
            std::string_view s = string_at(L, idx);
            return PropValue{os::LargeString(s.data(), s.size())};
        }
        case LUA_TTABLE:
            if (type == PropType::Pad) return PropValue{pad_sides_value(L, idx)};
            return PropValue{};
        default:
            return PropValue{};
    }
}

// Checked by the caller: not a table or function, at most
// kMaxPropStringBytes.
bool add_prop(Collect* c, lua_State* L, int idx, Prop p) {
    size_t added = kOpOverheadBytes + (lua_type(L, idx) == LUA_TSTRING ? lua_rawlen(L, idx) : 0);
    if (c->bytes + added > kMaxBatchBytes) return false;
    c->bytes += added;
    c->out->back().props.emplace_back(static_cast<PropId>(p), to_value(L, idx, prop_spec(p).type));
    return true;
}

void add_events(Collect* c, uint32_t bits) { c->out->back().events |= bits; }

uint32_t event_bit(Prop p) {
    switch (p) {
        case Prop::OnClick:
            return kEventClick;
        case Prop::OnLongPress:
            return kEventLongPress;
        case Prop::OnChange:
            return kEventChange;
        default:
            return 0;
    }
}

// The property whose value a ui.ref handle's :value() reports.
std::optional<Prop> value_prop(Widget w) {
    switch (w) {
        case Widget::Input:
            return Prop::Text;
        case Widget::Slider:
            return Prop::Value;
        case Widget::Checkbox:
        case Widget::Switch:
            return Prop::Checked;
        default:
            return std::nullopt;
    }
}

// Pushes the default :value() of a widget that was given none.
void push_default_value(lua_State* L, Widget w, int record) {
    if (w == Widget::Input) {
        lua_pushliteral(L, "");
    } else if (w == Widget::Slider) {
        // The slider starts at its minimum when that is a plain number.
        if (lua_rawgeti(L, record, prop_key(Prop::Min)) != LUA_TNUMBER) {
            lua_pop(L, 1);
            lua_pushinteger(L, 0);
        }
    } else {
        lua_pushboolean(L, 0);
    }
}

// Adds the property value on top (popping it) to the node being built:
// event bit, ref, :value() and the NodeDesc prop. `id` is the node's id.
void take_prop(lua_State* L, Collect* c, int node, Prop p, NodeId id, bool& has_value) {
    Widget w = static_cast<Widget>(static_cast<NodeHeader*>(lua_touserdata(L, node))->widget);
    if (reactive_kind(L, -1) != nullptr) {
        // watch(node, prop, source [, scope]) -> disposer, current value
        lua_rawgetp(L, LUA_REGISTRYINDEX, &kWatchKey);
        lua_pushinteger(L, id);
        lua_pushinteger(L, static_cast<lua_Integer>(p));
        lua_pushvalue(L, -4);
        int nargs = 3;
        if (c->scope_slot != 0) {
            lua_pushvalue(L, c->scope_slot);
            ++nargs;
        }
        lua_call(L, nargs, 2);
        lua_remove(L, -2);
        lua_remove(L, -2);
        Check check = check_value(L, -1, p);
        if (check != Check::Ok) {
            // Reported at the node's line, as for a plain value.
            char file[LUA_IDSIZE];
            int line = node_position(L, node, file);
            raise_prop(L, file, line, w, p, check, lua_gettop(L), nullptr);
        }
    }
    if (p == Prop::Ref) {
        // A row exists once per item and comes and goes with scrolling,
        // so no name could stand for one of its widgets.
        if (c->row)
            raise_at_node(L, node, "ui.%s: ref cannot be used inside a list row",
                          widget_name(w).data());
        lua_pushvalue(L, -1);
        if (lua_rawget(L, c->refs_slot) != LUA_TNIL)
            raise_at_node(L, node, "duplicate ref '%s'", lua_tostring(L, -2));
        lua_pop(L, 1);
        lua_pushvalue(L, -1);
        lua_pushinteger(L, id);
        lua_rawset(L, c->refs_slot);
    }
    if (value_prop(w) == p && !lua_isnil(L, -1)) {
        lua_pushvalue(L, -1);
        lua_rawseti(L, c->values_slot, id);
        has_value = true;
    }
    if (!add_prop(c, L, -1, p)) raise_at_node(L, node, "ui update too large");
    lua_pop(L, 1);
}

// Starts the widget effect of a list, canvas or chart (W.widget(), see
// the prelude): `record` is the node's record.
void start_widget(lua_State* L, Collect* c, Widget w, NodeId id, int record) {
    const char* kind = nullptr;
    Prop a = Prop::Items, b = Prop::Row;
    switch (w) {
        case Widget::List:
            kind = "list";
            break;
        case Widget::Canvas:
            kind = "canvas";
            a = Prop::Draw;
            break;
        case Widget::Chart:
            kind = "chart";
            a = Prop::Series;
            break;
        default:
            return;
    }
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kWidgetKey);
    lua_pushinteger(L, id);
    lua_pushstring(L, kind);
    lua_rawgeti(L, record, prop_key(a));
    if (w == Widget::List)
        lua_rawgeti(L, record, prop_key(b));
    else
        lua_pushnil(L);
    if (c->scope_slot != 0)
        lua_pushvalue(L, c->scope_slot);
    else
        lua_pushnil(L);
    lua_call(L, 5, 0);
}

// Turns the node at `node` (from the walk stack, with its parent's id)
// into a NodeDesc, then replaces it on the stack with its children's
// (node, id) pairs, last child lowest so the first is taken next.
void visit(lua_State* L, Collect* c, int node, NodeId parent) {
    // The walk's pending pairs may have used up the slots a C function
    // starts with.
    luaL_checkstack(L, LUA_MINSTACK, "widget tree");
    auto* h = static_cast<NodeHeader*>(lua_touserdata(L, node));
    Widget w = static_cast<Widget>(h->widget);
    if (++c->count > kMaxNodes) raise_at_node(L, node, "too many widgets (limit 2000)");
    if (c->count % kCheckEvery == 0) c->vm->check_limits(L);
    // Nested lists would need rows of rows that come and go with their
    // own; one level keeps a row's lifetime that of its scope.
    if (c->row && w == Widget::List) raise_at_node(L, node, "ui.list cannot be inside a list row");
    NodeId id = c->next++;
    if (!begin_node(c, id, parent, w, h->line)) raise_at_node(L, node, "ui update too large");
    if (c->ids_slot != 0) {
        lua_pushinteger(L, id);
        lua_rawseti(L, c->ids_slot, ++c->ids);
    }
    lua_getiuservalue(L, node, 1);
    int record = lua_gettop(L);
    bool has_value = false;
    uint32_t events = 0;
    for (Prop p : widget_spec(w).props) {
        if (lua_rawgeti(L, record, prop_key(p)) == LUA_TNIL) {
            lua_pop(L, 1);
            continue;
        }
        PropType type = prop_spec(p).type;
        if (type == PropType::Fn || type == PropType::Items || type == PropType::Series) {
            // Kept in the node for push_node_prop(); handlers set event bits.
            events |= event_bit(p);
            lua_pop(L, 1);
            continue;
        }
        take_prop(L, c, node, p, id, has_value);
    }
    add_events(c, events);
    if (!has_value && value_prop(w)) {
        push_default_value(L, w, record);
        lua_rawseti(L, c->values_slot, id);
    }
    lua_pushvalue(L, node);
    lua_rawseti(L, c->nodes_slot, id);
    start_widget(L, c, w, id, record);
    luaL_checkstack(L, 2 * h->children + 2, "widget tree");
    for (int i = h->children; i >= 1; --i) {
        lua_rawgeti(L, record, i);
        lua_pushinteger(L, id);
    }
    lua_remove(L, record);
    lua_remove(L, node);
}

// collect_entry(ctx, root), under Vm::call(). The walk keeps its pending
// nodes on the Lua stack instead of recursing, so a deep tree costs stack
// slots, not C stack or C call levels.
int collect_entry(lua_State* L) {
    auto* c = static_cast<Collect*>(lua_touserdata(L, kCtxSlot));
    lua_settop(L, kRootSlot);
    NodeHeader* root = to_node(L, kRootSlot);
    if (root == nullptr)
        return raise(L, nullptr, 0, "entry must return ui.screen (got %s)",
                     luaL_typename(L, kRootSlot));
    if (root->widget != static_cast<uint16_t>(Widget::Screen))
        return raise_at_node(L, kRootSlot, "entry must return ui.screen (got ui.%s)",
                             widget_name(static_cast<Widget>(root->widget)).data());
    if (!push_wisp_internal(L)) return luaL_error(L, "ui: the reactive prelude is not installed");
    lua_pushboolean(L, 1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kCollectingKey);
    // The old tree's lists and widget effects go with it.
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kResetWidgetsKey);
    lua_call(L, 0, 0);
    // So do its watches: the next tree has other node ids.
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &kTreeScopeKey) != LUA_TNIL) {
        lua_getfield(L, -2, "dispose");
        lua_insert(L, -2);
        lua_call(L, 1, 0);
    } else {
        lua_pop(L, 1);
    }
    reset_tree_tables(L, lua_gettop(L));
    lua_getfield(L, -1, "scope");
    lua_call(L, 0, 1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kTreeScopeKey);
    lua_pop(L, 1);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kNodesKey);      // kNodesSlot
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kRefsKey);       // kRefsSlot
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kValuesKey);     // kValuesSlot
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kTreeScopeKey);  // kScopeSlot
    lua_pushvalue(L, kRootSlot);
    lua_pushinteger(L, 0);
    while (lua_gettop(L) > kWalkBase) {
        auto parent = static_cast<NodeId>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        visit(L, c, lua_gettop(L), parent);
    }
    lua_pushboolean(L, 0);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kCollectingKey);
    lua_pushinteger(L, static_cast<lua_Integer>(c->count));
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kTreeNodesKey);
    return 0;
}

// Clears kCollectingKey after a failed collect. install_ui() created the
// key, so changing its value allocates nothing and cannot raise outside
// a protected call; without install_ui() there is nothing to clear.
void stop_collecting(lua_State* L) {
    if (!lua_checkstack(L, 1)) return;
    bool present = lua_rawgetp(L, LUA_REGISTRYINDEX, &kCollectingKey) == LUA_TBOOLEAN;
    lua_pop(L, 1);
    if (!present) return;
    lua_pushboolean(L, 0);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kCollectingKey);
}

// What build_list_rows() collects into: its rows, one ListRows per run
// of consecutive indexes.
struct Rows {
    Vm* vm;
    NodeId list;
    NodeId next;
    PatchBatch* batch;
    os::LargeVector<ListRows>* runs;
    size_t pending;  // the PatchBatch estimate of `runs`
};

// Stack slots of collect_row().
constexpr int kRowIndexSlot = 1;
constexpr int kRowNodeSlot = 2;
constexpr int kRowScopeSlot = 3;
constexpr int kRowIdsSlot = 4;
constexpr int kRowUsedSlot = 5;
constexpr int kRowNodesSlot = 6;
constexpr int kRowRefsSlot = 7;
constexpr int kRowValuesSlot = 8;
constexpr int kRowWalkBase = 8;

// log_warning(message): logs it to the scene's log.
int log_warning(lua_State* L) {
    log_lua_args(L, LogLevel::Warn);
    return 0;
}

// Logs that rows from `index` (0-based) on are not built. Runs Lua (and
// may raise), so no C++ object may be alive across it.
void warn_widget_limit(lua_State* L, uint32_t index) {
    lua_pushcfunction(L, log_warning);
    lua_pushfstring(L, "ui.list: widget limit %d reached, rows from %d on are not built",
                    static_cast<int>(kMaxNodes), static_cast<int>(index) + 1);
    lua_call(L, 1, 0);
}

// collect_row(index, node, scope, ids, row_nodes), upvalue 1 the Rows:
// the C half of W.build_rows() (prelude). Collects one row the way
// collect_entry() collects a tree, with the list as the root's parent,
// and returns true; or, when the tree, the row_nodes of the rows already
// built and this row together pass kMaxNodes (spec 4.5), collects
// nothing, logs a warning and returns false.
int collect_row(lua_State* L) {
    auto* r = static_cast<Rows*>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_settop(L, kRowUsedSlot);
    NodeHeader* root = to_node(L, kRowNodeSlot);
    if (root == nullptr || root->widget == static_cast<uint16_t>(Widget::Screen)) {
        const char* got = root != nullptr ? "ui.screen" : luaL_typename(L, kRowNodeSlot);
        char file[LUA_IDSIZE] = "";
        int line = 0;
        if (lua_rawgetp(L, LUA_REGISTRYINDEX, &kNodesKey) == LUA_TTABLE &&
            lua_rawgeti(L, -1, r->list) == LUA_TUSERDATA)
            line = node_position(L, lua_gettop(L), file);
        return raise(L, file, line, "ui.list: row must return a widget (got %s)", got);
    }
    auto index = static_cast<uint32_t>(lua_tointeger(L, kRowIndexSlot));
    size_t used = static_cast<size_t>(std::max<lua_Integer>(lua_tointeger(L, kRowUsedSlot), 0));
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &kTreeNodesKey) == LUA_TNUMBER)
        used += static_cast<size_t>(std::max<lua_Integer>(lua_tointeger(L, -1), 0));
    lua_pop(L, 1);
    if (used >= kMaxNodes) {
        warn_widget_limit(L, index);
        lua_pushboolean(L, 0);
        return 1;
    }
    os::LargeVector<ListRows>& runs = *r->runs;
    const bool new_run = runs.empty() || runs.back().first + runs.back().rows.size() != index;
    const size_t batch_bytes = r->batch != nullptr ? r->batch->bytes() : 0;
    const size_t start = batch_bytes + r->pending + (new_run ? kOpOverheadBytes : 0);
    if (start > kMaxBatchBytes) return raise_at_node(L, kRowNodeSlot, "ui update too large");
    if (new_run) {
        ListRows& run = runs.emplace_back();
        run.list = r->list;
        run.first = index;
    }
    // The row's own count starts at 0, so a single row of more than
    // kMaxNodes is still the scene's error.
    Collect c{r->vm,        &runs.back().rows.emplace_back(),
              r->next,      0,
              start,        kRowNodesSlot,
              kRowRefsSlot, kRowValuesSlot,
              true,         kRowScopeSlot,
              kRowIdsSlot,  0};
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kNodesKey);   // kRowNodesSlot
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kRefsKey);    // kRowRefsSlot
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kValuesKey);  // kRowValuesSlot
    lua_pushboolean(L, 1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kCollectingKey);
    lua_pushvalue(L, kRowNodeSlot);
    lua_pushinteger(L, r->list);
    while (lua_gettop(L) > kRowWalkBase) {
        auto parent = static_cast<NodeId>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        visit(L, &c, lua_gettop(L), parent);
    }
    lua_pushboolean(L, 0);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kCollectingKey);
    r->next = c.next;
    if (used + c.count > kMaxNodes) {
        // The row's nodes and effects go when W.build_rows() drops it.
        runs.back().rows.pop_back();
        if (runs.back().rows.empty()) runs.pop_back();
        warn_widget_limit(L, index);
        lua_pushboolean(L, 0);
        return 1;
    }
    r->pending = c.bytes - batch_bytes;
    lua_pushboolean(L, 1);
    return 1;
}

// rows_entry(rows, list, first, count), under Vm::call():
// W.build_rows(list, first, count, collect_row).
int rows_entry(lua_State* L) {
    lua_settop(L, 4);
    if (!push_wisp_internal(L)) return luaL_error(L, "ui: the reactive prelude is not installed");
    lua_getfield(L, -1, "build_rows");
    lua_pushvalue(L, 2);
    lua_pushvalue(L, 3);
    lua_pushvalue(L, 4);
    lua_pushvalue(L, 1);
    lua_pushcclosure(L, collect_row, 1);
    // Checks the limits on every row it collects.
    push_checked_function(L, -1);
    lua_remove(L, -2);
    lua_call(L, 4, 0);
    return 0;
}

}  // namespace

namespace node {

void sync_value(lua_State* L, NodeId id, Prop prop, int idx) {
    idx = lua_absindex(L, idx);
    luaL_checkstack(L, 4, "sync_value");
    int top = lua_gettop(L);
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &kNodesKey) != LUA_TTABLE ||
        lua_rawgeti(L, -1, id) != LUA_TUSERDATA) {
        lua_settop(L, top);
        return;
    }
    // The nodes table holds only nodes.
    auto w = static_cast<Widget>(static_cast<NodeHeader*>(lua_touserdata(L, -1))->widget);
    if (value_prop(w) != prop) {
        lua_settop(L, top);
        return;
    }
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kValuesKey);
    int values = lua_gettop(L);
    if (lua_isnil(L, idx)) {
        lua_getiuservalue(L, values - 1, 1);
        push_default_value(L, w, lua_gettop(L));
    } else {
        lua_pushvalue(L, idx);
    }
    lua_rawseti(L, values, id);
    lua_settop(L, top);
}

}  // namespace node

std::optional<ScriptError> build_list_rows(Vm& vm, NodeId list, uint32_t first, uint32_t count,
                                           NodeId& next_id) {
    lua_State* L = vm.L();
    if (!lua_checkstack(L, 5)) return ScriptError{ScriptFault::Memory, "", 0, "stack overflow"};
    os::LargeVector<ListRows> runs;
    Rows rows{&vm, list, next_id, patch_batch(L), &runs, 0};
    lua_pushcfunction(L, rows_entry);
    lua_pushlightuserdata(L, &rows);
    lua_pushinteger(L, list);
    lua_pushinteger(L, first);
    lua_pushinteger(L, count);
    auto err = vm.call(4, 0, Budget::Fresh);
    if (err) {
        stop_collecting(L);
        return err;
    }
    next_id = rows.next;
    for (ListRows& run : runs) {
        if (run.rows.empty()) continue;
        // The walk counted every byte against the same limit, so this only
        // fails if the batch is not the one collect_row() measured.
        if (rows.batch == nullptr || !rows.batch->push(std::move(run)).is_ok())
            return ScriptError{ScriptFault::Runtime, "", 0, "ui update too large"};
    }
    return std::nullopt;
}

std::optional<ScriptError> collect_tree(Vm& vm, int index, NodeId& next_id, Build& out) {
    lua_State* L = vm.L();
    index = lua_absindex(L, index);
    out.nodes.clear();
    if (!lua_checkstack(L, 3)) return ScriptError{ScriptFault::Memory, "", 0, "stack overflow"};
    PatchBatch* batch = patch_batch(L);
    Collect c{
        &vm,        &out.nodes, next_id,     0,     (batch ? batch->bytes() : 0) + kOpOverheadBytes,
        kNodesSlot, kRefsSlot,  kValuesSlot, false, kScopeSlot};
    lua_pushcfunction(L, collect_entry);
    lua_pushlightuserdata(L, &c);
    lua_pushvalue(L, index);
    auto err = vm.call(2, 0, Budget::Continue);
    if (err) {
        stop_collecting(L);
        out.nodes.clear();
        return err;
    }
    next_id = c.next;
    return std::nullopt;
}

bool push_node_prop(lua_State* L, NodeId node, Prop prop) {
    if (!lua_checkstack(L, 4)) return false;
    int top = lua_gettop(L);
    bool found = lua_rawgetp(L, LUA_REGISTRYINDEX, &kNodesKey) == LUA_TTABLE &&
                 lua_rawgeti(L, -1, node) == LUA_TUSERDATA &&
                 lua_getiuservalue(L, -1, 1) == LUA_TTABLE &&
                 lua_rawgeti(L, -1, prop_key(prop)) != LUA_TNIL;
    // At most four values were pushed; the last one is the answer when
    // found, and anything else means nil.
    if (!found) {
        lua_settop(L, top);
        lua_pushnil(L);
        return false;
    }
    lua_replace(L, top + 1);
    lua_settop(L, top + 1);
    return true;
}

}  // namespace wisp::scene
