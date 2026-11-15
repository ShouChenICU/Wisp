#pragma once

// Spec 6.3: publishes the two sub-project-1 power topics --
// "power.charging.changed" and "power.battery.low" -- by polling
// wisp::hal::Power (device id "power.main") on its own periodic
// os::Timer. Lives in wisp_core (not
// wisp_app) for the same reason wisp::core::StorageManager does: it only
// needs a hal:: device, an EventBus and Settings, nothing from wisp_app/
// wisp_ui.
//
// Owns its independent sample timer ("wisp.pwrmon"): active 1s and
// standby 30s (spec 6.7). Button input mode never changes this cadence.
// set_standby() swaps the timer without holding sample_mutex_: stopping
// a timer may wait for the shared timer task, whose callbacks take that
// mutex. PowerManager likewise drops its transition lock around this call.
//
// Device-optional, like every other hal:: pointer App hands around: if no
// "power.main" device is registered, `power` is null and this class is
// completely inert -- start()/stop()/set_standby()/sample() all become
// no-ops, never crash. Event source is kPowerMonitorSource, the
// component's name: the events come from the supply cell's mirror, a plain
// function that cannot carry the device id (and no consumer reads it).
//
// The supply cell (supply()) holds what the samples established: charging,
// usb_present, battery_percent and the low-battery state after the
// hysteresis below. Its mirror publishes the two events from the cell's
// before/after values, so they fire exactly when the rules below say.
// The first successful sample seeds the cell (no event) and then sets the
// computed `low`, so a battery already low at boot still publishes
// power.battery.low, as it did before the cell.
//
// Events published (payload field names match wisp::hal::PowerStatus's
// own members, not a separate vocabulary -- see docs/architecture.md's
// note on why this isn't "vbus_present", the axp2101_status_t driver
// struct's field name for the same thing):
//   - "power.charging.changed": whenever `charging` or `usb_present`
//     differs from the previous sample (never on the very first sample,
//     since there is no previous value to differ from -- same "no event
//     for the baseline" rule as StorageManager::refresh()'s hot-plug
//     detection). Payload: {"charging": bool, "usb_present": bool,
//     "battery_percent": int}.
//   - "power.battery.low": once, the first time `battery_percent` is
//     known (!= -1) and drops to <= Settings "power"/"low_batt_pct"
//     (default 15). Re-arms (may fire again) only after a later sample
//     observes battery_percent > threshold + 5 (hysteresis -- avoids
//     repeated firing while hovering right at the threshold). A
//     battery_percent of -1 (unknown) never triggers this, and never
//     re-arms it either (an unknown reading is simply skipped for this
//     check). Payload: {"battery_percent": int}.
//
// Thread-safety: every public method may be called from any thread,
// concurrently, including from this class's own Timer callback (sample()
// itself never calls start()/stop()/set_standby()). call_mutex_ makes
// concurrent sample() calls (the Timer callback, and direct calls from
// tests, same as ButtonService::poll()) run one after the other, from the
// status() read through the cell's set(), so the cell always ends at the
// status read last. sample_mutex_ guards the previous-sample bookkeeping
// (have_sample_, last_status_ and the supply cell's writes) and is not
// held across the status() read, so last_status() never waits for the
// chip. timer_mutex_ guards
// only the Timer pointer and the active/standby period choice, and is
// never held across anything sample_mutex_ also guards or across
// EventBus::publish() (which, per event_bus.hpp, never blocks on a
// handler, so holding a lock across it is fine in general -- this note is
// about *not* holding timer_mutex_ there, which would serve no purpose).
// Lock order: call_mutex_ -> sample_mutex_ -> the supply cell's lock ->
// the bus's publish lock; nothing takes them the other way.

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "wisp/core/event_bus.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/state_cell.hpp"
#include "wisp/hal/power.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

// What the power samples established, for components that follow it.
struct PowerSupply {
    bool charging = false;
    bool usb_present = false;
    int battery_percent = -1;  // -1: unknown
    bool low = false;          // the low-battery state after the hysteresis
    bool operator==(const PowerSupply&) const = default;
};

// Event::source of power.charging.changed and power.battery.low. Not a
// device id: boards choose those.
inline constexpr char kPowerMonitorSource[] = "power_monitor";

class PowerMonitor {
public:
    // `power` may be null (no "power.main" device registered -- see file
    // header's "device-optional" note). `settings` should already be
    // bound to the "power" namespace (the same namespace wisp::app::
    // PowerManager binds its own Settings to for "screen_off_s"); this
    // class reads "low_batt_pct" (default 15) lazily, on the first
    // successful sample() (not here in the constructor), caches it, and
    // re-reads it at most once every kThresholdRefreshSamples samples
    // thereafter (also forced on the next sample() after a start() call)
    // -- not fresh on every sample() -- so a later Settings change still
    // takes effect without reconstructing this object, just not
    // necessarily on the very next sample(). See sample()'s own doc
    // comment for why: re-reading Settings (NVS + JSON parsing on the
    // real backend) on every sample, on the shared esp_timer task, was
    // more I/O than this value's staleness tolerance actually needs.
    PowerMonitor(EventBus& bus, std::shared_ptr<hal::Power> power, Settings settings);

    // Calls stop() if the Timer is still active.
    ~PowerMonitor();

    PowerMonitor(const PowerMonitor&) = delete;
    PowerMonitor& operator=(const PowerMonitor&) = delete;

    // Starts the periodic sample Timer at the active-mode period (~1s), or
    // the standby period if set_standby(true) was already called before
    // this. A no-op if `power` is null, or if already started. Also
    // forces the next sample() to re-read "low_batt_pct" from Settings
    // (see the constructor's doc comment on threshold caching), even if
    // this isn't the first start() this object has seen.
    void start();

    // Stops the Timer. Idempotent, and safe even if start() was never
    // called.
    void stop();

    // Swaps the sample Timer for one at the standby (~30s) or active
    // (~1s) period -- see the file header's "hook position" note on how
    // wisp::app::PowerManager calls this. A no-op if `power` is null, if
    // the Timer hasn't been start()ed yet (the new period still takes
    // effect, for whenever start() does run), or if already in the
    // requested state.
    void set_standby(bool standby);

    // Reads `power`'s status() once and publishes power.charging.changed/
    // power.battery.low as described in the file header. A no-op if
    // `power` is null. Safe to call directly (e.g. from a test), without
    // ever calling start(), also while the Timer samples: concurrent calls
    // run in turn, so the supply ends at the status read last.
    void sample();

    // What the last sample() read: nullopt before the first one, after a
    // failed read, or without a device. For callers that want the battery
    // state without touching the power chip themselves (GET /device).
    std::optional<hal::PowerStatus> last_status() const;

    // The supply as the samples left it; moves on a successful sample that
    // changes any field (the percent included), never on a failed read.
    // A subscriber releases its subscription before PowerMonitor is
    // destroyed (App::stop() order; wisp/core/state_cell.hpp).
    const StateCell<PowerSupply>& supply() const { return supply_; }

    // The sample period currently in effect (1000 or 30000), regardless
    // of whether the Timer has actually been start()ed yet. Mainly for
    // tests; thread-safe.
    uint32_t sample_interval_ms() const;

private:
    EventBus& bus_;
    std::shared_ptr<hal::Power> power_;
    Settings settings_;

    // Held by sample() for the whole call (file header, Thread-safety).
    os::Mutex call_mutex_;
    // Set while status() is being called, under call_mutex_ but outside
    // sample_mutex_ -- see sample()'s own definition for why. Only used
    // to dedupe the WLOG_W below to state transitions instead of once per
    // failed sample: a real failure (I2C bus fault, device unplugged)
    // tends to persist for many consecutive samples, and logging every
    // single one of those at 1 Hz would flood the log for no added
    // information.
    std::atomic<bool> status_ok_{true};

    mutable os::Mutex sample_mutex_;
    // Whether a sample has succeeded yet. Not derivable from supply_: a
    // first sample may equal PowerSupply{}.
    bool have_sample_ = false;
    // Set only under sample_mutex_. Declared after bus_, which its
    // constructor uses; PowerMonitor itself is destroyed before the bus
    // stops (App::stop()).
    StateCell<PowerSupply> supply_;
    std::optional<hal::PowerStatus> last_status_;  // guarded by sample_mutex_

    // Threshold cache (constructor's doc comment on why this isn't a
    // fresh Settings read every sample): cached_low_batt_pct_ is read in
    // sample() once every kThresholdRefreshSamples samples (power_
    // monitor.cpp), or on the very next sample() after any start() call.
    // samples_until_threshold_refresh_ == 0 means "read it on the next
    // sample() call" -- true from construction, so the very first
    // sample() this object ever makes always reads a fresh value.
    int64_t cached_low_batt_pct_ = 0;
    uint32_t samples_until_threshold_refresh_ = 0;

    mutable os::Mutex timer_mutex_;
    std::unique_ptr<os::Timer> timer_;
    bool standby_ = false;
};

}  // namespace wisp::core
