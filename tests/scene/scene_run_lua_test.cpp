// SceneRuntime::run_lua(): a throwaway headless VM on the scene task for the
// agent's lua.run (agent-tools spec 4.2, 4.3).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "support/fake_http.hpp"
#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/scene_files.hpp"
#include "wisp/net/scene_http.hpp"
#include "wisp/os/os.hpp"
#include "wisp/scene/scene_runtime.hpp"

namespace {

namespace fs = std::filesystem;

using wisp::ErrorCode;
using wisp::core::SceneState;
using wisp::scene::kLuaRunOutputBytes;
using wisp::scene::LuaRunOptions;
using wisp::scene::LuaRunResult;
using wisp::test::wait_until;

std::string str(const wisp::os::LargeString& s) { return {s.data(), s.size()}; }

class RunLua : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        SceneRuntimeFixture::SetUp();
        http = std::make_unique<wisp::net::SceneHttp>(client);
        ASSERT_TRUE(http->start());
        wisp::core::SceneFilesConfig fc;
        fc.data_root = in + "/scenes";
        fc.submit_io = [this](std::function<void()> job) { return worker.submit(std::move(job)); };
        files = std::make_unique<wisp::core::SceneFiles>(*storage, fc);
        notes = std::make_unique<wisp::core::NotificationService>(wisp::core::NotificationDeps{
            .bus = bus, .storage = *storage, .submit_io = {}, .make_timer = {}});
        configure_deps = [this](wisp::scene::SceneRuntimeDeps& d) {
            d.http = http.get();
            d.files = files.get();
            d.notifications = notes.get();
        };
    }

    void TearDown() override {
        client.hold = false;
        rt.reset();
        http.reset();
        SceneRuntimeFixture::TearDown();
        files.reset();
        notes.reset();
    }

    LuaRunResult run(const std::string& code, LuaRunOptions opts = {}) {
        auto r = rt->run_lua(code, std::move(opts));
        EXPECT_TRUE(r) << (r ? "" : r.error().message);
        return r ? std::move(r.value()) : LuaRunResult{};
    }

    wisp::test::FakeHttpClient client;
    std::unique_ptr<wisp::net::SceneHttp> http;
    std::unique_ptr<wisp::core::SceneFiles> files;
    std::unique_ptr<wisp::core::NotificationService> notes;
};

TEST_F(RunLua, ReturnAndPrint) {
    start();
    LuaRunResult r = run(R"(print("hi") return {a=1})");
    EXPECT_TRUE(r.ok) << r.error << " " << r.message;
    EXPECT_EQ(str(r.json), R"({"a":1})");
    EXPECT_EQ(str(r.output), "hi\n");
    EXPECT_GT(r.memory_peak, 0u);
    // The log keeps it as well, under the agent's own tag.
    EXPECT_TRUE(log_has("agent.lua", "hi"));

    r = run("print('a', 1) wisp.log.warn('w') return 1, 'x'");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(str(r.json), R"([1,"x"])");
    EXPECT_EQ(str(r.output), "a\t1\nw\n");

    r = run("local x = 1");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(str(r.json), "null");
}

TEST_F(RunLua, Errors) {
    start();
    LuaRunResult r = run("print('before') error('boom')");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "runtime");
    EXPECT_EQ(r.message, "run:1: boom");
    EXPECT_EQ(str(r.output), "before\n");

    r = run("return (");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "compile");
    EXPECT_EQ(r.message.rfind("run:1:", 0), 0u) << r.message;

    r = run("return string.rep('x', 70000)");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "too_large");
}

TEST_F(RunLua, OutputKeepsTail) {
    start();
    // 20 KiB in 100-byte lines, the last one marked.
    LuaRunResult r = run(R"(
for i = 1, 204 do print(string.rep("x", 99)) end
print("the end")
)");
    ASSERT_TRUE(r.ok) << r.message;
    const std::string out = str(r.output);
    EXPECT_EQ(out.rfind("…", 0), 0u);
    EXPECT_LE(out.size(), kLuaRunOutputBytes + 3);
    EXPECT_GE(out.size(), kLuaRunOutputBytes - 100);
    EXPECT_TRUE(out.ends_with("xxx\nthe end\n"));

    // A single print longer than the whole buffer keeps its own tail.
    r = run(R"(print(string.rep("y", 20000) .. "Z"))");
    ASSERT_TRUE(r.ok) << r.message;
    const std::string one = str(r.output);
    EXPECT_EQ(one.rfind("…", 0), 0u);
    EXPECT_LE(one.size(), kLuaRunOutputBytes + 3);
    EXPECT_TRUE(one.ends_with("yZ\n"));
}

TEST_F(RunLua, AwaitHttpAgentPrincipal) {
    start();
    client.next.status = 200;
    client.next.body = "pong";
    // Plain http:// to an undeclared host: only the Agent principal may.
    LuaRunResult r = run(R"(
local err, res = wisp.http.get("http://lan/x")
return tostring(err), res and res.status, res and res.body
)");
    ASSERT_TRUE(r.ok) << r.error << " " << r.message;
    // Several values, not a table: the order of a table's keys is not fixed.
    EXPECT_EQ(str(r.json), R"(["nil",200,"pong"])");
    ASSERT_TRUE(client.last());
    EXPECT_EQ(client.last()->url, "http://lan/x");
    // SceneHttp asks the client to truncate only for the Agent principal.
    // The scene id is not passed to the client at all: CancelWhileAwaiting
    // shows it through cancel_scene("@agent").
    EXPECT_TRUE(client.last()->truncate_at_limit);
}

TEST_F(RunLua, FilesInAgentArea) {
    start();
    LuaRunResult r = run(R"(
local err = wisp.files.write("note.txt", "hello")
assert(err == nil, err)
local err2, data = wisp.files.read("note.txt", 0, 100)
return data
)");
    ASSERT_TRUE(r.ok) << r.error << " " << r.message;
    EXPECT_EQ(str(r.json), R"("hello")");
    std::ifstream f(fs::path(in) / "library" / "agent" / "files" / "note.txt");
    std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_EQ(body, "hello");
}

// Spec 4.2: posted as Wisp itself, and kept like a background scene's.
TEST_F(RunLua, NotifyAsWisp) {
    start();
    LuaRunResult r = run(R"(return wisp.notify{title = "done", body = "b", id = "n1"})");
    ASSERT_TRUE(r.ok) << r.error << " " << r.message;
    EXPECT_EQ(str(r.json), R"("n1")");
    auto n = notes->find(wisp::core::kAgentSource, "n1");
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->source_name, "Wisp");
    EXPECT_EQ(n->title, "done");
}

TEST_F(RunLua, Sandbox) {
    start();
    LuaRunResult r = run(R"(
local absent = {}
for _, k in ipairs({"ui", "store", "timer", "scene", "sensor", "power", "import"}) do
  if wisp[k] ~= nil then absent[#absent + 1] = k end
end
local present = {}
for _, k in ipairs({"json", "files", "notify", "device", "log", "sleep", "http"}) do
  if wisp[k] ~= nil then present[#present + 1] = k end
end
return table.concat(absent, ","), table.concat(present, ","), ui == nil, scene == nil,
       state == nil
)");
    ASSERT_TRUE(r.ok) << r.message;
    EXPECT_EQ(str(r.json), R"(["","json,files,notify,device,log,sleep,http",true,true,true])");
}

TEST_F(RunLua, Timeout) {
    start();
    const size_t before = rt->vm_pool_used();
    const auto t0 = std::chrono::steady_clock::now();
    LuaRunOptions opts;
    opts.timeout_ms = 1000;
    LuaRunResult r = run("print('zz') wisp.sleep(5000) return 1", opts);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "timeout");
    EXPECT_EQ(str(r.output), "zz\n");
    EXPECT_GE(ms, 900);
    EXPECT_LT(ms, 3000);
    EXPECT_EQ(rt->vm_pool_used(), before);
}

TEST_F(RunLua, SliceLimit) {
    start();
    const auto t0 = std::chrono::steady_clock::now();
    LuaRunResult r = run("while true do end");
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "timeout");
    EXPECT_NE(r.message.find("1000 ms"), std::string::npos) << r.message;
    EXPECT_GE(ms, 900);
    EXPECT_LT(ms, 5000);
}

TEST_F(RunLua, MemoryLimit) {
    start();
    LuaRunOptions opts;
    opts.memory_bytes = 256 * 1024;
    LuaRunResult r = run("local t = {} for i = 1, 1e7 do t[i] = {i} end return #t", opts);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "memory");
    EXPECT_LE(r.memory_peak, 256u * 1024);
    EXPECT_GT(r.memory_peak, 128u * 1024);
}

TEST_F(RunLua, NoMemory) {
    configure_config = [](wisp::scene::SceneRuntimeConfig& c) {
        c.vm_budget = 1024 * 1024;
        c.vm.memory_bytes = 1024 * 1024;
    };
    start();
    install("big", R"(scene.keep = string.rep("x", 800 * 1024)
return ui.screen { ui.label { text = "big" } }
)");
    ASSERT_TRUE(rt->open("big"));
    LuaRunResult r = run("return 1");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "no_memory");
    EXPECT_EQ(rt->status("big").state, SceneState::Running);
    EXPECT_EQ(rt->foreground(), std::optional<std::string>("big"));
}

TEST_F(RunLua, CancelWhileAwaiting) {
    start();
    client.hold = true;
    std::atomic<bool> cancel{false};
    LuaRunOptions opts;
    opts.cancelled = [&] { return cancel.load(); };
    std::thread t([&] {
        ASSERT_TRUE(wait_until([&] { return client.held.load() == 1; }, 3000));
        cancel = true;
    });
    LuaRunResult r = run("local err, res = wisp.http.get('http://lan/slow') return err", opts);
    t.join();
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "cancelled");
    // The proxy stopped the request (cancel_scene("@agent")).
    EXPECT_TRUE(wait_until([&] { return client.saw_cancel.load(); }, 3000));
    EXPECT_TRUE(wait_until([&] { return http->idle(); }, 3000));
    // The runtime still works, and a second run is not refused.
    r = run("return 2");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(str(r.json), "2");
}

TEST_F(RunLua, SceneTaskNotBlocked) {
    start();
    install("tick", R"(wisp.timer.every(100, function() print("tick") end)
return ui.screen { ui.label { text = "t" } }
)");
    ASSERT_TRUE(rt->open("tick"));
    std::atomic<bool> stop{false};
    std::atomic<bool> done{false};
    const size_t idle = rt->vm_pool_used();
    std::thread t([&] {
        LuaRunOptions opts;
        opts.cancelled = [&] { return stop.load(); };
        run("wisp.sleep(60000) return 1", opts);
        done = true;
    });
    // The run's VM is up and waiting: the scene's timer still fires.
    ASSERT_TRUE(wait_until([&] { return rt->vm_pool_used() > idle; }, 5000));
    const size_t before = scene_log("tick").size();
    EXPECT_TRUE(wait_until([&] { return scene_log("tick").size() >= before + 3; }, 10000));
    EXPECT_FALSE(done.load());
    stop = true;
    t.join();
}

TEST_F(RunLua, BusyAndBadOptions) {
    start();
    std::atomic<bool> stop{false};
    const size_t idle = rt->vm_pool_used();
    std::thread t([&] {
        LuaRunOptions opts;
        opts.cancelled = [&] { return stop.load(); };
        run("wisp.sleep(60000) return 1", opts);
    });
    ASSERT_TRUE(wait_until([&] { return rt->vm_pool_used() > idle; }, 5000));
    auto second = rt->run_lua("return 1", {});
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code, ErrorCode::Busy);
    stop = true;
    t.join();

    LuaRunOptions opts;
    opts.timeout_ms = 999;
    auto bad = rt->run_lua("return 1", opts);
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, ErrorCode::InvalidArgument);
    opts = {};
    opts.memory_bytes = 3 * 1024 * 1024;
    bad = rt->run_lua("return 1", opts);
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, ErrorCode::InvalidArgument);
}

}  // namespace
