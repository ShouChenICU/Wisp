#include "self_check_sampler.hpp"

#include <mutex>
#include <utility>

namespace wisp::app {

namespace {

constexpr const char* kHolder = "system.selfcheck";
// The IMU rate the screen asks for (refinement 19 of
// docs/superpowers/plans/2026-11-05-scene-device.md).
constexpr uint32_t kImuRateHz = 50;
constexpr float kStandardGravity = 9.80665f;
constexpr uint32_t kSlowSampleIntervalMs = 1000;  // power/clock.
// How often loop() still takes one power/clock "slow sample" while
// standby_ is true -- spec 6.7 asks for "约 30s", not a full stop.
constexpr uint32_t kStandbySlowSampleMs = 30000;

}  // namespace

SelfCheckSampler::SelfCheckSampler(core::SensorService* sensors, std::shared_ptr<hal::Power> power,
                                   std::shared_ptr<hal::Clock> clock)
    : sensors_(sensors), power_(std::move(power)), clock_(std::move(clock)) {}

SelfCheckSampler::~SelfCheckSampler() { stop(); }

bool SelfCheckSampler::has_imu() const {
    return sensors_ != nullptr && sensors_->available(core::SensorKind::Accel);
}

void SelfCheckSampler::start() {
    {
        std::lock_guard<os::Mutex> lock(imu_mutex_);
        imu_wanted_ = true;
        if (!covered_) subscribe_imu();
    }
    running_.store(true, std::memory_order_release);
    os::TaskConfig config;
    config.name = "wisp.sample";
    task_ = os::Task::spawn(config, [this] { loop(); });
}

void SelfCheckSampler::set_covered(bool covered) {
    std::lock_guard<os::Mutex> lock(imu_mutex_);
    if (covered_ == covered) return;
    covered_ = covered;
    if (!imu_wanted_) return;
    if (covered) {
        unsubscribe_imu();
    } else {
        subscribe_imu();
    }
}

void SelfCheckSampler::subscribe_imu() {
    if (sensors_ == nullptr || accel_sub_ != 0 || gyro_sub_ != 0) return;
    // On wisp.imu: only the newest sample of each batch is shown.
    auto accel = sensors_->subscribe(core::SensorKind::Accel, kImuRateHz, kHolder,
                                     [this](const core::SensorBatch& b) {
                                         const core::SensorSample& s = b.back();
                                         std::lock_guard<os::Mutex> lock(mutex_);
                                         snapshot_.imu.ax = s.x / kStandardGravity;
                                         snapshot_.imu.ay = s.y / kStandardGravity;
                                         snapshot_.imu.az = s.z / kStandardGravity;
                                         snapshot_.imu.ts_ms = s.t_ms;
                                     });
    if (accel) accel_sub_ = accel.value();
    auto gyro = sensors_->subscribe(core::SensorKind::Gyro, kImuRateHz, kHolder,
                                    [this](const core::SensorBatch& b) {
                                        const core::SensorSample& s = b.back();
                                        std::lock_guard<os::Mutex> lock(mutex_);
                                        snapshot_.imu.gx = s.x;
                                        snapshot_.imu.gy = s.y;
                                        snapshot_.imu.gz = s.z;
                                    });
    if (gyro) gyro_sub_ = gyro.value();
    imu_live_.store(accel_sub_ != 0 || gyro_sub_ != 0);
}

void SelfCheckSampler::unsubscribe_imu() {
    // After this no IMU callback runs (SensorService::unsubscribe()).
    if (sensors_ != nullptr) {
        if (accel_sub_ != 0) sensors_->unsubscribe(accel_sub_);
        if (gyro_sub_ != 0) sensors_->unsubscribe(gyro_sub_);
    }
    accel_sub_ = gyro_sub_ = 0;
    imu_live_.store(false);
}

void SelfCheckSampler::stop() {
    {
        std::lock_guard<os::Mutex> lock(imu_mutex_);
        imu_wanted_ = false;
        unsubscribe_imu();
    }
    running_.store(false, std::memory_order_release);
    // loop() may be blocked on wake_: wake it so it notices running_ went
    // false now rather than after its bounded wait.
    wake_.give();
    if (task_) {
        task_->join();
        task_.reset();
    }
}

void SelfCheckSampler::set_standby(bool standby) {
    bool was_standby = standby_.exchange(standby, std::memory_order_acq_rel);
    if (was_standby && !standby) {
        wake_.give();
    }
}

SampleSnapshot SelfCheckSampler::snapshot() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    SampleSnapshot snap = snapshot_;
    // The service's last read, live: the screen shows a failing IMU as
    // soon as it fails, and stops once a read works again.
    if (imu_live_.load()) snap.imu_ok = sensors_->read_ok();
    return snap;
}

void SelfCheckSampler::loop() {
    uint64_t last_slow = 0;
    bool first = true;
    while (running_.load(std::memory_order_acquire)) {
        // Standby: power/charging status and the clock keep being read
        // "约 30s". take() returning false means kStandbySlowSampleMs
        // elapsed with no wake signal -- take one slow sample; true means
        // something woke this task (stop(), or standby ended).
        if (standby_.load(std::memory_order_acquire)) {
            if (!wake_.take(kStandbySlowSampleMs)) {
                sample_slow();
            }
            continue;
        }
        const uint64_t now = os::now_ms();
        if (first || now - last_slow >= kSlowSampleIntervalMs) {
            first = false;
            last_slow = now;
            sample_slow();
        }
        wake_.take(kSlowSampleIntervalMs);
    }
}

void SelfCheckSampler::sample_slow() {
    if (power_) {
        auto r = power_->status();
        std::lock_guard<os::Mutex> lock(mutex_);
        snapshot_.power_ok = static_cast<bool>(r);
        if (r) {
            snapshot_.power = r.value();
        }
    }
    if (clock_) {
        auto r = clock_->get_unix();
        std::lock_guard<os::Mutex> lock(mutex_);
        snapshot_.rtc_ok = static_cast<bool>(r);
        if (r) {
            snapshot_.rtc_unix = r.value();
        }
    }
}

}  // namespace wisp::app
