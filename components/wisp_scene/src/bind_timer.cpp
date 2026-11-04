// wisp.timer (scene_api.hpp). Every function here follows the longjmp rule
// (vm.hpp): Lua API calls that can raise happen only while the frame holds
// trivially destructible locals.

#include <cstdint>

#include "scene_api.hpp"

namespace wisp::scene {

namespace {

// Registry key of the table of callbacks, by timer id.
char kCallbacksKey;
constexpr const char* kHandleType = "wisp.timer";

enum class AddResult : uint8_t { Ok, TooSmall, TooMany };

// The C++ part of after()/every(); its Result is gone before the caller
// touches Lua again.
AddResult add_timer(SceneApi& api, bool repeat, uint32_t ms, uint32_t* id) {
    const uint64_t now = api.now_ms();
    auto r = repeat ? api.timers.every(ms, now) : api.timers.after(ms, now);
    if (r) {
        *id = r.value();
        return AddResult::Ok;
    }
    return r.error().code == ErrorCode::InvalidArgument ? AddResult::TooSmall : AddResult::TooMany;
}

int add(lua_State* L, bool repeat) {
    const lua_Integer ms = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (ms > static_cast<lua_Integer>(UINT32_MAX)) return luaL_argerror(L, 1, "interval too large");
    lua_settop(L, 2);
    // The handle is made first: once the timer is set, nothing below
    // allocates but the callback's slot.
    auto* handle = static_cast<uint32_t*>(lua_newuserdatauv(L, sizeof(uint32_t), 0));  // 3
    luaL_setmetatable(L, kHandleType);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kCallbacksKey);  // 4
    uint32_t id = 0;
    const AddResult r = ms < 0
                            ? AddResult::TooSmall
                            : add_timer(detail::api_of(L), repeat, static_cast<uint32_t>(ms), &id);
    if (r != AddResult::Ok) {
        lua_pushnil(L);
        lua_pushstring(L, r == AddResult::TooSmall ? "interval_too_small" : "too_many_timers");
        return 2;
    }
    lua_pushvalue(L, 2);
    lua_rawseti(L, 4, id);
    *handle = id;
    lua_pushvalue(L, 3);
    return 1;
}

int after(lua_State* L) { return add(L, false); }
int every(lua_State* L) { return add(L, true); }

int cancel(lua_State* L) {
    const uint32_t id = *static_cast<uint32_t*>(luaL_checkudata(L, 1, kHandleType));
    detail::api_of(L).timers.cancel(id);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kCallbacksKey);
    const bool pending = lua_rawgeti(L, -1, id) != LUA_TNIL;
    lua_pop(L, 1);
    if (pending) {
        lua_pushnil(L);
        lua_rawseti(L, -2, id);
    }
    lua_pushboolean(L, pending ? 1 : 0);
    return 1;
}

struct RunCtx {
    uint32_t id;
    bool once;
};

int run_entry(lua_State* L) {
    const auto* c = static_cast<const RunCtx*>(lua_touserdata(L, 1));
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kCallbacksKey);        // 2
    if (lua_rawgeti(L, 2, c->id) != LUA_TFUNCTION) return 0;  // 3: cancelled
    if (c->once) {
        lua_pushnil(L);
        lua_rawseti(L, 2, c->id);
    }
    lua_call(L, 0, 0);
    return 0;
}

}  // namespace

namespace detail {

void install_timer(lua_State* L, int wisp) {
    wisp = lua_absindex(L, wisp);
    lua_newtable(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kCallbacksKey);

    new_binding_metatable(L, kHandleType);
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, cancel);
    lua_setfield(L, -2, "cancel");
    check_c_functions(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, after);
    lua_setfield(L, -2, "after");
    lua_pushcfunction(L, every);
    lua_setfield(L, -2, "every");
    check_c_functions(L, -1);
    lua_setfield(L, wisp, "timer");
}

}  // namespace detail

std::optional<ScriptError> run_timer(Vm& vm, uint32_t id, bool once) {
    lua_State* L = vm.L();
    if (!lua_checkstack(L, 2)) return ScriptError{ScriptFault::Memory, "", 0, "stack overflow"};
    RunCtx ctx{id, once};
    lua_pushcfunction(L, run_entry);
    lua_pushlightuserdata(L, &ctx);
    return vm.call(1, 0, Budget::Fresh);
}

}  // namespace wisp::scene
