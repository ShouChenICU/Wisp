// wisp.device (scene_api.hpp). Every function here follows the longjmp rule
// (vm.hpp): Lua API calls that can raise happen only while the frame holds
// trivially destructible locals.
//
// list/get/call (device model spec 4.3) are thin adapters over
// core::DeviceService, which holds every check. Each checks its arguments
// with raising calls first; the service call, the argument JSON and the
// result Value live only in a do_*() helper, which pushes its result
// through protect() (so an out-of-memory error there unwinds no C++
// frame) and reports plain data back; only once the helper has returned
// does the binding raise again (a memory error or the VM's limits) or push
// nil, code.

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "scene_api.hpp"
#include "wisp/base/json.hpp"
#include "wisp/scene/lua_json.hpp"

namespace wisp::scene {

namespace {

// Registry key of the handler lists: a table indexed by DeviceEvent + 1,
// each a sequence of functions.
char kHandlersKey;

const char* shape_name(hal::ScreenShapeKind kind) {
    switch (kind) {
        case hal::ScreenShapeKind::Rect:
            return "rect";
        case hal::ScreenShapeKind::RoundedRect:
            return "rounded_rect";
        case hal::ScreenShapeKind::Circle:
            return "circle";
    }
    return "rect";
}

void set_int(lua_State* L, const char* key, int64_t v) {
    lua_pushinteger(L, static_cast<lua_Integer>(v));
    lua_setfield(L, -2, key);
}

// A circle's radius is half its smaller side; a rect has no corners.
int32_t corner_radius(const hal::ScreenShape& s) {
    switch (s.kind) {
        case hal::ScreenShapeKind::Rect:
            return 0;
        case hal::ScreenShapeKind::RoundedRect:
            return s.corner_radius;
        case hal::ScreenShapeKind::Circle:
            return std::min(s.width, s.height) / 2;
    }
    return 0;
}

// row_span(y) -> x1, x2, or nil for a row with no visible pixel or one
// off the screen. hal::row_span() is consistent with hal::visible() by
// construction, and scans one row: cheap enough to call per row.
int row_span(lua_State* L) {
    const lua_Integer y = luaL_checkinteger(L, 1);
    const hal::ScreenShape& shape = detail::api_of(L).screen;
    if (y < 0 || y >= shape.height) {
        lua_pushnil(L);
        return 1;
    }
    const hal::Span span = hal::row_span(shape, static_cast<int32_t>(y));
    if (span.empty()) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, span.x1);
    lua_pushinteger(L, span.x2);
    return 2;
}

int info(lua_State* L) {
    SceneApi& api = detail::api_of(L);
    const DeviceInfo i = api.device ? api.device() : DeviceInfo{};
    detail::push_device_info(L, i);
    return 1;
}

int on(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    int ev = -1;
    for (size_t k = 0; k < kDeviceEventCount; ++k)
        if (std::strcmp(name, device_event_name(static_cast<DeviceEvent>(k))) == 0)
            ev = static_cast<int>(k);
    if (ev < 0)
        return luaL_argerror(
            L, 1,
            lua_pushfstring(L, "unknown event '%s' (expected battery, minute or lang)", name));
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kHandlersKey);  // 3
    lua_rawgeti(L, 3, ev + 1);                         // 4
    lua_pushvalue(L, 2);
    lua_rawseti(L, 4, static_cast<lua_Integer>(lua_rawlen(L, 4)) + 1);
    detail::api_of(L).has_handlers[ev] = true;
    return 0;
}

// --- list / get / call ------------------------------------------------------

// A command's arguments are a few scalars (hal::kMaxCmdArgs of them), so
// their JSON is small; the cap bounds the text (PSRAM) and the Value tree
// parsed from it (default heap, internal RAM on the device). Past it, or
// nested at all, the arguments cannot match a schema: "invalid_argument".
constexpr size_t kMaxArgsBytes = 4096;
constexpr uint8_t kMaxArgsDepth = 2;

int fail(lua_State* L, const char* code) {
    lua_pushnil(L);
    lua_pushstring(L, code);
    return 2;
}

bool granted(const SceneApi& api, std::string_view id) {
    if (api.device_grant_all) return true;
    return std::find(api.device_grants.begin(), api.device_grants.end(), id) !=
           api.device_grants.end();
}

// What a do_*() helper leaves for its binding: nil, code; a VM limit hit
// while the arguments were encoded; or the protect() status of pushing
// the result (on failure the error object is on the stack).
struct Outcome {
    const char* code = nullptr;
    bool limits = false;
    int status = LUA_OK;
};

struct PushCtx {
    const Value* value;
    bool null_is_true;
};

void push_result(lua_State* L, void* ctx) {
    const auto* c = static_cast<const PushCtx*>(ctx);
    if (c->null_is_true && c->value->is_null()) {
        lua_pushboolean(L, 1);
        return;
    }
    detail::push_value(L, *c->value);
}

// Finishes a binding after its helper: raises again what must be raised,
// else returns nil, code or the one value the helper pushed.
int finish(lua_State* L, const Outcome& o) {
    if (o.limits) {
        Vm::of(L)->check_limits(L);
        return luaL_error(L, "wisp.device: limits reached");
    }
    if (o.status != LUA_OK) {
        Vm::of(L)->check_limits(L);
        return lua_error(L);
    }
    if (o.code != nullptr) return fail(L, o.code);
    return 1;
}

void push_with(lua_State* L, const Result<Value>& r, bool null_is_true, Outcome* o) {
    if (!r) {
        o->code = wisp::to_string(r.error().code);
        return;
    }
    PushCtx c{&r.value(), null_is_true};
    o->status = protect(L, push_result, &c);
}

void do_get(lua_State* L, const core::DeviceService& devices, std::string_view id,
            std::string_view prop, Outcome* o) {
    push_with(L, devices.get(std::string(id), std::string(prop)), false, o);
}

// The arguments at index 3 (a table, or nil for none) as a Value.
bool read_args(lua_State* L, Value* out, Outcome* o) {
    if (lua_isnoneornil(L, 3)) return true;
    os::LargeString json;
    const char* code = lua_to_json(L, 3, json, kMaxArgsBytes);
    if (code == kJsonLimits) {
        o->limits = true;
        return false;
    }
    // Mixed or non-string keys, a cycle, a function, too deep or too large.
    if (code != nullptr) {
        o->code = wisp::to_string(ErrorCode::InvalidArgument);
        return false;
    }
    auto v = from_json(std::string_view(json.data(), json.size()), kMaxArgsDepth);
    if (!v) {
        o->code = wisp::to_string(ErrorCode::InvalidArgument);
        return false;
    }
    *out = std::move(v.value());
    return true;
}

void do_call(lua_State* L, SceneApi& api, std::string_view id, std::string_view cmd, Outcome* o) {
    Value args;
    if (!read_args(L, &args, o)) return;
    const std::string holder = api.device_grant_all ? std::string("agent") : api.holder;
    push_with(L, api.devices->call(std::string(id), std::string(cmd), args, holder), true, o);
}

// The service's entries one at a time, each pushed through protect() and
// set into the table at `table`, which has room for kMaxListedDevices
// entries already, so setting one allocates nothing and cannot raise.
void do_list(lua_State* L, const core::DeviceService& devices, int table, Outcome* o) {
    Vm* vm = Vm::of(L);
    lua_Integer n = 0;
    devices.for_each([&](Value entry) {
        if (static_cast<size_t>(n) == kMaxListedDevices) return false;
        if (vm != nullptr && vm->past_limits()) {
            o->limits = true;
            return false;
        }
        PushCtx c{&entry, false};
        o->status = protect(L, push_result, &c);
        if (o->status != LUA_OK) return false;
        lua_rawseti(L, table, ++n);
        return true;
    });
}

// list() -> { {id, class, model, role, schema}, ... }
int list(lua_State* L) {
    SceneApi& api = detail::api_of(L);
    lua_settop(L, 0);
    luaL_checkstack(L, 4, "wisp.device.list");
    lua_createtable(L, api.devices != nullptr ? static_cast<int>(kMaxListedDevices) : 0, 0);
    if (api.devices == nullptr) return 1;
    Outcome o;
    do_list(L, *api.devices, 1, &o);
    if (o.limits || o.status != LUA_OK) return finish(L, o);
    lua_settop(L, 1);
    return 1;
}

// get(id, prop) -> value | nil, code
int get(lua_State* L) {
    size_t id_len = 0;
    size_t prop_len = 0;
    const char* id = luaL_checklstring(L, 1, &id_len);
    const char* prop = luaL_checklstring(L, 2, &prop_len);
    lua_settop(L, 2);
    SceneApi& api = detail::api_of(L);
    if (api.devices == nullptr) return fail(L, "unavailable");
    if (!granted(api, std::string_view(id, id_len))) return fail(L, "permission_denied");
    luaL_checkstack(L, 4, "wisp.device.get");
    Outcome o;
    do_get(L, *api.devices, std::string_view(id, id_len), std::string_view(prop, prop_len), &o);
    return finish(L, o);
}

// call(id, cmd, args) -> result | nil, code
int call(lua_State* L) {
    size_t id_len = 0;
    size_t cmd_len = 0;
    const char* id = luaL_checklstring(L, 1, &id_len);
    const char* cmd = luaL_checklstring(L, 2, &cmd_len);
    if (!lua_isnoneornil(L, 3)) luaL_checktype(L, 3, LUA_TTABLE);
    lua_settop(L, 3);
    SceneApi& api = detail::api_of(L);
    if (api.devices == nullptr) return fail(L, "unavailable");
    if (!granted(api, std::string_view(id, id_len))) return fail(L, "permission_denied");
    luaL_checkstack(L, kLuaJsonStackSlots + 4, "wisp.device.call");
    Outcome o;
    do_call(L, api, std::string_view(id, id_len), std::string_view(cmd, cmd_len), &o);
    return finish(L, o);
}

struct RunCtx {
    DeviceEvent ev;
    DeviceInfo info;
};

int run_entry(lua_State* L) {
    const auto* c = static_cast<const RunCtx*>(lua_touserdata(L, 1));
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kHandlersKey);        // 2
    lua_rawgeti(L, 2, static_cast<lua_Integer>(c->ev) + 1);  // 3
    // Handlers added by a handler wait for the next event.
    const lua_Unsigned n = lua_rawlen(L, 3);
    for (lua_Unsigned i = 1; i <= n; ++i) {
        lua_rawgeti(L, 3, static_cast<lua_Integer>(i));
        detail::push_device_info(L, c->info);
        lua_call(L, 1, 0);
    }
    return 0;
}

}  // namespace

const char* device_event_name(DeviceEvent ev) {
    switch (ev) {
        case DeviceEvent::Battery:
            return "battery";
        case DeviceEvent::Minute:
            return "minute";
        case DeviceEvent::Lang:
            return "lang";
    }
    return "?";
}

namespace detail {

void push_device_info(lua_State* L, const DeviceInfo& i) {
    luaL_checkstack(L, 3, "device info");
    lua_createtable(L, 0, 13);
    set_int(L, "width", i.shape.width);
    set_int(L, "height", i.shape.height);
    lua_pushstring(L, shape_name(i.shape.kind));
    lua_setfield(L, -2, "shape");
    set_int(L, "corner_radius", corner_radius(i.shape));
    lua_createtable(L, 0, 4);
    set_int(L, "x1", i.safe.x1);
    set_int(L, "y1", i.safe.y1);
    set_int(L, "x2", i.safe.x2);
    set_int(L, "y2", i.safe.y2);
    lua_setfield(L, -2, "safe_area");
    lua_createtable(L, 0, 4);
    set_int(L, "x1", i.status_area.x1);
    set_int(L, "y1", i.status_area.y1);
    set_int(L, "x2", i.status_area.x2);
    set_int(L, "y2", i.status_area.y2);
    lua_setfield(L, -2, "status_area");
    lua_pushstring(L, i.lang);
    lua_setfield(L, -2, "lang");
    set_int(L, "utc_offset", i.utc_offset_min);
    lua_pushboolean(L, i.time_24h ? 1 : 0);
    lua_setfield(L, -2, "time_24h");
    if (i.has_time) set_int(L, "time", i.time);
    if (i.has_battery) set_int(L, "battery", i.battery);
    lua_pushboolean(L, i.charging ? 1 : 0);
    lua_setfield(L, -2, "charging");
    lua_pushstring(L, i.wifi);
    lua_setfield(L, -2, "wifi");
}

void install_device(lua_State* L, int wisp) {
    wisp = lua_absindex(L, wisp);
    lua_createtable(L, static_cast<int>(kDeviceEventCount), 0);
    for (size_t k = 0; k < kDeviceEventCount; ++k) {
        lua_newtable(L);
        lua_rawseti(L, -2, static_cast<lua_Integer>(k) + 1);
    }
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kHandlersKey);

    lua_createtable(L, 0, 6);
    lua_pushcfunction(L, info);
    lua_setfield(L, -2, "info");
    lua_pushcfunction(L, row_span);
    lua_setfield(L, -2, "row_span");
    lua_pushcfunction(L, on);
    lua_setfield(L, -2, "on");
    lua_pushcfunction(L, list);
    lua_setfield(L, -2, "list");
    lua_pushcfunction(L, get);
    lua_setfield(L, -2, "get");
    lua_pushcfunction(L, call);
    lua_setfield(L, -2, "call");
    check_c_functions(L, -1);
    lua_setfield(L, wisp, "device");
}

}  // namespace detail

std::optional<ScriptError> run_device_handlers(Vm& vm, DeviceEvent ev, const DeviceInfo& info) {
    lua_State* L = vm.L();
    if (!lua_checkstack(L, 2)) return ScriptError{ScriptFault::Memory, "", 0, "stack overflow"};
    RunCtx ctx{ev, info};
    lua_pushcfunction(L, run_entry);
    lua_pushlightuserdata(L, &ctx);
    return vm.call(1, 0, Budget::Fresh);
}

}  // namespace wisp::scene
