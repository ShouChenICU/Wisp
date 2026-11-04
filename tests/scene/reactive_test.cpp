#include "wisp/scene/reactive.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "wisp/scene/patch_batch.hpp"
#include "wisp/scene/vm.hpp"

using wisp::os::LargeString;
using wisp::scene::Budget;
using wisp::scene::flush_reactive;
using wisp::scene::install_prelude;
using wisp::scene::PatchBatch;
using wisp::scene::push_wisp_internal;
using wisp::scene::ScriptError;
using wisp::scene::ScriptFault;
using wisp::scene::SetProp;
using wisp::scene::UiBatch;
using wisp::scene::Vm;

namespace {

struct FakeClock {
    uint64_t now = 1000;
};

// _G.advance(ms) moves the fake clock on.
int advance(lua_State* L) {
    auto* clock = static_cast<FakeClock*>(lua_touserdata(L, lua_upvalueindex(1)));
    clock->now += static_cast<uint64_t>(luaL_checkinteger(L, 1));
    return 0;
}

std::string describe(const std::optional<ScriptError>& err) {
    return err ? err->text() : std::string("(no error)");
}

// A scene VM with the prelude installed. Each message() runs one chunk
// (as main.lua, with the internal table as its argument `...`) and then
// flushes, the way the scene task handles a message.
struct Scene {
    FakeClock clock;
    PatchBatch batch;
    std::unique_ptr<Vm> vm;

    Scene() {
        vm = Vm::create("t", {}, [this] { return clock.now; });
        if (!vm) return;
        lua_pushlightuserdata(vm->L(), &clock);
        lua_pushcclosure(vm->L(), advance, 1);
        lua_setglobal(vm->L(), "advance");
        installed = install_prelude(*vm, batch);
    }

    std::optional<ScriptError> call_only(std::string_view src) {
        if (auto err = vm->load("main.lua", src)) return err;
        if (!push_wisp_internal(vm->L())) lua_pushnil(vm->L());
        return vm->call(1, 0);
    }

    std::optional<ScriptError> message(std::string_view src) {
        if (auto err = call_only(src)) return err;
        return flush_reactive(*vm);
    }

    std::optional<ScriptError> installed;
};

const SetProp& set_at(const UiBatch& b, size_t i) {
    static const SetProp none{};
    const auto* s = std::get_if<SetProp>(&b.ops.at(i));
    EXPECT_NE(s, nullptr) << "op " << i << " is not a SetProp";
    return s ? *s : none;
}

std::string str_of(const SetProp& s) {
    const auto* v = std::get_if<LargeString>(&s.value);
    return v ? std::string(v->data(), v->size()) : std::string("(not a string)");
}

}  // namespace

TEST(Reactive, StateGetSet) {
    Scene s;
    ASSERT_NE(s.vm, nullptr);
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.message(R"(
        local n = state(1)
        assert(n:get() == 1)
        n:set(2)
        assert(n:get() == 2)
        local t = {}
        n:set(t)
        assert(n:get() == t)
        n:set(nil)
        assert(n:get() == nil)
        assert(state():get() == nil)
        local ok, msg = pcall(function() n.value = 3 end)
        assert(not ok and msg:find("read%-only"), msg)
        ok, msg = pcall(n.get)
        assert(not ok and msg:find("expected a state"), msg)
    )");
    EXPECT_FALSE(err) << describe(err);
    EXPECT_EQ(s.batch.size(), 0u);
}

TEST(Reactive, InternalTableIsUnreachableFromScenes) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.message(R"(
        assert(_G.__wisp == nil)
        assert(rawget(_G, "__wisp") == nil)
        for k in pairs(_G) do assert(k ~= "__wisp") end
        local W = ...
        for _, name in ipairs({"watch", "effect", "scope", "dispose", "flush", "refresh_all", "emit", "kind"}) do
            assert(type(W[name]) == "function", name)
        end
        assert(W.kind(state(1)) == "state" and W.kind(computed(print)) == "computed")
        assert(W.kind(bind(state(1), tostring)) == "bind" and W.kind({}) == nil)
    )");
    EXPECT_FALSE(err) << describe(err);

    auto compile = s.vm->load("main.lua", "local x = __wisp");
    ASSERT_TRUE(compile);
    EXPECT_EQ(compile->fault, ScriptFault::Compile);
    // The globals the prelude defines are read-only names for scenes.
    compile = s.vm->load("main.lua", "state = 1");
    ASSERT_TRUE(compile);
    EXPECT_EQ(compile->fault, ScriptFault::Compile);
}

TEST(Reactive, EqualWriteDoesNotNotify) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.message(R"(
        local W = ...
        _G.n, _G.str, _G.t = state(1), state("a"), {1, 2}
        _G.tab = state(_G.t)
        W.watch(1, 1, _G.n)
        W.watch(2, 1, _G.str)
        W.watch(3, 1, bind(_G.tab, function(v) return #v end))
    )");
    ASSERT_FALSE(err) << describe(err);
    EXPECT_EQ(s.batch.size(), 0u);  // creating a watch emits nothing

    err = s.message("_G.n:set(1) _G.n:set(1.0) _G.str:set('a') _G.tab:set(_G.t)");
    ASSERT_FALSE(err) << describe(err);
    EXPECT_EQ(s.batch.size(), 0u);

    err = s.message("_G.tab:set({1, 2})");
    ASSERT_FALSE(err) << describe(err);
    auto b = s.batch.take(1);
    ASSERT_EQ(b.ops.size(), 1u);
    EXPECT_EQ(set_at(b, 0).node, 3u);
    EXPECT_EQ(std::get<int64_t>(set_at(b, 0).value), 2);
}

TEST(Reactive, ComputedTracksDependenciesDynamically) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.message(R"(
        local flag, a, b = state(true), state(1), state(10)
        local runs = 0
        local c = computed(function()
            runs = runs + 1
            if flag:get() then return a:get() else return b:get() end
        end)
        assert(runs == 0, "lazy")
        assert(c:get() == 1 and runs == 1)
        assert(c:get() == 1 and runs == 1, "cached")
        b:set(11)
        assert(c:get() == 1 and runs == 1, "b is not a dependency yet")
        flag:set(false)
        assert(c:get() == 11 and runs == 2)
        a:set(2)
        assert(c:get() == 11 and runs == 2, "a is no longer a dependency")
        b:set(12)
        assert(c:get() == 12 and runs == 3)
        -- A computed over a computed.
        local d = computed(function() return c:get() * 2 end)
        assert(d:get() == 24)
        b:set(13)
        assert(d:get() == 26)
    )");
    EXPECT_FALSE(err) << describe(err);
}

TEST(Reactive, BindAppliesFn) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.message(R"(
        local W = ...
        local count = state(0)
        local b = bind(count, function(v) return "已点击 " .. v .. " 次" end)
        local dispose, initial = W.watch(5, 15, b)
        assert(type(dispose) == "function")
        assert(initial == "已点击 0 次", initial)
        assert(b:get() == "已点击 0 次")
        _G.count = count
    )");
    ASSERT_FALSE(err) << describe(err);
    EXPECT_EQ(s.batch.size(), 0u);

    err = s.message("_G.count:set(1)");
    ASSERT_FALSE(err) << describe(err);
    auto b = s.batch.take(1);
    ASSERT_EQ(b.ops.size(), 1u);
    EXPECT_EQ(set_at(b, 0).node, 5u);
    EXPECT_EQ(set_at(b, 0).prop, 15u);
    EXPECT_EQ(str_of(set_at(b, 0)), "已点击 1 次");
}

TEST(Reactive, EmitConvertsValueTypes) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.message(R"(
        local W = ...
        W.watch(1, 1, state(true))
        W.watch(2, 1, state(42))
        W.watch(3, 1, state(1.5))
        W.watch(4, 1, state("s"))
        W.watch(5, 1, state(nil))
        W.refresh_all()
    )");
    ASSERT_FALSE(err) << describe(err);
    auto b = s.batch.take(1);
    ASSERT_EQ(b.ops.size(), 5u);
    EXPECT_EQ(std::get<bool>(set_at(b, 0).value), true);
    EXPECT_EQ(std::get<int64_t>(set_at(b, 1).value), 42);
    EXPECT_EQ(std::get<double>(set_at(b, 2).value), 1.5);
    EXPECT_EQ(str_of(set_at(b, 3)), "s");
    EXPECT_TRUE(std::holds_alternative<std::monostate>(set_at(b, 4).value));

    // A table cannot be a property value.
    err = s.message("local W = ... _G.bad = state(1) W.watch(6, 1, _G.bad)");
    ASSERT_FALSE(err) << describe(err);
    err = s.message("_G.bad:set({})");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Runtime);
    EXPECT_NE(err->message.find("table"), std::string::npos) << err->message;
    EXPECT_EQ(s.batch.size(), 0u);
}

TEST(Reactive, MultipleSetsCoalesceToOnePatch) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.message(R"(
        local W = ...
        _G.count = state(0)
        _G.runs = 0
        W.watch(1, 1, _G.count)
        W.watch(2, 1, bind(_G.count, function(v) _G.runs = _G.runs + 1 return v end))
    )");
    ASSERT_FALSE(err) << describe(err);
    err = s.message("_G.runs = 0 _G.count:set(1) _G.count:set(2) _G.count:set(3)");
    ASSERT_FALSE(err) << describe(err);
    auto b = s.batch.take(1);
    ASSERT_EQ(b.ops.size(), 2u);
    EXPECT_EQ(set_at(b, 0).node, 1u);
    EXPECT_EQ(std::get<int64_t>(set_at(b, 0).value), 3);
    EXPECT_EQ(std::get<int64_t>(set_at(b, 1).value), 3);
    err = s.message("assert(_G.runs == 1, _G.runs)");  // the bind fn ran once, at the flush
    EXPECT_FALSE(err) << describe(err);
}

TEST(Reactive, CyclicComputedErrorsOnFirstEval) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.message(
        "local a, b\n"
        "a = computed(function() return b:get() end)\n"
        "b = computed(function() return a:get() end)\n"
        "return a:get()\n");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Runtime);
    EXPECT_EQ(err->file, "main.lua");
    EXPECT_EQ(err->line, 3) << err->text();
    EXPECT_EQ(
        err->message,
        "cyclic dependency: computed@main.lua:2 -> computed@main.lua:3 -> computed@main.lua:2");
}

TEST(Reactive, SetInsideComputedErrors) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.message(
        "local n = state(0)\n"
        "local c = computed(function() n:set(1) return 1 end)\n"
        "return c:get()\n");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Runtime);
    EXPECT_EQ(err->line, 2);
    EXPECT_EQ(err->message, "state:set() inside computed");

    // The same holds for a bind fn.
    err = s.message(
        "local n = state(0)\n"
        "return bind(n, function(v) n:set(v + 1) end):get()\n");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->message, "state:set() inside computed");
}

TEST(Reactive, CaughtErrorInComputedRestoresTracking) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.message(R"(
        local n = state(1)
        local bad = computed(function()
            if n:get() > 0 then error("no") end
            return 0
        end)
        local ok, msg = pcall(bad.get, bad)
        assert(not ok and msg:find("no"), msg)
        ok, msg = pcall(bad.get, bad)
        assert(not ok and not msg:find("cyclic"), msg)
        n:set(0)  -- no longer counts as inside the computed
        assert(bad:get() == 0)
        -- A computed cannot yield: its evaluation is a C call boundary.
        local y = computed(function() coroutine.yield() end)
        ok, msg = coroutine.resume(coroutine.create(function() return y:get() end))
        assert(not ok and msg:find("yield"), msg)
        n:set(2)
    )");
    EXPECT_FALSE(err) << describe(err);
}

TEST(Reactive, DeepComputedChainEvaluates) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.message(R"(
        local W = ...
        _G.base = state(1)
        local c = _G.base
        for i = 1, 30 do
            local prev = c
            c = computed(function() return prev:get() + 1 end)
        end
        assert(c:get() == 31)
        W.watch(1, 1, c)
    )");
    ASSERT_FALSE(err) << describe(err);
    err = s.message("_G.base:set(2)");
    ASSERT_FALSE(err) << describe(err);
    auto b = s.batch.take(1);
    ASSERT_EQ(b.ops.size(), 1u);
    EXPECT_EQ(std::get<int64_t>(set_at(b, 0).value), 32);
}

TEST(Reactive, UnsettledUpdatesError) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.message(R"(
        local W = ...
        _G.runs = 0
        _G.n = state(0)
        W.watch(1, 1, _G.n)
        -- Writes what it reads, so every run schedules the next one.
        W.effect(function()
            _G.runs = _G.runs + 1
            _G.n:set(_G.n:get() + 1)
        end)
    )");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Runtime);
    EXPECT_EQ(err->message, "reactive updates did not settle");
    EXPECT_EQ(s.batch.size(), 0u);
    // One run when the effect was made, then 100 rounds.
    err = s.call_only("assert(_G.runs == 101, _G.runs)");
    EXPECT_FALSE(err) << describe(err);
}

TEST(Reactive, ErrorInBindFnDiscardsBatch) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.message(
        "local W = ...\n"
        "_G.n = state(1)\n"
        "W.watch(1, 1, _G.n)\n"
        "W.watch(2, 1, bind(_G.n, function(v) if v > 1 then error('too big') end return v end))\n");
    ASSERT_FALSE(err) << describe(err);
    err = s.message("_G.n:set(2)");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Runtime);
    EXPECT_EQ(err->file, "main.lua");
    EXPECT_EQ(err->line, 4);
    EXPECT_EQ(err->message, "too big");
    EXPECT_EQ(s.batch.size(), 0u);  // the first watch's patch went too
}

TEST(Reactive, DisposedScopeStopsPatches) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.message(R"(
        local W = ...
        _G.n = state(0)
        local sc = W.scope()
        W.watch(1, 1, _G.n, sc)
        W.watch(2, 1, bind(_G.n, tostring), sc)
        _G.d = W.watch(3, 1, _G.n)
        W.dispose(sc)
        W.dispose(sc)  -- twice is harmless
        local ok, msg = pcall(W.watch, 4, 1, _G.n, sc)
        assert(not ok and msg:find("disposed"), msg)
    )");
    ASSERT_FALSE(err) << describe(err);
    err = s.message("_G.n:set(1)");
    ASSERT_FALSE(err) << describe(err);
    auto b = s.batch.take(1);
    ASSERT_EQ(b.ops.size(), 1u);
    EXPECT_EQ(set_at(b, 0).node, 3u);

    err = s.message("_G.d() _G.d() _G.n:set(2)");
    ASSERT_FALSE(err) << describe(err);
    EXPECT_EQ(s.batch.size(), 0u);
}

TEST(Reactive, RefreshAllReemitsLiveWatches) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.message(R"(
        local W = ...
        local n = state("x")
        W.watch(1, 1, n)
        local d = W.watch(2, 1, n)
        W.watch(3, 1, bind(n, function(v) return v .. "!" end))
        d()
        W.refresh_all()
    )");
    ASSERT_FALSE(err) << describe(err);
    auto b = s.batch.take(1);
    ASSERT_EQ(b.ops.size(), 2u);
    EXPECT_EQ(set_at(b, 0).node, 1u);
    EXPECT_EQ(str_of(set_at(b, 0)), "x");
    EXPECT_EQ(set_at(b, 1).node, 3u);
    EXPECT_EQ(str_of(set_at(b, 1)), "x!");
}

TEST(Reactive, FlushChargedToSameMessage) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.message(R"(
        local W = ...
        _G.n = state(0)
        W.watch(1, 1, bind(_G.n, function(v) if v > 0 then _G.advance(100) end return v end))
    )");
    ASSERT_FALSE(err) << describe(err);
    // 150 ms in the handler plus 100 ms in the flush: over 200 ms.
    err = s.message("_G.advance(150) _G.n:set(1)");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Timeout);
    EXPECT_EQ(s.batch.size(), 0u);
}

TEST(Reactive, FlushWithinBudgetPasses) {
    Scene s;
    ASSERT_FALSE(s.installed) << describe(s.installed);
    auto err = s.message(R"(
        local W = ...
        _G.n = state(0)
        W.watch(1, 1, bind(_G.n, function(v) if v > 0 then _G.advance(100) end return v end))
    )");
    ASSERT_FALSE(err) << describe(err);
    err = s.message("_G.advance(50) _G.n:set(1)");
    EXPECT_FALSE(err) << describe(err);
    EXPECT_EQ(s.batch.size(), 1u);
}

TEST(VmBudget, ContinueKeepsTheLastCallsClock) {
    FakeClock clock;
    auto vm = Vm::create("t", {}, [&clock] { return clock.now; });
    ASSERT_NE(vm, nullptr);
    lua_pushlightuserdata(vm->L(), &clock);
    lua_pushcclosure(vm->L(), advance, 1);
    lua_setglobal(vm->L(), "advance");
    // With no call before it, Continue starts a fresh budget.
    ASSERT_FALSE(vm->load("main.lua", "_G.advance(150)"));
    EXPECT_FALSE(vm->call(0, 0, Budget::Continue));
    ASSERT_FALSE(vm->load("main.lua", "_G.advance(100) for i = 1, 5000 do end"));
    auto err = vm->call(0, 0, Budget::Continue);
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Timeout);
}
