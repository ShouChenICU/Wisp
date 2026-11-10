// wisp.json.encode / decode (agent-tools spec 4.2) on a bare Vm with
// install_api().

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>

#include "scene_api.hpp"
#include "wisp/scene/vm.hpp"

namespace {

using wisp::scene::SceneApi;
using wisp::scene::Vm;

class JsonBinding : public ::testing::Test {
protected:
    void SetUp() override {
        vm = Vm::create("j");
        ASSERT_TRUE(vm);
        ASSERT_FALSE(wisp::scene::install_api(*vm, api));
    }

    // Runs `src` and returns its one result through tostring, or the error.
    std::string eval(const std::string& src) {
        if (auto err = vm->load("j", src)) return "compile: " + err->text();
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

TEST_F(JsonBinding, RoundTrip) {
    EXPECT_EQ(eval("return wisp.json.encode({1, 2, 3})"), "[1,2,3]");
    EXPECT_EQ(eval("return wisp.json.encode({a = {b = \"c\"}})"), R"({"a":{"b":"c"}})");
    EXPECT_EQ(eval("return wisp.json.encode(3.0)"), "3.0");
    EXPECT_EQ(eval("return wisp.json.encode(nil)"), "null");
    EXPECT_EQ(eval("return wisp.json.encode({})"), "{}");
    EXPECT_EQ(eval(R"(
local v = wisp.json.decode('{"a":[1,2.5,"x",true],"b":null,"c":{"d":-3}}')
return table.concat({math.type(v.a[1]), v.a[2], v.a[3], tostring(v.a[4]), tostring(v.b),
                     v.c.d}, ",")
)"),
              "integer,2.5,x,true,nil,-3");
    EXPECT_EQ(eval(R"(
local s = wisp.json.encode(wisp.json.decode('{"k":[1,{"z":"w"}]}'))
return s
)"),
              R"({"k":[1,{"z":"w"}]})");
    EXPECT_EQ(eval(R"(return wisp.json.decode('"\\u00e9"'))"), "\xC3\xA9");
}

TEST_F(JsonBinding, Errors) {
    EXPECT_EQ(eval("local v, e = wisp.json.encode(print) return tostring(v) .. ',' .. e"),
              "nil,not_serializable");
    EXPECT_EQ(eval("local t = {} t.t = t local v, e = wisp.json.encode(t) return e"),
              "not_serializable");
    EXPECT_EQ(eval("local t = {} for i = 1, 40 do t = {t} end "
                   "local v, e = wisp.json.encode(t) return e"),
              "too_deep");
    EXPECT_EQ(eval("local v, e = wisp.json.decode('{') return tostring(v) .. ',' .. e"),
              "nil,bad_json");
    EXPECT_EQ(eval("local v, e = wisp.json.decode('') return e"), "bad_json");
    EXPECT_EQ(eval("local v, e = wisp.json.decode(string.rep('[', 40) .. string.rep(']', 40)) "
                   "return e"),
              "too_deep");
    EXPECT_EQ(eval("return pcall(wisp.json.decode, 5)"), "false");
    EXPECT_EQ(eval("local ok, e = pcall(wisp.json.decode) return e:find('bad argument') ~= nil"),
              "true");
    EXPECT_FALSE(vm->faulted());
}

TEST_F(JsonBinding, BareVmWithoutApiCallbacks) {
    // Only the conversion is needed: no http, files or task hooks.
    EXPECT_EQ(eval("return type(wisp.json.encode)"), "function");
    EXPECT_EQ(eval("return type(wisp.json.decode)"), "function");
}

}  // namespace
