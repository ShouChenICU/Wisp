// Lua <-> JSON conversion for wisp.store (scene-runtime-core spec 5.4).

#include "wisp/scene/lua_json.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "wisp/os/large.hpp"
#include "wisp/scene/vm.hpp"

using wisp::scene::lua_to_json;
using wisp::scene::push_json;
using wisp::scene::Vm;

namespace {

class LuaJson : public ::testing::Test {
protected:
    void SetUp() override {
        vm = Vm::create("json");
        ASSERT_NE(vm, nullptr);
        L = vm->L();
    }

    // Runs `src` (a chunk) and leaves its one result on the stack.
    void run(const std::string& src) {
        auto err = vm->load("t.lua", src);
        ASSERT_FALSE(err) << err->text();
        err = vm->call(0, 1);
        ASSERT_FALSE(err) << err->text();
    }

    // Encodes what `src` returns: {code or "", json}.
    std::pair<std::string, std::string> encode(const std::string& src,
                                               size_t max = wisp::scene::kLuaJsonMaxBytes) {
        run(src);
        const int top = lua_gettop(L);
        wisp::os::LargeString out;
        const char* code = lua_to_json(L, -1, out, max);
        EXPECT_EQ(lua_gettop(L), top);
        lua_pop(L, 1);
        return {code ? code : "", std::string(out.data(), out.size())};
    }

    std::string ok(const std::string& src) {
        auto [code, json] = encode(src);
        EXPECT_EQ(code, "") << src;
        return json;
    }

    std::string fails(const std::string& src) { return encode(src).first; }

    // Decodes `json` and passes the result to the function `check` returns;
    // returns what that function returns, as a string.
    std::string decode_check(std::string_view json, const std::string& check) {
        run(check);
        const int top = lua_gettop(L);
        EXPECT_TRUE(push_json(L, json)) << json;
        EXPECT_EQ(lua_gettop(L), top + 1);
        auto err = vm->call(1, 1);
        EXPECT_FALSE(err) << (err ? err->text() : "");
        size_t n = 0;
        const char* s = lua_tolstring(L, -1, &n);
        std::string r = s ? std::string(s, n) : "<not a string>";
        lua_pop(L, 1);
        return r;
    }

    std::unique_ptr<Vm> vm;
    lua_State* L = nullptr;
};

TEST_F(LuaJson, RoundTripsNestedValues) {
    EXPECT_EQ(ok("return nil"), "null");
    EXPECT_EQ(ok("return true"), "true");
    EXPECT_EQ(ok("return false"), "false");
    EXPECT_EQ(ok("return 3"), "3");
    EXPECT_EQ(ok("return -9007199254740993"), "-9007199254740993");
    EXPECT_EQ(ok("return 3.5"), "3.5");
    EXPECT_EQ(ok("return 3.0"), "3.0");
    EXPECT_EQ(ok("return 0.1"), "0.1");
    EXPECT_EQ(ok("return 1e300"), "1e+300");
    EXPECT_EQ(ok(R"(return "a\"b\\c\n\1\127/")"), R"("a\"b\\c\n\u0001)"
                                                  "\x7f"
                                                  R"(/")");
    EXPECT_EQ(ok("return \"héllo 世界\""), "\"héllo 世界\"");
    EXPECT_EQ(ok("return {}"), "{}");
    EXPECT_EQ(ok("return {1, 2, 3}"), "[1,2,3]");
    EXPECT_EQ(ok("return {a = 1}"), R"({"a":1})");
    EXPECT_EQ(ok("return {{}, {1}, {x = {true}}}"), R"([{},[1],{"x":[true]}])");
    // The same table twice is not a cycle.
    EXPECT_EQ(ok("local a = {1} return {a, a}"), "[[1],[1]]");
    // Raw contents: a metatable's __index adds nothing.
    EXPECT_EQ(ok("return setmetatable({}, {__index = function() return 1 end})"), "{}");

    const std::string json = ok(R"(return {
        n = 3, f = 3.5, w = 2.0, s = "héllo\t\"世界\"\0end",
        arr = {1, "two", {x = true, y = {}}}, neg = -7, big = 1e300,
        deep = {a = {b = {c = {"d"}}}}})");
    EXPECT_EQ(decode_check(json, R"(return function(t)
        return table.concat({
            math.type(t.n), t.n, math.type(t.f), t.f, math.type(t.w), tostring(t.w),
            t.s == "héllo\t\"世界\"\0end" and "s" or "bad s",
            #t.arr, t.arr[1], t.arr[2], tostring(t.arr[3].x), tostring(next(t.arr[3].y)),
            t.neg, tostring(t.big == 1e300), t.deep.a.b.c[1]}, ",")
    end)"),
              "integer,3,float,3.5,float,2.0,s,3,1,two,true,nil,-7,true,d");

    // Every scalar decodes alone too.
    EXPECT_EQ(decode_check("12", "return function(v) return math.type(v) .. v end"), "integer12");
    EXPECT_EQ(decode_check("-0.25e1", "return function(v) return math.type(v) .. v end"),
              "float-2.5");
    EXPECT_EQ(decode_check(R"("\u00e9\ud83d\ude00\/")", "return function(v) return v end"), "é😀/");
    EXPECT_EQ(decode_check("true", "return function(v) return tostring(v) end"), "true");
    // Past int64 a number is a float.
    EXPECT_EQ(decode_check("9223372036854775808", "return function(v) return math.type(v) end"),
              "float");
}

TEST_F(LuaJson, RecordLeavesOutTopLevelId) {
    auto record = [&](const std::string& src) {
        run(src);
        wisp::os::LargeString out;
        const char* code = wisp::scene::lua_record_to_json(L, -1, out);
        lua_pop(L, 1);
        return std::string(code ? code : "") + std::string(out.data(), out.size());
    };
    EXPECT_EQ(record("return {id = 5, a = 1}"), R"({"a":1})");
    EXPECT_EQ(record("return {id = 5}"), "{}");
    // Only the outermost table's field.
    EXPECT_EQ(record("return {x = {id = 1}}"), R"({"x":{"id":1}})");
    // `id` does not make a mixed table an object.
    EXPECT_EQ(record("return {id = 1, 2}"), "[2]");
    EXPECT_EQ(record("return {id = 1, 2, x = 3}"), "not_serializable");
}

TEST_F(LuaJson, RejectsUnserializable) {
    EXPECT_EQ(fails("return print"), "not_serializable");
    EXPECT_EQ(fails("return function() end"), "not_serializable");
    EXPECT_EQ(fails("return coroutine.create(function() end)"), "not_serializable");
    EXPECT_EQ(fails("local t = {} t.self = t return t"), "not_serializable");
    EXPECT_EQ(fails("local t = {} t[1] = {{t}} return t"), "not_serializable");
    EXPECT_EQ(fails("return 0/0"), "not_serializable");
    EXPECT_EQ(fails("return 1/0"), "not_serializable");
    EXPECT_EQ(fails("return {x = -1/0}"), "not_serializable");
    EXPECT_EQ(fails("return {1, x = 2}"), "not_serializable");
    EXPECT_EQ(fails("return {[2] = 1}"), "not_serializable");
    EXPECT_EQ(fails("return {1, nil, 3}"), "not_serializable");
    EXPECT_EQ(fails("return {[1.5] = 1}"), "not_serializable");
    EXPECT_EQ(fails("return {[true] = 1}"), "not_serializable");
    EXPECT_EQ(fails("return {f = print}"), "not_serializable");
    // JSON text is UTF-8.
    EXPECT_EQ(fails(R"(return "\xff")"), "not_serializable");
    EXPECT_EQ(fails(R"(return {["\xc3"] = 1})"), "not_serializable");
    // Past the size cap (the quotes count).
    EXPECT_EQ(encode("return string.rep('x', 9)", 11).first, "");
    EXPECT_EQ(encode("return string.rep('x', 9)", 11).second, "\"xxxxxxxxx\"");
    EXPECT_EQ(encode("return string.rep('x', 10)", 11).first, "record_too_large");
    EXPECT_EQ(encode("return string.rep('x', 10)", 11).second, "");
    EXPECT_EQ(encode("return {string.rep('x', 4), string.rep('x', 4)}", 14).first,
              "record_too_large");
}

TEST_F(LuaJson, DepthLimit32) {
    const char* nest = "local t = {} for i = 2, %d do t = {t} end return t";
    char src[128];
    std::snprintf(src, sizeof(src), nest, 32);
    const std::string deepest = ok(src);
    EXPECT_EQ(deepest, std::string(31, '[') + "{}" + std::string(31, ']'));
    std::snprintf(src, sizeof(src), nest, 33);
    EXPECT_EQ(fails(src), "too_deep");
    EXPECT_EQ(fails("local t = {} for i = 2, 33 do t = {k = t} end return t"), "too_deep");

    // Decoding stops at the same depth.
    EXPECT_EQ(decode_check(std::string(32, '[') + std::string(32, ']'),
                           "return function(t) local n = 0 while t do n = n + 1 t = t[1] end "
                           "return tostring(n) end"),
              "32");
    const int top = lua_gettop(L);
    EXPECT_FALSE(push_json(L, std::string(33, '[') + std::string(33, ']')));
    EXPECT_EQ(lua_gettop(L), top);
}

TEST_F(LuaJson, NullBecomesNil) {
    EXPECT_EQ(decode_check("null",
                           "return function(...) return select('#', ...) .. "
                           "tostring((...)) end"),
              "1nil");
    EXPECT_EQ(decode_check("[1,null,3]",
                           "return function(t) return tostring(t[1]) .. "
                           "tostring(t[2]) .. tostring(t[3]) end"),
              "1nil3");
    EXPECT_EQ(decode_check(R"({"a":null,"b":1})",
                           "return function(t) local n = 0 for _ in pairs(t) do n = n + 1 end "
                           "return tostring(t.a) .. n end"),
              "nil1");

    // Malformed text: false, the stack as it was.
    const int top = lua_gettop(L);
    for (std::string_view bad : {"", "[1,", "{\"a\"}", "nul", "[1] x", "\"\\x\"", "01", "{1:2}"}) {
        EXPECT_FALSE(push_json(L, bad)) << bad;
        EXPECT_EQ(lua_gettop(L), top) << bad;
    }
}

// Encodes its argument; on kJsonLimits raises the VM's fault the way a
// binding does.
int encode_entry(lua_State* L) {
    const char* code = nullptr;
    {
        wisp::os::LargeString out;
        code = lua_to_json(L, 1, out);
    }
    if (code == wisp::scene::kJsonLimits) Vm::of(L)->check_limits(L);
    lua_pushstring(L, code ? code : "ok");
    return 1;
}

int decode_entry(lua_State* L) {
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    if (!push_json(L, std::string_view(s, n))) {
        Vm::of(L)->check_limits(L);
        return luaL_error(L, "bad json");
    }
    return 1;
}

TEST(LuaJsonLimits, StopsAtTheTimeLimit) {
    // Once `ticking`, every clock read moves time on by 60 ms: past the
    // 200 ms limit by the fourth read after call() starts, long before
    // 4096 values are done.
    uint64_t now = 0;
    bool ticking = false;
    auto clock = [&] { return ticking ? now += 60 : now; };

    auto vm = Vm::create("json", {}, clock);
    ASSERT_NE(vm, nullptr);
    lua_State* L = vm->L();
    auto err = vm->load("t.lua", "local t = {} for i = 1, 4096 do t[i] = {i} end return t");
    ASSERT_FALSE(err);
    err = vm->call(0, 1);
    ASSERT_FALSE(err) << err->text();
    lua_pushcfunction(L, encode_entry);
    lua_insert(L, -2);
    ticking = true;
    err = vm->call(1, 1);
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, wisp::scene::ScriptFault::Timeout) << err->text();

    ticking = false;
    vm = Vm::create("json", {}, clock);
    ASSERT_NE(vm, nullptr);
    L = vm->L();
    std::string json = "[";
    for (int i = 1; i <= 4096; ++i) json += (i > 1 ? ",[" : "[") + std::to_string(i) + "]";
    json += "]";
    lua_pushcfunction(L, decode_entry);
    lua_pushlstring(L, json.data(), json.size());
    ticking = true;
    err = vm->call(1, 1);
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, wisp::scene::ScriptFault::Timeout) << err->text();
}

}  // namespace
