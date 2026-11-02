#pragma once

// Spec 6.3: publishes the two sub-project-1 power topics --
// "power.charging.changed" and "power.battery.low" -- by polling
// wisp::hal::Power (device id "power.main") on its own periodic
// os::Timer. Lives in wisp_core (not
// wisp_app) for the same reason wisp::core::StorageManager does: it only
// needs a hal:: device, an EventBus and Settings, nothing from wisp_app/
// wisp_ui.
//
// Modeled directly on wisp::core::ButtonService's own Timer-ownership
// pattern (see button_service.hpp): this class owns its sample Timer
// outright (named "wisp.pwrmon") rather than being driven by a Timer some
// owner (e.g. App) constructs externally, because -- unlike StorageManager
// ::refresh(), which always runs on the same fixed 2s cadence regardless
// of standby -- this class's own sample period must itself change between
// active (~1s) and standby (~30s, spec 6.7), the same way ButtonService's
// poll period widens from 20ms to 50ms. set_standby() below is this
// class's equivalent of ButtonService::set_poll_interval_ms(): it swaps
// the Timer for a new one at the appropriate period, idempotent, and
// never actually stops/destroys the old Timer while holding a lock
// sample() itself takes (see timer_mutex_'s doc comment) -- the same
// deadlock os::Timer::stop() warns about (any Timer's callback may be
// dispatched from one shared task; stopping one while holding a lock a
// *different* due Timer's callback needs can hang that shared task, and
// so every Timer in the process, not just this one).
//
// Hook position: wisp::app::PowerManager calls set_standby(true)/(false)
// from inside enter_standby()/exit_standby(), in the same relative
// position (and under the same "release the transition lock first"
// discipline) as its existing StorageManager::set_standby() call -- see
// power_manager.cpp. Unlike StorageManager's set_standby() (a pure
// software hint to a device-owned background thread that never fails and
// never blocks), this one *does* block (the Timer swap), which is exactly
// why PowerManager releases its own lock around the call, the same way it
// already does around ButtonService::set_poll_interval_ms().
//
// Device-optional, like every other hal:: pointer App hands around: if no
// "power.main" device is registered, `power` is null and this class is
// completely inert -- start()/stop()/set_standby()/sample() all become
// no-ops, never crash. Event source is `power`'s own device id (so
// "power.main" on every board that has one).
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
// itself never calls start()/stop()/set_standby()). sample_mutex_ guards
// the previous-sample bookkeeping (have_sample_/last_charging_/
// last_usb_present_/low_fired_) against concurrent sample() calls (in
// practice only the Timer callback calls it, but sample() is also exposed
// directly for tests, same as ButtonService::poll()). timer_mutex_ guards
// only the Timer pointer and the active/standby period choice, and is
// never held across anything sample_mutex_ also guards or across
// EventBus::publish() (which, per event_bus.hpp, never blocks on a
// handler, so holding a lock across it is fine in general -- this note is
// about *not* holding timer_mutex_ there, which would serve no purpose).

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "wisp/core/event_bus.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/hal/power.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

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
    // ever calling start().
    void sample();

    // The sample period currently in effect (1000 or 30000), regardless
    // of whether the Timer has actually been start()ed yet. Mainly for
    // tests; thread-safe.
    uint32_t sample_interval_ms() const;

private:
    EventBus& bus_;
    std::shared_ptr<hal::Power> power_;
    std::string source_;  // power_->info().id if power_ is non-null, else empty.
    Settings settings_;

    // Set (not held) while status() is being called, outside
    // sample_mutex_ -- see sample()'s own definition for why. Only used
    // to dedupe the WLOG_W below to state transitions instead of once per
    // failed sample: a real failure (I2C bus fault, device unplugged)
    // tends to persist for many consecutive samples, and logging every
    // single one of those at 1 Hz would flood the log for no added
    // information.
    std::atomic<bool> status_ok_{true};

    os::Mutex sample_mutex_;
    bool have_sample_ = false;
    bool last_charging_ = false;
    bool last_usb_present_ = false;
    bool low_fired_ = false;

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
