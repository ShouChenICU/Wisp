// wisp.tools.register and detail::push_tool (scene_api.hpp; agent-tools
// spec 5.2) on a bare Vm with install_api().

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>

#include "scene_api.hpp"
#include "wisp/scene/vm.hpp"

namespace {

using wisp::scene::SceneApi;
using wisp::scene::Vm;

class ToolsBinding : public ::testing::Test {
protected:
    void SetUp() override {
        api.tool_names = {"today", "forecast"};
        vm = Vm::create("t");
        ASSERT_TRUE(vm);
        ASSERT_FALSE(wisp::scene::install_api(*vm, api));
    }

    // Runs `src` and returns its one result through tostring, or the error.
    std::string eval(const std::string& src) {
        if (auto err = vm->load("t", src)) return "compile: " + err->text();
        if (auto err = vm->call(0, 1)) return "error: " + err->message;
        size_t n = 0;
        const char* s = luaL_tolstring(vm->L(), -1, &n);
        std::string out(s, n);
        lua_pop(vm->L(), 2);
        return out;
    }

    SceneApi api;
    std::unique_ptr<Vm> vm;
};

TEST_F(ToolsBinding, Register) {
    // A declared name may be registered.
    EXPECT_EQ(eval("wisp.tools.register('today', function() end) return 'ok'"), "ok");
    // Registering it again is a duplicate.
    EXPECT_EQ(eval("return pcall(wisp.tools.register, 'today', function() end)"), "false");
    EXPECT_EQ(eval("local ok, e = pcall(wisp.tools.register, 'today', function() end) "
                   "return tostring(e:find('already registered') ~= nil)"),
              "true");
    // A name scene.json did not declare.
    EXPECT_EQ(eval("return pcall(wisp.tools.register, 'nope', function() end)"), "false");
    EXPECT_EQ(eval("local ok, e = pcall(wisp.tools.register, 'nope', function() end) "
                   "return tostring(e:find('declared') ~= nil)"),
              "true");
    // fn must be a function.
    EXPECT_EQ(eval("return pcall(wisp.tools.register, 'forecast', 1)"), "false");
}

TEST_F(ToolsBinding, NotRegistered) {
    lua_State* L = vm->L();
    const int top = lua_gettop(L);
    // Declared ("forecast" is in api.tool_names) but never registered.
    EXPECT_FALSE(wisp::scene::detail::push_tool(L, "forecast"));
    EXPECT_EQ(lua_gettop(L), top);

    ASSERT_EQ(eval("wisp.tools.register('today', function() return 42 end) return 'ok'"), "ok");
    EXPECT_TRUE(wisp::scene::detail::push_tool(L, "today"));
    ASSERT_EQ(lua_gettop(L), top + 1);
    EXPECT_EQ(lua_type(L, -1), LUA_TFUNCTION);
    lua_pop(L, 1);
}

}  // namespace
