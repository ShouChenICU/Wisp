#include "self_check_sampler.hpp"

#include <mutex>
#include <utility>

namespace wisp::app {

namespace {

constexpr uint32_t kSampleTaskPeriodMs = 20;      // ~50Hz IMU sampling.
constexpr uint64_t kSlowSampleIntervalMs = 1000;  // power/clock.
// How often loop() still takes one power/clock "slow sample" while
// standby_ is true -- spec 6.7 asks for "约 30s", not a full stop.
constexpr uint32_t kStandbySlowSampleMs = 30000;

}  // namespace

SelfCheckSampler::SelfCheckSampler(std::shared_ptr<hal::Imu> imu, std::shared_ptr<hal::Power> power,
                                   std::shared_ptr<hal::Clock> clock)
    : imu_(std::move(imu)), power_(std::move(power)), clock_(std::move(clock)) {}

SelfCheckSampler::~SelfCheckSampler() { stop(); }

void SelfCheckSampler::start() {
    running_.store(true, std::memory_order_release);
    os::TaskConfig config;
    config.name = "wisp.sample";
    task_ = os::Task::spawn(config, [this] { loop(); });
}

void SelfCheckSampler::stop() {
    running_.store(false, std::memory_order_release);
    // loop() may be blocked on wake_ (in standby): wake it so it notices
    // running_ went false now rather than after its bounded wait.
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
    return snapshot_;
}

void SelfCheckSampler::loop() {
    uint64_t last_slow = 0;
    while (running_.load(std::memory_order_acquire)) {
        // Standby means "no periodic high-frequency polling" (spec 6.7):
        // IMU sampling stops entirely. Power/charging status and the
        // clock keep being read "约 30s". take() returning false means
        // kStandbySlowSampleMs elapsed with no wake signal -- take one
        // slow sample; true means something woke this task (stop(), or
        // standby ended) and there is nothing to sample right now.
        if (standby_.load(std::memory_order_acquire)) {
            if (!wake_.take(kStandbySlowSampleMs)) {
                sample_slow();
            }
            continue;
        }
        if (imu_) {
            auto r = imu_->read();
            // enter_standby() can power the IMU off while this read()
            // (which runs with no lock held) is still in flight. Publishing
            // the resulting failure would leave one bogus "IMU 读取失败"
            // frame visible right after waking -- the screen only shows
            // what is in the snapshot and cannot tell a stale failure from
            // a fresh one. Drop the result once standby_ has flipped; the
            // next iteration takes the standby branch instead.
            if (!standby_.load(std::memory_order_acquire)) {
                std::lock_guard<os::Mutex> lock(mutex_);
                snapshot_.imu_ok = static_cast<bool>(r);
                if (r) {
                    snapshot_.imu = r.value();
                }
            }
        }

        uint64_t now = os::now_ms();
        if (now - last_slow >= kSlowSampleIntervalMs) {
            last_slow = now;
            sample_slow();
        }

        os::sleep_ms(kSampleTaskPeriodMs);
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
