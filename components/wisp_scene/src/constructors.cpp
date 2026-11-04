// The `ui` table (constructors, ui.show_if, ui.ref) and collect_tree();
// see wisp/scene/schema.hpp.
//
// Everything here that runs inside Lua follows the longjmp rule in
// vm.hpp: frames hold only trivially destructible locals (char buffers,
// lua_Debug, string_view arrays), and the C++ work on the Build happens
// in small helpers that call no Lua API that can raise.
//
// A node is a userdata (NodeHeader) with two user values: its record
// table and the file it was built in. The record keeps the children at
// 1..n and property p at -(p + 1). User values and a hidden metatable
// put both out of the scene's reach, so collect_tree() can trust what it
// walks: the properties were checked when the node was built, a node is
// a child of at most one parent (marked in its header), and only C code
// changes a record afterwards (ui.show_if).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

#include "wisp/scene/patch_batch.hpp"
#include "wisp/scene/reactive.hpp"
#include "wisp/scene/schema.hpp"

namespace wisp::scene {

namespace {

constexpr const char* kNodeType = "wisp.node";
constexpr const char* kRefType = "wisp.ref";
constexpr const char* kUiType = "wisp.ui";

// Registry keys (their addresses, as light userdata).
char kKindKey;
char kWatchKey;
char kNegateKey;
char kNodesKey;
char kRefsKey;
char kValuesKey;

constexpr size_t kMaxRefBytes = 64;
// Package paths are at most 128 bytes (spec 6.2).
constexpr size_t kMaxSrcBytes = 128;
constexpr size_t kMaxSeries = 4;
constexpr size_t kMaxSeriesPoints = 128;
// How far up the call stack a constructor looks for the scene line that
// called it (through pcall, for instance).
constexpr int kMaxWhereLevels = 8;
// How much of a scene-supplied name or value an error message quotes.
constexpr size_t kMaxShown = 40;

struct NodeHeader {
    uint16_t widget;
    uint16_t children;  // at 1..children of the record
    uint8_t attached;   // already the child of another node
    int32_t line;       // 0 if unknown
};

struct Where {
    char file[LUA_IDSIZE];
    int line;
};

lua_Integer prop_key(Prop p) { return -1 - static_cast<lua_Integer>(p); }

std::string_view widget_name(Widget w) { return widget_spec(w).name; }

// The scene line that called the running C function: the first Lua frame
// above it.
void where(lua_State* L, Where& at) {
    at.file[0] = '\0';
    at.line = 0;
    lua_Debug ar;
    for (int level = 1; level <= kMaxWhereLevels && lua_getstack(L, level, &ar); ++level) {
        if (!lua_getinfo(L, "Sl", &ar) || ar.currentline <= 0) continue;
        std::snprintf(at.file, sizeof(at.file), "%s", ar.short_src);
        at.line = ar.currentline;
        return;
    }
}

// Raises the formatted message, prefixed with "file:line: " when the
// position is known.
int raise(lua_State* L, const char* file, int line, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    lua_pushvfstring(L, fmt, ap);
    va_end(ap);
    if (file != nullptr && file[0] != '\0' && line > 0) {
        lua_pushfstring(L, "%s:%d: ", file, line);
        lua_insert(L, -2);
        lua_concat(L, 2);
    }
    return lua_error(L);
}

// Copies at most kMaxShown bytes of s into out (cut on a UTF-8 boundary,
// "..." marking the cut).
template <size_t N>
const char* shown(std::string_view s, char (&out)[N]) {
    static_assert(N >= kMaxShown + 4);
    size_t n = s.size();
    bool cut = n > kMaxShown;
    if (cut) {
        n = kMaxShown;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    }
    std::memcpy(out, s.data(), n);
    if (cut) {
        std::memcpy(out + n, "...", 3);
        n += 3;
    }
    out[n] = '\0';
    return out;
}

std::string_view string_at(lua_State* L, int idx) {
    size_t n = 0;
    const char* s = lua_tolstring(L, idx, &n);
    return {s, n};
}

// nearest_name() into a fixed buffer, so its std::string is gone before
// the caller raises.
template <size_t N>
void nearest_into(char (&out)[N], std::string_view wrong, std::span<const std::string_view> names) {
    std::string best = nearest_name(wrong, names);
    std::snprintf(out, N, "%s", best.c_str());
}

// --- Reactive objects -----------------------------------------------------

// "state", "computed" or "bind" if the value at idx is one, else nullptr.
// Calls the prelude's kind(), which runs no scene code.
const char* reactive_kind(lua_State* L, int idx) {
    if (lua_type(L, idx) != LUA_TTABLE) return nullptr;
    idx = lua_absindex(L, idx);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kKindKey);
    lua_pushvalue(L, idx);
    lua_call(L, 1, 1);
    const char* kind = nullptr;
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char* k = lua_tostring(L, -1);
        if (std::strcmp(k, "state") == 0)
            kind = "state";
        else if (std::strcmp(k, "computed") == 0)
            kind = "computed";
        else if (std::strcmp(k, "bind") == 0)
            kind = "bind";
    }
    lua_pop(L, 1);
    return kind;
}

// --- Value checks (none of these raise) -------------------------------------

enum class Check { Ok, WrongType, BadValue, TooLarge };

bool in(std::string_view s, std::span<const std::string_view> words) {
    return std::find(words.begin(), words.end(), s) != words.end();
}

// An integer, or a float with an integral value, within int32.
bool integral(lua_State* L, int idx, lua_Integer& out) {
    if (lua_isinteger(L, idx)) {
        out = lua_tointeger(L, idx);
    } else {
        lua_Number d = lua_tonumber(L, idx);
        if (!(d >= INT32_MIN && d <= INT32_MAX) || std::floor(d) != d) return false;
        out = static_cast<lua_Integer>(d);
    }
    return out >= INT32_MIN && out <= INT32_MAX;
}

bool is_hex_color(std::string_view s) {
    if (s.size() != 7 || s[0] != '#') return false;
    for (char c : s.substr(1))
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    return true;
}

// assets/<segments>.png, no empty, "." or ".." segment, no backslash.
bool is_asset_path(std::string_view s) {
    constexpr std::string_view prefix = "assets/", suffix = ".png";
    if (s.size() > kMaxSrcBytes || s.size() <= prefix.size() + suffix.size() ||
        s.substr(0, prefix.size()) != prefix || s.substr(s.size() - suffix.size()) != suffix)
        return false;
    std::string_view rest = s.substr(prefix.size());
    while (true) {
        size_t slash = rest.find('/');
        std::string_view seg = rest.substr(0, slash);
        if (seg.empty() || seg == "." || seg == ".." || seg.find('\\') != std::string_view::npos)
            return false;
        if (slash == std::string_view::npos) return true;
        rest = rest.substr(slash + 1);
    }
}

Check check_series(lua_State* L, int idx) {
    size_t n = lua_rawlen(L, idx);
    if (n > kMaxSeries) return Check::BadValue;
    for (size_t i = 1; i <= n; ++i) {
        lua_rawgeti(L, idx, static_cast<lua_Integer>(i));
        bool ok = lua_type(L, -1) == LUA_TTABLE;
        size_t points = ok ? lua_rawlen(L, -1) : 0;
        ok = ok && points <= kMaxSeriesPoints;
        for (size_t j = 1; ok && j <= points; ++j) {
            ok = lua_rawgeti(L, -1, static_cast<lua_Integer>(j)) == LUA_TNUMBER;
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        if (!ok) return Check::BadValue;
    }
    return Check::Ok;
}

// Checks a value that is not a reactive object. nil is accepted (only a
// bound value can be nil: it means "not set").
Check check_value(lua_State* L, int idx, Prop p) {
    const PropSpec& spec = prop_spec(p);
    int t = lua_type(L, idx);
    if (t == LUA_TNIL) return Check::Ok;
    if (t == LUA_TSTRING && lua_rawlen(L, idx) > kMaxPropStringBytes) return Check::TooLarge;
    lua_Integer i = 0;
    switch (spec.type) {
        case PropType::Size:
        case PropType::Pad:
            if (t == LUA_TNUMBER) return integral(L, idx, i) ? Check::Ok : Check::BadValue;
            if (t == LUA_TSTRING)
                return in(string_at(L, idx), spec.choices) ? Check::Ok : Check::BadValue;
            return Check::WrongType;
        case PropType::Int:
            if (t != LUA_TNUMBER) return Check::WrongType;
            if (!integral(L, idx, i)) return Check::BadValue;
            if ((p == Prop::Cols || p == Prop::RowH) && i < 1) return Check::BadValue;
            if (p == Prop::MaxLen && i < 0) return Check::BadValue;
            return Check::Ok;
        case PropType::Number:
            if (t != LUA_TNUMBER) return Check::WrongType;
            if (p == Prop::Opa) {
                lua_Number d = lua_tonumber(L, idx);
                if (!(d >= 0 && d <= 1)) return Check::BadValue;
            }
            return Check::Ok;
        case PropType::String:
            if (p == Prop::Ref) {
                if (t != LUA_TSTRING) return Check::WrongType;
                size_t n = lua_rawlen(L, idx);
                return n >= 1 && n <= kMaxRefBytes ? Check::Ok : Check::BadValue;
            }
            if (p == Prop::Src) {
                if (t != LUA_TSTRING) return Check::WrongType;
                return is_asset_path(string_at(L, idx)) ? Check::Ok : Check::BadValue;
            }
            return t == LUA_TSTRING || t == LUA_TNUMBER ? Check::Ok : Check::WrongType;
        case PropType::Bool:
            return t == LUA_TBOOLEAN ? Check::Ok : Check::WrongType;
        case PropType::Color:
            if (t != LUA_TSTRING) return Check::WrongType;
            return is_hex_color(string_at(L, idx)) || in(string_at(L, idx), spec.choices)
                       ? Check::Ok
                       : Check::BadValue;
        case PropType::Font:
        case PropType::Align:
        case PropType::Justify:
        case PropType::Enum:
            if (t != LUA_TSTRING) return Check::WrongType;
            return in(string_at(L, idx), spec.choices) ? Check::Ok : Check::BadValue;
        case PropType::Fn:
            return t == LUA_TFUNCTION ? Check::Ok : Check::WrongType;
        case PropType::Items:
            return t == LUA_TTABLE ? Check::Ok : Check::WrongType;
        case PropType::Series:
            if (t != LUA_TTABLE) return Check::WrongType;
            return check_series(L, idx);
    }
    return Check::WrongType;
}

// What the value at idx was, for "got ..." ("" when a table's content was
// wrong: the message says what it expects).
template <size_t N>
void describe_got(lua_State* L, int idx, Check c, const char* kind, char (&out)[N]) {
    char quoted[kMaxShown + 4];
    if (kind != nullptr) {
        std::snprintf(out, N, "%s", kind);
    } else if (c == Check::WrongType) {
        std::snprintf(out, N, "%s", lua_typename(L, lua_type(L, idx)));
    } else if (lua_type(L, idx) == LUA_TSTRING) {
        std::snprintf(out, N, "'%s'", shown(string_at(L, idx), quoted));
    } else if (lua_isinteger(L, idx)) {
        std::snprintf(out, N, "%lld", static_cast<long long>(lua_tointeger(L, idx)));
    } else if (lua_type(L, idx) == LUA_TNUMBER) {
        std::snprintf(out, N, "%.14g", static_cast<double>(lua_tonumber(L, idx)));
    } else {
        out[0] = '\0';
    }
}

int raise_prop(lua_State* L, const char* file, int line, Widget w, Prop p, Check c, int idx,
               const char* kind) {
    const char* wname = widget_name(w).data();
    const char* pname = prop_spec(p).name.data();
    if (c == Check::TooLarge)
        return raise(L, file, line, "ui.%s: property '%s': value too large", wname, pname);
    char got[kMaxShown + 8];
    describe_got(L, idx, c, kind, got);
    char hint[48] = "";
    if (p == Prop::Name && kind == nullptr && lua_type(L, idx) == LUA_TSTRING) {
        char best[24];
        nearest_into(best, string_at(L, idx), prop_spec(p).choices);
        std::snprintf(hint, sizeof(hint), " (did you mean '%s'?)", best);
    }
    const char* expects = prop_expects(p).data();
    if (got[0] == '\0')
        return raise(L, file, line, "ui.%s: property '%s' expects %s", wname, pname, expects);
    return raise(L, file, line, "ui.%s: property '%s' expects %s, got %s%s", wname, pname, expects,
                 got, hint);
}

// --- Nodes ----------------------------------------------------------------

NodeHeader* to_node(lua_State* L, int idx) {
    return static_cast<NodeHeader*>(luaL_testudata(L, idx, kNodeType));
}

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
    int n = compact_children(L, 3, last);
    attach_children(L, at, w, 3, n);
    h->children = static_cast<uint16_t>(n);
    lua_setiuservalue(L, 2, 1);
    if (at.file[0] != '\0') {
        lua_pushstring(L, at.file);
        lua_setiuservalue(L, 2, 2);
    }
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

// Resolves the handle at 1 in the tree collected last. Leaves its name at
// index `name` (the stack is cut to that).
Resolved resolve(lua_State* L, int name) {
    if (luaL_testudata(L, 1, kRefType) == nullptr)
        luaL_error(L, "ui.ref: call handle methods with ':' (h:value())");
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

// Fresh nodes, refs and values tables, in the registry (for C) and in the
// internal table at `internal` (for Lua).
void reset_tree_tables(lua_State* L, int internal) {
    struct Table {
        char* key;
        const char* name;
    };
    const Table tables[] = {{&kNodesKey, "nodes"}, {&kRefsKey, "refs"}, {&kValuesKey, "values"}};
    for (const Table& t : tables) {
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, t.key);
        lua_setfield(L, internal, t.name);
    }
}

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

// --- collect_tree -----------------------------------------------------------

struct Collect {
    Build* out;
    NodeId next;
    size_t count;
    size_t bytes;  // the PatchBatch's estimate once `out` is pushed
};

// Stack slots of collect_entry().
constexpr int kCtxSlot = 1;
constexpr int kRootSlot = 2;
constexpr int kNodesSlot = 3;
constexpr int kRefsSlot = 4;
constexpr int kValuesSlot = 5;
constexpr int kWalkBase = 5;  // (node, parent id) pairs above this

// Where the node at `node` was built.
template <size_t N>
int node_position(lua_State* L, int node, char (&file)[N]) {
    file[0] = '\0';
    if (lua_getiuservalue(L, node, 2) == LUA_TSTRING)
        std::snprintf(file, N, "%s", lua_tostring(L, -1));
    lua_pop(L, 1);
    return static_cast<NodeHeader*>(lua_touserdata(L, node))->line;
}

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
    NodeDesc& n = c->out->nodes.emplace_back();
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
            if ((type == PropType::Size || type == PropType::Int || type == PropType::Pad) &&
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
    c->out->nodes.back().props.emplace_back(static_cast<PropId>(p),
                                            to_value(L, idx, prop_spec(p).type));
    return true;
}

void add_events(Collect* c, uint32_t bits) { c->out->nodes.back().events |= bits; }

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
        // watch(node, prop, source) -> disposer, current value
        lua_rawgetp(L, LUA_REGISTRYINDEX, &kWatchKey);
        lua_pushinteger(L, id);
        lua_pushinteger(L, static_cast<lua_Integer>(p));
        lua_pushvalue(L, -4);
        lua_call(L, 3, 2);
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
        lua_pushvalue(L, -1);
        if (lua_rawget(L, kRefsSlot) != LUA_TNIL)
            raise_at_node(L, node, "duplicate ref '%s'", lua_tostring(L, -2));
        lua_pop(L, 1);
        lua_pushvalue(L, -1);
        lua_pushinteger(L, id);
        lua_rawset(L, kRefsSlot);
    }
    if (value_prop(w) == p && !lua_isnil(L, -1)) {
        lua_pushvalue(L, -1);
        lua_rawseti(L, kValuesSlot, id);
        has_value = true;
    }
    if (!add_prop(c, L, -1, p)) raise_at_node(L, node, "ui update too large");
    lua_pop(L, 1);
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
    NodeId id = c->next++;
    if (!begin_node(c, id, parent, w, h->line)) raise_at_node(L, node, "ui update too large");
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
        lua_rawseti(L, kValuesSlot, id);
    }
    lua_pushvalue(L, node);
    lua_rawseti(L, kNodesSlot, id);
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
    reset_tree_tables(L, lua_gettop(L));
    lua_pop(L, 1);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kNodesKey);   // kNodesSlot
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kRefsKey);    // kRefsSlot
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kValuesKey);  // kValuesSlot
    lua_pushvalue(L, kRootSlot);
    lua_pushinteger(L, 0);
    while (lua_gettop(L) > kWalkBase) {
        auto parent = static_cast<NodeId>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        visit(L, c, lua_gettop(L), parent);
    }
    return 0;
}

}  // namespace

std::optional<ScriptError> install_ui(Vm& vm) {
    lua_State* L = vm.L();
    if (!lua_checkstack(L, 1)) return ScriptError{ScriptFault::Memory, "", 0, "stack overflow"};
    lua_pushcfunction(L, install);
    return vm.call(0, 0);
}

std::optional<ScriptError> collect_tree(Vm& vm, int index, NodeId& next_id, Build& out) {
    lua_State* L = vm.L();
    index = lua_absindex(L, index);
    out.nodes.clear();
    if (!lua_checkstack(L, 3)) return ScriptError{ScriptFault::Memory, "", 0, "stack overflow"};
    PatchBatch* batch = patch_batch(L);
    Collect c{&out, next_id, 0, (batch ? batch->bytes() : 0) + kOpOverheadBytes};
    lua_pushcfunction(L, collect_entry);
    lua_pushlightuserdata(L, &c);
    lua_pushvalue(L, index);
    auto err = vm.call(2, 0, Budget::Continue);
    if (err) {
        out.nodes.clear();
        return err;
    }
    next_id = c.next;
    return std::nullopt;
}

bool push_node_prop(lua_State* L, NodeId node, Prop prop) {
    if (!lua_checkstack(L, 4)) return false;
    int top = lua_gettop(L);
    bool found = false;
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &kNodesKey) == LUA_TTABLE &&
        lua_rawgeti(L, -1, node) == LUA_TUSERDATA && lua_getiuservalue(L, -1, 1) == LUA_TTABLE)
        found = lua_rawgeti(L, -1, prop_key(prop)) != LUA_TNIL;
    else
        lua_pushnil(L);
    if (!found) lua_pushnil(L);
    lua_replace(L, top + 1);
    lua_settop(L, top + 1);
    return found;
}

}  // namespace wisp::scene
