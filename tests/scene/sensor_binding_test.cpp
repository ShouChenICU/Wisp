// wisp.sensor (scene-device spec 6.2): subscriptions and gestures through
// core::SensorService, paused in the background, released on exit.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/core/occupancy.hpp"
#include "wisp/core/sensor_service.hpp"
#include "wisp/scene/scene_runtime.hpp"

namespace {

using wisp::core::OccupancyRegistry;
using wisp::core::SceneState;
using wisp::core::SensorService;
using wisp::scene::LaunchReason;
using wisp::test::wait_until;

std::string bg_manifest(const std::string& id) {
    return R"({"id":")" + id +
           R"(","name":"Demo","version":"1.0.0","schema_version":1,"background":true})";
}

// Lying face up; with `shaking`, every other read is a 3 g peak, so the
// shake's three edges come within a few polls whatever the scheduling.
class ShakyImu : public wisp::test::FakeImu {
public:
    ShakyImu() : FakeImu("imu.main") {}
    wisp::Result<wisp::hal::ImuSample> read() override {
        auto r = FakeImu::read();
        if (!r) return r;
        wisp::hal::ImuSample s = r.value();
        s.az = 1;
        if (shaking.load() && (n_++ % 2) == 0) s.az = 3;
        return wisp::Result<wisp::hal::ImuSample>::ok(s);
    }
    std::atomic<bool> shaking{false};

private:
    size_t n_ = 0;
};

class SensorFixture : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        SceneRuntimeFixture::SetUp();
        configure_deps = [this](wisp::scene::SceneRuntimeDeps& d) { d.sensors = sensors.get(); };
    }

    void TearDown() override {
        rt.reset();
        if (sensors) sensors->stop();
        SceneRuntimeFixture::TearDown();
        sensors.reset();
    }

    void make_sensors(std::shared_ptr<wisp::hal::Imu> imu) {
        sensors = std::make_unique<SensorService>(std::move(imu), occupancy);
        ASSERT_TRUE(sensors->start());
    }

    void install_bg(const std::string& id, const std::string& lua) {
        install(id, lua, {{"scene.json", bg_manifest(id)}});
    }

    bool wait_log(const std::string& id, const std::string& line, uint32_t ms = 3000) {
        return wait_until(
            [&] {
                auto log = scene_log(id);
                return std::find(log.begin(), log.end(), line) != log.end();
            },
            ms);
    }

    size_t count_log(const std::string& id, const std::string& line) {
        auto log = scene_log(id);
        return static_cast<size_t>(std::count(log.begin(), log.end(), line));
    }

    std::shared_ptr<ShakyImu> imu = std::make_shared<ShakyImu>();
    OccupancyRegistry occupancy;
    std::unique_ptr<SensorService> sensors;
};

using SensorBinding = SensorFixture;

TEST_F(SensorBinding, SubscribeDeliversSamples) {
    make_sensors(imu);
    start();
    install("demo", R"(
local total = 0
local done = false
local h = wisp.sensor.subscribe("accel", { rate = 50 }, function(samples)
  total = total + #samples
  local s = samples[#samples]
  if total >= 5 and not done then
    done = true
    print("ok", math.type(s.t), s.x == 0, s.y == 0, s.z > 9.8 and s.z < 9.81)
  end
end)
print(type(h))
return ui.screen {}
)");
    ASSERT_TRUE(rt->open("demo"));
    ASSERT_TRUE(wait_log("demo", "ok\tinteger\ttrue\ttrue\ttrue"));
    EXPECT_EQ(scene_log("demo")[0], "userdata");
    EXPECT_EQ(sensors->imu_rate(), 50u);
    auto occ = occupancy.list();
    ASSERT_EQ(occ.size(), 1u);
    EXPECT_EQ(occ[0].holder, "scene.demo");
}

TEST_F(SensorBinding, GestureFiresHandler) {
    make_sensors(imu);
    start();
    install("demo", R"(
local g = wisp.sensor.on("shake", function(name) print("gesture", name) end)
wisp.sensor.on("flip", function(name) print("gesture", name) end)
return ui.screen {}
)");
    ASSERT_TRUE(rt->open("demo"));
    ASSERT_TRUE(
        wait_until([&] { return sensors->imu_rate() == SensorService::kGestureRate; }, 2000));
    imu->shaking = true;
    ASSERT_TRUE(wait_log("demo", "gesture\tshake"));
    EXPECT_EQ(count_log("demo", "gesture\tflip"), 0u);
}

TEST_F(SensorBinding, BackgroundPausesForegroundResumes) {
    make_sensors(imu);
    start();
    install_bg("demo", R"(
wisp.sensor.subscribe("gyro", { rate = 25 }, function(samples) print("batch") end)
return ui.screen {}
)");
    ASSERT_TRUE(rt->open("demo"));
    ASSERT_TRUE(wait_until([&] { return count_log("demo", "batch") >= 2; }, 3000));
    ASSERT_TRUE(rt->close("demo"));
    ASSERT_EQ(rt->status("demo").state, SceneState::Background);
    // Paused: the IMU is off, and nothing more arrives (one batch may
    // already have been on its way).
    EXPECT_EQ(sensors->imu_rate(), 0u);
    EXPECT_FALSE(imu->powered());
    wisp::os::sleep_ms(50);
    const size_t paused = count_log("demo", "batch");
    wisp::os::sleep_ms(400);
    EXPECT_EQ(count_log("demo", "batch"), paused);
    // Still held while paused.
    EXPECT_EQ(occupancy.list().size(), 1u);

    ASSERT_TRUE(rt->open("demo"));
    EXPECT_EQ(sensors->imu_rate(), 25u);
    EXPECT_TRUE(wait_until([&] { return count_log("demo", "batch") > paused; }, 3000));
}

TEST_F(SensorBinding, NotInBackground) {
    make_sensors(imu);
    start();
    install_bg("demo", R"(
scene.on_state = function(s)
  if s ~= "background" then return end
  wisp.timer.after(50, function()
    print("sub", wisp.sensor.subscribe("accel", { rate = 10 }, function() end))
    print("on", wisp.sensor.on("shake", function() end))
  end)
end
return ui.screen {}
)");
    ASSERT_TRUE(rt->launch("demo", LaunchReason::Boot));
    ASSERT_TRUE(wait_log("demo", "on\tnil\tnot_in_background"));
    EXPECT_TRUE(wait_log("demo", "sub\tnil\tnot_in_background"));
    EXPECT_TRUE(occupancy.list().empty());
    EXPECT_EQ(sensors->imu_rate(), 0u);
}

TEST_F(SensorBinding, AvailableEmptyWithoutImu) {
    make_sensors(nullptr);
    start();
    install("demo", R"(
print("available", next(wisp.sensor.available()))
print("sub", wisp.sensor.subscribe("accel", { rate = 10 }, function() end))
print("on", wisp.sensor.on("flip", function() end))
return ui.screen {}
)");
    ASSERT_TRUE(rt->open("demo"));
    EXPECT_EQ(scene_log("demo"),
              (std::vector<std::string>{"available\tnil", "sub\tnil\tunavailable",
                                        "on\tnil\tunavailable"}));
}

TEST_F(SensorBinding, AvailableEmptyWithoutService) {
    start();
    install("demo", R"(
print("available", next(wisp.sensor.available()))
print("sub", wisp.sensor.subscribe("gyro", { rate = 100 }, function() end))
return ui.screen {}
)");
    ASSERT_TRUE(rt->open("demo"));
    EXPECT_EQ(scene_log("demo"),
              (std::vector<std::string>{"available\tnil", "sub\tnil\tunavailable"}));
}

TEST_F(SensorBinding, AvailableFromCaps) {
    make_sensors(imu);
    start();
    install("demo", R"(
local a = wisp.sensor.available()
print("available", a.accel, a.gyro)
return ui.screen {}
)");
    ASSERT_TRUE(rt->open("demo"));
    EXPECT_EQ(scene_log("demo"), (std::vector<std::string>{"available\ttrue\ttrue"}));
}

TEST_F(SensorBinding, ArgumentErrors) {
    make_sensors(imu);
    start();
    install("demo", R"(
local function try(f) local ok, err = pcall(f) print(ok, err) end
try(function() wisp.sensor.subscribe("mag", { rate = 10 }, function() end) end)
try(function() wisp.sensor.subscribe("accel", { rate = 30 }, function() end) end)
try(function() wisp.sensor.subscribe("accel", { rate = "fast" }, function() end) end)
try(function() wisp.sensor.subscribe("accel", { rate = 10 }) end)
try(function() wisp.sensor.on("wave", function() end) end)
local h = wisp.sensor.subscribe("accel", nil, function() end)
print("default", h ~= nil)
print("stop", h:stop(), h:stop())
return ui.screen {}
)");
    ASSERT_TRUE(rt->open("demo"));
    auto log = scene_log("demo");
    ASSERT_EQ(log.size(), 7u);
    EXPECT_NE(log[0].find("false\t"), std::string::npos);
    EXPECT_NE(log[0].find("kind must be"), std::string::npos);
    EXPECT_NE(log[1].find("rate must be 10, 25, 50 or 100"), std::string::npos);
    EXPECT_NE(log[2].find("rate must be"), std::string::npos);
    EXPECT_NE(log[3].find("function expected"), std::string::npos);
    EXPECT_NE(log[4].find("gesture must be"), std::string::npos);
    EXPECT_EQ(log[5], "default\ttrue");
    EXPECT_EQ(log[6], "stop\ttrue\tfalse");
    EXPECT_TRUE(occupancy.list().empty());
    EXPECT_EQ(sensors->imu_rate(), 0u);
}

TEST_F(SensorBinding, FunctionAsSecondArgumentUsesDefaultRate) {
    make_sensors(imu);
    start();
    install("demo", R"(
local h = wisp.sensor.subscribe("gyro", function() end)
print("handle", h ~= nil)
return ui.screen {}
)");
    ASSERT_TRUE(rt->open("demo"));
    EXPECT_EQ(scene_log("demo"), (std::vector<std::string>{"handle\ttrue"}));
    EXPECT_EQ(sensors->imu_rate(), 25u);
}

TEST_F(SensorBinding, ServiceErrorsKeepTheirCode) {
    make_sensors(imu);
    // Someone else holds the IMU exclusively: the service says Busy.
    ASSERT_TRUE(occupancy.acquire("imu.main", "system.test", wisp::core::OccupancyMode::Exclusive));
    start();
    install("demo", R"(
print("sub", wisp.sensor.subscribe("accel", { rate = 10 }, function() end))
print("on", wisp.sensor.on("shake", function() end))
return ui.screen {}
)");
    ASSERT_TRUE(rt->open("demo"));
    EXPECT_EQ(scene_log("demo"), (std::vector<std::string>{"sub\tnil\tbusy", "on\tnil\tbusy"}));
    EXPECT_EQ(sensors->imu_rate(), 0u);
}

TEST_F(SensorBinding, EvictionReleases) {
    configure_config = [](wisp::scene::SceneRuntimeConfig& c) { c.max_background = 1; };
    make_sensors(imu);
    start();
    install_bg("a", R"(
wisp.sensor.subscribe("accel", { rate = 50 }, function() end)
return ui.screen {}
)");
    install_bg("b", "return ui.screen {}");
    install_bg("c", "return ui.screen {}");
    ASSERT_TRUE(rt->open("a"));
    EXPECT_EQ(occupancy.list().size(), 1u);
    // a goes to the background (paused, still holding imu.main), then b
    // joins it there and a, the least recently in front, is evicted.
    ASSERT_TRUE(rt->open("b"));
    ASSERT_EQ(rt->status("a").state, SceneState::Background);
    EXPECT_EQ(occupancy.list().size(), 1u);
    ASSERT_TRUE(rt->open("c"));
    ASSERT_EQ(rt->status("a").state, SceneState::Stopped);
    EXPECT_TRUE(occupancy.list().empty());
    EXPECT_EQ(sensors->imu_rate(), 0u);
    EXPECT_FALSE(imu->powered());
}

TEST_F(SensorBinding, ExitReleases) {
    make_sensors(imu);
    start();
    install_bg("demo", R"(
wisp.sensor.subscribe("accel", { rate = 100 }, function() end)
wisp.sensor.on("flip", function() end)
return ui.screen {}
)");
    ASSERT_TRUE(rt->open("demo"));
    EXPECT_EQ(occupancy.list().size(), 1u);
    EXPECT_TRUE(imu->powered());
    ASSERT_TRUE(rt->exit("demo"));
    EXPECT_TRUE(occupancy.list().empty());
    EXPECT_EQ(sensors->imu_rate(), 0u);
    EXPECT_FALSE(imu->powered());

    // A script error releases them too.
    install("bad", R"(
wisp.sensor.subscribe("accel", { rate = 10 }, function() end)
wisp.timer.after(50, function() error("boom") end)
return ui.screen {}
)");
    ASSERT_TRUE(rt->open("bad"));
    ASSERT_TRUE(wait_until([&] { return rt->status("bad").state == SceneState::Error; }, 3000));
    EXPECT_TRUE(occupancy.list().empty());
    EXPECT_EQ(sensors->imu_rate(), 0u);
}

}  // namespace
