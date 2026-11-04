// wisp.log (scene_api.hpp): the line print makes, at another level. The
// log goes to its RAM ring and its sinks (console, admin WebSocket), never
// to a file, so it is safe on the scene task.

#include "scene_api.hpp"

namespace wisp::scene {

namespace {

int log_info(lua_State* L) {
    log_lua_args(L, LogLevel::Info);
    return 0;
}

int log_warn(lua_State* L) {
    log_lua_args(L, LogLevel::Warn);
    return 0;
}

int log_error(lua_State* L) {
    log_lua_args(L, LogLevel::Error);
    return 0;
}

}  // namespace

namespace detail {

void install_log(lua_State* L, int wisp) {
    wisp = lua_absindex(L, wisp);
    lua_createtable(L, 0, 3);
    lua_pushcfunction(L, log_info);
    lua_setfield(L, -2, "info");
    lua_pushcfunction(L, log_warn);
    lua_setfield(L, -2, "warn");
    lua_pushcfunction(L, log_error);
    lua_setfield(L, -2, "error");
    check_c_functions(L, -1);
    lua_setfield(L, wisp, "log");
}

}  // namespace detail

}  // namespace wisp::scene
