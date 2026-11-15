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
        follow_cells = true;
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

    // As SystemSettings::set() leaves the settings cell.
    template <class F>
    bool change_settings(F&& f) {
        return settings_cell.update(
            [&](wisp::core::SystemSettingsValues& v) { std::forward<F>(f)(v); });
    }

    // As TimeSyncService leaves its cell after an attempt.
    void sync_attempt(wisp::core::TimeSyncResult::Outcome outcome, int64_t time = 0) {
        time_cell.update([&](wisp::core::TimeSyncResult& r) {
            r.outcome = outcome;
            r.server.assign("pool.ntp.org");
            r.time = time;
            ++r.attempts;
        });
    }

    std::string corner_radius_on(const char* shape) {
        start(shape);
        install("radius", std::string("print(wisp.device.info().corner_radius)\n") + kScreen);
        if (!rt->open("radius")) return "(not opened)";
        auto log = scene_log("radius");
        return log.size() == 1 ? log[0] : "(" + std::to_string(log.size()) + " lines)";
    }

    void check_row_spans(const char* display_shape) {
        start(display_shape);
        install("span", std::string(R"(for _, y in ipairs({0, 10, 39, 40, 240, 479, -1, 480}) do
  print(y, wisp.device.row_span(y))
end
print(pcall(wisp.device.row_span, 1.5))
print(pcall(wisp.device.row_span, "x"))
)") + kScreen);
        ASSERT_TRUE(rt->open("span"));
        auto log = scene_log("span");
        ASSERT_EQ(log.size(), 10u);
        const int ys[] = {0, 10, 39, 40, 240, 479, -1, 480};
        for (size_t i = 0; i < 8; ++i) {
            SCOPED_TRACE(ys[i]);
            const wisp::hal::Span sp = wisp::hal::row_span(shape, ys[i]);
            std::string want = std::to_string(ys[i]);
            want +=
                sp.empty() ? "\tnil" : "\t" + std::to_string(sp.x1) + "\t" + std::to_string(sp.x2);
            EXPECT_EQ(log[i], want);
            // The ends are visible() and the pixels just past them are not.
            if (!sp.empty()) {
                EXPECT_TRUE(wisp::hal::visible(shape, sp.x1, ys[i]));
                EXPECT_TRUE(wisp::hal::visible(shape, sp.x2, ys[i]));
                EXPECT_FALSE(wisp::hal::visible(shape, sp.x1 - 1, ys[i]));
                EXPECT_FALSE(wisp::hal::visible(shape, sp.x2 + 1, ys[i]));
            }
        }
        EXPECT_EQ(log[8].rfind("false\t", 0), 0u) << log[8];
        EXPECT_NE(log[8].find("bad argument #1"), std::string::npos) << log[8];
        EXPECT_EQ(log[9].rfind("false\t", 0), 0u) << log[9];
        EXPECT_EQ(rt->status("span").state, SceneState::Running);
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
                  "480\t480\tcircle\t74\t74\t405\t405\tzh\tnil\t80\ttrue\tconnected"}));

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
                  "480\t480\tcircle\t74\t74\t405\t405\tzh\tnil\t80\ttrue\tconnected",
                  "480\t480\tcircle\t74\t74\t405\t405\tzh\t1793793605\tnil\tfalse\toff",
                  "480\t480\tcircle\t74\t74\t405\t405\tzh\t1793793605\tnil\ttrue\tbackoff",
                  "480\t480\tcircle\t74\t74\t405\t405\tzh\tnil\tnil\ttrue\tbackoff",
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

// corner_radius: the rounded rect's radius, a circle's half width, 0 for
// a rect.
TEST_F(SceneApiTest, DeviceInfoCornerRadiusCircle) { EXPECT_EQ(corner_radius_on("circle"), "240"); }

TEST_F(SceneApiTest, DeviceInfoCornerRadiusRoundedRect) {
    EXPECT_EQ(corner_radius_on("rounded_rect"), "40");
}

TEST_F(SceneApiTest, DeviceInfoCornerRadiusRect) { EXPECT_EQ(corner_radius_on("rect"), "0"); }

// status_area retains its original geometry for existing scenes.
TEST_F(SceneApiTest, DeviceInfoHasStatusArea) {
    start("circle");
    install("area", std::string(R"(local a = wisp.device.info().status_area
print(a.x1, a.y1, a.x2, a.y2)
)") + kScreen);
    ASSERT_TRUE(rt->open("area"));
    EXPECT_EQ(scene_log("area"), (std::vector<std::string>{"204\t78\t275\t101"}));
    const wisp::hal::Area want = wisp::ui::SystemLayer::status_area(shape);
    EXPECT_EQ(want.x1, 204);
    EXPECT_EQ(want.y2, 101);
}

// row_span(y): the visible columns of row y, from hal::row_span(); nil
// for a row with none or one off the screen.
TEST_F(SceneApiTest, DeviceRowSpanCircle) { check_row_spans("circle"); }

TEST_F(SceneApiTest, DeviceRowSpanRoundedRect) { check_row_spans("rounded_rect"); }

TEST_F(SceneApiTest, DeviceRowSpanRect) { check_row_spans("rect"); }

// Values worked out by hand: the circle's top row (a corner row) and its
// middle row, the rounded rect's top row and the first row below its
// corners.
TEST_F(SceneApiTest, DeviceRowSpanKnownValuesCircle) {
    start("circle");
    install("span", std::string("print(wisp.device.row_span(0))\n"
                                "print(wisp.device.row_span(240))\n") +
                        kScreen);
    ASSERT_TRUE(rt->open("span"));
    EXPECT_EQ(scene_log("span"), (std::vector<std::string>{"225\t254", "0\t479"}));
}

TEST_F(SceneApiTest, DeviceRowSpanKnownValuesRoundedRect) {
    start("rounded_rect");
    install("span", std::string("print(wisp.device.row_span(0))\n"
                                "print(wisp.device.row_span(40))\n") +
                        kScreen);
    ASSERT_TRUE(rt->open("span"));
    EXPECT_EQ(scene_log("span"), (std::vector<std::string>{"34\t445", "0\t479"}));
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
    supply_cell.set(wisp::core::PowerSupply{true, true, 80, false});
    start();
    install("dev", std::string(R"(
wisp.device.on("battery", function(i) print("battery", i.battery, i.charging) end)
wisp.device.on("lang", function(i) print("lang", i.lang, wisp.device.info().lang) end)
)") + kScreen);
    ASSERT_TRUE(rt->open("dev"));
    // Nothing changed: no event. The USB move wakes the battery check,
    // which finds the percent and charging the scene already saw.
    supply_cell.update([](wisp::core::PowerSupply& p) { p.usb_present = false; });
    change_settings([](wisp::core::SystemSettingsValues& v) { v.brightness = 100; });
    wisp::os::sleep_ms(150);
    EXPECT_TRUE(scene_log("dev").empty());

    set_status(Value(
        Map{{"battery", Value(Map{{"percent", Value(int64_t{79})}, {"charging", Value(false)}})}}));
    supply_cell.set(wisp::core::PowerSupply{false, false, 79, false});
    ASSERT_TRUE(wait_log("dev", 1));
    supply_cell.set(wisp::core::PowerSupply{false, false, 79, false});
    // A settings change that is not the language does not fire.
    change_settings([](wisp::core::SystemSettingsValues& v) { v.brightness = 50; });
    change_settings([](wisp::core::SystemSettingsValues& v) { v.language = "en"; });
    ASSERT_TRUE(wait_log("dev", 2));
    change_settings([](wisp::core::SystemSettingsValues& v) { v.brightness = 60; });
    wisp::os::sleep_ms(150);
    EXPECT_EQ(scene_log("dev"), (std::vector<std::string>{"battery\t79\tfalse", "lang\ten\ten"}));
}

// info().utc_offset follows the settings cell: the scene task never reads
// Settings, so it never touches flash for it. A changed offset fires
// "minute", as the local time a scene shows has moved.
TEST_F(SceneApiTest, UtcOffsetFollowsSettings) {
    wall = kNoon;
    change_settings([](wisp::core::SystemSettingsValues& v) { v.utc_offset_min = 330; });
    start();
    install("tz", std::string(R"(
print("open", wisp.device.info().utc_offset)
wisp.device.on("minute", function(i) print("minute", i.utc_offset, wisp.device.info().utc_offset) end)
)") + kScreen);
    ASSERT_TRUE(rt->open("tz"));
    ASSERT_TRUE(rt->check_sources({}));
    EXPECT_EQ(scene_log("tz"), (std::vector<std::string>{"open\t330"}));

    // The same offset: nothing fires.
    EXPECT_FALSE(
        change_settings([](wisp::core::SystemSettingsValues& v) { v.utc_offset_min = 330; }));
    change_settings([](wisp::core::SystemSettingsValues& v) { v.utc_offset_min = 0; });
    ASSERT_TRUE(wait_log("tz", 2));
    EXPECT_FALSE(
        change_settings([](wisp::core::SystemSettingsValues& v) { v.utc_offset_min = 0; }));
    change_settings([](wisp::core::SystemSettingsValues& v) { v.utc_offset_min = -570; });
    ASSERT_TRUE(wait_log("tz", 3));
    bus.wait_idle(1000);
    ASSERT_TRUE(rt->check_sources({}));
    EXPECT_EQ(scene_log("tz"),
              (std::vector<std::string>{"open\t330", "minute\t0\t0", "minute\t-570\t-570"}));
    // The value outlives the scene.
    ASSERT_TRUE(rt->reload("tz"));
    EXPECT_EQ(scene_log("tz").back(), "open\t-570");
}

// With the screen off a new offset fires nothing; the "minute" the screen
// coming on sends carries it.
TEST_F(SceneApiTest, UtcOffsetChangeWhileTheScreenIsOff) {
    wall = kNoon;
    start();
    install("tz", std::string(R"(
wisp.device.on("minute", function(i) print("minute", i.utc_offset) end)
)") + kScreen);
    ASSERT_TRUE(rt->open("tz"));
    rt->set_visible(false);
    change_settings([](wisp::core::SystemSettingsValues& v) { v.utc_offset_min = 60; });
    bus.wait_idle(1000);
    ASSERT_TRUE(rt->check_sources({}));
    EXPECT_TRUE(scene_log("tz").empty());
    rt->set_visible(true);
    ASSERT_TRUE(wait_log("tz", 1));
    bus.wait_idle(1000);
    ASSERT_TRUE(rt->check_sources({}));
    EXPECT_EQ(scene_log("tz"), (std::vector<std::string>{"minute\t60"}));
}

// A time sync moves the clock a scene shows: "minute" fires once at once
// while the screen is on. A failed sync moved nothing and fires nothing.
TEST_F(SceneApiTest, TimeSyncFiresMinute) {
    wall = kNoon;
    start();
    install("ts", std::string(R"(
wisp.device.on("minute", function(i) print("minute", i.time) end)
)") + kScreen);
    ASSERT_TRUE(rt->open("ts"));
    ASSERT_TRUE(rt->check_sources({}));
    sync_attempt(wisp::core::TimeSyncResult::Outcome::Failed);
    sync_attempt(wisp::core::TimeSyncResult::Outcome::Synced, kNoon);
    ASSERT_TRUE(wait_log("ts", 1));
    bus.wait_idle(1000);
    wisp::os::sleep_ms(150);
    ASSERT_TRUE(rt->check_sources({}));
    EXPECT_EQ(scene_log("ts"), (std::vector<std::string>{"minute\t" + std::to_string(kNoon)}));
}

// The sync that sets an unset or wrong clock moves the wall time across a
// minute boundary, which the minute ticker also sees: still one "minute".
TEST_F(SceneApiTest, TimeSyncThatMovesTheClockFiresMinuteOnce) {
    wall = kNoon;
    start();
    install("ts", std::string(R"(
wisp.device.on("minute", function(i) print("minute", i.time) end)
)") + kScreen);
    ASSERT_TRUE(rt->open("ts"));
    ASSERT_TRUE(rt->check_sources({}));
    const size_t before = scene_log("ts").size();
    wall = kNoon + 3600;
    sync_attempt(wisp::core::TimeSyncResult::Outcome::Synced, kNoon + 3600);
    ASSERT_TRUE(wait_log("ts", before + 1));
    bus.wait_idle(1000);
    wisp::os::sleep_ms(300);
    ASSERT_TRUE(rt->check_sources({}));
    const std::vector<std::string> log = scene_log("ts");
    ASSERT_EQ(log.size(), before + 1);
    EXPECT_EQ(log.back(), "minute\t" + std::to_string(kNoon + 3600));
}

TEST_F(SceneApiTest, TimeSyncThatSetsAnUnsetClockFiresMinuteOnce) {
    wall = 0;
    start();
    install("ts", std::string(R"(
wisp.device.on("minute", function(i) print("minute", i.time) end)
)") + kScreen);
    ASSERT_TRUE(rt->open("ts"));
    ASSERT_TRUE(rt->check_sources({}));
    wisp::os::sleep_ms(100);
    ASSERT_TRUE(rt->check_sources({}));
    const size_t before = scene_log("ts").size();
    wall = kNoon;
    sync_attempt(wisp::core::TimeSyncResult::Outcome::Synced, kNoon);
    ASSERT_TRUE(wait_log("ts", before + 1));
    bus.wait_idle(1000);
    wisp::os::sleep_ms(300);
    ASSERT_TRUE(rt->check_sources({}));
    const std::vector<std::string> log = scene_log("ts");
    ASSERT_EQ(log.size(), before + 1);
    EXPECT_EQ(log.back(), "minute\t" + std::to_string(kNoon));
}

// With the screen off a sync fires nothing; the "minute" the screen coming
// on sends carries the new time.
TEST_F(SceneApiTest, TimeSyncWhileTheScreenIsOff) {
    wall = kNoon;
    start();
    install("ts", std::string(R"(
wisp.device.on("minute", function(i) print("minute") end)
)") + kScreen);
    ASSERT_TRUE(rt->open("ts"));
    rt->set_visible(false);
    sync_attempt(wisp::core::TimeSyncResult::Outcome::Synced, kNoon);
    bus.wait_idle(1000);
    wisp::os::sleep_ms(150);
    ASSERT_TRUE(rt->check_sources({}));
    EXPECT_TRUE(scene_log("ts").empty());
    rt->set_visible(true);
    ASSERT_TRUE(wait_log("ts", 1));
    bus.wait_idle(1000);
    ASSERT_TRUE(rt->check_sources({}));
    EXPECT_EQ(scene_log("ts"), (std::vector<std::string>{"minute"}));
}

// info().time_24h (the Settings time format) follows the settings cell; a
// change fires "minute", since a clock on screen has to be written again.
TEST_F(SceneApiTest, Time24hFollowsSettings) {
    wall = kNoon;
    start();
    install("tf", std::string(R"(
print("open", tostring(wisp.device.info().time_24h))
wisp.device.on("minute", function(i) print("minute", tostring(i.time_24h)) end)
)") + kScreen);
    ASSERT_TRUE(rt->open("tf"));
    ASSERT_TRUE(rt->check_sources({}));
    EXPECT_EQ(scene_log("tf"), (std::vector<std::string>{"open\ttrue"}));

    // The same value: nothing fires.
    EXPECT_FALSE(change_settings([](wisp::core::SystemSettingsValues& v) { v.time_24h = true; }));
    change_settings([](wisp::core::SystemSettingsValues& v) { v.time_24h = false; });
    ASSERT_TRUE(wait_log("tf", 2));
    bus.wait_idle(1000);
    ASSERT_TRUE(rt->check_sources({}));
    EXPECT_EQ(scene_log("tf"), (std::vector<std::string>{"open\ttrue", "minute\tfalse"}));
    // The value outlives the scene.
    ASSERT_TRUE(rt->reload("tf"));
    EXPECT_EQ(scene_log("tf").back(), "open\tfalse");
}

// A runtime started after the settings changed shows them at once: the
// cell's value at start is what info() reports.
TEST_F(SceneApiTest, SettingsCellAtStartShowsInInfo) {
    change_settings([](wisp::core::SystemSettingsValues& v) {
        v.language = "en";
        v.utc_offset_min = 330;
        v.time_24h = false;
    });
    start();
    install("cfg", std::string(R"(
local i = wisp.device.info()
scene.on_show = function() print(i.lang, i.utc_offset, tostring(i.time_24h)) end
)") + kScreen);
    ASSERT_TRUE(rt->open("cfg"));
    EXPECT_EQ(scene_log("cfg"), (std::vector<std::string>{"en\t330\tfalse"}));
}

// Without a settings cell (a runtime built without SystemSettings) info()
// reports SceneRuntimeDeps::utc_offset_min, ui::lang() and 24-hour time.
TEST_F(SceneApiTest, InfoWithoutASettingsCell) {
    follow_cells = false;
    auto base = configure_deps;
    configure_deps = [base](wisp::scene::SceneRuntimeDeps& d) {
        base(d);
        d.utc_offset_min = 330;
    };
    start();
    install("cfg", std::string(R"(
local i = wisp.device.info()
scene.on_show = function() print(i.lang, i.utc_offset, tostring(i.time_24h)) end
)") + kScreen);
    ASSERT_TRUE(rt->open("cfg"));
    EXPECT_EQ(scene_log("cfg"), (std::vector<std::string>{"zh\t330\ttrue"}));
}

}  // namespace
