#pragma once

// SensorService (scene-device spec 6.1): one IMU shared by every
// subscriber. Scenes (wisp.sensor) and the self-check screen read it here,
// never through the driver (AGENTS.md section 3, service layer).
//
// Power and rate: the IMU runs while at least one subscription is active
// (neither paused nor held off by standby): the first one powers it up
// (set_power(Active)), every change sets it to the highest rate the
// active subscriptions ask for (set_rate()), and the last one going
// powers it down (set_power(Standby)). imu_rate() is that rate, 0 while
// the IMU is off.
//
// Sampling: the "wisp.imu" task polls read() at imu_rate() (poll()).
// Accelerations come in g and leave in m/s² (x 9.80665); angular rates
// stay in degrees per second. Each subscription keeps, of the IMU's
// samples, the one nearest to each of its own periods, and every batch_ms
// its callback gets the samples kept since the last batch. A sample's t
// is os::now_ms() (the poll time), not the driver's own stamp. Gesture
// subscriptions take the accelerometer at 50 Hz and call back once per
// gesture (gesture.hpp).
//
// Occupancy (spec 6.3): every holder with at least one subscription
// holds "imu.main" shared in the OccupancyRegistry; its last unsubscribe
// releases it. Paused subscriptions still hold it.
//
// Standby: set_standby(true) (PowerManager's standby or deep_standby, via
// power.state) pauses everything and powers the IMU down; false resumes
// what is not paused on its own.
//
// Threads: every method may be called from any thread except the
// callbacks themselves, which run on "wisp.imu" with the service's lock
// held. That makes unsubscribe() a barrier (no callback of that
// subscription runs once it returns), and it means a callback must be
// quick and must not call back into the service. The task declares
// flash_access = false (task_config()), so the callbacks must not touch
// flash either: no Settings, no files, no logging that writes them. They
// only copy the samples out and hand them to their own thread.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "wisp/base/result.hpp"
#include "wisp/core/gesture.hpp"
#include "wisp/core/occupancy.hpp"
#include "wisp/hal/imu.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

enum class SensorKind : uint8_t { Accel, Gyro };
// "accel", "gyro"; nullopt for anything else.
std::optional<SensorKind> parse_sensor_kind(std::string_view s);

enum class Gesture : uint8_t { Shake, Flip };
// "shake", "flip"; nullopt for anything else.
std::optional<Gesture> parse_gesture(std::string_view s);
// "shake", "flip".
const char* to_string(Gesture g);

struct SensorSample {
    float x = 0, y = 0, z = 0;
    uint64_t t_ms = 0;
};
using SensorBatch = os::LargeVector<SensorSample>;

struct SensorServiceConfig {
    uint32_t batch_ms = 100;
    size_t stack_bytes = 4096;
    int priority = 3;
};

class SensorService {
public:
    static constexpr std::array<uint32_t, 4> kRates = {10, 25, 50, 100};
    // What gesture subscriptions sample the accelerometer at.
    static constexpr uint32_t kGestureRate = 50;
    // The device id taken in the OccupancyRegistry.
    static constexpr const char* kDevice = "imu.main";

    // `imu` may be null: nothing is available then.
    SensorService(std::shared_ptr<hal::Imu> imu, OccupancyRegistry& occupancy,
                  SensorServiceConfig c = {});
    ~SensorService();  // stop()

    SensorService(const SensorService&) = delete;
    SensorService& operator=(const SensorService&) = delete;

    // "wisp.imu", flash_access = false (see the file header), so the port
    // may put its stack in PSRAM.
    static os::TaskConfig task_config(const SensorServiceConfig& c);

    // Spawns the task; without an IMU there is nothing to poll and no
    // task (Ok). Busy if started before, NoMemory if the task cannot be
    // made.
    Result<void> start();
    // Stops the task. Idempotent. Subscriptions stay (no more callbacks).
    void stop();

    // From the IMU's caps ("accel", "gyro"); false without an IMU.
    bool available(SensorKind kind) const;

    // Unsupported ("unavailable") without the sensor; InvalidArgument for
    // a rate not in kRates. Returns the subscription's handle (> 0).
    // `cb` runs on wisp.imu with up to batch_ms of samples; it must be
    // quick (file header).
    Result<uint32_t> subscribe(SensorKind kind, uint32_t rate_hz, std::string holder,
                               std::function<void(const SensorBatch&)> cb);
    // Unsupported ("unavailable") without an accelerometer. `cb` runs on
    // wisp.imu once per gesture.
    Result<uint32_t> on_gesture(Gesture g, std::string holder, std::function<void()> cb);
    // Ends a subscription; its callback does not run once this returns.
    // Unknown handles are ignored. Not from inside a callback.
    void unsubscribe(uint32_t handle);
    // A paused subscription gets nothing and does not keep the IMU on;
    // samples it had not been given yet are dropped.
    void set_paused(uint32_t handle, bool paused);
    // power.state: standby and deep_standby pause everything.
    void set_standby(bool standby);
    // 0 while the IMU is off.
    uint32_t imu_rate() const;

    // False after a read() failed, until one succeeds (or standby ends):
    // the self-check screen shows it. Any thread.
    bool read_ok() const { return read_ok_.load(); }

    // One IMU period at `now_ms`: delivers the batches due (batch_ms after
    // the last delivery), then reads one sample and hands it to the
    // subscriptions due one. Does nothing while the IMU is off. The task
    // calls it; tests call it by hand, without start(), to run on a clock
    // of their own.
    void poll(uint64_t now_ms);

private:
    struct Sub;

    Result<uint32_t> add(os::LargeUnique<Sub> s);
    // Recomputes the rate from the active subscriptions and powers or
    // sets the IMU to match. With mutex_ held.
    void apply_locked();
    Sub* find_locked(uint32_t handle);
    // Drops what `s` had collected and starts its sampling afresh.
    void restart_locked(Sub& s);
    void deliver_locked();

    std::shared_ptr<hal::Imu> imu_;
    OccupancyRegistry& occupancy_;
    SensorServiceConfig config_;
    bool has_accel_ = false;
    bool has_gyro_ = false;

    mutable os::Mutex mutex_;
    os::LargeVector<os::LargeUnique<Sub>> subs_;
    uint32_t next_handle_ = 1;
    bool standby_ = false;
    std::atomic<uint32_t> rate_{0};
    // The last delivery; nullopt until the first poll after the IMU came
    // on.
    std::optional<uint64_t> last_batch_ms_;

    std::atomic<bool> read_ok_{true};
    std::unique_ptr<os::Task> task_;
    std::atomic<bool> quit_{false};
    // Given whenever the rate changes and by stop(): the task waits on it.
    os::Semaphore wake_{1, 0};
};

}  // namespace wisp::core
