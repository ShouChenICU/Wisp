#include "wisp/scene/reactive.hpp"

#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

#include "node.hpp"
#include "wisp/scene/schema.hpp"

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
        case LUA_TTABLE:  // only a pad's sides get this far
            return PropValue{node::pad_sides_value(L, idx)};
        default:
            return PropValue{};
    }
}

// Adds the SetProp; false if the batch refused it. Its own frame, so
// every C++ object is gone before emit() raises.
bool add_patch(lua_State* L, PatchBatch* batch) {
    return batch
        ->set_prop(static_cast<NodeId>(lua_tointeger(L, 1)),
                   static_cast<PropId>(lua_tointeger(L, 2)), to_value(L, 3))
        .is_ok();
}

// emit(node, prop, value): upvalue 1 is the PatchBatch. Every check that
// can raise comes first, the C++ work happens in add_patch().
int emit(lua_State* L) {
    auto* batch = static_cast<PatchBatch*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!lua_isinteger(L, 1) || lua_tointeger(L, 1) < 0 || lua_tointeger(L, 1) > UINT32_MAX)
        return luaL_error(L, "emit: bad node id");
    if (!lua_isinteger(L, 2) || lua_tointeger(L, 2) < 0 || lua_tointeger(L, 2) > UINT16_MAX)
        return luaL_error(L, "emit: bad property id");
    int t = lua_type(L, 3);
    // A table only as pad = {top, right, bottom, left}, checked below.
    const bool pad_sides =
        t == LUA_TTABLE && lua_tointeger(L, 2) == static_cast<lua_Integer>(Prop::Pad);
    if (t != LUA_TNONE && t != LUA_TNIL && t != LUA_TBOOLEAN && t != LUA_TNUMBER &&
        t != LUA_TSTRING && !pad_sides)
        return luaL_error(L, "emit: unsupported property value (%s)", lua_typename(L, t));
    // Checked before the string is copied.
    if (t == LUA_TSTRING && lua_rawlen(L, 3) > kMaxPropStringBytes)
        return luaL_error(L, "value too large");
    // A property of the schema takes only what its constructor would have
    // taken; other ids pass (the UI ignores them, and only collect_tree()
    // makes watches, always with schema ids).
    lua_Integer prop = lua_tointeger(L, 2);
    if (prop < static_cast<lua_Integer>(kPropCount)) {
        char why[kPropErrorBytes];
        if (!bound_value_ok(L, static_cast<NodeId>(lua_tointeger(L, 1)), static_cast<Prop>(prop), 3,
                            why, sizeof(why))) {
            lua_pushstring(L, why);
            return lua_error(L);
        }
    }
    if (!add_patch(L, batch)) return luaL_error(L, "ui update too large");
    if (prop < static_cast<lua_Integer>(kPropCount))
        node::sync_value(L, static_cast<NodeId>(lua_tointeger(L, 1)), static_cast<Prop>(prop), 3);
    return 0;
}

// where(): "file:line" of the innermost Lua frame outside the prelude,
// or nil. Errors and labels made by the prelude point at scene code.
int where(lua_State* L) {
    char file[LUA_IDSIZE];
    int line = 0;
    if (!scene_where(L, file, line)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushfstring(L, "%s:%d", file, line);
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
    lua_pushinteger(L, static_cast<lua_Integer>(Prop::Pad));
    lua_call(L, 4, 4);  // 3 internal, 4 state, 5 computed, 6 bind
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
    if (!lua_checkstack(L, 1))
        return ScriptError{ScriptFault::Memory, std::string(), 0, "stack overflow"};
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &kFlushKey) != LUA_TFUNCTION) {
        lua_pop(L, 1);
        return std::nullopt;
    }
    auto err = vm.call(0, 0, Budget::Continue);
    if (err) {
        if (PatchBatch* batch = patch_batch(L)) batch->clear();
        // A position inside the prelude (an error it raised without one,
        // or a fault outside any scene frame) is not in any file of the
        // scene.
        if (err->file == kPreludeFile) {
            err->file.clear();
            err->line = 0;
        }
    }
    return err;
}

bool scene_where(lua_State* L, char (&file)[LUA_IDSIZE], int& line) {
    lua_Debug ar;
    for (int level = 1; lua_getstack(L, level, &ar); ++level) {
        if (!lua_getinfo(L, "Sl", &ar) || ar.currentline <= 0) continue;
        if (std::strcmp(ar.source, kPreludeChunk) == 0) continue;
        std::memcpy(file, ar.short_src, sizeof(file));
        line = ar.currentline;
        return true;
    }
    file[0] = '\0';
    line = 0;
    return false;
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
