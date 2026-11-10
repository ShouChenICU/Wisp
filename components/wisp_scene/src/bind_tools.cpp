// wisp.tools.register (scene_api.hpp; agent-tools spec 5.2): binds a
// function to a tool name the scene declared in scene.json's "tools"
// array. Every function here follows the longjmp rule (vm.hpp): the
// std::string_view held across a raise is trivially destructible, and
// the SceneApi::tool_names scan that decides whether to raise runs no
// Lua API call.

#include <string_view>

#include "scene_api.hpp"

namespace wisp::scene {

namespace {

// Registry key of the table of registered functions, by tool name.
char kToolsKey;

bool is_declared(const SceneApi& api, std::string_view name) {
    for (const std::string& n : api.tool_names) {
        if (n == name) return true;
    }
    return false;
}

// register(name, fn): name must be one scene.json declared and not yet
// bound; fn must be a function.
int register_tool(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    const std::string_view name(data, len);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!is_declared(detail::api_of(L), name)) {
        return luaL_argerror(L, 1, "not declared in scene.json's tools");
    }
    lua_settop(L, 2);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kToolsKey);  // 3: the tools table
    lua_pushvalue(L, 1);                            // 4: name
    lua_rawget(L, 3);
    const bool already = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (already) return luaL_argerror(L, 1, "already registered");
    lua_pushvalue(L, 1);  // name
    lua_pushvalue(L, 2);  // fn
    lua_rawset(L, 3);
    return 0;
}

}  // namespace

namespace detail {

void install_tools(lua_State* L, int wisp) {
    wisp = lua_absindex(L, wisp);
    lua_newtable(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kToolsKey);
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, register_tool);
    lua_setfield(L, -2, "register");
    check_c_functions(L, -1);
    lua_setfield(L, wisp, "tools");
}

bool push_tool(lua_State* L, std::string_view name) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kToolsKey);
    lua_pushlstring(L, name.data(), name.size());
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 2);
        return false;
    }
    lua_remove(L, -2);
    return true;
}

}  // namespace detail

}  // namespace wisp::scene
