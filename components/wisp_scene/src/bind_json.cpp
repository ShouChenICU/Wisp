// wisp.json (scene_api.hpp; agent-tools spec 4.2): lua_json.hpp's
// conversion, with failures returned as nil, code instead of raised.
//
// longjmp rule (vm.hpp): encode() holds the output string only in
// encode_into(), which pushes it through protect() so no error can unwind
// that frame; decode() decodes inside protect() as well, and any error that
// is the VM's limits (or memory) is raised again once nothing is held.

#include <string_view>

#include "scene_api.hpp"
#include "wisp/scene/lua_json.hpp"

namespace wisp::scene {

namespace {

struct Text {
    const char* data;
    size_t size;
};

void push_text(lua_State* L, void* ctx) {
    const auto* t = static_cast<const Text*>(ctx);
    lua_pushlstring(L, t->data, t->size);
}

// Encodes the value at 1. Returns the protect() status of pushing the
// result (LUA_OK with nothing pushed when *code is set).
int encode_into(lua_State* L, const char** code) {
    os::LargeString out;
    *code = lua_to_json(L, 1, out);
    if (*code != nullptr) return LUA_OK;
    Text t{out.data(), out.size()};
    return protect(L, push_text, &t);
}

int encode(lua_State* L) {
    luaL_checkany(L, 1);
    lua_settop(L, 1);
    luaL_checkstack(L, kLuaJsonStackSlots, "wisp.json");
    const char* code = nullptr;
    if (encode_into(L, &code) != LUA_OK) return lua_error(L);
    if (code == kJsonLimits) {
        Vm::of(L)->check_limits(L);
        return luaL_error(L, "wisp.json: limits reached");
    }
    if (code != nullptr) {
        lua_pushnil(L);
        lua_pushstring(L, code == kJsonTooLarge ? "too_large" : code);
        return 2;
    }
    return 1;
}

struct DecodeCtx {
    Text text;
    const char* kind;
};

void decode_text(lua_State* L, void* ctx) {
    auto* c = static_cast<DecodeCtx*>(ctx);
    decode_json(L, std::string_view(c->text.data, c->text.size), &c->kind);
}

int decode(lua_State* L) {
    luaL_checktype(L, 1, LUA_TSTRING);
    lua_settop(L, 1);
    DecodeCtx c{};
    c.text.data = lua_tolstring(L, 1, &c.text.size);
    const int status = protect(L, decode_text, &c);
    if (status == LUA_OK) return 1;
    Vm* vm = Vm::of(L);
    // Anything but the text's own fault (memory, the VM's limits) is
    // raised again.
    if (c.kind == nullptr || vm->past_limits()) {
        vm->check_limits(L);
        return lua_error(L);
    }
    lua_pushnil(L);
    lua_pushstring(L, c.kind);
    return 2;
}

}  // namespace

namespace detail {

void install_json(lua_State* L, int wisp) {
    wisp = lua_absindex(L, wisp);
    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, encode);
    lua_setfield(L, -2, "encode");
    lua_pushcfunction(L, decode);
    lua_setfield(L, -2, "decode");
    check_c_functions(L, -1);
    lua_setfield(L, wisp, "json");
}

}  // namespace detail

}  // namespace wisp::scene
