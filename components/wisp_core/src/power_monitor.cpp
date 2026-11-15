#include "wisp/core/power_monitor.hpp"

#include <mutex>
#include <type_traits>
#include <utility>

#include "wisp/base/error.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/topics.hpp"

namespace wisp::core {

static_assert(std::is_trivially_copyable_v<PowerSupply>,
              "the cell copies the supply under its lock without allocating");

namespace {

constexpr const char* kTag = "power_monitor";
constexpr uint32_t kActiveSampleMs = 1000;
constexpr uint32_t kStandbySampleMs = 30000;
constexpr int64_t kDefaultLowBattPct = 15;
constexpr int64_t kLowBattHysteresis = 5;
// How many sample() calls the cached "low_batt_pct" threshold is reused
// for before it's read from Settings again -- the real backend hits NVS
// (or the sim's JSON file) plus a parse on every get_int() call, which
// doing that on every 1 Hz sample on the shared esp_timer task was more
// I/O than this value's staleness tolerance needs -- see the header's
// constructor doc comment.
constexpr uint32_t kThresholdRefreshSamples = 20;

uint32_t interval_for(bool standby) { return standby ? kStandbySampleMs : kActiveSampleMs; }

// The supply cell's mirror: the two events sample() published before the
// cell, in the same order, under the same conditions (the file header of
// power_monitor.hpp). A change of the percent alone publishes nothing.
void supply_mirror(EventBus& bus, const PowerSupply& before, const PowerSupply& after) {
    if (after.charging != before.charging || after.usb_present != before.usb_present) {
        Event e;
        e.topic = topics::kPowerChargingChanged;
        e.source = kPowerMonitorSource;
        e.payload = Properties{
            {"charging", Value(after.charging)},
            {"usb_present", Value(after.usb_present)},
            {"battery_percent", Value(static_cast<int64_t>(after.battery_percent))},
        };
        bus.publish(std::move(e));
    }
    if (!before.low && after.low) {
        Event e;
        e.topic = topics::kPowerBatteryLow;
        e.source = kPowerMonitorSource;
        e.payload = Properties{
            {"battery_percent", Value(static_cast<int64_t>(after.battery_percent))},
        };
        bus.publish(std::move(e));
    }
}

}  // namespace

PowerMonitor::PowerMonitor(EventBus& bus, std::shared_ptr<hal::Power> power, Settings settings)
    : bus_(bus),
      power_(std::move(power)),
      settings_(std::move(settings)),
      supply_(bus_, PowerSupply{}, &supply_mirror) {}

PowerMonitor::~PowerMonitor() { stop(); }

void PowerMonitor::start() {
    std::lock_guard<os::Mutex> lock(timer_mutex_);
    if (power_ == nullptr || timer_ != nullptr) {
        return;
    }
    {
        // Force the next sample() to re-read "low_batt_pct" -- see the
        // header's constructor doc comment on threshold caching.
        // sample_mutex_ is never held by anything that also needs
        // timer_mutex_ (the header's thread-safety note), so taking it
        // here, nested under timer_mutex_, is safe.
        std::lock_guard<os::Mutex> sample_lock(sample_mutex_);
        samples_until_threshold_refresh_ = 0;
    }
    timer_ = std::make_unique<os::Timer>("wisp.pwrmon", interval_for(standby_), /*periodic=*/true,
                                         [this] { sample(); });
    timer_->start();
}

void PowerMonitor::stop() {
    std::lock_guard<os::Mutex> lock(timer_mutex_);
    if (timer_ == nullptr) {
        return;
    }
    timer_->stop();
    timer_.reset();
}

void PowerMonitor::set_standby(bool standby) {
    std::lock_guard<os::Mutex> lock(timer_mutex_);
    if (standby_ == standby) {
        return;
    }
    standby_ = standby;
    if (power_ == nullptr || timer_ == nullptr) {
        return;  // Nothing running yet -- the next start() uses standby_ already.
    }
    // Swap the
    // Timer for a new one at the new period. `timer_mutex_` is never held
    // by sample() (see the header's thread-safety note), so this never
    // deadlocks against the Timer's own callback the way holding
    // sample_mutex_ here would -- see os::Timer::stop()'s doc comment on
    // why that matters even for a Timer other than this one, on ports
    // that dispatch every Timer from one shared task.
    bool was_active = timer_->active();
    if (was_active) {
        timer_->stop();
    }
    timer_ = std::make_unique<os::Timer>("wisp.pwrmon", interval_for(standby_), /*periodic=*/true,
                                         [this] { sample(); });
    if (was_active) {
        timer_->start();
    }
}

uint32_t PowerMonitor::sample_interval_ms() const {
    std::lock_guard<os::Mutex> lock(timer_mutex_);
    return interval_for(standby_);
}

std::optional<hal::PowerStatus> PowerMonitor::last_status() const {
    std::lock_guard<os::Mutex> lock(sample_mutex_);
    return last_status_;
}

void PowerMonitor::sample() {
    if (power_ == nullptr) {
        return;
    }
    // Whole calls in turn, from the read through the cell's set(): two
    // concurrent calls (the timer and a direct one) would otherwise let
    // the one that read the older status set the cell last. Not
    // sample_mutex_, so last_status() never waits for the chip's read.
    std::lock_guard<os::Mutex> call(call_mutex_);
    auto result = power_->status();
    if (!result) {
        // Logged only on the ok->failed transition, not once per failed
        // sample -- a real failure (I2C bus fault, device unplugged)
        // tends to persist for many consecutive 1 Hz samples, and a
        // WLOG_W every single one would flood the log without adding
        // information. have_sample_ and the supply cell are deliberately
        // left untouched here: a failed read must not be treated as "no
        // change" against the next good sample, nor silently
        // re-arm/disarm the low-battery check.
        if (status_ok_.exchange(false, std::memory_order_relaxed)) {
            WLOG_W(kTag, "status() failed: %s", result.error().message.c_str());
        }
        std::lock_guard<os::Mutex> lock(sample_mutex_);
        last_status_.reset();
        return;
    }
    if (!status_ok_.exchange(true, std::memory_order_relaxed)) {
        WLOG_I(kTag, "status() recovered");
    }
    hal::PowerStatus status = result.value();

    std::lock_guard<os::Mutex> lock(sample_mutex_);
    last_status_ = status;

    if (samples_until_threshold_refresh_ == 0) {
        cached_low_batt_pct_ = settings_.get_int("low_batt_pct", kDefaultLowBattPct);
        samples_until_threshold_refresh_ = kThresholdRefreshSamples;
    }
    --samples_until_threshold_refresh_;

    // Read under sample_mutex_, which every write of the cell holds too, so
    // nothing moves it between this read and the set() below.
    const PowerSupply prev = supply_.get();
    if (have_sample_ &&
        (status.charging != prev.charging || status.usb_present != prev.usb_present)) {
        WLOG_I(kTag, "power.charging.changed: charging=%d usb_present=%d battery_percent=%d",
               status.charging, status.usb_present, status.battery_percent);
    }

    // An unknown percent (-1) neither triggers nor re-arms: `low` stays.
    bool low = prev.low;
    if (status.battery_percent != -1) {
        int64_t threshold = cached_low_batt_pct_;
        if (!prev.low && status.battery_percent <= threshold) {
            low = true;
            WLOG_I(kTag, "power.battery.low: battery_percent=%d (threshold=%lld)",
                   status.battery_percent, static_cast<long long>(threshold));
        } else if (prev.low && status.battery_percent > threshold + kLowBattHysteresis) {
            low = false;
        }
    }

    if (!have_sample_) {
        // The baseline is not a change anyone announced: seed it with `low`
        // still clear, so the set() below publishes power.battery.low only
        // when the first sample is already low, as before the cell.
        have_sample_ = true;
        supply_.seed(
            PowerSupply{status.charging, status.usb_present, status.battery_percent, false});
    }
    // No event is lost to an equal value: each event fires only when a
    // field of the cell changes (charging/usb_present, or low false ->
    // true), and such a value never equals the one before it.
    supply_.set(PowerSupply{status.charging, status.usb_present, status.battery_percent, low});
}

}  // namespace wisp::core
