#include "wisp/core/sensor_service.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/wait.hpp"
#include "wisp/os/os.hpp"

using wisp::ErrorCode;
using wisp::core::Gesture;
using wisp::core::OccupancyMode;
using wisp::core::OccupancyRegistry;
using wisp::core::SensorBatch;
using wisp::core::SensorKind;
using wisp::core::SensorSample;
using wisp::core::SensorService;
using wisp::test::FakeImu;

namespace {

constexpr float kG = 9.80665f;

// What one subscription received: the batches, and their samples in order.
struct Seen {
    std::mutex mu;
    std::vector<size_t> batches;
    std::vector<SensorSample> samples;

    auto cb() {
        return [this](const SensorBatch& b) {
            std::lock_guard<std::mutex> lock(mu);
            batches.push_back(b.size());
            samples.insert(samples.end(), b.begin(), b.end());
        };
    }
    size_t count() {
        std::lock_guard<std::mutex> lock(mu);
        return samples.size();
    }
};

// A service over a FakeImu, polled by hand (no task) on a made-up clock.
struct Fixture {
    Fixture() : imu(std::make_shared<FakeImu>("imu.main")), svc(imu, occupancy) {
        wisp::hal::ImuSample s;
        s.az = 1;
        imu->set_next_sample(s);
    }

    // Polls at the IMU's period from `now` until `until` (exclusive).
    void run_until(uint64_t until) {
        while (now < until) {
            svc.poll(now);
            const uint32_t rate = svc.imu_rate();
            now += rate == 0 ? 10 : 1000 / rate;
        }
    }

    std::shared_ptr<FakeImu> imu;
    OccupancyRegistry occupancy;
    SensorService svc;
    uint64_t now = 0;
};

}  // namespace

TEST(SensorService, RefcountPowersImu) {
    Fixture f;
    Seen a, b;
    auto h1 = f.svc.subscribe(SensorKind::Accel, 25, "scene.a", a.cb());
    ASSERT_TRUE(h1);
    EXPECT_TRUE(f.imu->powered());
    EXPECT_EQ(f.svc.imu_rate(), 25u);
    EXPECT_EQ(f.imu->rate_hz(), 25u);
    auto h2 = f.svc.subscribe(SensorKind::Gyro, 10, "scene.b", b.cb());
    ASSERT_TRUE(h2);
    f.svc.unsubscribe(h1.value());
    EXPECT_TRUE(f.imu->powered());
    EXPECT_EQ(f.svc.imu_rate(), 10u);
    f.svc.unsubscribe(h2.value());
    EXPECT_FALSE(f.imu->powered());
    EXPECT_EQ(f.svc.imu_rate(), 0u);
    // Unknown handles are ignored.
    f.svc.unsubscribe(h2.value());
    f.svc.unsubscribe(12345);
}

TEST(SensorService, RunsAtHighestRateAndDownsamples) {
    Fixture f;
    Seen slow, fast;
    ASSERT_TRUE(f.svc.subscribe(SensorKind::Accel, 10, "scene.a", slow.cb()));
    ASSERT_TRUE(f.svc.subscribe(SensorKind::Accel, 50, "scene.b", fast.cb()));
    EXPECT_EQ(f.svc.imu_rate(), 50u);
    EXPECT_EQ(f.imu->rate_hz(), 50u);
    f.run_until(1001);
    EXPECT_NEAR(static_cast<double>(slow.count()), 10.0, 1.0);
    EXPECT_NEAR(static_cast<double>(fast.count()), 50.0, 1.0);
    // The slow one's samples are about 100 ms apart.
    std::lock_guard<std::mutex> lock(slow.mu);
    for (size_t i = 1; i < slow.samples.size(); ++i)
        EXPECT_NEAR(static_cast<double>(slow.samples[i].t_ms - slow.samples[i - 1].t_ms), 100.0,
                    20.0);
}

TEST(SensorService, BatchesEvery100ms) {
    Fixture f;
    Seen s;
    ASSERT_TRUE(f.svc.subscribe(SensorKind::Accel, 50, "scene.a", s.cb()));
    f.run_until(1001);
    std::lock_guard<std::mutex> lock(s.mu);
    ASSERT_GE(s.batches.size(), 9u);
    ASSERT_LE(s.batches.size(), 11u);
    for (size_t n : s.batches) {
        EXPECT_GE(n, 4u);
        EXPECT_LE(n, 6u);
    }
}

TEST(SensorService, ConvertsUnits) {
    Fixture f;
    wisp::hal::ImuSample in;
    in.ax = 0.5f;
    in.az = 1;
    in.gx = 12.5f;
    in.gz = -3;
    f.imu->set_next_sample(in);
    Seen accel, gyro;
    ASSERT_TRUE(f.svc.subscribe(SensorKind::Accel, 100, "scene.a", accel.cb()));
    ASSERT_TRUE(f.svc.subscribe(SensorKind::Gyro, 100, "scene.a", gyro.cb()));
    f.now = 5000;
    f.run_until(5201);
    ASSERT_GT(accel.count(), 0u);
    ASSERT_GT(gyro.count(), 0u);
    std::lock_guard<std::mutex> l1(accel.mu);
    std::lock_guard<std::mutex> l2(gyro.mu);
    EXPECT_EQ(accel.samples[0].z, 9.80665f);
    EXPECT_FLOAT_EQ(accel.samples[0].x, 0.5f * kG);
    EXPECT_EQ(accel.samples[0].y, 0.0f);
    EXPECT_EQ(accel.samples[0].t_ms, 5000u);
    EXPECT_EQ(gyro.samples[0].x, 12.5f);
    EXPECT_EQ(gyro.samples[0].z, -3.0f);
}

TEST(SensorService, StandbyPausesAll) {
    Fixture f;
    Seen s;
    ASSERT_TRUE(f.svc.subscribe(SensorKind::Accel, 50, "scene.a", s.cb()));
    f.run_until(500);
    f.svc.set_standby(true);
    EXPECT_FALSE(f.imu->powered());
    EXPECT_EQ(f.svc.imu_rate(), 0u);
    const size_t before = s.count();
    const size_t reads = f.imu->read_count();
    f.run_until(1500);
    EXPECT_EQ(s.count(), before);
    EXPECT_EQ(f.imu->read_count(), reads);
    f.svc.set_standby(false);
    EXPECT_TRUE(f.imu->powered());
    EXPECT_EQ(f.svc.imu_rate(), 50u);
    f.run_until(2001);
    EXPECT_GT(s.count(), before);
    // Standby with no subscription leaves the IMU off when it ends.
    OccupancyRegistry occ;
    auto imu = std::make_shared<FakeImu>("imu.main");
    SensorService idle(imu, occ);
    idle.set_standby(true);
    idle.set_standby(false);
    EXPECT_EQ(idle.imu_rate(), 0u);
    // PowerManager powers every IMU up on waking; with nobody to read it,
    // the service puts it back down.
    EXPECT_FALSE(imu->powered());
}

TEST(SensorService, PausedHandleGetsNothing) {
    Fixture f;
    Seen paused, live;
    auto h = f.svc.subscribe(SensorKind::Accel, 50, "scene.a", paused.cb());
    ASSERT_TRUE(h);
    ASSERT_TRUE(f.svc.subscribe(SensorKind::Accel, 10, "scene.b", live.cb()));
    f.run_until(150);
    f.svc.set_paused(h.value(), true);
    // Only the 10 Hz one keeps the IMU going.
    EXPECT_EQ(f.svc.imu_rate(), 10u);
    const size_t before = paused.count();
    f.run_until(1000);
    EXPECT_EQ(paused.count(), before);
    EXPECT_GT(live.count(), 5u);
    f.svc.set_paused(h.value(), false);
    EXPECT_EQ(f.svc.imu_rate(), 50u);
    f.run_until(1500);
    EXPECT_GT(paused.count(), before);
    // All paused: the IMU goes off.
    OccupancyRegistry occ;
    auto imu = std::make_shared<FakeImu>("imu.main");
    SensorService one(imu, occ);
    Seen s;
    auto only = one.subscribe(SensorKind::Gyro, 25, "scene.a", s.cb());
    ASSERT_TRUE(only);
    one.set_paused(only.value(), true);
    EXPECT_FALSE(imu->powered());
    EXPECT_EQ(one.imu_rate(), 0u);
    // Paused, it still holds the device.
    EXPECT_EQ(occ.list().size(), 1u);
}

TEST(SensorService, NoImuIsUnavailable) {
    OccupancyRegistry occ;
    SensorService svc(nullptr, occ);
    EXPECT_FALSE(svc.available(SensorKind::Accel));
    EXPECT_FALSE(svc.available(SensorKind::Gyro));
    ASSERT_TRUE(svc.start());
    Seen s;
    auto r = svc.subscribe(SensorKind::Accel, 25, "scene.a", s.cb());
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
    EXPECT_EQ(r.error().message, "unavailable");
    auto g = svc.on_gesture(Gesture::Shake, "scene.a", [] {});
    ASSERT_FALSE(g);
    EXPECT_EQ(g.error().code, ErrorCode::Unsupported);
    EXPECT_TRUE(occ.list().empty());
    svc.stop();

    // A rate outside the list.
    Fixture f;
    EXPECT_TRUE(f.svc.available(SensorKind::Accel));
    EXPECT_TRUE(f.svc.available(SensorKind::Gyro));
    auto bad = f.svc.subscribe(SensorKind::Accel, 30, "scene.a", s.cb());
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, ErrorCode::InvalidArgument);
    EXPECT_FALSE(f.svc.subscribe(SensorKind::Accel, 0, "scene.a", s.cb()));
    EXPECT_TRUE(f.occupancy.list().empty());
}

TEST(SensorService, RegistersOccupancyPerHolder) {
    Fixture f;
    Seen s;
    auto a1 = f.svc.subscribe(SensorKind::Accel, 25, "scene.a", s.cb());
    auto a2 = f.svc.on_gesture(Gesture::Flip, "scene.a", [] {});
    auto b = f.svc.subscribe(SensorKind::Gyro, 25, "system.selfcheck", s.cb());
    ASSERT_TRUE(a1 && a2 && b);
    auto list = f.occupancy.list();
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].device, "imu.main");
    EXPECT_EQ(list[0].holder, "scene.a");
    EXPECT_EQ(list[0].mode, OccupancyMode::Shared);
    EXPECT_EQ(list[1].holder, "system.selfcheck");
    f.svc.unsubscribe(b.value());
    list = f.occupancy.list();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].holder, "scene.a");
    // The holder's last subscription releases it.
    f.svc.unsubscribe(a1.value());
    EXPECT_EQ(f.occupancy.list().size(), 1u);
    f.svc.unsubscribe(a2.value());
    EXPECT_TRUE(f.occupancy.list().empty());
}

TEST(SensorService, TaskConfigIsPsramStack) {
    wisp::core::SensorServiceConfig c;
    auto t = SensorService::task_config(c);
    EXPECT_STREQ(t.name, "wisp.imu");
    EXPECT_FALSE(t.flash_access);
    EXPECT_EQ(t.stack_bytes, 4096u);
    EXPECT_EQ(t.priority, 3);
}

TEST(SensorService, GesturesFireFromSamples) {
    Fixture f;
    std::atomic<int> shakes{0}, flips{0};
    auto s = f.svc.on_gesture(Gesture::Shake, "scene.a", [&] { ++shakes; });
    auto p = f.svc.on_gesture(Gesture::Flip, "scene.a", [&] { ++flips; });
    ASSERT_TRUE(s && p);
    EXPECT_EQ(f.svc.imu_rate(), SensorService::kGestureRate);
    f.run_until(200);
    wisp::hal::ImuSample up, peak, down;
    up.az = 1;
    peak.az = 3;
    down.az = -1;
    for (int i = 0; i < 3; ++i) {
        f.imu->set_next_sample(peak);
        f.run_until(f.now + 40);
        f.imu->set_next_sample(up);
        f.run_until(f.now + 60);
    }
    EXPECT_EQ(shakes.load(), 1);
    f.imu->set_next_sample(down);
    f.run_until(f.now + 400);
    EXPECT_EQ(flips.load(), 1);
}

TEST(SensorService, TaskDeliversInRealTime) {
    auto imu = std::make_shared<FakeImu>("imu.main");
    OccupancyRegistry occ;
    SensorService svc(imu, occ);
    ASSERT_TRUE(svc.start());
    Seen s;
    auto h = svc.subscribe(SensorKind::Accel, 50, "scene.a", s.cb());
    ASSERT_TRUE(h);
    EXPECT_TRUE(wisp::test::wait_until([&] { return s.count() >= 5; }, 3000));
    svc.unsubscribe(h.value());
    const size_t after = s.count();
    wisp::os::sleep_ms(150);
    EXPECT_EQ(s.count(), after);
    EXPECT_FALSE(imu->powered());
    svc.stop();
    svc.stop();
}
