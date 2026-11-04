// wisp.device (scene_api.hpp). Every function here follows the longjmp rule
// (vm.hpp): Lua API calls that can raise happen only while the frame holds
// trivially destructible locals.

#include <cstring>

#include "scene_api.hpp"

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
    lua_createtable(L, 0, 9);
    set_int(L, "width", i.shape.width);
    set_int(L, "height", i.shape.height);
    lua_pushstring(L, shape_name(i.shape.kind));
    lua_setfield(L, -2, "shape");
    lua_createtable(L, 0, 4);
    set_int(L, "x1", i.safe.x1);
    set_int(L, "y1", i.safe.y1);
    set_int(L, "x2", i.safe.x2);
    set_int(L, "y2", i.safe.y2);
    lua_setfield(L, -2, "safe_area");
    lua_pushstring(L, i.lang);
    lua_setfield(L, -2, "lang");
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

    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, info);
    lua_setfield(L, -2, "info");
    lua_pushcfunction(L, on);
    lua_setfield(L, -2, "on");
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
