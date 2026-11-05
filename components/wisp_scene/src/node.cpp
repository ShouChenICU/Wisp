#include "node.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "widget_data.hpp"
#include "wisp/core/zip.hpp"
#include "wisp/scene/patch_batch.hpp"
#include "wisp/scene/reactive.hpp"

namespace wisp::scene::node {

char kKindKey;
char kWatchKey;
char kNegateKey;
char kNodesKey;
char kRefsKey;
char kValuesKey;
char kCollectingKey;
char kWidgetKey;
char kResetWidgetsKey;

namespace {

constexpr size_t kMaxRefBytes = 64;
// The longest path a scene package can hold.
constexpr size_t kMaxSrcBytes = core::ZipLimits{}.max_path;

bool in(std::string_view s, std::span<const std::string_view> words) {
    return std::find(words.begin(), words.end(), s) != words.end();
}

bool in_range(lua_State* L, int idx, Prop p) {
    lua_Integer i = 0;
    if (!integral(L, idx, i)) return false;
    IntRange r = prop_range(p);
    return i >= r.min && i <= r.max;
}

// pad = {top, right, bottom, left}: exactly the keys 1 to 4, each an
// integer in pad's range. Never raises.
Check check_pad_sides(lua_State* L, int idx) {
    idx = lua_absindex(L, idx);
    if (!lua_checkstack(L, 3) || lua_rawlen(L, idx) != 4) return Check::BadValue;
    int keys = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        lua_pop(L, 1);
        if (++keys > 4) {
            lua_pop(L, 1);
            return Check::BadValue;
        }
    }
    if (keys != 4) return Check::BadValue;
    for (int i = 1; i <= 4; ++i) {
        lua_rawgeti(L, idx, i);
        const bool ok = lua_type(L, -1) == LUA_TNUMBER && in_range(L, -1, Prop::Pad);
        lua_pop(L, 1);
        if (!ok) return Check::BadValue;
    }
    return Check::Ok;
}

bool is_hex_color(std::string_view s) {
    if (s.size() != 7 || s[0] != '#') return false;
    for (char c : s.substr(1))
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    return true;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

// assets/<segments>.png, .jpg or .jpeg (the images the UI decodes), no
// empty, "." or ".." segment, no backslash.
bool is_asset_path(std::string_view s) {
    constexpr std::string_view prefix = "assets/";
    size_t suffix = ends_with(s, ".png") || ends_with(s, ".jpg") ? 4
                    : ends_with(s, ".jpeg")                      ? 5
                                                                 : 0;
    if (suffix == 0 || s.size() > kMaxSrcBytes || s.size() <= prefix.size() + suffix ||
        s.substr(0, prefix.size()) != prefix)
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
    if (!lua_checkstack(L, 2)) return Check::BadValue;
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

}  // namespace

NodeHeader* to_node(lua_State* L, int idx) {
    return static_cast<NodeHeader*>(luaL_testudata(L, idx, kNodeType));
}

void where(lua_State* L, Where& at) { scene_where(L, at.file, at.line); }

int node_position(lua_State* L, int node, char (&file)[LUA_IDSIZE]) {
    file[0] = '\0';
    if (lua_getiuservalue(L, node, 2) == LUA_TSTRING)
        std::snprintf(file, sizeof(file), "%s", lua_tostring(L, -1));
    lua_pop(L, 1);
    return static_cast<NodeHeader*>(lua_touserdata(L, node))->line;
}

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

std::string_view string_at(lua_State* L, int idx) {
    size_t n = 0;
    const char* s = lua_tolstring(L, idx, &n);
    return {s, n};
}

const char* shown(std::string_view s, char (&out)[kMaxShown + 4]) {
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

void nearest_into(char (&out)[24], std::string_view wrong,
                  std::span<const std::string_view> names) {
    std::string best = nearest_name(wrong, names);
    std::snprintf(out, sizeof(out), "%s", best.c_str());
}

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

int64_t pad_sides_value(lua_State* L, int idx) {
    idx = lua_absindex(L, idx);
    int32_t side[4] = {};
    for (int i = 0; i < 4; ++i) {
        lua_rawgeti(L, idx, i + 1);
        lua_Integer v = 0;
        if (integral(L, -1, v)) side[i] = static_cast<int32_t>(v);
        lua_pop(L, 1);
    }
    return encode_pad_sides(side[0], side[1], side[2], side[3]);
}

Check check_value(lua_State* L, int idx, Prop p) {
    const PropSpec& spec = prop_spec(p);
    int t = lua_type(L, idx);
    if (t == LUA_TNIL || t == LUA_TNONE) return Check::Ok;
    if (t == LUA_TSTRING && lua_rawlen(L, idx) > kMaxPropStringBytes) return Check::TooLarge;
    switch (spec.type) {
        case PropType::Size:
        case PropType::Pad:
            if (t == LUA_TNUMBER) return in_range(L, idx, p) ? Check::Ok : Check::BadValue;
            if (t == LUA_TSTRING)
                return in(string_at(L, idx), spec.choices) ? Check::Ok : Check::BadValue;
            if (t == LUA_TTABLE && spec.type == PropType::Pad) return check_pad_sides(L, idx);
            return Check::WrongType;
        case PropType::Int:
            if (t != LUA_TNUMBER) return Check::WrongType;
            return in_range(L, idx, p) ? Check::Ok : Check::BadValue;
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
            if (t == LUA_TNUMBER) return in_range(L, idx, p) ? Check::Ok : Check::BadValue;
            if (t != LUA_TSTRING) return Check::WrongType;
            return in(string_at(L, idx), spec.choices) ? Check::Ok : Check::BadValue;
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

void describe_problem(lua_State* L, int idx, Prop p, Check c, const char* kind, char* out,
                      size_t size) {
    const char* pname = prop_spec(p).name.data();
    if (c == Check::TooLarge) {
        std::snprintf(out, size, "property '%s': value too large", pname);
        return;
    }
    // What the value was ("" when a table's content was wrong: the
    // message says what it expects).
    char got[kMaxShown + 8];
    char quoted[kMaxShown + 4];
    if (kind != nullptr) {
        std::snprintf(got, sizeof(got), "%s", kind);
    } else if (c == Check::WrongType) {
        std::snprintf(got, sizeof(got), "%s", lua_typename(L, lua_type(L, idx)));
    } else if (lua_type(L, idx) == LUA_TSTRING) {
        std::snprintf(got, sizeof(got), "'%s'", shown(string_at(L, idx), quoted));
    } else if (lua_isinteger(L, idx)) {
        std::snprintf(got, sizeof(got), "%lld", static_cast<long long>(lua_tointeger(L, idx)));
    } else if (lua_type(L, idx) == LUA_TNUMBER) {
        std::snprintf(got, sizeof(got), "%.14g", static_cast<double>(lua_tonumber(L, idx)));
    } else {
        got[0] = '\0';
    }
    char hint[96] = "";
    if (p == Prop::Name && kind == nullptr && lua_type(L, idx) == LUA_TSTRING) {
        char best[24];
        nearest_into(best, string_at(L, idx), prop_spec(p).choices);
        std::snprintf(hint, sizeof(hint), " (did you mean '%s'?)", best);
    }
    // Scenes own the whole screen (AGENTS.md section 3), so no pad word
    // stands for the safe area: say how to pad by it instead.
    if (p == Prop::Pad && kind == nullptr && lua_type(L, idx) == LUA_TSTRING &&
        string_at(L, idx) == "safe")
        std::snprintf(hint, sizeof(hint),
                      " (\"safe\" was removed: compute the padding from "
                      "wisp.device.info().safe_area)");
    const char* expects = prop_expects(p).data();
    if (got[0] == '\0')
        std::snprintf(out, size, "property '%s' expects %s", pname, expects);
    else
        std::snprintf(out, size, "property '%s' expects %s, got %s%s", pname, expects, got, hint);
}

int raise_prop(lua_State* L, const char* file, int line, Widget w, Prop p, Check c, int idx,
               const char* kind) {
    char why[kPropErrorBytes];
    describe_problem(L, idx, p, c, kind, why, sizeof(why));
    return raise(L, file, line, "ui.%s: %s", widget_name(w).data(), why);
}

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

}  // namespace wisp::scene::node

namespace wisp::scene {

using namespace node;

bool prop_value_ok(lua_State* L, int idx, Prop prop, char* why, size_t why_size) {
    Check c = check_value(L, idx, prop);
    if (c == Check::Ok) return true;
    describe_problem(L, idx, prop, c, nullptr, why, why_size);
    return false;
}

bool bound_value_ok(lua_State* L, NodeId node, Prop prop, int idx, char* why, size_t why_size) {
    idx = lua_absindex(L, idx);
    Check c = check_value(L, idx, prop);
    if (c == Check::Ok) return true;
    char problem[kPropErrorBytes];
    describe_problem(L, idx, prop, c, nullptr, problem, sizeof(problem));
    int top = lua_gettop(L);
    if (lua_checkstack(L, 3) && lua_rawgetp(L, LUA_REGISTRYINDEX, &kNodesKey) == LUA_TTABLE &&
        lua_rawgeti(L, -1, node) == LUA_TUSERDATA) {
        // The nodes table holds only nodes.
        auto* h = static_cast<NodeHeader*>(lua_touserdata(L, -1));
        char file[LUA_IDSIZE];
        int line = node_position(L, lua_gettop(L), file);
        const char* wname = widget_name(static_cast<Widget>(h->widget)).data();
        if (file[0] != '\0' && line > 0)
            std::snprintf(why, why_size, "%s:%d: ui.%s: %s", file, line, wname, problem);
        else
            std::snprintf(why, why_size, "ui.%s: %s", wname, problem);
    } else {
        std::snprintf(why, why_size, "%s", problem);
    }
    lua_settop(L, top);
    return false;
}

}  // namespace wisp::scene
