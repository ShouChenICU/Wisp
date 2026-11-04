// The `ui` table: the constructors, ui.show_if and ui.ref handles (see
// wisp/scene/schema.hpp; the node layout is in node.hpp).

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "node.hpp"
#include "wisp/scene/patch_batch.hpp"
#include "wisp/scene/reactive.hpp"
#include "wisp/scene/schema.hpp"

namespace wisp::scene {

namespace {

using namespace node;

constexpr const char* kRefType = "wisp.ref";
constexpr const char* kUiType = "wisp.ui";
constexpr size_t kMaxRefBytes = 64;

const PropSpec* find_prop(const WidgetSpec& w, std::string_view name) {
    for (Prop p : w.props)
        if (prop_spec(p).name == name) return &prop_spec(p);
    return nullptr;
}

int raise_unknown_prop(lua_State* L, const Where& at, const WidgetSpec& w, std::string_view key) {
    std::array<std::string_view, kPropCount> names{};
    size_t n = 0;
    for (Prop p : w.props) names[n++] = prop_spec(p).name;
    char best[24];
    nearest_into(best, key, std::span<const std::string_view>(names.data(), n));
    char shown_key[kMaxShown + 4];
    return raise(L, at.file, at.line, "ui.%s: unknown property '%s' (did you mean '%s'?)",
                 w.name.data(), shown(key, shown_key), best);
}

// Checks property `key` (a string, at -2) = value (at -1) and stores the
// value in the record.
void put_prop(lua_State* L, const Where& at, const WidgetSpec& w, int record) {
    const PropSpec* spec = find_prop(w, string_at(L, -2));
    if (spec == nullptr) raise_unknown_prop(L, at, w, string_at(L, -2));
    const char* kind = reactive_kind(L, -1);
    if (kind != nullptr) {
        if (!spec->bindable)
            raise_prop(L, at.file, at.line, w.widget, spec->prop, Check::WrongType, -1, kind);
    } else {
        Check c = check_value(L, -1, spec->prop);
        if (c != Check::Ok) raise_prop(L, at.file, at.line, w.widget, spec->prop, c, -1, nullptr);
    }
    lua_pushvalue(L, -1);
    lua_rawseti(L, record, prop_key(spec->prop));
}

// Checks child `key` (an integer, at -2) = value (at -1) and stores it in
// the record at the same index.
void put_child(lua_State* L, const Where& at, const WidgetSpec& w, int record, lua_Integer& last) {
    const char* wname = w.name.data();
    if (!w.children) raise(L, at.file, at.line, "ui.%s: takes no children", wname);
    lua_Integer i = lua_tointeger(L, -2);
    if (i < 1 || i > static_cast<lua_Integer>(kMaxNodes))
        raise(L, at.file, at.line, "ui.%s: child index %I is out of range (1 to %d)", wname,
              static_cast<LUAI_UACINT>(i), static_cast<int>(kMaxNodes));
    NodeHeader* child = to_node(L, -1);
    if (child == nullptr)
        raise(L, at.file, at.line, "ui.%s: child %I is not a widget (got %s)", wname,
              static_cast<LUAI_UACINT>(i), luaL_typename(L, -1));
    if (child->widget == static_cast<uint16_t>(Widget::Screen))
        raise(L, at.file, at.line, "ui.%s: ui.screen can only be the root", wname);
    if (child->attached)
        raise(L, at.file, at.line, "ui.%s: child %I is already inside another widget", wname,
              static_cast<LUAI_UACINT>(i));
    lua_pushvalue(L, -1);
    lua_rawseti(L, record, i);
    last = std::max(last, i);
}

// Moves the children at 1..last of the record down to 1..n (dropping the
// holes a nil in the scene's table left) and returns n.
int compact_children(lua_State* L, int record, lua_Integer last) {
    int n = 0;
    for (lua_Integer i = 1; i <= last; ++i) {
        if (lua_rawgeti(L, record, i) == LUA_TNIL) {
            lua_pop(L, 1);
            continue;
        }
        if (++n == i) {
            lua_pop(L, 1);
            continue;
        }
        lua_rawseti(L, record, n);
        lua_pushnil(L);
        lua_rawseti(L, record, i);
    }
    return n;
}

// Marks the record's children as attached; a node that appears twice
// undoes the marks and raises.
void attach_children(lua_State* L, const Where& at, const WidgetSpec& w, int record, int n) {
    for (int j = 1; j <= n; ++j) {
        lua_rawgeti(L, record, j);
        auto* child = static_cast<NodeHeader*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        if (child->attached) {
            for (int k = 1; k < j; ++k) {
                lua_rawgeti(L, record, k);
                static_cast<NodeHeader*>(lua_touserdata(L, -1))->attached = 0;
                lua_pop(L, 1);
            }
            raise(L, at.file, at.line, "ui.%s: the same widget appears twice", w.name.data());
        }
        child->attached = 1;
    }
}

// ui.<widget>{...}: upvalue 1 is the Widget. Checks every key and value,
// copying each into a fresh record as it goes (the record holds them
// strongly, so nothing the checks passed can disappear afterwards, even
// from a weak table), and only marks the children as attached once
// everything passed.
int construct(lua_State* L) {
    const WidgetSpec& w = widget_spec(static_cast<Widget>(lua_tointeger(L, lua_upvalueindex(1))));
    Where at;
    where(L, at);
    int t = lua_type(L, 1);
    if (t == LUA_TNONE || t == LUA_TNIL) {
        lua_settop(L, 0);
        lua_newtable(L);
    } else if (t != LUA_TTABLE) {
        return raise(L, at.file, at.line, "ui.%s: expected a table, got %s", w.name.data(),
                     lua_typename(L, t));
    }
    lua_settop(L, 1);
    auto* h = static_cast<NodeHeader*>(lua_newuserdatauv(L, sizeof(NodeHeader), 2));  // 2
    *h = NodeHeader{static_cast<uint16_t>(w.widget), 0, 0, at.line};
    luaL_setmetatable(L, kNodeType);
    lua_newtable(L);  // 3: the record
    lua_Integer last = 0;
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        if (lua_type(L, -2) == LUA_TSTRING)
            put_prop(L, at, w, 3);
        else if (lua_isinteger(L, -2))
            put_child(L, at, w, 3, last);
        else
            raise(L, at.file, at.line,
                  "ui.%s: keys must be property names or child positions, got %s", w.name.data(),
                  luaL_typename(L, -2));
        lua_pop(L, 1);
    }
    if (w.widget == Widget::List) {
        if (lua_rawgeti(L, 3, prop_key(Prop::Row)) == LUA_TNIL)
            raise(L, at.file, at.line, "ui.list: property 'row' is required");
        lua_pop(L, 1);
    }
    // Everything that can fail (the file string can raise a memory
    // error) comes before the children are marked: a scene that catches
    // the error can still use them.
    if (at.file[0] != '\0') {
        lua_pushstring(L, at.file);
        lua_setiuservalue(L, 2, 2);
    }
    int n = compact_children(L, 3, last);
    attach_children(L, at, w, 3, n);
    h->children = static_cast<uint16_t>(n);
    lua_setiuservalue(L, 2, 1);
    return 1;
}

// ui.show_if(s, node): node's hidden becomes bind(s, not v), or not s.
int show_if(lua_State* L) {
    Where at;
    where(L, at);
    lua_settop(L, 2);
    if (reactive_kind(L, 1) != nullptr) {
        lua_rawgetp(L, LUA_REGISTRYINDEX, &kNegateKey);
        lua_pushvalue(L, 1);
        lua_call(L, 1, 1);
    } else if (lua_type(L, 1) == LUA_TBOOLEAN) {
        lua_pushboolean(L, !lua_toboolean(L, 1));
    } else {
        return raise(L, at.file, at.line,
                     "ui.show_if: expected a state, computed, bind or boolean as the first "
                     "argument");
    }
    if (to_node(L, 2) == nullptr)
        return raise(L, at.file, at.line, "ui.show_if: expected a widget as the second argument");
    lua_getiuservalue(L, 2, 1);
    lua_insert(L, -2);
    lua_rawseti(L, -2, prop_key(Prop::Hidden));
    lua_settop(L, 2);
    return 1;
}

// Unknown keys of the ui table.
int unknown_widget(lua_State* L) {
    if (lua_type(L, 2) != LUA_TSTRING) return 0;
    Where at;
    where(L, at);
    std::array<std::string_view, kWidgetCount + 2> names{};
    for (size_t i = 0; i < kWidgetCount; ++i) names[i] = widget_name(static_cast<Widget>(i));
    names[kWidgetCount] = "show_if";
    names[kWidgetCount + 1] = "ref";
    char best[24];
    nearest_into(best, string_at(L, 2), names);
    char key[kMaxShown + 4];
    return raise(L, at.file, at.line, "unknown widget 'ui.%s' (did you mean 'ui.%s'?)",
                 shown(string_at(L, 2), key), best);
}

int ui_read_only(lua_State* L) { return luaL_error(L, "ui is read-only"); }

// --- ui.ref ---------------------------------------------------------------

int ref(lua_State* L) {
    if (lua_type(L, 1) != LUA_TSTRING || lua_rawlen(L, 1) < 1 || lua_rawlen(L, 1) > kMaxRefBytes)
        return luaL_error(L, "ui.ref: expected a name of 1 to %d bytes",
                          static_cast<int>(kMaxRefBytes));
    lua_newuserdatauv(L, 0, 1);
    luaL_setmetatable(L, kRefType);
    lua_pushvalue(L, 1);
    lua_setiuservalue(L, -2, 1);
    return 1;
}

struct Resolved {
    NodeId id;
    Widget widget;
};

// Every handle method starts with this, before it reads its arguments.
void check_handle(lua_State* L) {
    if (luaL_testudata(L, 1, kRefType) == nullptr)
        luaL_error(L, "ui.ref: call handle methods with ':' (h:value())");
}

// Resolves the handle at 1 in the tree collected last. Leaves its name at
// index `name` (the stack is cut to that). Refused while collect_tree()
// runs: the tables are half built, and an op pushed then would come
// before the Build that creates its widget.
Resolved resolve(lua_State* L, int name) {
    check_handle(L);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kCollectingKey);
    if (lua_toboolean(L, -1))
        luaL_error(L, "ui.ref: widgets cannot be used while the tree is built");
    lua_settop(L, name - 1);
    lua_getiuservalue(L, 1, 1);  // name
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kRefsKey);
    lua_pushvalue(L, name);
    if (lua_type(L, -2) != LUA_TTABLE || lua_rawget(L, -2) != LUA_TNUMBER)
        luaL_error(L, "ui.ref: no widget with ref '%s'", lua_tostring(L, name));
    Resolved r{static_cast<NodeId>(lua_tointeger(L, -1)), Widget::Screen};
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kNodesKey);
    lua_rawgeti(L, -1, r.id);
    if (NodeHeader* h = to_node(L, -1)) r.widget = static_cast<Widget>(h->widget);
    lua_settop(L, name);
    return r;
}

int unsupported(lua_State* L, int name, Widget w, const char* method) {
    return luaL_error(L, "ui.ref: '%s' (ui.%s) does not support %s", lua_tostring(L, name),
                      widget_name(w).data(), method);
}

int ref_value(lua_State* L) {
    Resolved r = resolve(L, 2);
    if (r.widget != Widget::Input && r.widget != Widget::Slider && r.widget != Widget::Checkbox &&
        r.widget != Widget::Switch)
        return luaL_error(L, "ui.ref: '%s' has no value", lua_tostring(L, 2));
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kValuesKey);
    lua_rawgeti(L, -1, r.id);
    return 1;
}

// Push a ScrollTo / Focus onto the scene's PatchBatch; false if there is
// none or it is full. The op only exists in these frames, so it is gone
// before the caller raises.
bool push_scroll_to(lua_State* L, NodeId node, uint32_t index) {
    PatchBatch* batch = patch_batch(L);
    return batch != nullptr && batch->push(ScrollTo{node, index}).is_ok();
}

bool push_focus(lua_State* L, NodeId node) {
    PatchBatch* batch = patch_batch(L);
    return batch != nullptr && batch->push(Focus{node}).is_ok();
}

int ref_scroll_to(lua_State* L) {
    check_handle(L);
    lua_Integer i = luaL_checkinteger(L, 2);
    Resolved r = resolve(L, 3);
    if (r.widget != Widget::List && r.widget != Widget::Scroll)
        return unsupported(L, 3, r.widget, "scroll_to");
    if (i < 1 || i > static_cast<lua_Integer>(UINT32_MAX))
        return luaL_error(L, "ui.ref: scroll_to expects an index from 1");
    if (!push_scroll_to(L, r.id, static_cast<uint32_t>(i - 1)))
        return luaL_error(L, "ui update too large");
    return 0;
}

int ref_focus(lua_State* L) {
    Resolved r = resolve(L, 2);
    if (r.widget != Widget::Input) return unsupported(L, 2, r.widget, "focus");
    if (!push_focus(L, r.id)) return luaL_error(L, "ui update too large");
    return 0;
}

// __wisp.set_value(node, v)
int set_value(lua_State* L) {
    if (!lua_isinteger(L, 1) || lua_tointeger(L, 1) < 0 || lua_tointeger(L, 1) > UINT32_MAX)
        return luaL_error(L, "set_value: bad node id");
    lua_settop(L, 2);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kValuesKey);
    lua_insert(L, 1);
    lua_rawset(L, 1);
    return 0;
}

// --- Installation -----------------------------------------------------------

void keep_internal(lua_State* L, int internal, const char* name, char* key) {
    lua_getfield(L, internal, name);
    if (lua_type(L, -1) != LUA_TFUNCTION) luaL_error(L, "ui: the prelude has no %s()", name);
    lua_rawsetp(L, LUA_REGISTRYINDEX, key);
}

void set_checked(lua_State* L, int table, const char* name, lua_CFunction fn) {
    lua_pushcfunction(L, fn);
    push_checked_function(L, -1);
    lua_setfield(L, table, name);
    lua_pop(L, 1);
}

// Runs under Vm::call().
int install(lua_State* L) {
    lua_settop(L, 0);
    if (!push_wisp_internal(L)) return luaL_error(L, "ui: the reactive prelude is not installed");
    keep_internal(L, 1, "kind", &kKindKey);
    keep_internal(L, 1, "watch", &kWatchKey);
    keep_internal(L, 1, "negate", &kNegateKey);
    set_checked(L, 1, "set_value", set_value);
    reset_tree_tables(L, 1);
    // Created here so that collect_tree() only ever changes its value,
    // which cannot fail.
    lua_pushboolean(L, 0);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kCollectingKey);

    new_binding_metatable(L, kNodeType);
    lua_pop(L, 1);

    new_binding_metatable(L, kRefType);  // 2
    lua_newtable(L);                     // 3
    lua_pushcfunction(L, ref_value);
    lua_setfield(L, 3, "value");
    lua_pushcfunction(L, ref_scroll_to);
    lua_setfield(L, 3, "scroll_to");
    lua_pushcfunction(L, ref_focus);
    lua_setfield(L, 3, "focus");
    check_c_functions(L, 3);
    lua_setfield(L, 2, "__index");
    lua_pop(L, 1);

    lua_createtable(L, 0, static_cast<int>(kWidgetCount) + 2);  // 2
    for (size_t i = 0; i < kWidgetCount; ++i) {
        lua_pushinteger(L, static_cast<lua_Integer>(i));
        lua_pushcclosure(L, construct, 1);
        lua_setfield(L, 2, widget_name(static_cast<Widget>(i)).data());
    }
    lua_pushcfunction(L, show_if);
    lua_setfield(L, 2, "show_if");
    lua_pushcfunction(L, ref);
    lua_setfield(L, 2, "ref");
    check_c_functions(L, 2);
    new_binding_metatable(L, kUiType);  // 3
    set_checked(L, 3, "__index", unknown_widget);
    set_checked(L, 3, "__newindex", ui_read_only);
    lua_setmetatable(L, 2);
    lua_setglobal(L, "ui");
    return 0;
}

}  // namespace

std::optional<ScriptError> install_ui(Vm& vm) {
    lua_State* L = vm.L();
    if (!lua_checkstack(L, 1)) return ScriptError{ScriptFault::Memory, "", 0, "stack overflow"};
    lua_pushcfunction(L, install);
    return vm.call(0, 0);
}

}  // namespace wisp::scene
