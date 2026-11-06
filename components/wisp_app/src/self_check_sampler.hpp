#pragma once

// The self-check screen's readings, in a mutex-protected snapshot the
// screen's 100 ms refresh copies out (snapshot()):
//   - the IMU through core::SensorService (accelerometer and gyroscope at
//     50 Hz, as holder "system.selfcheck"; refinement 19 of
//     docs/superpowers/plans/2026-11-05-scene-device.md), never the driver
//     itself. The service pauses it in standby. The
//     accelerations go back to g, which the screen shows.
//   - the battery and the clock, read by the "wisp.sample" task once a
//     second, about every 30 s in standby (spec 6.7).
//
// Threading: start(), stop() and the destructor from the screen's owner;
// set_standby() from the bus dispatcher; snapshot() from the UI thread;
// the IMU callbacks on the service's "wisp.imu" task, which must not touch
// flash: they only copy the newest sample into the snapshot.

#include <atomic>
#include <cstdint>
#include <memory>

#include "wisp/core/sensor_service.hpp"
#include "wisp/hal/clock.hpp"
#include "wisp/hal/imu.hpp"
#include "wisp/hal/power.hpp"
#include "wisp/os/os.hpp"

namespace wisp::app {

// The sampler's latest readings. An `_ok` flag is false while the last
// read of that device failed (its value then keeps the last good one).
// `imu` is in g and degrees per second.
struct SampleSnapshot {
    hal::ImuSample imu{};
    bool imu_ok = true;
    hal::PowerStatus power{};
    bool power_ok = true;
    int64_t rtc_unix = 0;
    bool rtc_ok = true;
};

class SelfCheckSampler {
public:
    // Any of them may be null; it is then never read. `sensors` must
    // outlive the sampler.
    SelfCheckSampler(core::SensorService* sensors, std::shared_ptr<hal::Power> power,
                     std::shared_ptr<hal::Clock> clock);
    // Stops and joins the task.
    ~SelfCheckSampler();

    SelfCheckSampler(const SelfCheckSampler&) = delete;
    SelfCheckSampler& operator=(const SelfCheckSampler&) = delete;

    // Subscribes to the IMU and starts the task.
    void start();
    // Unsubscribes, stops the task and waits for it; a no-op if it is not
    // running.
    void stop();

    // Follows PowerManager's standby (the "power.state" event) for the
    // battery/clock reads; the service pauses the IMU on its own.
    void set_standby(bool standby);

    // There is an accelerometer to show.
    bool has_imu() const;

    SampleSnapshot snapshot() const;

private:
    void loop();
    // The battery/clock read: once a second while active, about every
    // 30 s in standby.
    void sample_slow();
    void subscribe_imu();

    core::SensorService* sensors_;
    uint32_t accel_sub_ = 0;
    uint32_t gyro_sub_ = 0;
    // Subscribed; snapshot() reads it on the UI thread while the owner
    // may be stopping.
    std::atomic<bool> imu_live_{false};
    std::shared_ptr<hal::Power> power_;
    std::shared_ptr<hal::Clock> clock_;

    mutable os::Mutex mutex_;
    SampleSnapshot snapshot_;
    std::atomic<bool> running_{false};
    std::unique_ptr<os::Task> task_;

    // Mirrors PowerManager::standby() from the bus rather than reading it
    // live: loop() runs on its own thread, with no UI-thread affinity to
    // synchronize against.
    std::atomic<bool> standby_{false};
    // Given on wake-up and by stop(), so a loop() blocked in its wait
    // notices at once.
    os::Semaphore wake_{1, 0};
};

}  // namespace wisp::app
