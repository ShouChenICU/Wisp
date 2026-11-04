#include "wisp/scene/reactive.hpp"

#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

namespace wisp::scene {

// The prelude's Lua source (prelude.cpp).
std::string_view prelude_source();

namespace {

// Registry keys (their addresses, as light userdata).
char kInternalKey;
char kBatchKey;
char kFlushKey;

// The prelude's chunk name; where() skips its frames.
constexpr const char* kPreludeChunk = "=wisp.reactive";
constexpr std::string_view kPreludeFile = "wisp.reactive";

// Only called after the arguments were checked, so nothing here raises.
PropValue to_value(lua_State* L, int idx) {
    switch (lua_type(L, idx)) {
        case LUA_TBOOLEAN:
            return PropValue{lua_toboolean(L, idx) != 0};
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx))
                return PropValue{static_cast<int64_t>(lua_tointeger(L, idx))};
            return PropValue{static_cast<double>(lua_tonumber(L, idx))};
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            return PropValue{os::LargeString(s, len)};
        }
        default:
            return PropValue{};
    }
}

// emit(node, prop, value): upvalue 1 is the PatchBatch. Every check that
// can raise comes first; the PropValue is made after the last of them.
int emit(lua_State* L) {
    auto* batch = static_cast<PatchBatch*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!lua_isinteger(L, 1) || lua_tointeger(L, 1) < 0 || lua_tointeger(L, 1) > UINT32_MAX)
        return luaL_error(L, "emit: bad node id");
    if (!lua_isinteger(L, 2) || lua_tointeger(L, 2) < 0 || lua_tointeger(L, 2) > UINT16_MAX)
        return luaL_error(L, "emit: bad property id");
    int t = lua_type(L, 3);
    if (t != LUA_TNONE && t != LUA_TNIL && t != LUA_TBOOLEAN && t != LUA_TNUMBER &&
        t != LUA_TSTRING)
        return luaL_error(L, "emit: unsupported property value (%s)", lua_typename(L, t));
    batch->set_prop(static_cast<NodeId>(lua_tointeger(L, 1)),
                    static_cast<PropId>(lua_tointeger(L, 2)), to_value(L, 3));
    return 0;
}

// where(): "file:line" of the innermost Lua frame outside the prelude,
// or nil. Errors and labels made by the prelude point at scene code.
int where(lua_State* L) {
    lua_Debug ar;
    for (int level = 1; lua_getstack(L, level, &ar); ++level) {
        if (!lua_getinfo(L, "Sl", &ar) || ar.currentline <= 0) continue;
        if (std::strcmp(ar.source, kPreludeChunk) == 0) continue;
        lua_pushfstring(L, "%s:%d", ar.short_src, ar.currentline);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

// tcall(f, ...): like pcall with one result, but not yieldable (a plain
// lua_pcall), so a computed that yields fails instead of leaving the
// tracking state half-way. Upvalue 1 is the Vm, whose limits are checked
// again on return so a fault never comes back as a result.
int tcall(lua_State* L) {
    auto* vm = static_cast<Vm*>(lua_touserdata(L, lua_upvalueindex(1)));
    int status = lua_pcall(L, lua_gettop(L) - 1, 1, 0);
    vm->check_limits(L);
    lua_pushboolean(L, status == LUA_OK);
    lua_insert(L, -2);
    return 2;
}

// Pushes the C function on top as its checked copy, in its place.
void replace_with_checked(lua_State* L) {
    push_checked_function(L, -1);
    lua_replace(L, -2);
}

// Runs under Vm::call(): 1 = PatchBatch, 2 = Vm (light userdata).
int install(lua_State* L) {
    lua_settop(L, 2);
    std::string_view src = prelude_source();
    if (luaL_loadbufferx(L, src.data(), src.size(), kPreludeChunk, "t") != LUA_OK)
        return lua_error(L);  // 3
    lua_pushvalue(L, 1);
    lua_pushcclosure(L, emit, 1);
    replace_with_checked(L);  // 4
    lua_pushcfunction(L, where);
    replace_with_checked(L);  // 5
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, tcall, 1);
    replace_with_checked(L);  // 6
    lua_call(L, 3, 4);        // 3 internal, 4 state, 5 computed, 6 bind
    if (lua_type(L, 3) != LUA_TTABLE) return luaL_error(L, "reactive prelude: no internal table");
    check_c_functions(L, 3);
    lua_setglobal(L, "bind");
    lua_setglobal(L, "computed");
    lua_setglobal(L, "state");
    lua_getfield(L, 3, "flush");
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kFlushKey);
    lua_pushvalue(L, 3);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kInternalKey);
    lua_pushvalue(L, 1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kBatchKey);
    return 0;
}

}  // namespace

std::optional<ScriptError> install_prelude(Vm& vm, PatchBatch& out) {
    lua_State* L = vm.L();
    if (!lua_checkstack(L, 3))
        return ScriptError{ScriptFault::Memory, std::string(kPreludeFile), 0, "stack overflow"};
    // None of these allocate (light C function, light userdata).
    lua_pushcfunction(L, install);
    lua_pushlightuserdata(L, &out);
    lua_pushlightuserdata(L, &vm);
    return vm.call(2, 0);
}

std::optional<ScriptError> flush_reactive(Vm& vm) {
    lua_State* L = vm.L();
    if (!lua_checkstack(L, 1)) return std::nullopt;
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &kFlushKey) != LUA_TFUNCTION) {
        lua_pop(L, 1);
        return std::nullopt;
    }
    auto err = vm.call(0, 0, Budget::Continue);
    if (err) {
        if (PatchBatch* batch = patch_batch(L)) batch->clear();
        // An error the prelude raises without a position (or a fault
        // outside any scene frame) is not in any file of the scene.
        if (err->line == 0 && err->file == kPreludeFile) err->file.clear();
    }
    return err;
}

bool push_wisp_internal(lua_State* L) {
    if (!lua_checkstack(L, 1)) return false;
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &kInternalKey) == LUA_TTABLE) return true;
    lua_pop(L, 1);
    return false;
}

PatchBatch* patch_batch(lua_State* L) {
    if (!lua_checkstack(L, 1)) return nullptr;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kBatchKey);
    auto* batch = static_cast<PatchBatch*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return batch;
}

}  // namespace wisp::scene
