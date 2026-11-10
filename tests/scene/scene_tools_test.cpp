// SceneRuntime::call_tool() and headless scenes (agent-tools spec 5.2, 5.4).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "scene_message.hpp"
#include "support/fake_http.hpp"
#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/core/scene_files.hpp"
#include "wisp/net/scene_http.hpp"
#include "wisp/scene/scene_runtime.hpp"

namespace {

using wisp::ErrorCode;
using wisp::core::SceneState;
using wisp::scene::ToolCallResult;
using wisp::test::StateEvent;
using wisp::test::wait_until;

std::string str(const wisp::os::LargeString& s) { return {s.data(), s.size()}; }

// scene.json with the tools every test scene declares.
std::string tools_manifest(const std::string& id, const std::string& extra = "") {
    return R"({"id":")" + id + R"(","name":"Notes","version":"1.0.0","schema_version":1,)" +
           R"("permissions":{"http":["api.example.com"]},"tools":[)" +
           R"({"name":"get","description":"Get the count"},)" +
           R"({"name":"missing","description":"Never registered"},)" +
           R"({"name":"boom","description":"Raises"},)" +
           R"({"name":"spin","description":"Loops"},)" +
           R"({"name":"fetch","description":"Waits for HTTP"},)" +
           R"({"name":"nap","description":"Sleeps"}])" + extra + "}";
}

const char* kNotes = R"(
print("main")
scene.on_state = function(s) print("state:" .. s) end
scene.on_close = function() print("close") end
wisp.store.set("n", 41)
wisp.tools.register("get", function(args)
  return wisp.store.get("n"), args.x, wisp.scene.state()
end)
wisp.tools.register("boom", function() error("x") end)
wisp.tools.register("spin", function() while true do end end)
wisp.tools.register("fetch", function()
  local err, res = wisp.http.get("https://api.example.com/v")
  print("fetched")
  return res and res.body or err
end)
wisp.tools.register("nap", function() wisp.sleep(60000) print("woke") return 1 end)
return ui.screen { ui.label { text = "notes scene" } }
)";

constexpr uint32_t kIdleMs = 800;

class CallTool : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        SceneRuntimeFixture::SetUp();
        http = std::make_unique<wisp::net::SceneHttp>(client);
        ASSERT_TRUE(http->start());
        wisp::core::SceneFilesConfig fc;
        fc.data_root = in + "/scenes";
        fc.submit_io = [this](std::function<void()> job) { return worker.submit(std::move(job)); };
        files = std::make_unique<wisp::core::SceneFiles>(*storage, fc);
        configure_deps = [this](wisp::scene::SceneRuntimeDeps& d) {
            d.http = http.get();
            d.files = files.get();
            d.background_list_path = in + "/background.json";
        };
        configure_config = [](wisp::scene::SceneRuntimeConfig& c) { c.headless_idle_ms = kIdleMs; };
    }

    void TearDown() override {
        client.hold = false;
        rt.reset();
        http.reset();
        SceneRuntimeFixture::TearDown();
        files.reset();
    }

    void install_notes(const std::string& id = "notes") {
        install(id, kNotes, {{"scene.json", tools_manifest(id)}});
    }

    ToolCallResult call(const std::string& id, const std::string& tool,
                        const std::string& args = "{}", uint32_t timeout_ms = 5000,
                        std::function<bool()> cancelled = {}) {
        auto r = rt->call_tool(id, tool, args, timeout_ms, std::move(cancelled));
        EXPECT_TRUE(r) << (r ? "" : r.error().message);
        return r ? std::move(r.value()) : ToolCallResult{};
    }

    bool wait_state(const std::string& id, SceneState state, uint32_t ms = 3000) {
        return wait_until([&] { return rt->status(id).state == state; }, ms);
    }

    size_t count_log(const std::string& id, const std::string& line) {
        size_t n = 0;
        for (const auto& l : scene_log(id)) n += l == line ? 1 : 0;
        return n;
    }

    wisp::test::FakeHttpClient client;
    std::unique_ptr<wisp::net::SceneHttp> http;
    std::unique_ptr<wisp::core::SceneFiles> files;
};

TEST_F(CallTool, RunningForeground) {
    start();
    install_notes();
    ASSERT_TRUE(rt->open("notes"));
    ToolCallResult r = call("notes", "get", R"({"x":5})");
    ASSERT_TRUE(r.ok) << r.error << " " << r.message;
    EXPECT_EQ(str(r.json), R"([41,5,"foreground"])");
    EXPECT_EQ(rt->status("notes").state, SceneState::Running);
    EXPECT_EQ(count_log("notes", "main"), 1u);
}

TEST_F(CallTool, HeadlessStart) {
    start();
    install_notes();
    rt->restore_background();
    ToolCallResult r = call("notes", "get", R"({"x":"y"})");
    ASSERT_TRUE(r.ok) << r.error << " " << r.message;
    EXPECT_EQ(str(r.json), R"([41,"y","headless"])");
    EXPECT_EQ(rt->status("notes").state, SceneState::Headless);
    EXPECT_EQ(scene_log("notes"), (std::vector<std::string>{"main", "state:headless"}));
    EXPECT_TRUE(rt->background_scenes().empty());
    EXPECT_EQ(rt->foreground(), std::nullopt);
    EXPECT_EQ(events(), (std::vector<StateEvent>{{"notes", "headless"}}));
    // Nothing on screen.
    EXPECT_TRUE(label("notes scene").empty());
    // The background list is not written for it.
    r = call("notes", "get");
    ASSERT_TRUE(r.ok);
    std::ifstream f(in + "/background.json");
    std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_EQ(body.find("notes"), std::string::npos) << body;
    EXPECT_EQ(count_log("notes", "main"), 1u);
}

TEST_F(CallTool, HeadlessIdleClose) {
    start();
    install_notes();
    ASSERT_TRUE(call("notes", "get").ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(kIdleMs * 6 / 10));
    ASSERT_EQ(rt->status("notes").state, SceneState::Headless);
    // A new call starts the idle time again.
    ASSERT_TRUE(call("notes", "get").ok);
    const auto t1 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(kIdleMs * 6 / 10));
    EXPECT_EQ(rt->status("notes").state, SceneState::Headless);
    ASSERT_TRUE(wait_state("notes", SceneState::Stopped));
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1)
            .count();
    EXPECT_GE(ms, kIdleMs - 50);
    EXPECT_EQ(count_log("notes", "close"), 1u);
    EXPECT_EQ(events().back(), (StateEvent{"notes", "stopped"}));
    EXPECT_TRUE(log_has("scene", "notes: closed"));
}

TEST_F(CallTool, OnlyOneHeadless) {
    start();
    install_notes("a");
    install_notes("b");
    ASSERT_TRUE(call("a", "get").ok);
    EXPECT_EQ(rt->status("a").state, SceneState::Headless);
    ASSERT_TRUE(call("b", "get").ok);
    EXPECT_EQ(rt->status("a").state, SceneState::Stopped);
    EXPECT_EQ(rt->status("b").state, SceneState::Headless);
    EXPECT_EQ(count_log("a", "close"), 1u);
}

TEST_F(CallTool, OpenWhileHeadless) {
    start();
    install_notes();
    ASSERT_TRUE(call("notes", "get").ok);
    const uint32_t instance = wisp::scene::scene_instance(*rt, "notes");
    ASSERT_NE(instance, 0u);
    ASSERT_TRUE(rt->open("notes"));
    EXPECT_EQ(rt->status("notes").state, SceneState::Running);
    EXPECT_EQ(rt->foreground(), std::optional<std::string>("notes"));
    EXPECT_EQ(wisp::scene::scene_instance(*rt, "notes"), instance);
    ASSERT_TRUE(wait_label("notes scene"));
    EXPECT_EQ(count_log("notes", "main"), 1u);
    EXPECT_EQ(count_log("notes", "state:foreground"), 1u);
    ToolCallResult r = call("notes", "get");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(str(r.json), R"([41,null,"foreground"])");
    // The idle close no longer applies.
    std::this_thread::sleep_for(std::chrono::milliseconds(kIdleMs + 300));
    EXPECT_EQ(rt->status("notes").state, SceneState::Running);
}

TEST_F(CallTool, AwaitInsideTool) {
    start();
    install_notes();
    client.next.status = 200;
    client.next.body = "pong";
    ToolCallResult r = call("notes", "fetch");
    ASSERT_TRUE(r.ok) << r.error << " " << r.message;
    EXPECT_EQ(str(r.json), R"("pong")");
}

TEST_F(CallTool, Errors) {
    start();
    install_notes();
    ToolCallResult r = call("notes", "missing");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "not_registered");

    r = call("notes", "boom");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "scene_error");
    EXPECT_EQ(r.message.rfind("main.lua:", 0), 0u) << r.message;
    EXPECT_TRUE(r.message.ends_with(": x")) << r.message;
    // An error in the tool is not the scene's.
    EXPECT_EQ(rt->status("notes").state, SceneState::Headless);

    // The call's own time runs out while it waits: only the call ends.
    r = call("notes", "nap", "{}", 1000);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "timeout");
    EXPECT_EQ(rt->status("notes").state, SceneState::Headless);
    EXPECT_TRUE(call("notes", "get").ok);

    // A slice over the scene's limit faults the scene.
    r = call("notes", "spin");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "timeout");
    EXPECT_EQ(rt->status("notes").state, SceneState::Error);

    install("broken", "error('bad')\n", {{"scene.json", tools_manifest("broken")}});
    r = call("broken", "get");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "load_failed");
    EXPECT_NE(r.message.find("main.lua:1: bad"), std::string::npos) << r.message;

    r = call("nosuch", "get");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "not_found");
}

TEST_F(CallTool, NoMemory) {
    configure_config = [](wisp::scene::SceneRuntimeConfig& c) {
        c.vm_budget = 1024 * 1024;
        c.vm.memory_bytes = 1024 * 1024;
        c.headless_idle_ms = kIdleMs;
    };
    start();
    install("big", R"(scene.keep = string.rep("x", 800 * 1024)
return ui.screen { ui.label { text = "big" } }
)");
    install_notes();
    ASSERT_TRUE(rt->open("big"));
    ToolCallResult r = call("notes", "get");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "no_memory");
    EXPECT_EQ(rt->status("big").state, SceneState::Running);
    EXPECT_EQ(rt->foreground(), std::optional<std::string>("big"));
    EXPECT_EQ(rt->status("notes").state, SceneState::Stopped);
}

// Spec 5.4: room for a headless scene comes from the background first.
TEST_F(CallTool, EvictsBackgroundForRoom) {
    std::vector<wisp::scene::SceneNotice> notices;
    std::mutex notice_mu;
    configure_deps = [&](wisp::scene::SceneRuntimeDeps& d) {
        d.http = http.get();
        d.files = files.get();
        d.on_notice = [&](const wisp::scene::SceneNotice& n) {
            std::lock_guard<std::mutex> lock(notice_mu);
            notices.push_back(n);
        };
    };
    configure_config = [](wisp::scene::SceneRuntimeConfig& c) {
        c.vm_budget = 1536 * 1024;
        c.vm.memory_bytes = 1024 * 1024;
        c.headless_idle_ms = kIdleMs;
    };
    start();
    install("big", R"(scene.keep = string.rep("x", 800 * 1024)
return ui.screen { ui.label { text = "big" } }
)",
            {{"scene.json", R"({"id":"big","name":"Big","version":"1.0.0","schema_version":1,)"
                            R"("background":true})"}});
    install_notes();
    ASSERT_TRUE(rt->open("big"));
    ASSERT_TRUE(rt->close("big"));
    ASSERT_EQ(rt->status("big").state, SceneState::Background);
    ToolCallResult r = call("notes", "get");
    EXPECT_TRUE(r.ok) << r.error << " " << r.message;
    EXPECT_EQ(rt->status("big").state, SceneState::Stopped);
    std::lock_guard<std::mutex> lock(notice_mu);
    ASSERT_EQ(notices.size(), 1u);
    EXPECT_EQ(notices[0].kind, wisp::scene::SceneNoticeKind::EvictedMemory);
    EXPECT_EQ(notices[0].id, "big");
}

TEST_F(CallTool, SceneClosedWhileAwaiting) {
    start();
    install_notes();
    client.hold = true;
    std::thread t([&] {
        ASSERT_TRUE(wait_until([&] { return client.held.load() == 1; }, 3000));
        EXPECT_TRUE(rt->exit("notes"));
    });
    ToolCallResult r = call("notes", "fetch");
    t.join();
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "cancelled");
    EXPECT_EQ(rt->status("notes").state, SceneState::Stopped);
    client.hold = false;
    EXPECT_TRUE(wait_until([&] { return http->idle(); }, 3000));
    // The runtime goes on: the scene comes up again for the next call.
    r = call("notes", "get");
    EXPECT_TRUE(r.ok) << r.error << " " << r.message;
}

TEST_F(CallTool, Cancel) {
    start();
    install_notes();
    client.hold = true;
    std::atomic<bool> cancel{false};
    std::thread t([&] {
        ASSERT_TRUE(wait_until([&] { return client.held.load() == 1; }, 3000));
        cancel = true;
    });
    ToolCallResult r = call("notes", "fetch", "{}", 5000, [&] { return cancel.load(); });
    t.join();
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "cancelled");
    // The answer that arrives later resumes nothing.
    client.hold = false;
    EXPECT_TRUE(wait_until([&] { return http->idle(); }, 3000));
    EXPECT_TRUE(call("notes", "get").ok);
    EXPECT_EQ(count_log("notes", "fetched"), 0u);
    EXPECT_EQ(rt->status("notes").state, SceneState::Headless);
}

TEST_F(CallTool, BusyOnSameScene) {
    start();
    install_notes();
    std::atomic<bool> stop{false};
    std::thread t([&] {
        ToolCallResult r = call("notes", "nap", "{}", 5000, [&] { return stop.load(); });
        EXPECT_EQ(r.error, "cancelled");
    });
    ASSERT_TRUE(wait_state("notes", SceneState::Headless));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto second = rt->call_tool("notes", "get", "{}", 5000, {});
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code, ErrorCode::Busy);
    stop = true;
    t.join();
    EXPECT_EQ(count_log("notes", "woke"), 0u);
}

}  // namespace
