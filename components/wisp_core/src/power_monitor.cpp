#include "wisp/core/power_monitor.hpp"

#include <mutex>
#include <utility>

#include "wisp/base/error.hpp"
#include "wisp/core/log.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "power_monitor";
constexpr uint32_t kActiveSampleMs = 1000;
constexpr uint32_t kStandbySampleMs = 30000;
constexpr int64_t kDefaultLowBattPct = 15;
constexpr int64_t kLowBattHysteresis = 5;

uint32_t interval_for(bool standby) { return standby ? kStandbySampleMs : kActiveSampleMs; }

}  // namespace

PowerMonitor::PowerMonitor(EventBus& bus, std::shared_ptr<hal::Power> power, Settings settings)
    : bus_(bus),
      power_(std::move(power)),
      source_(power_ != nullptr ? power_->info().id : std::string()),
      settings_(std::move(settings)) {}

PowerMonitor::~PowerMonitor() { stop(); }

void PowerMonitor::start() {
    std::lock_guard<os::Mutex> lock(timer_mutex_);
    if (power_ == nullptr || timer_ != nullptr) {
        return;
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
    // Mirrors wisp::core::ButtonService::set_poll_interval_ms(): swap the
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

void PowerMonitor::sample() {
    if (power_ == nullptr) {
        return;
    }
    auto result = power_->status();
    if (!result) {
        WLOG_W(kTag, "status() failed: %s", result.error().message.c_str());
        return;
    }
    hal::PowerStatus status = result.value();

    std::lock_guard<os::Mutex> lock(sample_mutex_);

    if (have_sample_ &&
        (status.charging != last_charging_ || status.usb_present != last_usb_present_)) {
        Event e;
        e.topic = "power.charging.changed";
        e.source = source_;
        e.payload = Properties{
            {"charging", Value(status.charging)},
            {"usb_present", Value(status.usb_present)},
            {"battery_percent", Value(static_cast<int64_t>(status.battery_percent))},
        };
        bus_.publish(std::move(e));
    }
    have_sample_ = true;
    last_charging_ = status.charging;
    last_usb_present_ = status.usb_present;

    if (status.battery_percent != -1) {
        int64_t threshold = settings_.get_int("low_batt_pct", kDefaultLowBattPct);
        if (!low_fired_ && status.battery_percent <= threshold) {
            low_fired_ = true;
            Event e;
            e.topic = "power.battery.low";
            e.source = source_;
            e.payload = Properties{
                {"battery_percent", Value(static_cast<int64_t>(status.battery_percent))},
            };
            bus_.publish(std::move(e));
        } else if (low_fired_ && status.battery_percent > threshold + kLowBattHysteresis) {
            low_fired_ = false;
        }
    }
}

}  // namespace wisp::core
