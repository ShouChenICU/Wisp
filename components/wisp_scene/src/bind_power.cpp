// wisp.power (scene_api.hpp): a thin adapter over core::WakeLockRegistry.
// Every function here follows the longjmp rule (vm.hpp): the registry is
// called from helpers whose std::string temporaries are gone before any
// Lua API call that can raise.

#include "scene_api.hpp"

namespace wisp::scene {

namespace {

constexpr const char* kReason = "keep_screen_on";

void take_screen(SceneApi& api) {
    // The user may have released the lock from its reminder notice, and
    // its id may belong to another holder by now: take a new one.
    if (api.screen_lock != 0 && api.locks->holds(api.screen_lock, api.holder)) return;
    api.screen_lock = api.locks->acquire(api.holder, core::WakeLockKind::Screen, kReason);
}

void drop_screen(SceneApi& api) {
    if (api.screen_lock == 0) return;
    // Only this scene's own lock: an id released from a reminder may have
    // gone to another holder since.
    api.locks->release_if(api.screen_lock, api.holder);
    api.screen_lock = 0;
}

int fail(lua_State* L, const char* error) {
    lua_pushnil(L);
    lua_pushstring(L, error);
    return 2;
}

// keep_screen_on(on) -> true | nil, error
int keep_screen_on(lua_State* L) {
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    const bool on = lua_toboolean(L, 1) != 0;
    SceneApi& api = detail::api_of(L);
    // Spec 3.4: a scene in the background cannot keep the screen on.
    if (api.background) return fail(L, "not_in_background");
    if (api.locks == nullptr) return fail(L, "unavailable");
    if (on)
        take_screen(api);
    else
        drop_screen(api);
    lua_pushboolean(L, 1);
    return 1;
}

}  // namespace

namespace detail {

void install_power(lua_State* L, int wisp) {
    wisp = lua_absindex(L, wisp);
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, keep_screen_on);
    lua_setfield(L, -2, "keep_screen_on");
    check_c_functions(L, -1);
    lua_setfield(L, wisp, "power");
}

}  // namespace detail

}  // namespace wisp::scene
