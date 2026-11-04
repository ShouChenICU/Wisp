#include "wisp/scene/vm.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/core/log.hpp"

using wisp::scene::compile_check;
using wisp::scene::ScriptError;
using wisp::scene::ScriptFault;
using wisp::scene::Vm;
using wisp::scene::VmLimits;

namespace {

constexpr size_t kLimit = 2 * 1024 * 1024;

// A clock the test drives: every read returns `now` and then moves it on
// by `step`, so a loop that keeps reading it (the time hook) sees time
// pass; scripts can also jump it with _G.advance(ms).
struct FakeClock {
    uint64_t now = 1000;
    uint64_t step = 0;
    uint64_t read() {
        uint64_t v = now;
        now += step;
        return v;
    }
};

int advance(lua_State* L) {
    auto* clock = static_cast<FakeClock*>(lua_touserdata(L, lua_upvalueindex(1)));
    clock->now += static_cast<uint64_t>(luaL_checkinteger(L, 1));
    return 0;
}

// _G.probe() records the highest used_bytes() seen while a script runs.
struct Probe {
    Vm* vm = nullptr;
    size_t max_used = 0;
};

int probe(lua_State* L) {
    auto* p = static_cast<Probe*>(lua_touserdata(L, lua_upvalueindex(1)));
    p->max_used = std::max(p->max_used, p->vm->used_bytes());
    return 0;
}

std::unique_ptr<Vm> make(FakeClock& clock, VmLimits limits = {}, std::string id = "t") {
    auto vm = Vm::create(std::move(id), limits, [&clock] { return clock.read(); });
    if (vm) {
        lua_pushlightuserdata(vm->L(), &clock);
        lua_pushcclosure(vm->L(), advance, 1);
        lua_setglobal(vm->L(), "advance");
    }
    return vm;
}

void add_probe(Vm& vm, Probe& p) {
    p.vm = &vm;
    lua_pushlightuserdata(vm.L(), &p);
    lua_pushcclosure(vm.L(), probe, 1);
    lua_setglobal(vm.L(), "probe");
}

std::optional<ScriptError> run(Vm& vm, std::string_view src, std::string_view path = "main.lua") {
    if (auto err = vm.load(path, src)) return err;
    return vm.call(0, 0);
}

bool global_is_nil(Vm& vm, const char* name) {
    lua_getglobal(vm.L(), name);
    bool nil = lua_isnil(vm.L(), -1);
    lua_pop(vm.L(), 1);
    return nil;
}

std::string describe(const std::optional<ScriptError>& err) {
    return err ? err->text() : std::string("(no error)");
}

std::vector<wisp::LogEntry> scene_lines(const char* tag) {
    std::vector<wisp::LogEntry> out;
    for (auto& e : wisp::log::ring_entries())
        if (e.tag == tag) out.push_back(e);
    return out;
}

}  // namespace

TEST(Vm, OnlySandboxLibraries) {
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    for (const char* name :
         {"io", "os", "debug", "package", "require", "dofile", "loadfile", "warn"})
        EXPECT_TRUE(global_is_nil(*vm, name)) << name;
    for (const char* name : {"utf8", "coroutine", "math", "table", "string"}) {
        lua_getglobal(vm->L(), name);
        EXPECT_EQ(lua_type(vm->L(), -1), LUA_TTABLE) << name;
        lua_pop(vm->L(), 1);
    }
    auto err = run(*vm, R"(
        assert(string.dump == nil, "string.dump")
        assert(("x").dump == nil, "string metatable dump")
        assert(_G.io == nil and _G.os == nil and _G.debug == nil and _G.require == nil)
        assert(type(print) == "function" and type(load) == "function")
    )");
    EXPECT_FALSE(err) << describe(err);
}

TEST(Vm, LoadIsTextOnly) {
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    auto err = vm->load("main.lua", std::string_view("\x1bLua\x55\x00\x19\x93", 8));
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Compile);
    EXPECT_EQ(err->file, "main.lua");
    EXPECT_NE(err->message.find("attempt to load a binary chunk"), std::string::npos)
        << err->message;
    EXPECT_EQ(lua_gettop(vm->L()), 0);

    err = run(*vm, R"(
        local f, e = load("\27Lua")
        assert(f == nil)
        assert(string.find(e, "attempt to load a binary chunk", 1, true), e)
        local g, e2 = load("\27Lua", "x", "b")
        assert(g == nil and string.find(e2, "binary chunk", 1, true), e2)
        assert(load("return 1 + 1")() == 2)
    )");
    EXPECT_FALSE(err) << describe(err);
}

TEST(Vm, CollectgarbageRestricted) {
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    auto err = run(*vm, R"(
        assert(type(collectgarbage("count")) == "number")
        collectgarbage("collect")
        collectgarbage()
        collectgarbage("step")
        for _, opt in ipairs({"stop", "generational", "incremental", "restart", "isrunning"}) do
            local ok, e = pcall(collectgarbage, opt)
            assert(not ok, opt)
            assert(string.find(e, "collectgarbage option '" .. opt .. "' is not allowed", 1, true), e)
        end
    )");
    EXPECT_FALSE(err) << describe(err);
}

TEST(Vm, UndeclaredGlobalIsCompileErrorWithLine) {
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    auto err = vm->load("main.lua", "local a = 1\nb = 2");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Compile);
    EXPECT_EQ(err->file, "main.lua");
    EXPECT_EQ(err->line, 2);
    EXPECT_NE(err->message.find("variable 'b' not declared"), std::string::npos) << err->message;
    EXPECT_EQ(err->text(), "main.lua:2: " + err->message);
    EXPECT_EQ(lua_gettop(vm->L()), 0);
}

TEST(Vm, PreambleKeepsLineNumbers) {
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    auto err = run(*vm, "local a = 1\nlocal b = 2\nerror(\"x\")");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Runtime);
    EXPECT_EQ(err->file, "main.lua");
    EXPECT_EQ(err->line, 3);
    EXPECT_EQ(err->message, "x");
    EXPECT_EQ(err->text(), "main.lua:3: x");
    EXPECT_FALSE(vm->faulted());
    EXPECT_EQ(lua_gettop(vm->L()), 0);
}

TEST(Vm, ErrorWithoutPositionUsesCalleeFile) {
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    auto err = run(*vm, "error('plain', 0)", "lib/w.lua");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Runtime);
    EXPECT_EQ(err->text(), "lib/w.lua: plain");
    err = run(*vm, "error({})");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->text(), "main.lua: (error object is a table value)");
    ScriptError bare{ScriptFault::Runtime, "", 0, "m"};
    EXPECT_EQ(bare.text(), "m");
}

TEST(Vm, DeclaredGlobalsWork) {
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    auto err = run(*vm, "global counter\ncounter = 1");
    EXPECT_FALSE(err) << describe(err);
    err = run(*vm, "global function f() return 1 end");
    EXPECT_FALSE(err) << describe(err);
    lua_getglobal(vm->L(), "counter");
    EXPECT_EQ(lua_tointeger(vm->L(), -1), 1);
    lua_pop(vm->L(), 1);
    EXPECT_FALSE(global_is_nil(*vm, "f"));
}

TEST(Vm, InjectedNamesAreConst) {
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    auto err = vm->load("main.lua", "ui = 1");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Compile);
    EXPECT_NE(err->message.find("attempt to assign to const variable 'ui'"), std::string::npos)
        << err->message;
    // Fields of the injected names stay writable.
    err = vm->load("main.lua", "scene.on_show = function() end");
    EXPECT_FALSE(err) << describe(err);
    lua_settop(vm->L(), 0);
}

TEST(Vm, PreambleIsOneLine) {
    std::string_view p = wisp::scene::global_preamble();
    EXPECT_EQ(p.find('\n'), std::string_view::npos);
    EXPECT_EQ(p.substr(0, 14), "global<const> ");
    EXPECT_EQ(p.back(), ' ');
}

TEST(Vm, TimeoutAt200ms) {
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    // The budget is "more than 200 ms": 199 and 200 pass.
    auto err = run(*vm, "_G.advance(199) for i = 1, 100000 do end");
    EXPECT_FALSE(err) << describe(err);
    err = run(*vm, "_G.advance(200) for i = 1, 100000 do end");
    EXPECT_FALSE(err) << describe(err);
    // Each call() gets a fresh budget.
    err = run(*vm, "_G.advance(150) for i = 1, 100000 do end");
    EXPECT_FALSE(err) << describe(err);
    EXPECT_FALSE(vm->faulted());

    err = run(*vm, "_G.advance(201)\nwhile true do end");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Timeout);
    EXPECT_EQ(err->message, "execution exceeded 200 ms");
    EXPECT_EQ(err->text(), "main.lua:2: execution exceeded 200 ms");
    EXPECT_TRUE(vm->faulted());
    EXPECT_EQ(lua_gettop(vm->L()), 0);

    // A faulted VM refuses to run anything else.
    err = run(*vm, "return 1");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Timeout);
    EXPECT_EQ(lua_gettop(vm->L()), 0);
}

TEST(Vm, TimeoutLimitFollowsVmLimits) {
    FakeClock clock;
    auto vm = make(clock, VmLimits{.exec_ms = 50});
    ASSERT_NE(vm, nullptr);
    auto err = run(*vm, "_G.advance(51) while true do end");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Timeout);
    EXPECT_EQ(err->message, "execution exceeded 50 ms");
}

TEST(Vm, TimeoutEscapesPcallXpcallAndCoroutines) {
    const char* scripts[] = {
        "pcall(function() while true do end end)\n"
        "global flag; flag = true",

        "for i = 1, 3 do pcall(function() while true do end end) end\n"
        "global flag; flag = true",

        "xpcall(function() while true do end end, function(e) _G.h = true; return e end)\n"
        "global flag; flag = true",

        "pcall(coroutine.wrap(function() while true do end end))\n"
        "global flag; flag = true",

        "local co = coroutine.create(function() pcall(function() while true do end end)\n"
        "  _G.after = true end)\n"
        "pcall(coroutine.resume, co)\n"
        "global flag; flag = true",

        // A coroutine catches its child's fault; one created before the
        // fault must not run either.
        "local pre = coroutine.create(function() while true do\n"
        "  _G.n = (_G.n or 0) + 1; coroutine.yield() end end)\n"
        "coroutine.resume(pre)\n"
        "local a = coroutine.wrap(function()\n"
        "  local b = coroutine.create(function() while true do end end)\n"
        "  pcall(coroutine.resume, b)\n"
        "  coroutine.resume(pre)\n"
        "  _G.after = true\n"
        "end)\n"
        "pcall(a)\n"
        "coroutine.resume(pre)\n"
        "global flag; flag = true",

        // to-be-closed handlers and message handlers do not get to run on.
        "pcall(function() local x <close> = setmetatable({}, {__close = function() _G.h = true "
        "end})\n"
        "  while true do end end)\n"
        "global flag; flag = true",

        // A coroutine that dies in a fault keeps its pending to-be-closed
        // values: Lua would run them with hooks off.
        "local f = coroutine.wrap(function()\n"
        "  local x <close> = setmetatable({}, {__close = function()\n"
        "    for i = 1, 1000000 do end _G.h = true end})\n"
        "  while true do end end)\n"
        "f()\n"
        "global flag; flag = true",

        "local f = coroutine.wrap(function()\n"
        "  local c = setmetatable({}, {__close = function()\n"
        "    for i = 1, 1000000 do end _G.h = true end})\n"
        "  for _ in function() return true end, nil, nil, c do end end)\n"
        "pcall(f)\n"
        "global flag; flag = true",

        "local co = coroutine.create(function()\n"
        "  local x <close> = setmetatable({}, {__close = function()\n"
        "    for i = 1, 1000000 do end _G.h = true end})\n"
        "  while true do end end)\n"
        "coroutine.resume(co)\n"
        "coroutine.close(co)\n"
        "global flag; flag = true",
    };
    for (const char* src : scripts) {
        FakeClock clock;
        clock.step = 1;
        auto vm = make(clock);
        ASSERT_NE(vm, nullptr);
        auto err = run(*vm, src);
        ASSERT_TRUE(err) << src;
        EXPECT_EQ(err->fault, ScriptFault::Timeout) << src << "\n" << err->text();
        EXPECT_EQ(err->message, "execution exceeded 200 ms") << src;
        EXPECT_TRUE(vm->faulted());
        EXPECT_TRUE(global_is_nil(*vm, "flag")) << src;
        EXPECT_TRUE(global_is_nil(*vm, "h")) << src;
        EXPECT_TRUE(global_is_nil(*vm, "after")) << src;
        lua_getglobal(vm->L(), "n");
        EXPECT_TRUE(lua_isnil(vm->L(), -1) || lua_tointeger(vm->L(), -1) == 1) << src;
        lua_pop(vm->L(), 1);
        EXPECT_EQ(lua_gettop(vm->L()), 0);
    }
}

// Lua calls an xpcall message handler from inside the hook that raised
// the timeout, with hooks off: a looping handler must not hang the VM.
TEST(Vm, LoopingMessageHandlerCannotOutliveTimeout) {
    FakeClock clock;
    clock.step = 1;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    auto err = run(*vm,
                   "xpcall(function() while true do end end, function(e) while true do end end)\n"
                   "global flag; flag = true");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Timeout);
    EXPECT_TRUE(global_is_nil(*vm, "flag"));
}

TEST(Vm, PcallAndXpcallStillWork) {
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    auto err = run(*vm, R"(
        local ok, e = pcall(error, "x", 0)
        assert(not ok and e == "x", e)
        local r = table.pack(pcall(function(...) return ... end, 1, nil, 3))
        assert(r.n == 4 and r[1] == true and r[2] == 1 and r[3] == nil and r[4] == 3)
        assert(select("#", pcall(function() end)) == 1)
        local ok1, e1 = pcall(pcall)
        assert(not ok1 and string.find(e1, "bad argument #1 to 'pcall'", 1, true), e1)
        local ok2, e2 = pcall(nil)
        assert(not ok2 and string.find(e2, "attempt to call a nil value", 1, true), e2)
        assert(select("#", xpcall(function() end, print)) == 1)
        local ok3, e3 = pcall(xpcall, print)
        assert(not ok3 and string.find(e3, "bad argument #2 to 'xpcall'", 1, true), e3)
        -- f can still yield across pcall inside a coroutine.
        local co = coroutine.wrap(function()
            return pcall(function() local v = coroutine.yield(1); return v * 2 end)
        end)
        assert(co() == 1)
        local ok4, v = co(21)
        assert(ok4 and v == 42)
        local co2 = coroutine.wrap(function()
            return pcall(function() coroutine.yield(1); error("late", 0) end)
        end)
        co2()
        local ok5, e5 = co2()
        assert(not ok5 and e5 == "late")
    )");
    EXPECT_FALSE(err) << describe(err);
}

TEST(Vm, XpcallStillWorks) {
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    auto err = run(*vm, R"(
        local ok, e = xpcall(error, function(m) return "h:" .. m end, "x", 0)
        assert(not ok and e == "h:x", e)
        local ok2, a, b = xpcall(function(p, q) return p + q, "r" end, print, 1, 2)
        assert(ok2 and a == 3 and b == "r")
        assert(not pcall(xpcall, function() end, 42))
        -- f can still yield across xpcall inside a coroutine.
        local co = coroutine.wrap(function()
            return xpcall(function() local v = coroutine.yield(1); return v * 2 end, print)
        end)
        assert(co() == 1)
        local ok3, v = co(21)
        assert(ok3 and v == 42)
    )");
    EXPECT_FALSE(err) << describe(err);
}

TEST(Vm, CoroutineWrapStillWorks) {
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    auto err = run(*vm, R"(
        local gen = coroutine.wrap(function(a)
            local b = coroutine.yield(a + 1)
            local c = coroutine.yield(b * 2)
            return c, "done"
        end)
        assert(gen(1) == 2)
        assert(gen(5) == 10)
        local c, d = gen(7)
        assert(c == 7 and d == "done")
        local ok, e = pcall(gen)
        assert(not ok and string.find(e, "cannot resume dead coroutine", 1, true), e)

        -- An ordinary error closes the coroutine's pending values and
        -- reaches the caller (with the caller's position in front, none
        -- here since the caller is pcall).
        local closed = false
        local bad = coroutine.wrap(function()
            local x <close> = setmetatable({}, {__close = function() closed = true end})
            error("boom")
        end)
        ok, e = pcall(bad)
        assert(not ok and closed, e)
        assert(string.find(e, "^main.lua:%d+: boom$"), e)
        ok, e = pcall(function() bad() end)
        assert(string.find(e, "cannot resume dead coroutine", 1, true), e)
        local bad2 = coroutine.wrap(function() error("again") end)
        ok, e = pcall(function() bad2() end)
        assert(string.find(e, "^main.lua:%d+: main.lua:%d+: again$"), e)

        -- A non-string error object passes through unchanged.
        local obj = {}
        ok, e = pcall(coroutine.wrap(function() error(obj) end))
        assert(not ok and e == obj)

        assert(not pcall(coroutine.wrap, 42))
        -- Nested wraps and yields across them.
        local outer = coroutine.wrap(function()
            local inner = coroutine.wrap(function() coroutine.yield(1) return 2 end)
            coroutine.yield(inner())
            coroutine.yield(inner())
        end)
        assert(outer() == 1 and outer() == 2)
        -- coroutine.close still runs to-be-closed values of a suspended one.
        local n = 0
        local co = coroutine.create(function()
            local x <close> = setmetatable({}, {__close = function() n = n + 1 end})
            coroutine.yield()
        end)
        coroutine.resume(co)
        assert(coroutine.close(co) and n == 1)
    )");
    EXPECT_FALSE(err) << describe(err);
}

TEST(Vm, LuaRecursionIsNotLimitedByCCalls) {
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    auto err = run(*vm, R"(
        local function depth(n) if n == 0 then return 0 end return 1 + depth(n - 1) end
        assert(depth(10000) == 10000)
        -- C-level nesting is what is bounded: it fails as a Lua error.
        local function f() return pcall(f) end
        local r = table.pack(f())
        assert(r[r.n - 1] == false and r[r.n] == "C stack overflow", tostring(r[r.n]))
        local function g(s) return (string.gsub(s, ".", g)) end
        assert(not pcall(g, "ab"))
    )");
    EXPECT_FALSE(err) << describe(err);
}

TEST(Vm, TableLoopsAreCapped) {
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    auto err = run(*vm, R"(
        local function too_large(f, ...)
            local ok, e = pcall(f, ...)
            assert(not ok and string.find(e, "range too large", 1, true), tostring(e))
        end
        local big = 1 << 20
        -- Every count reaches the C loops through __len or an argument.
        local huge = setmetatable({}, {__len = function() return 100000000 end,
                                       __index = rawlen, __newindex = rawequal})
        too_large(table.move, {}, 1, big + 1, 1, {})
        too_large(table.move, {}, 1, math.maxinteger // 2, 1, {})
        too_large(table.move, {}, -5, big - 5, 1, {})
        too_large(table.insert, huge, 1, "v")
        too_large(table.insert, huge, "v")
        too_large(table.remove, huge, 1)
        too_large(table.remove, huge)
        too_large(table.sort, huge)
        too_large(table.sort, huge, function(a, b) return a < b end)
        too_large(table.concat, huge)
        too_large(table.concat, {}, "", 1, big + 1)
        too_large(table.concat, {}, "", -10, big)

        -- At the cap and below everything behaves as before.
        assert(#table.move({}, 1, big, 1, {}) == 0)
        local m = table.move({1, 2, 3}, 1, 3, 2)
        assert(m[1] == 1 and m[2] == 1 and m[3] == 2 and m[4] == 3)
        local m2 = table.move({1, 2, 3}, 2, 3, 1, {9})
        assert(m2[1] == 2 and m2[2] == 3)
        assert(table.move({1}, 3, 1, 1)[1] == 1)

        local t = {1, 2, 3}
        table.insert(t, 4)
        table.insert(t, 1, 0)
        table.insert(t, 3, 1.5)
        assert(table.concat(t, ",") == "0,1,1.5,2,3,4")
        assert(not pcall(table.insert, t, 9, "x"))
        assert(not pcall(table.insert, t, 0, "x"))
        assert(not pcall(table.insert, t))
        assert(not pcall(table.insert, t, 1, 2, 3))
        assert(not pcall(table.insert, 42, 1))
        assert(table.remove(t) == 4)
        assert(table.remove(t, 1) == 0)
        assert(table.remove(t, 2) == 1.5)
        assert(table.concat(t, ",") == "1,2,3")
        assert(table.remove({}) == nil)
        assert(table.remove({}, 0) == nil)
        local r = {1, 2}
        assert(table.remove(r, 3) == nil and #r == 2)
        assert(not pcall(table.remove, r, 5))

        local s = {5, 2, 8, 1}
        table.sort(s)
        assert(table.concat(s, ",") == "1,2,5,8")
        table.sort(s, function(a, b) return a > b end)
        assert(table.concat(s, ",") == "8,5,2,1")
        assert(not pcall(table.sort, {1, "x"}))

        -- Tables with metamethods keep working through them.
        local backing = {3, 1, 2}
        local proxy = setmetatable({}, {
            __len = function() return #backing end,
            __index = function(_, k) return backing[k] end,
            __newindex = function(_, k, v) backing[k] = v end,
        })
        table.sort(proxy)
        assert(table.concat(backing, ",") == "1,2,3")
        table.insert(proxy, 1, 0)
        assert(table.concat(backing, ",") == "0,1,2,3")
        assert(table.remove(proxy, 2) == 1)
        assert(table.concat(backing, ",") == "0,2,3")
        assert(table.concat(proxy, "-") == "0-2-3")
        local holes = setmetatable({}, {__len = function() return 3 end})
        holes[1] = 2 holes[3] = 1
        assert(not pcall(table.sort, holes))
    )");
    EXPECT_FALSE(err) << describe(err);
}

TEST(Vm, NegativeLengthsAndPositionsAreCapped) {
    FakeClock clock;
    clock.step = 1;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    // The __index function makes an uncapped loop run Lua code, so it
    // ends in a timeout on the fake clock instead of hanging the test.
    auto err = run(*vm, R"(
        local function too_large(f, ...)
            local ok, e = pcall(f, ...)
            assert(not ok and string.find(e, "range too large", 1, true), tostring(e))
        end
        local function neg(index)
            return setmetatable({}, {__len = function() return -2 end, __index = index})
        end
        local slow = function() end
        too_large(table.remove, neg(slow), -100000000)
        too_large(table.insert, neg(slow), -100000000, "x")
        too_large(table.remove, neg(slow), math.mininteger)
        too_large(table.sort, neg(slow))
        too_large(table.remove, neg(), -100000000)
        too_large(table.insert, neg(), -100000000, "x")
        too_large(table.remove, neg(), math.mininteger)
        too_large(table.insert, neg(), "x")
        too_large(table.remove, neg())
    )");
    EXPECT_FALSE(err) << describe(err);
}

// A chain of C functions that call each other and catch errors runs no
// Lua instruction, so the count hook never fires; every such function
// checks the deadline when it is entered.
TEST(Vm, CFanOutStillTimesOut) {
    FakeClock clock;
    clock.step = 1;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    uint64_t start = clock.now;
    auto err = run(*vm, R"(
        local t = setmetatable({true}, {__index = pcall, __call = table.unpack})
        table.unpack(t, -12)
    )");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Timeout) << describe(err);
    EXPECT_EQ(err->text(), "main.lua:3: execution exceeded 200 ms");
    EXPECT_LT(clock.now - start, 100000u);
    EXPECT_TRUE(vm->faulted());
}

TEST(Vm, CFanOutInsideCoroutinesTimesOut) {
    for (const char* src : {
             R"(local t = setmetatable({true}, {__index = pcall, __call = table.unpack})
                local co = coroutine.wrap(table.unpack)
                co(t, -12))",
             R"(local t = setmetatable({true}, {__index = pcall, __call = table.unpack})
                coroutine.resume(coroutine.create(table.unpack), t, -12))",
         }) {
        FakeClock clock;
        clock.step = 1;
        auto vm = make(clock);
        ASSERT_NE(vm, nullptr);
        uint64_t start = clock.now;
        auto err = run(*vm, src);
        ASSERT_TRUE(err) << src;
        EXPECT_EQ(err->fault, ScriptFault::Timeout) << src << "\n" << describe(err);
        EXPECT_EQ(err->file, "main.lua") << describe(err);
        EXPECT_LT(clock.now - start, 100000u) << src;
    }
}

TEST(Vm, BindingMetatablesAreHidden) {
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    lua_State* L = vm->L();
    lua_newuserdatauv(L, 8, 0);
    EXPECT_TRUE(wisp::scene::new_binding_metatable(L, "wisp.test"));
    lua_setmetatable(L, -2);
    lua_setglobal(L, "u");
    EXPECT_FALSE(wisp::scene::new_binding_metatable(L, "wisp.test"));
    lua_pop(L, 1);
    auto err = run(*vm, R"(
        assert(getmetatable(_G.u) == false)
    )");
    EXPECT_FALSE(err) << describe(err);
}

TEST(Vm, MemoryLimit2MBIsUncatchable) {
    const char* scripts[] = {
        "pcall(function() local s = string.rep('x', 3 * 1024 * 1024) end)\n"
        "global flag; flag = true",

        "local keep = {}\n"
        "pcall(function() for i = 1, 10000000 do keep[i] = string.rep('y', 512) .. i\n"
        "  if i % 32 == 0 then _G.probe() end end end)\n"
        "global flag; flag = true",

        // The coroutine catches its own fault and then runs code that
        // allocates nothing (`after` already exists in _G).
        "_G.after = false\n"
        "local co = coroutine.wrap(function()\n"
        "  local keep = {}\n"
        "  pcall(function() for i = 1, 10000000 do keep[i] = string.rep('z', 700) .. i\n"
        "    if i % 32 == 0 then _G.probe() end end end)\n"
        "  local k = 0 for i = 1, 100 do k = k + 1 end\n"
        "  _G.after = true end)\n"
        "pcall(co)\n"
        "global flag; flag = true",

        // table.concat's buffer is refused once, with no retry by Lua.
        "local parts = {}\n"
        "for i = 1, 1500 do parts[i] = string.rep('w', 1000) end\n"
        "pcall(table.concat, parts)\n"
        "global flag; flag = true",

        "local s = 'x'\n"
        "pcall(function() while true do s = s .. s end end)\n"
        "global flag; flag = true",

        // Caught inside a wrapped coroutine, then raised again by the hook
        // with a to-be-closed value pending.
        "local f = coroutine.wrap(function()\n"
        "  local x <close> = setmetatable({}, {__close = function()\n"
        "    for i = 1, 1000000 do end _G.after = true end})\n"
        "  pcall(string.rep, 'x', 3 * 1024 * 1024)\n"
        "  local k = 0 while true do k = k + 1 end end)\n"
        "f()\n"
        "global flag; flag = true",

        // The resumer must stop as soon as its child's refusal is known
        // to be unrecovered.
        "_G.after = false\n"
        "local co = coroutine.create(function() local s = string.rep('x', 3 * 1024 * 1024) end)\n"
        "coroutine.resume(co)\n"
        "local k = 0 for i = 1, 100 do k = k + 1 end\n"
        "_G.after = true",
    };
    for (const char* src : scripts) {
        FakeClock clock;
        auto vm = make(clock);
        ASSERT_NE(vm, nullptr);
        Probe p;
        add_probe(*vm, p);
        auto err = run(*vm, src);
        ASSERT_TRUE(err) << src;
        EXPECT_EQ(err->fault, ScriptFault::Memory) << src << "\n" << err->text();
        EXPECT_EQ(err->message, "memory limit 2 MB exceeded") << src;
        EXPECT_TRUE(vm->faulted());
        EXPECT_TRUE(global_is_nil(*vm, "flag")) << src;
        lua_getglobal(vm->L(), "after");
        EXPECT_FALSE(lua_toboolean(vm->L(), -1)) << src;
        lua_pop(vm->L(), 1);
        EXPECT_LE(p.max_used, kLimit);
        EXPECT_LE(vm->used_bytes(), kLimit);
        EXPECT_EQ(lua_gettop(vm->L()), 0);
    }
}

TEST(Vm, GarbageChurnIsNotAFault) {
    const char* src =
        "for i = 1, 10 * 1024 do local s = string.rep('z', 1024) .. i\n"
        "  if i % 64 == 0 then _G.probe() end end";
    {
        FakeClock clock;
        auto vm = make(clock);
        ASSERT_NE(vm, nullptr);
        Probe p;
        add_probe(*vm, p);
        auto err = run(*vm, src);
        EXPECT_FALSE(err) << describe(err);
        EXPECT_FALSE(vm->faulted());
        EXPECT_LE(p.max_used, kLimit);
    }
    {
        // With the collector stopped, every allocation past the limit is
        // refused once, recovered by Lua's emergency GC and retried: one
        // refusal followed by a successful retry is not a fault.
        FakeClock clock;
        auto vm = make(clock, VmLimits{.memory_bytes = 256 * 1024});
        ASSERT_NE(vm, nullptr);
        Probe p;
        add_probe(*vm, p);
        lua_gc(vm->L(), LUA_GCSTOP);
        auto err = run(*vm, src);
        EXPECT_FALSE(err) << describe(err);
        EXPECT_FALSE(vm->faulted());
        EXPECT_LE(p.max_used, 256u * 1024);
    }
}

TEST(Vm, SmallLimitMessage) {
    FakeClock clock;
    auto vm = make(clock, VmLimits{.memory_bytes = 512 * 1024});
    ASSERT_NE(vm, nullptr);
    auto err = run(*vm, "local s = string.rep('x', 600 * 1024)");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Memory);
    EXPECT_EQ(err->message, "memory limit 512 KB exceeded");
    EXPECT_LE(vm->used_bytes(), 512u * 1024);
}

TEST(Vm, CreateFailsWhenStateDoesNotFit) {
    FakeClock clock;
    EXPECT_EQ(make(clock, VmLimits{.memory_bytes = 1024}), nullptr);
}

TEST(Vm, FreshVmAfterFault) {
    FakeClock clock;
    clock.step = 1;
    {
        auto vm = make(clock);
        ASSERT_NE(vm, nullptr);
        auto err = run(*vm, "while true do end");
        ASSERT_TRUE(err);
        EXPECT_TRUE(vm->faulted());
    }
    {
        auto vm = make(clock);
        ASSERT_NE(vm, nullptr);
        auto err = run(*vm, "pcall(function() local s = string.rep('x', 3 * 1024 * 1024) end)");
        ASSERT_TRUE(err);
        EXPECT_EQ(err->fault, ScriptFault::Memory);
    }
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    ASSERT_FALSE(vm->load("main.lua", "local t = {} for i = 1, 1000 do t[i] = i end return #t"));
    auto err = vm->call(0, 1);
    ASSERT_FALSE(err) << describe(err);
    EXPECT_EQ(lua_tointeger(vm->L(), -1), 1000);
    lua_pop(vm->L(), 1);
    EXPECT_FALSE(vm->faulted());
}

TEST(Vm, ImportStaysInLib) {
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    std::map<std::string, std::string, std::less<>> files = {
        {"lib/util.lua", "_G.loads = (_G.loads or 0) + 1\nreturn { v = 42 }"},
        {"lib/x/y-z_1.lua", "return 7"},
        {"lib/none.lua", "local x = 1"},
        {"lib/a.lua", "return wisp.import('lib/b')"},
        {"lib/b.lua", "return wisp.import('lib/a')"},
        {"lib/bad.lua", "local x = 1\nerror('bad')"},
        {"lib/syn.lua", "zz = 1"},
        {"lib/bin.lua", std::string("\x1bLua", 4)},
        {"main.lua", "return 1"},
    };
    vm->set_sources([&files](std::string_view path) -> std::optional<std::string_view> {
        auto it = files.find(path);
        if (it == files.end()) return std::nullopt;
        return std::string_view(it->second);
    });
    auto err = run(*vm, R"(
        local u1 = wisp.import("lib/util")
        local u2 = wisp.import("lib/util")
        assert(u1 == u2 and u1.v == 42, "cached")
        assert(_G.loads == 1, "ran once")
        assert(wisp.import("lib/x/y-z_1") == 7)
        assert(wisp.import("lib/none") == true)
        assert(wisp.import("lib/none") == true)
        for _, p in ipairs({"../x", "/lib/x", "lib/../main", "main", "lib/x.lua", "lib/missing",
                            "lib/", "lib//x", "lib/x/", "lib/X", "lib", "lib/.", "lib/a b"}) do
            local ok, e = pcall(wisp.import, p)
            assert(not ok, p)
        end
        local ok, e = pcall(wisp.import, 42)
        assert(not ok)
        ok, e = pcall(wisp.import, "lib/a")
        assert(not ok and string.find(e, "import cycle: lib/a -> lib/b -> lib/a", 1, true), e)
        -- A failed import leaves no stale state behind.
        ok, e = pcall(wisp.import, "lib/a")
        assert(not ok and string.find(e, "import cycle: lib/a -> lib/b -> lib/a", 1, true), e)
        ok, e = pcall(wisp.import, "lib/missing")
        assert(not ok and string.find(e, "lib/missing.lua", 1, true), e)
        ok, e = pcall(wisp.import, "lib/bin")
        assert(not ok and string.find(e, "binary chunk", 1, true), e)
        ok, e = pcall(wisp.import, "lib/syn")
        assert(not ok and string.find(e, "lib/syn.lua:1: variable 'zz' not declared", 1, true), e)
        assert(wisp.import("lib/util") == u1)
    )");
    EXPECT_FALSE(err) << describe(err);

    err = run(*vm, "wisp.import('lib/bad')");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Runtime);
    EXPECT_EQ(err->file, "lib/bad.lua");
    EXPECT_EQ(err->line, 2);
    EXPECT_EQ(err->message, "bad");
}

TEST(Vm, ImportWithoutSourcesFails) {
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    auto err = run(*vm, "wisp.import('lib/x')");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Runtime);
    EXPECT_NE(err->message.find("lib/x.lua"), std::string::npos) << err->message;
}

TEST(Vm, GcMetamethodIsRefused) {
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    auto err = run(*vm, R"(
        local ok, e = pcall(setmetatable, {}, {__gc = function() while true do end end})
        assert(not ok and string.find(e, "__gc metamethods are not allowed", 1, true), e)
        local t = setmetatable({}, {__index = function() return 5 end})
        assert(t.x == 5)
        local mt = {}
        local u = setmetatable({}, mt)
        mt.__gc = function() while true do end end  -- too late: u is not marked
        u = nil
        collectgarbage()
        ok, e = pcall(setmetatable, {}, mt)
        assert(not ok, "metatable with __gc added later")
    )");
    EXPECT_FALSE(err) << describe(err);
}

TEST(Vm, CompileCheckDoesNotExecute) {
    EXPECT_FALSE(compile_check("main.lua", "error('boom')"));
    EXPECT_FALSE(compile_check("main.lua", "global x\nx = 1\nwhile true do end"));
    auto err = compile_check("lib/u.lua", "local a = 1\nlocal = 2");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Compile);
    EXPECT_EQ(err->file, "lib/u.lua");
    EXPECT_EQ(err->line, 2);
    err = compile_check("main.lua", "local a = 1\n\nb = 2");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->line, 3);
    EXPECT_NE(err->message.find("variable 'b' not declared"), std::string::npos);
    err = compile_check("main.lua", std::string_view("\x1bLua", 4));
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Compile);
}

TEST(Vm, DeeplyNestedSourceFailsCleanly) {
    std::string src = "local x = " + std::string(1000, '(') + "1" + std::string(1000, ')');
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    auto err = vm->load("main.lua", src);
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Compile) << err->text();
    EXPECT_EQ(lua_gettop(vm->L()), 0);
    EXPECT_FALSE(vm->faulted());
    err = compile_check("main.lua", src);
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Compile);
    // The VM is still usable.
    EXPECT_FALSE(run(*vm, "local y = ((((1))))"));
}

TEST(Vm, PrintGoesToSceneLog) {
    wisp::log::reset_for_test();
    wisp::log::init(64 * 1024);
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    auto err = run(*vm, R"(print("a", 1))");
    ASSERT_FALSE(err) << describe(err);
    auto lines = scene_lines("scene.t");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].msg, "a\t1");
    EXPECT_EQ(lines[0].level, wisp::LogLevel::Info);

    err =
        run(*vm,
            R"(print(nil, true, 1.5, setmetatable({}, {__tostring = function() return "T" end})))");
    ASSERT_FALSE(err) << describe(err);
    lines = scene_lines("scene.t");
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[1].msg, "nil\ttrue\t1.5\tT");

    // print joins and cuts at kLuaLogMaxBytes; the log layer then keeps
    // at most 255 bytes of any line, so that is what reaches the ring.
    err = run(*vm, R"(print(string.rep("x", 600)))");
    ASSERT_FALSE(err) << describe(err);
    lines = scene_lines("scene.t");
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[2].msg, std::string(std::min<size_t>(wisp::scene::kLuaLogMaxBytes, 255), 'x'));

    // A failing __tostring is an ordinary, catchable error.
    err = run(*vm, R"(print(setmetatable({}, {__tostring = function() error("ts") end})))");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Runtime);
    EXPECT_EQ(err->message, "ts");
    EXPECT_FALSE(vm->faulted());
    wisp::log::reset_for_test();
}

TEST(Vm, LogLuaArgsCutsOnCharacterBoundary) {
    wisp::log::reset_for_test();
    wisp::log::init(64 * 1024);
    FakeClock clock;
    auto vm = make(clock, {}, "u");
    ASSERT_NE(vm, nullptr);
    // 3-byte characters: the cut lands inside one and must back off.
    lua_State* L = vm->L();
    std::string s;
    for (int i = 0; i < 200; ++i) s += "\xe4\xb8\xad";
    lua_pushlstring(L, s.data(), s.size());
    wisp::scene::log_lua_args(L, wisp::LogLevel::Warn);
    EXPECT_EQ(lua_gettop(L), 1);
    lua_settop(L, 0);
    auto lines = scene_lines("scene.u");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].level, wisp::LogLevel::Warn);
    EXPECT_EQ(lines[0].msg.size() % 3, 0u);
    EXPECT_EQ(lines[0].msg, s.substr(0, lines[0].msg.size()));
    wisp::log::reset_for_test();
}

namespace {

void push_two(lua_State* L, void* ctx) {
    lua_pushinteger(L, *static_cast<int*>(ctx));
    lua_pushliteral(L, "two");
}

void raise_it(lua_State* L, void*) { luaL_error(L, "inner %d", 7); }

}  // namespace

TEST(Vm, ProtectCatchesErrors) {
    FakeClock clock;
    auto vm = make(clock);
    ASSERT_NE(vm, nullptr);
    lua_State* L = vm->L();
    lua_pushliteral(L, "below");
    int one = 1;
    EXPECT_EQ(wisp::scene::protect(L, push_two, &one), LUA_OK);
    ASSERT_EQ(lua_gettop(L), 3);
    EXPECT_EQ(lua_tointeger(L, 2), 1);
    EXPECT_STREQ(lua_tostring(L, 3), "two");
    lua_settop(L, 1);
    EXPECT_EQ(wisp::scene::protect(L, raise_it, nullptr), LUA_ERRRUN);
    ASSERT_EQ(lua_gettop(L), 2);
    EXPECT_STREQ(lua_tostring(L, 2), "inner 7");
    EXPECT_STREQ(lua_tostring(L, 1), "below");
    lua_settop(L, 0);
}
