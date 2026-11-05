// The C side of the list, canvas and chart effects (see build_list_rows()
// in wisp/scene/schema.hpp; the Lua side is W.widget() in prelude.cpp):
// W.list_reset, W.set_series, W.canvas_begin, W.canvas_end and the
// canvas's `g` object. Every function that runs inside Lua follows the
// longjmp rule in vm.hpp: the C++ work (building and pushing an op) runs
// in a helper that returns before anything is raised.

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "node.hpp"
#include "widget_data.hpp"
#include "wisp/scene/patch_batch.hpp"
#include "wisp/scene/reactive.hpp"
#include "wisp/scene/schema.hpp"
#include "wisp/scene/theme.hpp"

namespace wisp::scene {

namespace {

using namespace node;

constexpr const char* kGType = "wisp.canvas";

bool node_id_at(lua_State* L, int idx, NodeId& out) {
    if (!lua_isinteger(L, idx)) return false;
    lua_Integer i = lua_tointeger(L, idx);
    if (i < 0 || i > static_cast<lua_Integer>(UINT32_MAX)) return false;
    out = static_cast<NodeId>(i);
    return true;
}

// Raises bound_value_ok()'s message unless the value at `idx` suits
// `prop` of node `id`.
void check_bound(lua_State* L, NodeId id, Prop prop, int idx) {
    char why[kPropErrorBytes];
    if (bound_value_ok(L, id, prop, idx, why, sizeof(why))) return;
    lua_pushstring(L, why);
    lua_error(L);
}

int too_large(lua_State* L) { return luaL_error(L, "ui update too large"); }

bool push_op(lua_State* L, UiOp op) {
    PatchBatch* batch = patch_batch(L);
    return batch != nullptr && batch->push(std::move(op)).is_ok();
}

// --- Lists ------------------------------------------------------------------

// W.list_reset(id, items): ListReset{id, #items} (nil items: none).
int list_reset(lua_State* L) {
    lua_settop(L, 2);
    NodeId id = 0;
    if (!node_id_at(L, 1, id)) return luaL_error(L, "list_reset: bad node id");
    check_bound(L, id, Prop::Items, 2);
    size_t n = lua_type(L, 2) == LUA_TTABLE ? lua_rawlen(L, 2) : 0;
    auto count = static_cast<uint32_t>(std::min<size_t>(n, UINT32_MAX));
    if (!push_op(L, ListReset{id, count})) return too_large(L);
    return 0;
}

// --- Charts -----------------------------------------------------------------

// Encodes the checked series at `idx` (widget_data.hpp) into a Lua
// userdata, so no C++ object is alive while the Lua calls that read the
// table run (vm.hpp's longjmp rule), and leaves it on the stack. Needs
// three free stack slots.
void encode_series(lua_State* L, int idx) {
    size_t count = lua_type(L, idx) == LUA_TTABLE ? lua_rawlen(L, idx) : 0;
    count = std::min(count, kMaxSeries);
    size_t size = 1;
    for (size_t k = 1; k <= count; ++k) {
        lua_rawgeti(L, idx, static_cast<lua_Integer>(k));
        size += sizeof(uint16_t) +
                std::min<size_t>(lua_rawlen(L, -1), kMaxSeriesPoints) * sizeof(double);
        lua_pop(L, 1);
    }
    auto* out = static_cast<uint8_t*>(lua_newuserdatauv(L, size, 0));
    size_t at = 0;
    out[at++] = static_cast<uint8_t>(count);
    for (size_t k = 1; k <= count; ++k) {
        lua_rawgeti(L, idx, static_cast<lua_Integer>(k));
        auto n = static_cast<uint16_t>(std::min<size_t>(lua_rawlen(L, -1), kMaxSeriesPoints));
        std::memcpy(out + at, &n, sizeof(n));
        at += sizeof(n);
        for (uint16_t i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            double v = static_cast<double>(lua_tonumber(L, -1));
            lua_pop(L, 1);
            std::memcpy(out + at, &v, sizeof(v));
            at += sizeof(v);
        }
        lua_pop(L, 1);
    }
}

// Sets the series encoded in the userdata at the top of the stack. Makes
// no Lua call while its string exists.
bool push_series(lua_State* L, NodeId id) {
    const auto* bytes = static_cast<const char*>(lua_touserdata(L, -1));
    const size_t size = lua_rawlen(L, -1);
    PatchBatch* batch = patch_batch(L);
    return batch != nullptr && batch
                                   ->set_prop(id, static_cast<PropId>(Prop::Series),
                                              PropValue{os::LargeString(bytes, size)})
                                   .is_ok();
}

// W.set_series(id, series)
int set_series(lua_State* L) {
    lua_settop(L, 2);
    NodeId id = 0;
    if (!node_id_at(L, 1, id)) return luaL_error(L, "set_series: bad node id");
    check_bound(L, id, Prop::Series, 2);
    luaL_checkstack(L, 3, "series");
    encode_series(L, 2);
    if (!push_series(L, id)) return too_large(L);
    return 0;
}

// --- Canvas -----------------------------------------------------------------

// The `g` a draw(g) gets. Its texts are in user value 1 (index i + 1 for
// command i), so the strings stay alive until canvas_end() copies them;
// text_bytes is their total, counted as each is recorded.
struct GState {
    NodeId node;
    uint32_t count;
    uint8_t open;
    size_t text_bytes;
    DrawCommand cmds[kMaxDrawCommands];
};

// The batch size the CanvasDraw of g would take with `more` commands and
// `more_text` text bytes added, the way PatchBatch counts it.
size_t draw_op_bytes(const GState* g, size_t more, size_t more_text) {
    return kOpOverheadBytes + kDrawHeaderBytes + (g->count + more) * sizeof(DrawCommand) +
           g->text_bytes + more_text;
}

// Whether that CanvasDraw still fits the batch.
bool draw_fits(lua_State* L, const GState* g, size_t more, size_t more_text) {
    PatchBatch* batch = patch_batch(L);
    if (batch == nullptr) return false;
    const size_t used = batch->bytes();
    return used <= kMaxBatchBytes && draw_op_bytes(g, more, more_text) <= kMaxBatchBytes - used;
}

// Raises the formatted message (lua_pushfstring formats) at the scene
// line that called the running method.
int raise_here(lua_State* L, const char* fmt, ...) {
    Where at;
    where(L, at);
    va_list ap;
    va_start(ap, fmt);
    lua_pushvfstring(L, fmt, ap);
    va_end(ap);
    if (at.file[0] != '\0' && at.line > 0) {
        lua_pushfstring(L, "%s:%d: ", at.file, at.line);
        lua_insert(L, -2);
        lua_concat(L, 2);
    }
    return lua_error(L);
}

// The g at 1, open and with room for one more command.
GState* check_g(lua_State* L) {
    auto* g = static_cast<GState*>(luaL_testudata(L, 1, kGType));
    if (g == nullptr) raise_here(L, "canvas: call g's methods with ':' (g:line(...))");
    if (!g->open) raise_here(L, "canvas: g is only valid while draw(g) runs");
    if (g->count >= kMaxDrawCommands) raise_here(L, "canvas: more than 512 draw commands");
    return g;
}

int32_t clamp_coord(lua_Number d, int32_t lo = -kMaxDrawCoord, int32_t hi = kMaxDrawCoord) {
    if (std::isnan(d)) return 0;
    return static_cast<int32_t>(
        std::lround(std::clamp(d, static_cast<lua_Number>(lo), static_cast<lua_Number>(hi))));
}

// Reads `n` numbers from stack index 2 on into out.
void numbers(lua_State* L, const char* method, int n, int32_t* out) {
    for (int i = 0; i < n; ++i) {
        int idx = 2 + i;
        if (lua_type(L, idx) != LUA_TNUMBER)
            raise_here(L, "canvas: g:%s expects numbers (argument %d is %s)", method, i + 1,
                       luaL_typename(L, idx));
        out[i] = clamp_coord(lua_tonumber(L, idx));
    }
}

uint32_t color_at(lua_State* L, int idx, const char* what) {
    if (check_value(L, idx, Prop::Bg) != Check::Ok || lua_type(L, idx) != LUA_TSTRING) {
        char shown_value[kMaxShown + 4];
        char got[kMaxShown + 8];
        if (lua_type(L, idx) == LUA_TSTRING)
            std::snprintf(got, sizeof(got), "'%s'", shown(string_at(L, idx), shown_value));
        else
            std::snprintf(got, sizeof(got), "%s", luaL_typename(L, idx));
        raise_here(L, "canvas: %s expects %s, got %s", what, prop_expects(Prop::Bg).data(), got);
    }
    return theme_rgb(string_at(L, idx));
}

// Reads the options table at `idx` (absent or nil: defaults) into c.
void options(lua_State* L, int idx, const char* method, DrawCommand& c) {
    int t = lua_type(L, idx);
    if (t == LUA_TNONE || t == LUA_TNIL) return;
    if (t != LUA_TTABLE)
        raise_here(L, "canvas: g:%s expects an options table (argument %d is %s)", method, idx - 1,
                   luaL_typename(L, idx));
    if (lua_getfield(L, idx, "color") != LUA_TNIL) c.rgb = color_at(L, -1, "color");
    lua_pop(L, 1);
    const auto integer = [&](const char* key, int32_t lo, int32_t hi, int32_t& out) {
        int ft = lua_getfield(L, idx, key);
        if (ft == LUA_TNUMBER)
            out = clamp_coord(lua_tonumber(L, -1), lo, hi);
        else if (ft != LUA_TNIL)
            raise_here(L, "canvas: %s expects a number, got %s", key, luaL_typename(L, -1));
        lua_pop(L, 1);
    };
    integer("width", 0, 512, c.width);
    integer("radius", 0, 4096, c.radius);
    lua_getfield(L, idx, "fill");
    c.fill = lua_toboolean(L, -1) ? 1 : 0;
    lua_pop(L, 1);
    if (lua_getfield(L, idx, "font") != LUA_TNIL) {
        lua_Integer px = 0;
        if (check_value(L, -1, Prop::Font) != Check::Ok) {
            char shown_value[kMaxShown + 4];
            char got[kMaxShown + 8];
            if (lua_type(L, -1) == LUA_TSTRING)
                std::snprintf(got, sizeof(got), "'%s'", shown(string_at(L, -1), shown_value));
            else if (lua_isinteger(L, -1))
                std::snprintf(got, sizeof(got), "%lld",
                              static_cast<long long>(lua_tointeger(L, -1)));
            else if (lua_type(L, -1) == LUA_TNUMBER)
                std::snprintf(got, sizeof(got), "%.14g", static_cast<double>(lua_tonumber(L, -1)));
            else
                std::snprintf(got, sizeof(got), "%s", luaL_typename(L, -1));
            raise_here(L, "canvas: font expects %s, got %s", prop_expects(Prop::Font).data(), got);
        }
        if (lua_type(L, -1) == LUA_TSTRING)
            c.font_px = static_cast<uint8_t>(theme_font_px(string_at(L, -1)));
        else if (integral(L, -1, px))
            c.font_px = static_cast<uint8_t>(px);
    }
    lua_pop(L, 1);
}

// Appends c to g's commands.
int add(GState* g, const DrawCommand& c) {
    g->cmds[g->count++] = c;
    return 0;
}

DrawCommand command(DrawOp op) {
    DrawCommand c;
    c.op = op;
    return c;
}

int g_line(lua_State* L) {
    GState* g = check_g(L);
    DrawCommand c = command(DrawOp::Line);
    numbers(L, "line", 4, c.v);
    options(L, 6, "line", c);
    return add(g, c);
}

int g_circle(lua_State* L) {
    GState* g = check_g(L);
    DrawCommand c = command(DrawOp::Circle);
    numbers(L, "circle", 3, c.v);
    options(L, 5, "circle", c);
    return add(g, c);
}

int g_arc(lua_State* L) {
    GState* g = check_g(L);
    DrawCommand c = command(DrawOp::Arc);
    numbers(L, "arc", 5, c.v);
    options(L, 7, "arc", c);
    return add(g, c);
}

int g_rect(lua_State* L) {
    GState* g = check_g(L);
    DrawCommand c = command(DrawOp::Rect);
    numbers(L, "rect", 4, c.v);
    options(L, 6, "rect", c);
    return add(g, c);
}

int g_fill(lua_State* L) {
    GState* g = check_g(L);
    DrawCommand c = command(DrawOp::Fill);
    c.rgb = color_at(L, 2, "g:fill");
    return add(g, c);
}

int g_text(lua_State* L) {
    GState* g = check_g(L);
    DrawCommand c = command(DrawOp::Text);
    numbers(L, "text", 2, c.v);
    int t = lua_type(L, 4);
    if (t != LUA_TSTRING && t != LUA_TNUMBER)
        raise_here(L, "canvas: g:text expects a string (argument 3 is %s)", luaL_typename(L, 4));
    options(L, 5, "text", c);
    size_t len = 0;
    lua_tolstring(L, 4, &len);  // a number becomes its text
    if (len > kMaxPropStringBytes) raise_here(L, "canvas: g:text: value too large");
    // Counted against the batch now, so a draw that cannot be sent stops
    // here, before its texts add up to anything.
    if (!draw_fits(L, g, 1, len)) raise_here(L, "ui update too large");
    c.text_size = static_cast<uint32_t>(len);
    lua_getiuservalue(L, 1, 1);
    lua_pushvalue(L, 4);
    lua_rawseti(L, -2, static_cast<lua_Integer>(g->count) + 1);
    lua_pop(L, 1);
    g->text_bytes += len;
    return add(g, c);
}

// W.canvas_begin(id) -> g
int canvas_begin(lua_State* L) {
    NodeId id = 0;
    if (!node_id_at(L, 1, id)) return luaL_error(L, "canvas_begin: bad node id");
    auto* g = static_cast<GState*>(lua_newuserdatauv(L, sizeof(GState), 1));
    g->node = id;
    g->count = 0;
    g->open = 1;
    g->text_bytes = 0;
    luaL_setmetatable(L, kGType);
    lua_newtable(L);
    lua_setiuservalue(L, -2, 1);
    return 1;
}

// Builds the CanvasDraw of g from the texts in `texts` (the pointers
// read from g's text table, which keeps the strings alive) and pushes it.
// Makes no Lua call, and allocates nothing the batch would refuse.
bool push_canvas(lua_State* L, const GState* g, const char* const* texts) {
    if (!draw_fits(L, g, 0, 0)) return false;
    CanvasDraw draw;
    draw.canvas = g->node;
    draw.commands.resize(kDrawHeaderBytes + g->count * sizeof(DrawCommand) + g->text_bytes);
    uint8_t* out = draw.commands.data();
    std::memcpy(out, &g->count, sizeof(g->count));
    uint8_t* text_out = out + kDrawHeaderBytes + g->count * sizeof(DrawCommand);
    uint32_t offset = 0;
    for (uint32_t i = 0; i < g->count; ++i) {
        DrawCommand c = g->cmds[i];
        if (c.op == DrawOp::Text) {
            if (texts[i] != nullptr) std::memcpy(text_out + offset, texts[i], c.text_size);
            c.text_offset = offset;
            offset += c.text_size;
        }
        std::memcpy(out + kDrawHeaderBytes + i * sizeof(DrawCommand), &c, sizeof(c));
    }
    return push_op(L, std::move(draw));
}

// W.canvas_end(g): sends what g recorded and closes it.
int canvas_end(lua_State* L) {
    auto* g = static_cast<GState*>(luaL_testudata(L, 1, kGType));
    if (g == nullptr || !g->open) return luaL_error(L, "canvas_end: expected an open g");
    g->open = 0;
    lua_settop(L, 1);
    lua_getiuservalue(L, 1, 1);
    // The text pointers, read with every Lua call done before
    // push_canvas() builds anything. The strings stay in the table (at
    // stack slot 2) until this returns.
    auto* texts = static_cast<const char**>(
        lua_newuserdatauv(L, std::max<size_t>(g->count, 1) * sizeof(const char*), 0));
    for (uint32_t i = 0; i < g->count; ++i) {
        texts[i] = nullptr;
        if (g->cmds[i].op != DrawOp::Text) continue;
        lua_rawgeti(L, 2, static_cast<lua_Integer>(i) + 1);
        size_t len = 0;
        const char* s = lua_tolstring(L, -1, &len);
        if (s != nullptr && len >= g->cmds[i].text_size) texts[i] = s;
        lua_pop(L, 1);
    }
    if (!push_canvas(L, g, texts)) return too_large(L);
    return 0;
}

void set_checked(lua_State* L, int table, const char* name, lua_CFunction fn) {
    lua_pushcfunction(L, fn);
    push_checked_function(L, -1);
    lua_setfield(L, table, name);
    lua_pop(L, 1);
}

}  // namespace

namespace node {

void install_widgets(lua_State* L, int internal) {
    internal = lua_absindex(L, internal);
    set_checked(L, internal, "list_reset", list_reset);
    set_checked(L, internal, "set_series", set_series);
    set_checked(L, internal, "canvas_begin", canvas_begin);
    set_checked(L, internal, "canvas_end", canvas_end);

    new_binding_metatable(L, kGType);
    lua_newtable(L);
    const luaL_Reg methods[] = {{"line", g_line},  {"circle", g_circle}, {"arc", g_arc},
                                {"rect", g_rect},  {"fill", g_fill},     {"text", g_text},
                                {nullptr, nullptr}};
    luaL_setfuncs(L, methods, 0);
    check_c_functions(L, lua_gettop(L));
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

}  // namespace node

}  // namespace wisp::scene
