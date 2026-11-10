// Task coroutines and the awaitable async calls (agent-tools spec 6), on a
// bare Vm with install_api(), fake http/files hooks and a fake clock.

#include "wisp/scene/lua_task.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "scene_api.hpp"
#include "wisp/base/value.hpp"
#include "wisp/scene/vm.hpp"

namespace {

using wisp::Map;
using wisp::Value;
using wisp::scene::FilesJob;
using wisp::scene::FilesResult;
using wisp::scene::SceneApi;
using wisp::scene::ScriptError;
using wisp::scene::ScriptFault;
using wisp::scene::TaskOutcome;
using wisp::scene::Vm;
using wisp::scene::VmLimits;

int advance(lua_State* L) {
    auto* now = static_cast<uint64_t*>(lua_touserdata(L, lua_upvalueindex(1)));
    *now += static_cast<uint64_t>(luaL_checkinteger(L, 1));
    return 0;
}

int mark(lua_State* L) {
    auto* marks = static_cast<int*>(lua_touserdata(L, lua_upvalueindex(1)));
    ++*marks;
    return 0;
}

class LuaTask : public ::testing::Test {
protected:
    void make(VmLimits limits = {}) {
        limits.hook_instructions = 1;
        vm = Vm::create("t", limits, [this] { return now; });
        ASSERT_TRUE(vm);
        api.now_ms = [this] { return now; };
        api.http_submit = [this](uint32_t id, wisp::net::SceneHttpRequest req) {
            http.emplace_back(id, std::move(req));
        };
        api.http_fail = [this](uint32_t id, const char* error) { fails.emplace_back(id, error); };
        api.files_run = [this](uint32_t id, FilesJob job) {
            files.emplace_back(id, std::move(job));
        };
        api.files_fail = [this](uint32_t id, const char* error) { fails.emplace_back(id, error); };
        api.task_done = [this](uint32_t id, TaskOutcome o) { done.emplace_back(id, std::move(o)); };
        ASSERT_FALSE(wisp::scene::install_api(*vm, api));
        lua_State* L = vm->L();
        lua_pushlightuserdata(L, &now);
        lua_pushcclosure(L, advance, 1);
        lua_setglobal(L, "advance");
        lua_pushlightuserdata(L, &marks);
        lua_pushcclosure(L, mark, 1);
        lua_setglobal(L, "mark");
    }

    wisp::Result<uint32_t> start(const std::string& src, const Value* args = nullptr,
                                 uint32_t slice_ms = 0,
                                 size_t max_json = wisp::scene::kEvalResultMaxBytes) {
        auto err = vm->load("task", src);
        EXPECT_FALSE(err) << err->text();
        if (err) return wisp::Result<uint32_t>::err(wisp::ErrorCode::InvalidArgument);
        return wisp::scene::start_task(*vm, args, slice_ms, max_json);
    }

    // Runs `src` as an ordinary (non-task) call.
    std::optional<ScriptError> run(const std::string& src) {
        if (auto err = vm->load("plain", src)) return err;
        return vm->call(0, 0);
    }

    // Fires every timer due now, as the scene task would.
    void fire_timers() {
        for (uint32_t id : api.timers.due(now)) EXPECT_FALSE(wisp::scene::run_timer(*vm, id, true));
    }

    static wisp::net::SceneHttpResult status(int s) {
        wisp::net::SceneHttpResult r;
        r.status = s;
        return r;
    }

    uint64_t now = 1000;
    int marks = 0;
    SceneApi api;
    std::unique_ptr<Vm> vm;
    std::vector<std::pair<uint32_t, wisp::net::SceneHttpRequest>> http;
    std::vector<std::pair<uint32_t, FilesJob>> files;
    std::vector<std::pair<uint32_t, std::string>> fails;
    std::vector<std::pair<uint32_t, TaskOutcome>> done;
};

TEST_F(LuaTask, ReturnsJson) {
    make();
    auto id = start("return 1, \"a\"");
    ASSERT_TRUE(id);
    EXPECT_GT(id.value(), 0u);
    ASSERT_EQ(done.size(), 1u);
    EXPECT_EQ(done[0].first, id.value());
    EXPECT_TRUE(done[0].second.ok);
    EXPECT_EQ(std::string(done[0].second.json), "[1,\"a\"]");

    ASSERT_TRUE(start("local x = 1"));
    ASSERT_EQ(done.size(), 2u);
    EXPECT_TRUE(done[1].second.ok);
    EXPECT_EQ(std::string(done[1].second.json), "null");

    Value args(Map{{"x", Value(int64_t{5})}});
    ASSERT_TRUE(start("local a = ... return a.x", &args));
    ASSERT_EQ(done.size(), 3u);
    EXPECT_EQ(std::string(done[2].second.json), "5");

    ASSERT_TRUE(start("return (...) == nil"));
    ASSERT_EQ(done.size(), 4u);
    EXPECT_EQ(std::string(done[3].second.json), "true");
}

TEST_F(LuaTask, AwaitHttp) {
    make();
    auto id = start(R"(local e, r = wisp.http.get("https://x/") return r.status)");
    ASSERT_TRUE(id);
    EXPECT_TRUE(done.empty());
    ASSERT_EQ(http.size(), 1u);
    EXPECT_EQ(http[0].second.method, wisp::net::Method::Get);
    EXPECT_EQ(http[0].second.url, "https://x/");
    EXPECT_FALSE(wisp::scene::run_http_callback(*vm, http[0].first, status(200)));
    ASSERT_EQ(done.size(), 1u);
    EXPECT_EQ(done[0].first, id.value());
    EXPECT_TRUE(done[0].second.ok);
    EXPECT_EQ(std::string(done[0].second.json), "200");
}

TEST_F(LuaTask, AwaitErrorAndPost) {
    make();
    ASSERT_TRUE(start(R"(
local e, r = wisp.http.post("https://x/", {a = 1}, {headers = {X = "y"}})
return e, r == nil
)"));
    ASSERT_EQ(http.size(), 1u);
    EXPECT_EQ(http[0].second.method, wisp::net::Method::Post);
    EXPECT_EQ(std::string(http[0].second.body), "{\"a\":1}");
    wisp::net::SceneHttpResult r;
    r.error = "timeout";
    EXPECT_FALSE(wisp::scene::run_http_callback(*vm, http[0].first, r));
    ASSERT_EQ(done.size(), 1u);
    EXPECT_EQ(std::string(done[0].second.json), "[\"timeout\",true]");
}

TEST_F(LuaTask, AwaitRequest) {
    make();
    ASSERT_TRUE(start(R"(
local e, r = wisp.http.request{method = "PUT", url = "https://x/p", body = "b",
                               headers = {["X-A"] = "1"}, timeout_ms = 5000, follow = true}
return r.status
)"));
    ASSERT_EQ(http.size(), 1u);
    const auto& req = http[0].second;
    EXPECT_EQ(req.method, wisp::net::Method::Put);
    EXPECT_EQ(req.url, "https://x/p");
    EXPECT_EQ(std::string(req.body), "b");
    EXPECT_EQ(req.principal, wisp::net::HttpPrincipal::Scene);
    // A scene keeps the proxy's deadline and never follows redirects.
    EXPECT_EQ(req.timeout_ms, 0u);
    EXPECT_EQ(req.max_redirects, 0u);
    EXPECT_EQ(req.headers, (std::vector<std::pair<std::string, std::string>>{{"X-A", "1"}}));
    EXPECT_FALSE(wisp::scene::run_http_callback(*vm, http[0].first, status(201)));
    ASSERT_EQ(done.size(), 1u);
    EXPECT_EQ(std::string(done[0].second.json), "201");
}

TEST_F(LuaTask, AgentPrincipalFromApi) {
    make();
    api.http_principal = wisp::net::HttpPrincipal::Agent;
    ASSERT_TRUE(start(R"(wisp.http.request{method = "head", url = "https://x/"})"));
    ASSERT_EQ(http.size(), 1u);
    EXPECT_EQ(http[0].second.principal, wisp::net::HttpPrincipal::Agent);
    EXPECT_EQ(http[0].second.method, wisp::net::Method::Head);
    EXPECT_EQ(http[0].second.max_redirects, 0u);
    EXPECT_EQ(http[0].second.timeout_ms, 0u);
    ASSERT_TRUE(
        start(R"(wisp.http.request{url = "https://x/", timeout_ms = 5000, follow = true})"));
    ASSERT_EQ(http.size(), 2u);
    EXPECT_EQ(http[1].second.timeout_ms, 5000u);
    EXPECT_EQ(http[1].second.max_redirects, wisp::net::AgentHttpLimits::kMaxRedirects);
}

TEST_F(LuaTask, AwaitFilesAndSleep) {
    make();
    ASSERT_TRUE(start(R"(
local e, data = wisp.files.read("a", 0, 10)
_G.mark()
local r = wisp.sleep(100)
return e, data, r
)"));
    ASSERT_EQ(files.size(), 1u);
    FilesResult fr;
    auto buf = wisp::os::LargeBuffer::allocate(5);
    std::memcpy(buf.data(), "hello", 5);
    fr.payload = std::move(buf);
    EXPECT_FALSE(wisp::scene::run_files_callback(*vm, files[0].first, fr));
    EXPECT_EQ(marks, 1);
    EXPECT_TRUE(done.empty());
    now += 99;
    fire_timers();
    EXPECT_TRUE(done.empty());
    now += 1;
    fire_timers();
    ASSERT_EQ(done.size(), 1u);
    EXPECT_TRUE(done[0].second.ok);
    EXPECT_EQ(std::string(done[0].second.json), "[null,\"hello\",null]");
}

TEST_F(LuaTask, AwaitFilesWriteListInfo) {
    make();
    ASSERT_TRUE(start(R"(
local e1, ok = wisp.files.write("a", "x")
local e2, list = wisp.files.list()
return ok, #list
)"));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_FALSE(wisp::scene::run_files_callback(*vm, files[0].first, FilesResult{}));
    ASSERT_EQ(files.size(), 2u);
    FilesResult fr;
    fr.payload = wisp::core::SceneFileList{};
    EXPECT_FALSE(wisp::scene::run_files_callback(*vm, files[1].first, fr));
    ASSERT_EQ(done.size(), 1u);
    EXPECT_EQ(std::string(done[0].second.json), "[true,0]");
}

TEST_F(LuaTask, SleepArguments) {
    make();
    ASSERT_TRUE(start(R"(wisp.sleep(-1))"));
    ASSERT_TRUE(start(R"(wisp.sleep(300001))"));
    ASSERT_TRUE(start(R"(wisp.sleep(0) return "woke")"));
    ASSERT_EQ(done.size(), 2u);
    for (auto& d : done) {
        EXPECT_FALSE(d.second.ok);
        ASSERT_TRUE(d.second.error);
        EXPECT_NE(d.second.error->message.find("bad argument"), std::string::npos)
            << d.second.error->text();
    }
    now += wisp::scene::TimerSet::kMinIntervalMs;
    fire_timers();
    ASSERT_EQ(done.size(), 3u);
    EXPECT_EQ(std::string(done[2].second.json), "\"woke\"");
}

TEST_F(LuaTask, SliceExcludesWaiting) {
    make();
    ASSERT_TRUE(start(R"(
_G.advance(150)
wisp.sleep(100)
_G.advance(150)
return "ok"
)",
                      nullptr, 200));
    EXPECT_TRUE(done.empty());
    now += 10000;
    fire_timers();
    ASSERT_EQ(done.size(), 1u);
    EXPECT_TRUE(done[0].second.ok) << done[0].second.error->text();
    EXPECT_FALSE(vm->faulted());

    ASSERT_TRUE(start("_G.advance(250) return 1", nullptr, 200));
    ASSERT_EQ(done.size(), 2u);
    EXPECT_FALSE(done[1].second.ok);
    ASSERT_TRUE(done[1].second.error);
    EXPECT_EQ(done[1].second.error->fault, ScriptFault::Timeout);
    EXPECT_NE(done[1].second.error->message.find("200 ms"), std::string::npos);
    EXPECT_TRUE(vm->faulted());
    // A timeout in a coroutine can never be cleared.
    EXPECT_FALSE(vm->recover_timeout());
}

TEST_F(LuaTask, SliceDefaultsToExecMs) {
    VmLimits limits;
    limits.exec_ms = 1000;
    make(limits);
    ASSERT_TRUE(start("_G.advance(900) return 1"));
    ASSERT_EQ(done.size(), 1u);
    EXPECT_TRUE(done[0].second.ok);
}

TEST_F(LuaTask, NestedCoroutineAwaitIsError) {
    make();
    ASSERT_TRUE(start(R"(
local f = coroutine.wrap(function() return wisp.http.get("https://x/") end)
return f()
)"));
    EXPECT_TRUE(http.empty());
    ASSERT_EQ(done.size(), 1u);
    EXPECT_FALSE(done[0].second.ok);
    ASSERT_TRUE(done[0].second.error);
    EXPECT_EQ(done[0].second.error->fault, ScriptFault::Runtime);
    EXPECT_NE(done[0].second.error->message.find("function expected"), std::string::npos)
        << done[0].second.error->text();
    EXPECT_FALSE(vm->faulted());
}

TEST_F(LuaTask, ScriptCoroutinesStillWork) {
    make();
    ASSERT_TRUE(start(R"(
local co = coroutine.wrap(function(a) local b = coroutine.yield(a + 1) return b * 2 end)
local x = co(1)
local e, r = wisp.http.get("https://x/")
return x, co(r.status)
)"));
    ASSERT_EQ(http.size(), 1u);
    EXPECT_FALSE(wisp::scene::run_http_callback(*vm, http[0].first, status(21)));
    ASSERT_EQ(done.size(), 1u);
    EXPECT_EQ(std::string(done[0].second.json), "[2,42]");
}

TEST_F(LuaTask, AwaitInsidePcall) {
    make();
    ASSERT_TRUE(start(R"(
local ok, e, r = pcall(wisp.http.get, "https://x/")
return ok, r.status
)"));
    ASSERT_EQ(http.size(), 1u);
    EXPECT_FALSE(wisp::scene::run_http_callback(*vm, http[0].first, status(204)));
    ASSERT_EQ(done.size(), 1u);
    EXPECT_EQ(std::string(done[0].second.json), "[true,204]");
}

TEST_F(LuaTask, NoWaitInsideCCall) {
    make();
    ASSERT_TRUE(start(R"(
local t = {3, 1, 2}
table.sort(t, function(a, b) wisp.sleep(100) return a < b end)
)"));
    ASSERT_EQ(done.size(), 1u);
    ASSERT_TRUE(done[0].second.error);
    EXPECT_NE(done[0].second.error->message.find("cannot wait here"), std::string::npos)
        << done[0].second.error->text();
    EXPECT_EQ(api.timers.size(), 0u);
}

TEST_F(LuaTask, ScriptCannotResumeOrCloseTask) {
    make();
    auto id = start(R"(
_G.T = coroutine.running()
local e, r = wisp.http.get("https://x/")
return r.status
)");
    ASSERT_TRUE(id);
    ASSERT_EQ(http.size(), 1u);
    EXPECT_FALSE(run(R"(
local ok, e = pcall(coroutine.resume, _G.T)
assert(not ok and e:find("cannot resume a task"), e)
ok, e = pcall(coroutine.close, _G.T)
assert(not ok and e:find("cannot close a task"), e)
local w = coroutine.wrap(function() return coroutine.resume(_G.T) end)
ok, e = pcall(w)
assert(not ok and e:find("cannot resume a task"), e)
assert(coroutine.status(_G.T) == "suspended")
)"));
    // Another task cannot either.
    ASSERT_TRUE(start("return pcall(coroutine.resume, _G.T)"));
    ASSERT_EQ(done.size(), 1u);
    EXPECT_NE(std::string(done[0].second.json).find("cannot resume a task"), std::string::npos)
        << std::string(done[0].second.json);
    EXPECT_FALSE(vm->faulted());
    // The task still gets its own answer.
    EXPECT_FALSE(wisp::scene::run_http_callback(*vm, http[0].first, status(200)));
    ASSERT_EQ(done.size(), 2u);
    EXPECT_EQ(done[1].first, id.value());
    EXPECT_EQ(std::string(done[1].second.json), "200");
    // Once ended it is an ordinary dead coroutine.
    EXPECT_FALSE(run(R"(
local ok, e = coroutine.resume(_G.T)
assert(not ok and e:find("dead"), e)
)"));
}

TEST_F(LuaTask, BareYieldIsError) {
    make();
    ASSERT_TRUE(start("coroutine.yield(1) return 2"));
    ASSERT_TRUE(start("coroutine.yield() return 2"));
    ASSERT_TRUE(start("coroutine.yield(coroutine.running()) return 2"));
    ASSERT_EQ(done.size(), 3u);
    for (auto& d : done) {
        EXPECT_FALSE(d.second.ok);
        ASSERT_TRUE(d.second.error);
        EXPECT_EQ(d.second.error->fault, ScriptFault::Runtime);
        EXPECT_NE(d.second.error->message.find("yield"), std::string::npos);
    }
}

TEST_F(LuaTask, OutsideTaskNoCallbackIsError) {
    make();
    auto err = run(R"(wisp.http.get("https://x/"))");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Runtime);
    EXPECT_NE(err->message.find("function expected, got no value"), std::string::npos)
        << err->text();
    err = run(R"(wisp.http.request{url = "https://x/"})");
    ASSERT_TRUE(err);
    EXPECT_NE(err->message.find("function expected"), std::string::npos) << err->text();
    err = run(R"(wisp.files.list())");
    ASSERT_TRUE(err);
    EXPECT_NE(err->message.find("function expected"), std::string::npos) << err->text();
    err = run("wisp.sleep(1)");
    ASSERT_TRUE(err);
    EXPECT_NE(err->message.find("bad argument"), std::string::npos) << err->text();
    EXPECT_NE(err->message.find("task"), std::string::npos) << err->text();
    EXPECT_TRUE(http.empty());
    EXPECT_TRUE(files.empty());
    EXPECT_EQ(api.timers.size(), 0u);
    EXPECT_EQ(wisp::scene::current_task(vm->L()), 0u);
}

TEST_F(LuaTask, CallbackFormInsideTask) {
    make();
    ASSERT_TRUE(start(R"(
wisp.http.get("https://x/", function(e, r) _G.mark() end)
return "sent"
)"));
    ASSERT_EQ(done.size(), 1u);
    EXPECT_EQ(std::string(done[0].second.json), "\"sent\"");
    ASSERT_EQ(http.size(), 1u);
    EXPECT_FALSE(wisp::scene::run_http_callback(*vm, http[0].first, status(200)));
    EXPECT_EQ(marks, 1);
    EXPECT_EQ(done.size(), 1u);
}

TEST_F(LuaTask, FaultNeverResumed) {
    VmLimits limits;
    limits.memory_bytes = 512 * 1024;
    make(limits);
    auto waiting = start(R"(
local e, r = wisp.http.get("https://x/")
_G.mark()
return 1
)");
    ASSERT_TRUE(waiting);
    ASSERT_EQ(http.size(), 1u);
    auto hog = start(R"(
local t = {}
for i = 1, 1e8 do t[i] = string.rep("x", 64) .. i end
)");
    ASSERT_TRUE(hog);
    ASSERT_EQ(done.size(), 1u);
    EXPECT_EQ(done[0].first, hog.value());
    ASSERT_TRUE(done[0].second.error);
    EXPECT_EQ(done[0].second.error->fault, ScriptFault::Memory);
    EXPECT_TRUE(vm->faulted());

    // The waiting task is finished with the fault, never resumed.
    auto err = wisp::scene::run_http_callback(*vm, http[0].first, status(200));
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Memory);
    EXPECT_EQ(marks, 0);
    ASSERT_EQ(done.size(), 2u);
    EXPECT_EQ(done[1].first, waiting.value());
    ASSERT_TRUE(done[1].second.error);
    EXPECT_EQ(done[1].second.error->fault, ScriptFault::Memory);

    // A duplicate late answer does nothing more.
    wisp::scene::run_http_callback(*vm, http[0].first, status(200));
    wisp::scene::cancel_tasks(*vm);
    EXPECT_EQ(marks, 0);
    EXPECT_EQ(done.size(), 2u);
}

TEST_F(LuaTask, FaultInsideWaitingTaskReportedOnce) {
    VmLimits limits;
    limits.memory_bytes = 512 * 1024;
    make(limits);
    ASSERT_TRUE(start(R"(
local e, r = wisp.http.get("https://x/")
local t = {}
for i = 1, 1e8 do t[i] = string.rep("x", 64) .. i end
)"));
    ASSERT_EQ(http.size(), 1u);
    auto err = wisp::scene::run_http_callback(*vm, http[0].first, status(200));
    ASSERT_TRUE(err);
    EXPECT_EQ(err->fault, ScriptFault::Memory);
    ASSERT_EQ(done.size(), 1u);
    EXPECT_EQ(done[0].second.error->fault, ScriptFault::Memory);
    wisp::scene::cancel_tasks(*vm);
    EXPECT_EQ(done.size(), 1u);
    // Starting another task on the faulted VM is refused.
    lua_pushcfunction(vm->L(), mark);
    EXPECT_FALSE(wisp::scene::start_task(*vm, nullptr, 0, 100));
    EXPECT_EQ(done.size(), 1u);
}

TEST_F(LuaTask, CancelDropsWaits) {
    make();
    auto a = start(R"(local e, r = wisp.http.get("https://x/") _G.mark())");
    auto b = start(R"(wisp.sleep(100) _G.mark())");
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    ASSERT_EQ(http.size(), 1u);
    wisp::scene::cancel_tasks(*vm);
    ASSERT_EQ(done.size(), 2u);
    for (auto& d : done) {
        EXPECT_FALSE(d.second.ok);
        ASSERT_TRUE(d.second.error);
        EXPECT_EQ(d.second.error->fault, ScriptFault::Runtime);
        EXPECT_EQ(d.second.error->message, "cancelled");
    }
    // The cancelled sleep holds no timer slot any more.
    EXPECT_EQ(api.timers.size(), 0u);
    EXPECT_FALSE(wisp::scene::run_http_callback(*vm, http[0].first, status(200)));
    now += 100;
    fire_timers();
    EXPECT_EQ(marks, 0);
    EXPECT_EQ(done.size(), 2u);
    wisp::scene::cancel_tasks(*vm);
    EXPECT_EQ(done.size(), 2u);
    // The VM is still usable.
    EXPECT_FALSE(run("_G.mark()"));
    EXPECT_EQ(marks, 1);
}

// One task cancelled while the VM goes on (a scene tool's call timing out):
// its wait goes, it never resumes, and a script that kept its thread still
// cannot resume or close it; the other tasks are untouched.
TEST_F(LuaTask, CancelOneKeepsItRefused) {
    make();
    auto a = start(R"(
_G.T = coroutine.running()
local e, r = wisp.http.get("https://x/")
_G.mark()
)");
    auto b = start("wisp.sleep(100) return 2");
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    ASSERT_EQ(http.size(), 1u);
    wisp::scene::cancel_task(*vm, a.value());
    ASSERT_EQ(done.size(), 1u);
    EXPECT_EQ(done[0].first, a.value());
    EXPECT_FALSE(done[0].second.ok);
    ASSERT_TRUE(done[0].second.error);
    EXPECT_EQ(done[0].second.error->message, "cancelled");
    // An unknown or ended task: nothing happens.
    wisp::scene::cancel_task(*vm, a.value());
    wisp::scene::cancel_task(*vm, 999);
    EXPECT_EQ(done.size(), 1u);
    // Its late answer does not resume it.
    EXPECT_FALSE(wisp::scene::run_http_callback(*vm, http[0].first, status(200)));
    EXPECT_EQ(marks, 0);
    // The script still holds the thread: it stays refused.
    EXPECT_FALSE(run(R"(
local ok, e = pcall(coroutine.resume, _G.T)
assert(not ok and e:find("cannot resume a task"), e)
ok, e = pcall(coroutine.close, _G.T)
assert(not ok and e:find("cannot close a task"), e)
)"));
    EXPECT_EQ(marks, 0);
    // The other task goes on.
    now += 100;
    fire_timers();
    ASSERT_EQ(done.size(), 2u);
    EXPECT_EQ(done[1].first, b.value());
    EXPECT_EQ(std::string(done[1].second.json), "2");
    wisp::scene::cancel_tasks(*vm);
    EXPECT_EQ(done.size(), 2u);
    EXPECT_FALSE(vm->faulted());
}

TEST_F(LuaTask, ResultTooLarge) {
    make();
    ASSERT_TRUE(start(R"(return string.rep("a", 100))", nullptr, 0, 16));
    ASSERT_EQ(done.size(), 1u);
    EXPECT_FALSE(done[0].second.ok);
    ASSERT_TRUE(done[0].second.error);
    EXPECT_EQ(done[0].second.error->fault, ScriptFault::Runtime);
    EXPECT_EQ(done[0].second.error->message, "result too large");

    ASSERT_TRUE(start("return 1, 2, 3, 4, 5, 6, 7, 8, 9", nullptr, 0, 16));
    ASSERT_EQ(done.size(), 2u);
    ASSERT_TRUE(done[1].second.error);
    EXPECT_EQ(done[1].second.error->message, "result too large");

    ASSERT_TRUE(start("return print"));
    ASSERT_EQ(done.size(), 3u);
    ASSERT_TRUE(done[2].second.error);
    EXPECT_EQ(done[2].second.error->message, "result not_serializable");
}

TEST_F(LuaTask, RuntimeErrorHasPosition) {
    make();
    ASSERT_TRUE(start("local x = nil\nreturn x.y"));
    ASSERT_EQ(done.size(), 1u);
    ASSERT_TRUE(done[0].second.error);
    EXPECT_EQ(done[0].second.error->fault, ScriptFault::Runtime);
    EXPECT_EQ(done[0].second.error->file, "task");
    EXPECT_EQ(done[0].second.error->line, 2);
}

TEST_F(LuaTask, NotAFunctionIsRefused) {
    make();
    lua_pushinteger(vm->L(), 5);
    auto r = wisp::scene::start_task(*vm, nullptr, 0, 100);
    EXPECT_FALSE(r);
    EXPECT_EQ(lua_gettop(vm->L()), 0);
    EXPECT_TRUE(done.empty());
}

TEST_F(LuaTask, CurrentTaskInsideTask) {
    make();
    lua_State* L = vm->L();
    static uint32_t seen = 0;
    lua_pushcfunction(L, [](lua_State* co) {
        seen = wisp::scene::current_task(co);
        return 0;
    });
    lua_setglobal(L, "probe");
    auto id = start("_G.probe()");
    ASSERT_TRUE(id);
    EXPECT_EQ(seen, id.value());
}

}  // namespace
