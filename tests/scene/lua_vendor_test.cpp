#include <gtest/gtest.h>

#include "lua.hpp"
#include "wisp/scene/version.hpp"

TEST(LuaVendor, ReleaseIs551) { EXPECT_STREQ(wisp::scene::lua_release(), "Lua 5.5.1"); }

// Scripts get Lua 5.5's `global` declarations: once a chunk declares any
// global, assigning an undeclared name is a compile-time error.
TEST(LuaVendor, GlobalDeclarationsRejectUndeclaredNames) {
    lua_State* L = luaL_newstate();
    ASSERT_NE(L, nullptr);
    const char src[] = "global<const> print; local a = 1\ny = 2";
    int rc = luaL_loadbufferx(L, src, sizeof(src) - 1, "=main.lua", "t");
    EXPECT_EQ(rc, LUA_ERRSYNTAX);
    const char* msg = lua_tostring(L, -1);
    ASSERT_NE(msg, nullptr);
    EXPECT_STREQ(msg, "main.lua:2: variable 'y' not declared");
    lua_close(L);
}
