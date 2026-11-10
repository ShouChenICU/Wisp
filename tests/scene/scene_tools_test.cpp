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
           R"({"name":"nap","description":"Sleeps"},)" +
           R"({"name":"doze","description":"Sleeps a little"},)" +
           R"({"name":"hog","description":"Holds args.kb KiB"},)" +
           R"({"name":"deny","description":"Asks for the screen and a sensor"},)" +
           R"({"name":"poison","description":"Breaks a binding"}])" + extra + "}";
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
wisp.tools.register("doze", function() wisp.sleep(50) return "rested" end)
wisp.tools.register("hog", function(args)
  -- 64 KiB pieces: no single buffer twice the size, as one string.rep would need.
  scene.keep = {}
  for i = 1, args.kb // 64 do scene.keep[i] = string.rep("x", 65536) end
  return #scene.keep
end)
wisp.tools.register("deny", function()
  local a, e1 = wisp.power.keep_screen_on(true)
  local b, e2 = wisp.sensor.subscribe("accel", function() end)
  return e1, e2
end)
local level = state(0)
wisp.tools.register("poison", function() level:set(1) return "fine" end)
return ui.screen {
  ui.label { text = "notes scene" },
  ui.label { text = bind(level, function(v)
    if v > 0 then error("bad bind") end
    return "level ok"
  end) },
}
)";

constexpr uint32_t kIdleMs = 1500;

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
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    ASSERT_EQ(rt->status("notes").state, SceneState::Headless);
    // A new call starts the idle time again: 2000 ms after the first one,
    // past kIdleMs, it is still there.
    ASSERT_TRUE(call("notes", "get").ok);
    const auto t1 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    EXPECT_EQ(rt->status("notes").state, SceneState::Headless);
    ASSERT_TRUE(wait_state("notes", SceneState::Stopped, 3000));
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1)
            .count();
    EXPECT_GE(ms, kIdleMs - 100);
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

// Default budget ratios (3 MiB, 2 MiB per scene): a headless start with a
// scene in front leaves a small background scene where it is.
TEST_F(CallTool, BackgroundSurvivesHeadlessStart) {
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
    start();
    install("bgs", "return ui.screen { ui.label { text = 'bgs' } }",
            {{"scene.json", R"({"id":"bgs","name":"Bg","version":"1.0.0","schema_version":1,)"
                            R"("background":true})"}});
    install("front", "return ui.screen { ui.label { text = 'front' } }");
    install_notes();
    ASSERT_TRUE(rt->open("bgs"));
    ASSERT_TRUE(rt->open("front"));
    ASSERT_EQ(rt->status("bgs").state, SceneState::Background);
    ToolCallResult r = call("notes", "get");
    EXPECT_TRUE(r.ok) << r.error << " " << r.message;
    EXPECT_EQ(rt->status("bgs").state, SceneState::Background);
    EXPECT_EQ(rt->status("front").state, SceneState::Running);
    std::lock_guard<std::mutex> lock(notice_mu);
    EXPECT_TRUE(notices.empty());
}

// With no scene in front, a headless one still leaves vm.memory_bytes of
// the budget for a scene the user may open.
TEST_F(CallTool, HeadlessLeavesFrontReserve) {
    start();
    install_notes();
    ToolCallResult r = call("notes", "hog", R"({"kb":512})");
    ASSERT_TRUE(r.ok) << r.error << " " << r.message;
    EXPECT_EQ(str(r.json), "8");
    // 3 MiB budget - 2 MiB reserve: 1.5 MiB does not fit.
    r = call("notes", "hog", R"({"kb":1536})");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "scene_error");
}

// A headless scene's timers run with the screen off.
TEST_F(CallTool, ScreenOffTimersRun) {
    start();
    install_notes();
    rt->set_visible(false);
    ToolCallResult r = call("notes", "doze", "{}", 3000);
    ASSERT_TRUE(r.ok) << r.error << " " << r.message;
    EXPECT_EQ(str(r.json), R"("rested")");
}

// Spec 3.4's rules: no screen lock and no sensors without a view.
TEST_F(CallTool, HeadlessRefusesPowerAndSensors) {
    start();
    install_notes();
    ToolCallResult r = call("notes", "deny");
    ASSERT_TRUE(r.ok) << r.error << " " << r.message;
    EXPECT_EQ(str(r.json), R"(["not_in_background","not_in_background"])");
}

// A fault in the flush after the tool's last slice is the call's error.
TEST_F(CallTool, FlushFaultIsCallError) {
    start();
    install_notes();
    ToolCallResult r = call("notes", "poison");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "scene_error");
    EXPECT_NE(r.message.find("bad bind"), std::string::npos) << r.message;
    EXPECT_EQ(rt->status("notes").state, SceneState::Error);
}

// A boot launch or open_background() of a headless scene that declares
// "background" sends it there, onto the background list.
TEST_F(CallTool, BootOfHeadlessGoesToBackground) {
    start();
    install("bgn", kNotes, {{"scene.json", tools_manifest("bgn", R"(,"background":true)")}});
    install("bgo", kNotes, {{"scene.json", tools_manifest("bgo", R"(,"background":true)")}});
    ASSERT_TRUE(call("bgn", "get").ok);
    ASSERT_EQ(rt->status("bgn").state, SceneState::Headless);
    ASSERT_TRUE(rt->launch("bgn", wisp::scene::LaunchReason::Boot));
    EXPECT_EQ(rt->status("bgn").state, SceneState::Background);
    ASSERT_TRUE(call("bgo", "get").ok);
    ASSERT_TRUE(rt->open_background("bgo"));
    EXPECT_EQ(rt->status("bgo").state, SceneState::Background);
    std::vector<std::string> ids;
    for (const auto& b : rt->background_scenes()) ids.push_back(b.id);
    EXPECT_EQ(ids, (std::vector<std::string>{"bgn", "bgo"}));
    EXPECT_EQ(count_log("bgn", "main"), 1u);
    EXPECT_EQ(count_log("bgn", "state:background"), 1u);
    // No idle close any more.
    std::this_thread::sleep_for(std::chrono::milliseconds(kIdleMs + 300));
    EXPECT_EQ(rt->status("bgn").state, SceneState::Background);
    EXPECT_EQ(rt->status("bgo").state, SceneState::Background);
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
