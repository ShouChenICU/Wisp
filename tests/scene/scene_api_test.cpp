// wisp.timer, wisp.device, wisp.log and wisp.scene on a running SceneRuntime
// (scene-runtime-core spec 3.4, 5.1, 5.2, 5.5).

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"
#include "wisp/scene/minute_ticker.hpp"
#include "wisp/ui/strings.hpp"

namespace {

using wisp::List;
using wisp::Map;
using wisp::Value;
using wisp::core::SceneState;
using wisp::test::wait_until;

// 2026-11-04 12:00:00 UTC.
constexpr int64_t kNoon = 1793793600;

const char* kScreen = "return ui.screen { ui.label { text = \"x\" } }\n";

class SceneApiTest : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        SceneRuntimeFixture::SetUp();
        wisp::ui::set_lang(wisp::ui::Lang::Zh);
        status = Value(
            Map{{"battery", Value(Map{{"percent", Value(int64_t{80})}, {"charging", Value(true)}})},
                {"wifi", Value("connected")}});
        configure_deps = [this](wisp::scene::SceneRuntimeDeps& d) {
            d.device_status = [this] {
                std::lock_guard<std::mutex> lock(status_mu);
                return status;
            };
            d.wall_clock = [this] { return wall.load(); };
        };
    }

    void TearDown() override {
        SceneRuntimeFixture::TearDown();
        wisp::ui::set_lang(wisp::ui::Lang::Zh);
    }

    void set_status(Value v) {
        std::lock_guard<std::mutex> lock(status_mu);
        status = std::move(v);
    }

    bool wait_log(const std::string& id, size_t n, uint32_t ms = 2000) {
        return wait_until([&] { return scene_log(id).size() >= n; }, ms);
    }

    void publish(const std::string& topic, wisp::Properties payload) {
        wisp::core::Event e;
        e.topic = topic;
        e.source = "test";
        e.payload = std::move(payload);
        bus.publish(std::move(e));
    }

    std::mutex status_mu;
    Value status;
    std::atomic<int64_t> wall{0};
};

const char* kPrintInfo = R"(local function show(i)
  local s = i.safe_area
  print(i.width, i.height, i.shape, s.x1, s.y1, s.x2, s.y2, i.lang, tostring(i.time),
        tostring(i.battery), tostring(i.charging), i.wifi)
end
show(wisp.device.info())
)";

TEST_F(SceneApiTest, DeviceInfoShape) {
    start();
    install("info", std::string(kPrintInfo) + kScreen);
    ASSERT_TRUE(rt->open("info"));
    EXPECT_EQ(scene_log("info"),
              (std::vector<std::string>{
                  "480\t480\tcircle\t71\t71\t408\t408\tzh\tnil\t80\ttrue\tconnected"}));

    // A set clock, an unknown battery (none, or the -1 a power chip gives
    // when it cannot tell), no Wi-Fi field.
    wall = kNoon + 5;
    set_status(Value(Map{{"battery", Value()}}));
    ASSERT_TRUE(rt->reload("info"));
    set_status(Value(
        Map{{"battery", Value(Map{{"percent", Value(int64_t{-1})}, {"charging", Value(true)}})},
            {"wifi", Value("backoff")}}));
    ASSERT_TRUE(rt->reload("info"));
    wall = wisp::scene::MinuteTicker::kValidSince - 1;
    ASSERT_TRUE(rt->reload("info"));
    EXPECT_EQ(scene_log("info"),
              (std::vector<std::string>{
                  "480\t480\tcircle\t71\t71\t408\t408\tzh\tnil\t80\ttrue\tconnected",
                  "480\t480\tcircle\t71\t71\t408\t408\tzh\t1793793605\tnil\tfalse\toff",
                  "480\t480\tcircle\t71\t71\t408\t408\tzh\t1793793605\tnil\ttrue\tbackoff",
                  "480\t480\tcircle\t71\t71\t408\t408\tzh\tnil\tnil\ttrue\tbackoff",
              }));
}

TEST_F(SceneApiTest, DeviceInfoOnRectScreen) {
    start("rect");
    install("info", std::string(kPrintInfo) + kScreen);
    ASSERT_TRUE(rt->open("info"));
    auto log = scene_log("info");
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].rfind("480\t480\trect\t0\t0\t479\t479\t", 0), 0u) << log[0];
}

TEST_F(SceneApiTest, DeviceOnRejectsUnknownEvent) {
    start();
    install("on", std::string(R"(
print(pcall(wisp.device.on, "nope", function() end))
print(pcall(wisp.device.on, "minute", 5))
print(pcall(wisp.device.on, 7, function() end))
)") + kScreen);
    ASSERT_TRUE(rt->open("on"));
    auto log = scene_log("on");
    ASSERT_EQ(log.size(), 3u);
    EXPECT_EQ(log[0].rfind("false\t", 0), 0u) << log[0];
    EXPECT_NE(log[0].find("bad argument #1"), std::string::npos) << log[0];
    EXPECT_NE(log[0].find("unknown event 'nope'"), std::string::npos) << log[0];
    EXPECT_NE(log[1].find("bad argument #2"), std::string::npos) << log[1];
    EXPECT_NE(log[2].find("bad argument #1"), std::string::npos) << log[2];
    EXPECT_EQ(rt->status("on").state, SceneState::Running);
}

TEST_F(SceneApiTest, LogTagIsSceneId) {
    start();
    // The longest id a scene can have (scene.json allows 32 bytes).
    const std::string id = "abcdefghijklmnopqrstuvwxyz-01234";
    ASSERT_EQ(id.size(), 32u);
    install(id, std::string(R"(
wisp.log.warn("a", 1)
wisp.log.info(nil, true, {} ~= nil)
wisp.log.error("e")
print("a", 1)
wisp.log.info(string.rep("x", 600))
wisp.log.info(string.rep("y", 300))
)") + kScreen);
    ASSERT_TRUE(rt->open(id));
    std::vector<wisp::LogEntry> mine;
    for (auto& e : wisp::log::ring_entries())
        if (e.tag == "scene." + id) mine.push_back(e);
    ASSERT_EQ(mine.size(), 6u);
    EXPECT_EQ(mine[0].level, wisp::LogLevel::Warn);
    EXPECT_EQ(mine[0].msg, "a\t1");
    EXPECT_EQ(mine[1].level, wisp::LogLevel::Info);
    EXPECT_EQ(mine[1].msg, "nil\ttrue\ttrue");
    EXPECT_EQ(mine[2].level, wisp::LogLevel::Error);
    EXPECT_EQ(mine[2].msg, "e");
    // print is the same line at Info.
    EXPECT_EQ(mine[3].level, wisp::LogLevel::Info);
    EXPECT_EQ(mine[3].msg, mine[0].msg);
    // Lines are cut at 512 bytes, not at the 255 of a printf-style log.
    EXPECT_EQ(mine[4].msg, std::string(512, 'x'));
    EXPECT_EQ(mine[5].msg, std::string(300, 'y'));
}

TEST_F(SceneApiTest, SceneCloseAndArgs) {
    start();
    install("args", std::string(R"(
local a = wisp.scene.args()
print(a.n, a.name, a.list[1], a.list[2], a.map.k, #a.list)
a.n = 99
print(wisp.scene.args().n)
scene.on_close = function() print("on_close") end
return ui.screen { ui.button { ref = "x", text = "x", on_click = function()
  wisp.scene.close()
  print("after close")
end } }
)"));
    Value args(Map{{"n", Value(int64_t{3})},
                   {"name", Value("名字")},
                   {"list", Value(List{Value(int64_t{1}), Value("b")})},
                   {"map", Value(Map{{"k", Value(true)}})}});
    ASSERT_TRUE(rt->open("args", args));
    ASSERT_TRUE(wait_label("x"));
    ASSERT_TRUE(rt->click_ref("x"));
    ASSERT_TRUE(wait_until([&] { return rt->status("args").state == SceneState::Stopped; }, 2000));
    EXPECT_EQ(rt->foreground(), std::nullopt);
    EXPECT_EQ(scene_log("args"),
              (std::vector<std::string>{"3\t名字\t1\tb\ttrue\t2", "3", "after close", "on_close"}));

    // Opened without arguments: an empty table.
    install("noargs",
            std::string("print(type(wisp.scene.args()), next(wisp.scene.args()))\n") + kScreen);
    ASSERT_TRUE(rt->open("noargs"));
    EXPECT_EQ(scene_log("noargs"), (std::vector<std::string>{"table\tnil"}));
}

TEST_F(SceneApiTest, TimersFireAndCancel) {
    start();
    install("timers", std::string(R"(
local n = 0
local h
h = wisp.timer.every(50, function()
  n = n + 1
  print("every " .. n)
  if n == 3 then print("cancel", h:cancel(), h:cancel()) end
end)
wisp.timer.after(60, function() print("after") end)
local dead = wisp.timer.after(50, function() print("never") end)
print("cancelled", dead:cancel())
print(wisp.timer.after(49, function() end))
print(pcall(wisp.timer.after, 50, "x"))
local made = 2
while wisp.timer.after(10000, function() end) do made = made + 1 end
print("made", made, wisp.timer.every(10000, function() end))
)") + kScreen);
    ASSERT_TRUE(rt->open("timers"));
    ASSERT_TRUE(wait_log("timers", 9));
    wisp::os::sleep_ms(200);
    auto log = scene_log("timers");
    ASSERT_GE(log.size(), 4u);
    EXPECT_EQ(log[0], "cancelled\ttrue");
    EXPECT_EQ(log[1], "nil\tinterval_too_small");
    EXPECT_EQ(log[2].rfind("false\t", 0), 0u) << log[2];
    EXPECT_NE(log[2].find("bad argument #2"), std::string::npos) << log[2];
    // `dead` was cancelled, so 2 + 30 more fit.
    EXPECT_EQ(log[3], "made\t32\tnil\ttoo_many_timers");
    std::vector<std::string> rest(log.begin() + 4, log.end());
    EXPECT_EQ(std::count(rest.begin(), rest.end(), "after"), 1);
    EXPECT_EQ(std::count(rest.begin(), rest.end(), "never"), 0);
    EXPECT_EQ(std::count(rest.begin(), rest.end(), "every 3"), 1);
    EXPECT_EQ(std::count(rest.begin(), rest.end(), "every 4"), 0);
    EXPECT_EQ(std::count(rest.begin(), rest.end(), "cancel\ttrue\tfalse"), 1);
    EXPECT_EQ(rt->status("timers").state, SceneState::Running);
}

TEST_F(SceneApiTest, TimerErrorFaultsSceneAndStopsTimers) {
    start();
    install("bad", std::string(R"(
local n = 0
wisp.timer.every(50, function() n = n + 1; print("tick " .. n); error("boom") end)
)") + kScreen);
    ASSERT_TRUE(rt->open("bad"));
    ASSERT_TRUE(wait_until([&] { return rt->status("bad").state == SceneState::Error; }, 2000));
    EXPECT_EQ(rt->status("bad").error, "main.lua:3: boom");
    wisp::os::sleep_ms(200);
    auto log = scene_log("bad");
    EXPECT_EQ(std::count(log.begin(), log.end(), "tick 1"), 1);
    EXPECT_EQ(std::count(log.begin(), log.end(), "tick 2"), 0);
}

TEST_F(SceneApiTest, TimerCallbackHasItsOwnTimeLimit) {
    start();
    install("slow", std::string(R"(
wisp.timer.after(50, function() while true do end end)
)") + kScreen);
    ASSERT_TRUE(rt->open("slow"));
    ASSERT_TRUE(wait_until([&] { return rt->status("slow").state == SceneState::Error; }, 3000));
    EXPECT_NE(rt->status("slow").error.find("200 ms"), std::string::npos)
        << rt->status("slow").error;
}

TEST_F(SceneApiTest, HidePausesTimersShowResumes) {
    wall = kNoon + 30;  // a set clock that stands still: no minute of its own
    start();
    install("pause", std::string(R"(
local n = 0
wisp.timer.every(1000, function() n = n + 1; print("tick " .. n) end)
wisp.device.on("minute", function(i) print("minute", i.time) end)
scene.on_show = function() print("show") end
scene.on_hide = function() print("hide") end
)") + kScreen);
    ASSERT_TRUE(rt->open("pause"));
    rt->set_visible(false);
    ASSERT_TRUE(wait_log("pause", 2));
    wisp::os::sleep_ms(1500);
    EXPECT_EQ(scene_log("pause"), (std::vector<std::string>{"show", "hide"}));
    const uint64_t on_at = wisp::os::now_ms();
    rt->set_visible(true);
    ASSERT_TRUE(wait_log("pause", 5, 500));
    wisp::os::sleep_ms(300);
    EXPECT_EQ(scene_log("pause"),
              (std::vector<std::string>{"show", "hide", "tick 1", "minute\t1793793630", "show"}));
    // The next one a full period after the screen came on, not at the
    // timer's old phase (which was due 500 ms after it).
    ASSERT_TRUE(wait_log("pause", 6, 2000));
    EXPECT_GE(wisp::os::now_ms() - on_at, 950u);
    EXPECT_EQ(scene_log("pause").back(), "tick 2");
}

TEST_F(SceneApiTest, MinuteFiresOnWallMinute) {
    wall = kNoon + 59;
    start();
    install("clock", std::string(R"(
wisp.device.on("minute", function(i) print("minute", i.time) end)
)") + kScreen);
    ASSERT_TRUE(rt->open("clock"));
    // A round trip through the scene queue: the task has finished the
    // loop pass after the open, whose first ticker poll took its bearings
    // at kNoon + 59.
    ASSERT_TRUE(rt->check_sources({}));
    EXPECT_TRUE(scene_log("clock").empty());
    wall = kNoon + 60;
    ASSERT_TRUE(wait_log("clock", 1, 2500));
    EXPECT_EQ(scene_log("clock"), (std::vector<std::string>{"minute\t1793793660"}));
}

TEST_F(SceneApiTest, BatteryAndLangEvents) {
    start();
    install("dev", std::string(R"(
wisp.device.on("battery", function(i) print("battery", i.battery, i.charging) end)
wisp.device.on("lang", function(i) print("lang", i.lang, wisp.device.info().lang) end)
)") + kScreen);
    ASSERT_TRUE(rt->open("dev"));
    // Nothing changed: no event.
    publish("power.charging.changed", {{"charging", Value(true)}});
    publish("system.settings", {{"language", Value("zh")}, {"brightness", Value(int64_t{100})}});
    wisp::os::sleep_ms(150);
    EXPECT_TRUE(scene_log("dev").empty());

    set_status(Value(
        Map{{"battery", Value(Map{{"percent", Value(int64_t{79})}, {"charging", Value(false)}})}}));
    publish("power.charging.changed", {{"charging", Value(false)}});
    ASSERT_TRUE(wait_log("dev", 1));
    publish("power.charging.changed", {{"charging", Value(false)}});
    // A settings change that is not the language does not fire.
    publish("system.settings", {{"language", Value("zh")}, {"brightness", Value(int64_t{50})}});
    publish("system.settings", {{"language", Value("en")}, {"brightness", Value(int64_t{50})}});
    ASSERT_TRUE(wait_log("dev", 2));
    publish("system.settings", {{"language", Value("en")}, {"brightness", Value(int64_t{60})}});
    wisp::os::sleep_ms(150);
    EXPECT_EQ(scene_log("dev"), (std::vector<std::string>{"battery\t79\tfalse", "lang\ten\ten"}));
}

}  // namespace
