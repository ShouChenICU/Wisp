#pragma once

// The self-check screen's "wisp.sample" task: reads the IMU at about 50 Hz
// and the battery and the clock once a second into a mutex-protected
// snapshot, which the screen's 100 ms refresh copies out (snapshot()).
// Standby stops the IMU polling and slows the battery/clock reads to about
// every 30 s (spec 6.7).
//
// Threading: start(), stop() and the destructor from the screen's owner;
// set_standby() from the bus dispatcher; snapshot() from the UI thread.

#include <atomic>
#include <cstdint>
#include <memory>

#include "wisp/hal/clock.hpp"
#include "wisp/hal/imu.hpp"
#include "wisp/hal/power.hpp"
#include "wisp/os/os.hpp"

namespace wisp::app {

// The sampler's latest readings. An `_ok` flag is false while the last
// read of that device failed (its value then keeps the last good one).
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
    // Any device may be null; it is then never read.
    SelfCheckSampler(std::shared_ptr<hal::Imu> imu, std::shared_ptr<hal::Power> power,
                     std::shared_ptr<hal::Clock> clock);
    // Stops and joins the task.
    ~SelfCheckSampler();

    SelfCheckSampler(const SelfCheckSampler&) = delete;
    SelfCheckSampler& operator=(const SelfCheckSampler&) = delete;

    void start();
    // Stops the task and waits for it; a no-op if it is not running.
    void stop();

    // Follows PowerManager's standby (the "power.state" event); waking
    // resumes the IMU polling at once rather than after the next slow
    // read.
    void set_standby(bool standby);

    SampleSnapshot snapshot() const;

private:
    void loop();
    // The battery/clock read: once a second while active, about every
    // 30 s in standby.
    void sample_slow();

    std::shared_ptr<hal::Imu> imu_;
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
    // Given on wake-up and by stop(), so a loop() blocked in its standby
    // wait notices at once.
    os::Semaphore wake_{1, 0};
};

}  // namespace wisp::app
