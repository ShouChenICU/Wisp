// The scene task's lanes (scene_runtime.hpp, "Scheduling"): the message
// queue first, then the scene in front, then the background scenes and the
// agent's Lua, with kQueueBurst and kFrontBurst keeping any lane from
// waiting for ever. Each test holds the scene task inside a handler of the
// scene in front (StallGate), lines work up behind it, lets it go and
// checks the order in which the work then ran.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "support/fake_http.hpp"
#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/core/log.hpp"
#include "wisp/net/scene_http.hpp"
#include "wisp/os/os.hpp"
#include "wisp/scene/scene_runtime.hpp"

namespace {

using wisp::core::SceneState;
using wisp::test::wait_until;

std::string manifest_with_hosts(const std::string& id, bool background) {
    return R"({"id":")" + id + R"(","name":"Demo","version":"1.0.0","schema_version":1,)" +
           (background ? R"("background":true,)" : "") +
           R"("permissions":{"http":["api.example.com"]}})";
}

// A get the proxy refuses at once (undeclared host): its result is put
// into the box from inside the call.
std::string refused_get(const std::string& printed) {
    return "wisp.http.get('https://other.example.com/', function() print('" + printed + "') end)\n";
}

class SceneLanes : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        SceneRuntimeFixture::SetUp();
        http = std::make_unique<wisp::net::SceneHttp>(client);
        ASSERT_TRUE(http->start());
        configure_deps = [this](wisp::scene::SceneRuntimeDeps& d) {
            d.http = http.get();
            d.device_status = [this] { return gate.device_status(); };
        };
    }

    void TearDown() override {
        gate.release();
        client.hold = false;
        SceneRuntimeFixture::TearDown();
        http.reset();
    }

    // The scene in front: "tap" prints "tap"; "stall" runs `before`, holds
    // the scene task in wisp.device.info(), prints "stalled", runs `after`.
    void open_fg(const std::string& before, const std::string& after = "") {
        install("fg",
                R"(
return ui.screen {
  ui.button { ref = "tap", text = "tap", on_click = function() print("tap") end },
  ui.button { ref = "stall", text = "stall", on_click = function()
)" + before + "wisp.device.info()\nprint('stalled')\n" +
                    after + R"(
  end },
}
)",
                {{"scene.json", manifest_with_hosts("fg", false)}});
        ASSERT_TRUE(rt->open("fg"));
        ASSERT_TRUE(wait_label("stall"));
    }

    // A scene in the background that prints "bg" every 50 ms. Opening the
    // scene in front afterwards sends it there.
    void open_bg() {
        install("bg", R"(
wisp.timer.every(50, function() print("bg") end)
return ui.screen { ui.label { text = "bg" } }
)",
                {{"scene.json", manifest_with_hosts("bg", true)}});
        ASSERT_TRUE(rt->open("bg"));
        ASSERT_TRUE(wait_until([&] { return !printed_by("bg").empty(); }, 2000));
    }

    std::vector<std::string> printed_by(const std::string& id) {
        std::vector<std::string> out;
        for (auto& line : scene_log(id))
            if (line.rfind("http: ", 0) != 0 && line.rfind("slow message", 0) != 0)
                out.push_back(std::move(line));
        return out;
    }

    // Holds the scene task inside "stall".
    void stall() {
        gate.arm();
        ASSERT_TRUE(rt->click_ref("stall"));
        ASSERT_TRUE(gate.wait_entered());
    }

    // Waits until `ms` have passed, so a timer due by then is due.
    static void pass(uint32_t ms) {
        const uint64_t until = wisp::os::now_ms() + ms;
        while (wisp::os::now_ms() <= until) wisp::os::sleep_ms(5);
    }

    // What fg, bg and the agent's Lua printed after "stalled", in order.
    std::vector<std::string> order() {
        std::vector<std::string> out;
        bool seen = false;
        for (auto& e : wisp::log::ring_entries()) {
            if (e.tag != "scene.fg" && e.tag != "scene.bg" && e.tag != "agent.lua") continue;
            if (e.msg.rfind("http: ", 0) == 0 || e.msg.rfind("slow message", 0) == 0) continue;
            if (!seen) {
                seen = e.tag == "scene.fg" && e.msg == "stalled";
                continue;
            }
            out.push_back(std::move(e.msg));
        }
        return out;
    }

    // Waits until order() has `n` lines and returns its first `n`.
    std::vector<std::string> wait_order(size_t n) {
        EXPECT_TRUE(wait_until([&] { return order().size() >= n; }, 3000))
            << ::testing::PrintToString(order());
        std::vector<std::string> out = order();
        if (out.size() > n) out.resize(n);
        return out;
    }

    wisp::test::FakeHttpClient client;
    std::unique_ptr<wisp::net::SceneHttp> http;
    wisp::test::StallGate gate;
};

// A tap on the scene in front runs before a background timer that came
// due while it waited.
TEST_F(SceneLanes, TapRunsBeforeABackgroundTimer) {
    start();
    open_bg();
    open_fg("");
    ASSERT_EQ(rt->status("bg").state, SceneState::Background);
    stall();
    ASSERT_TRUE(rt->click_ref("tap"));
    pass(60);
    gate.release();
    EXPECT_EQ(wait_order(2), (std::vector<std::string>{"tap", "bg"}));
}

// The front's own result runs before a background timer.
TEST_F(SceneLanes, FrontResultRunsBeforeABackgroundTimer) {
    start();
    open_bg();
    open_fg(refused_get("fg http"));
    stall();
    pass(60);
    gate.release();
    EXPECT_EQ(wait_order(2), (std::vector<std::string>{"fg http", "bg"}));
}

// A front with more work than kFrontBurst items still lets the background
// have one item after every kFrontBurst.
TEST_F(SceneLanes, BackgroundGetsATurnAfterFourFrontItems) {
    static_assert(wisp::scene::kFrontBurst == 4);
    start();
    open_bg();
    std::string gets;
    for (int i = 1; i <= 6; ++i) gets += refused_get("f" + std::to_string(i));
    open_fg(gets);
    stall();
    pass(60);
    gate.release();
    EXPECT_EQ(wait_order(7), (std::vector<std::string>{"f1", "f2", "f3", "f4", "bg", "f5", "f6"}));
}

// The agent's Lua is background work: its wisp.sleep ends after a tap on
// the scene in front has run.
TEST_F(SceneLanes, AgentLuaWaitsBehindTheFront) {
    start();
    open_fg("");
    const size_t used = rt->vm_pool_used();
    bool ok = false;
    std::thread agent([&] {
        auto r = rt->run_lua("wisp.sleep(50) print('agent') return 1", {});
        ok = r && r.value().ok;
    });
    ASSERT_TRUE(wait_until([&] { return rt->vm_pool_used() > used; }, 2000));
    stall();
    ASSERT_TRUE(rt->click_ref("tap"));
    pass(60);
    gate.release();
    EXPECT_EQ(wait_order(2), (std::vector<std::string>{"tap", "agent"}));
    agent.join();
    EXPECT_TRUE(ok);
}

// After kQueueBurst messages in a row a waiting result gets its turn. The
// stall message ran while no other lane had work, so it does not count.
TEST_F(SceneLanes, QueueBurstLetsAResultThrough) {
    static_assert(wisp::scene::kQueueBurst == 8);
    start();
    open_fg("", refused_get("fg http"));
    stall();
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(rt->click_ref("tap"));
    gate.release();
    std::vector<std::string> want(8, "tap");
    want.push_back("fg http");
    want.push_back("tap");
    want.push_back("tap");
    EXPECT_EQ(wait_order(11), want);
}

}  // namespace
