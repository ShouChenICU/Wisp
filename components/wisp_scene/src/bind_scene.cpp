// wisp.scene and install_api() (scene_api.hpp). Every function here follows
// the longjmp rule (vm.hpp): Lua API calls that can raise happen only while
// the frame holds trivially destructible locals (references into the
// SceneApi's Value and map iterators are fine).

#include "scene_api.hpp"

namespace wisp::scene {

namespace {

// Registry key of the SceneApi (light userdata).
char kApiKey;

// Open arguments come from the admin API or the agent as JSON; this bounds
// the C recursion that copies them.
constexpr int kMaxArgsDepth = 32;

void push_value(lua_State* L, const Value& v, int depth) {
    if (depth > kMaxArgsDepth) luaL_error(L, "scene args nested too deeply");
    luaL_checkstack(L, 3, "scene args");
    switch (v.type()) {
        case Value::Type::Null:
            lua_pushnil(L);
            return;
        case Value::Type::Bool:
            lua_pushboolean(L, v.as_bool() ? 1 : 0);
            return;
        case Value::Type::Int:
            lua_pushinteger(L, static_cast<lua_Integer>(v.as_int()));
            return;
        case Value::Type::Double:
            lua_pushnumber(L, static_cast<lua_Number>(v.as_double()));
            return;
        case Value::Type::String: {
            const std::string& s = v.as_string();
            lua_pushlstring(L, s.data(), s.size());
            return;
        }
        case Value::Type::List: {
            const List& list = v.as_list();
            lua_createtable(L, static_cast<int>(list.size()), 0);
            lua_Integer i = 0;
            for (const Value& item : list) {
                push_value(L, item, depth + 1);
                lua_rawseti(L, -2, ++i);
            }
            return;
        }
        case Value::Type::Map: {
            const Map& map = v.as_map();
            lua_createtable(L, 0, static_cast<int>(map.size()));
            for (const auto& [key, item] : map) {
                lua_pushlstring(L, key.data(), key.size());
                push_value(L, item, depth + 1);
                lua_rawset(L, -3);
            }
            return;
        }
    }
    lua_pushnil(L);
}

int close(lua_State* L) {
    detail::api_of(L).close_requested = true;
    return 0;
}

int exit_scene(lua_State* L) {
    detail::api_of(L).exit_requested = true;
    return 0;
}

int state(lua_State* L) {
    const SceneApi& api = detail::api_of(L);
    lua_pushstring(L, api.headless ? "headless" : api.background ? "background" : "foreground");
    return 1;
}

int launch(lua_State* L) {
    lua_pushstring(L, detail::api_of(L).launch);
    return 1;
}

int args(lua_State* L) {
    const Value* a = detail::api_of(L).args;
    if (a == nullptr || a->type() != Value::Type::Map) {
        // Scenes index it as a table: anything but a map is no arguments.
        lua_newtable(L);
        return 1;
    }
    push_value(L, *a, 1);
    return 1;
}

int install(lua_State* L) {
    auto* api = static_cast<SceneApi*>(lua_touserdata(L, 1));
    lua_settop(L, 0);
    lua_pushlightuserdata(L, api);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kApiKey);
    if (lua_getglobal(L, "wisp") != LUA_TTABLE) return luaL_error(L, "wisp table missing");  // 1
    detail::install_timer(L, 1);
    detail::install_device(L, 1);
    detail::install_log(L, 1);
    detail::install_scene(L, 1);
    detail::install_store(L, 1);
    detail::install_http(L, 1);
    detail::install_power(L, 1);
    detail::install_notify(L, 1);
    detail::install_sensor(L, 1);
    detail::install_files(L, 1);
    detail::install_json(L, 1);
    detail::install_tools(L, 1);
    return 0;
}

}  // namespace

namespace detail {

SceneApi& api_of(lua_State* L) { return *find_api(L); }

void push_value(lua_State* L, const Value& v) { wisp::scene::push_value(L, v, 1); }

SceneApi* find_api(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kApiKey);
    auto* api = static_cast<SceneApi*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return api;
}

void install_scene(lua_State* L, int wisp) {
    wisp = lua_absindex(L, wisp);
    lua_createtable(L, 0, 5);
    lua_pushcfunction(L, close);
    lua_setfield(L, -2, "close");
    lua_pushcfunction(L, exit_scene);
    lua_setfield(L, -2, "exit");
    lua_pushcfunction(L, args);
    lua_setfield(L, -2, "args");
    lua_pushcfunction(L, state);
    lua_setfield(L, -2, "state");
    lua_pushcfunction(L, launch);
    lua_setfield(L, -2, "launch");
    check_c_functions(L, -1);
    lua_setfield(L, wisp, "scene");
}

}  // namespace detail

std::optional<ScriptError> install_api(Vm& vm, SceneApi& api) {
    lua_State* L = vm.L();
    if (!lua_checkstack(L, 2)) return ScriptError{ScriptFault::Memory, "", 0, "stack overflow"};
    lua_pushcfunction(L, install);
    lua_pushlightuserdata(L, &api);
    return vm.call(1, 0);
}

}  // namespace wisp::scene
