#pragma once

// PowerManager (spec 4.1-4.3): the single owner of the device's power level,
// one of hal::PowerLevel:
//
//   Active       normal.
//   Dim          kDimLeadMs before the idle screen-off: the panel drops to
//                kDimPercent of the set brightness (at least 10). No device
//                is told; any activity returns to Active.
//   Standby      after Settings "power"/"screen_off_s" (default 60, 0 never)
//                of idleness, or enter_standby() (a short press of the
//                power key). The UI pauses and every device that declares
//                caps "standby" gets set_power(Standby); CPU and Wi-Fi stay.
//   DeepStandby  after "power"/"deep_after_s" (default 300, 0 never) in
//                Standby while no network wake lock is held; devices get
//                set_power(DeepStandby). The Wi-Fi side follows level()
//                (see net_wiring.cpp). A network lock taken here returns
//                to Standby; once it is released deep standby returns at
//                once, since its timer runs from entering Standby whatever
//                the locks did meanwhile.
//
// Automatic light sleep (Platform::set_light_sleep) is on exactly while the
// level is DeepStandby, "power"/"light_sleep" (default true) is set and no
// cpu lock is held. A screen lock keeps the screen at Active: no dim, no
// idle screen-off; enter_standby() still turns it off and leaves the lock.
// Every level change publishes "power.state" ({"state": to_string(level)})
// and logs "power: <level>".
//
// Devices (spec 4.2): on the way down every device with a standby policy
// other than None is called in registry order (DeviceRegistry::all(), by
// id), on the way up in the reverse order. The audio devices are the
// exception: going down every AudioIn comes after the other devices and
// before every AudioOut, going up every AudioOut comes first and then
// every AudioIn, because in a duplex pair the output clocks the input
// (wisp/port/esp/i2s_power.hpp; there the output refuses with Busy while
// its input is still on). ErrorCode::Unsupported is ignored. Busy (a
// device still in use) is logged once and asked again on every tick and
// level change until it agrees. StorageManager, PowerMonitor and
// ButtonService are services, not devices, and keep their own standby
// calls (set_standby(true/false)); button input policy and PMU sampling
// cadence remain independent.
//
// Threading: whenever a UiRuntime exists, every transition runs on its
// task, because the display may only be called from there (spec 5.3).
// tick(), note_activity(), the wake-lock change callback and the
// "any button wakes it" subscriber post their work onto that task; callers
// already on it (the system duties' power key) call enter_standby()/
// exit_standby() directly. A wake-lock change is always posted (coalesced
// into one job), even from the UI task, so a lock taken or released
// anywhere -- inside a device's set_power() during a transition included
// -- never runs a transition in the caller's critical section. Headless
// (no UiRuntime), everything runs on the caller's thread, lock changes
// included, so there a device's set_power() must not take or release a
// wake lock (no board ships headless).
//
// Settings: the power keys are read by the constructor and then again by
// the next tick() (on the shared timer task, which may touch flash) after
// each system.settings event, and every kSettingsRefreshTicks ticks in
// case such an event was dropped; never on the UI task. A stored value
// core::SystemSettings::set() would refuse reads as its default, as
// SystemSettings::get() reports it.
//
// Any button event resets the idle clock and, while the screen is off,
// wakes the device (spec 6.7/8 of the foundation spec), except
// "button.power" once set_power_key_owned(true) handed that key to its
// owner. With a UiRuntime, touch activity resets the idle clock too
// (UiRuntime::set_touch_activity_cb(), which only fires when there is a
// touch device).

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wisp/core/button_service.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/power_monitor.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/hal/device.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/os/os.hpp"

namespace wisp::ui {
class UiRuntime;
}

namespace wisp::app {

struct PowerManagerDeps {
    core::EventBus& bus;
    core::DeviceRegistry& registry;
    core::WakeLockRegistry& locks;
    core::Settings settings;                // namespace "power"
    std::shared_ptr<hal::Display> display;  // brightness only; may be null
    // The rest may be null or empty: each is skipped when missing.
    core::StorageManager* storage = nullptr;
    core::PowerMonitor* power_monitor = nullptr;
    ui::UiRuntime* ui = nullptr;
    core::ButtonService* buttons = nullptr;
    std::function<void(bool enable)> set_light_sleep;  // Platform::set_light_sleep
    // Runs on the transition's thread right before the devices go to deep
    // standby, without PowerManager's lock: App writes what still waits
    // for flash (App::flush_storage(), spec 3.8 of the flash-thread
    // design). May block for seconds.
    std::function<void()> before_deep_standby;
    // Runs on every touch read with a finger down (the UI task) and every
    // button event (the bus dispatcher), besides resetting the idle clock:
    // the user is there (an alarm stops, spec 5.1). Keep it short and
    // non-blocking.
    std::function<void()> on_activity;
};

class PowerManager {
public:
    static constexpr uint32_t kDimLeadMs = 10000;
    static constexpr int kDimPercent = 30;
    // tick() re-reads the power settings at least this often, in case the
    // system.settings event that announced a change was dropped.
    static constexpr uint32_t kSettingsRefreshTicks = 60;

    explicit PowerManager(PowerManagerDeps deps);

    // Unsubscribes from the bus, drops the wake-lock change callback and
    // clears the touch-activity hook (on the UI task, after every job this
    // posted). Does not leave standby: App::stop() does that first.
    // Precondition: not called on the UI task. Jobs this posted (tick and
    // lock-change evaluations) capture `this` and could not be drained
    // from there; App::stop() destroys it from its own thread.
    ~PowerManager();

    PowerManager(const PowerManager&) = delete;
    PowerManager& operator=(const PowerManager&) = delete;

    // Active or Dim -> Standby (the power key, the idle timeout). A no-op
    // while the screen is already off.
    void enter_standby();
    // Any level -> Active. A no-op at Active. Resets the idle clock.
    void exit_standby();

    hal::PowerLevel level() const;
    // level() >= Standby: the screen is off.
    bool standby() const;
    // What set_light_sleep was last told (false before the first call).
    bool light_sleep_on() const;
    // Whether there is a set_light_sleep hook at all.
    bool light_sleep_supported() const;

    // Resets the idle clock to now (os::now_ms()); at Dim also returns to
    // Active (posted to the UI task when there is one).
    void note_activity();

    // Called about once a second (App's "wisp.power" timer): re-reads the
    // settings if they changed, then moves Active -> Dim -> Standby -> DeepStandby by the
    // idle clock and the deep timer, and back on locks, retries Busy
    // devices and updates light sleep. `now_ms` is os::now_ms() in
    // production; the work itself is posted to the UI task when there is
    // one (a transition can block for the panel's sleep delay and must not
    // hold up the shared timer task), and rechecked there against the
    // state at that moment.
    void tick(uint64_t now_ms);

    // Sets the panel brightness (core::SystemSettings' apply hook): applied
    // right away while the panel is on (the dimmed share of it at Dim),
    // remembered while it is off and applied on wake -- changing brightness
    // never wakes the screen. Callable from any thread and never blocks:
    // off the UI task one "apply the latest level" job is posted (later
    // calls only replace the level); if the post queue is full the change
    // is dropped with a warning. Callers must stop calling this before the
    // PowerManager is destroyed.
    void set_brightness(uint8_t level);

    // Excludes "button.power" from the "any button wakes it" subscriber
    // once some other code (the system duties' PowerKeyController)
    // decides what that key does. Without this one short press in standby
    // ("down" then "short") would be acted on twice: this class wakes on
    // "down", and the owner, reading the screen as on by then, turns it
    // off again on "short". The idle clock still resets for every button.
    void set_power_key_owned(bool owned);

private:
    enum class Dir : uint8_t { Down, Up };

    // Runs fn on the UI task (posted) when there is one, else right here.
    // `what` names the job in the warning logged when the queue is full.
    void run_on_ui(const char* what, std::function<void()> fn);

    // max(os::now_ms(), the latest tick's now): the time for transitions
    // not driven by a tick, consistent with a test clock running ahead.
    uint64_t now_for_eval() const;
    // Whether the screen is off and has been for deep_after_s by `now_ms`.
    bool deep_due_at(uint64_t now_ms) const;

    // The level the policy wants now. `now_ms` is set for tick-driven
    // checks (the idle clock is consulted); nullopt for lock changes.
    hal::PowerLevel wanted_level(std::optional<uint64_t> now_ms) const;
    // Whether light sleep should be on at `level` right now.
    bool wants_light_sleep(hal::PowerLevel level) const;

    // The posted bodies of tick() and of the lock-change callback.
    void evaluate(std::optional<uint64_t> now_ms);
    void schedule_tick_eval(uint64_t now_ms);
    void schedule_lock_eval();

    // Moves from the current level to `target`. Caller holds `lock` on
    // transition_mutex_; it is dropped around the PowerMonitor and
    // ButtonService calls (see transition_mutex_).
    void go_to(hal::PowerLevel target, uint64_t now_ms, std::unique_lock<os::Mutex>& lock);
    void screen_off(uint64_t now_ms, std::unique_lock<os::Mutex>& lock);
    void screen_on(std::unique_lock<os::Mutex>& lock);
    void set_level(hal::PowerLevel level);

    // Calls set_power(level) on every managed device in `dir` order and
    // records which answered Busy. Caller holds transition_mutex_.
    void walk_devices(hal::PowerLevel level, Dir dir);
    // Asks the Busy devices again. Caller holds transition_mutex_.
    void retry_busy();
    // Managed devices in `dir` order; with `only`, just those ids.
    std::vector<std::shared_ptr<hal::Device>> ordered_devices(
        Dir dir, const std::vector<std::string>* only) const;
    // One set_power() call; true if it answered Busy.
    bool power_device(hal::Device& device, hal::PowerLevel level, bool first_try);

    // Tells the hook when wants_light_sleep() changed. Caller holds
    // transition_mutex_.
    void update_light_sleep();
    void light_sleep_off();

    // Brightness: the panel shows `user_brightness_` at Active and its dim
    // share at Dim. Caller holds transition_mutex_.
    void apply_brightness(uint8_t level);
    void show_brightness(int level);
    int dim_brightness() const;

    void publish_state(hal::PowerLevel level);

    std::atomic<uint8_t> requested_brightness_{0};
    std::atomic<bool> brightness_job_queued_{false};

    core::EventBus& bus_;
    core::DeviceRegistry& registry_;
    core::WakeLockRegistry& locks_;
    core::Settings settings_;
    std::shared_ptr<hal::Display> display_;
    core::StorageManager* storage_;
    core::PowerMonitor* power_monitor_;
    ui::UiRuntime* ui_;
    core::ButtonService* buttons_;
    std::function<void(bool)> set_light_sleep_;
    std::function<void()> before_deep_standby_;
    std::function<void()> on_activity_;

    // Serializes transitions (and every field below it marked so) against
    // each other. Released around PowerMonitor::set_standby() and
    // ButtonService::set_standby(): service lifecycle work can wait for
    // background tasks, which also run tick(), so
    // holding this lock across that wait could deadlock. Released around
    // before_deep_standby too, which waits seconds for the storage worker. With a UiRuntime
    // every transition runs on its one task, so nothing interleaves in
    // that window; headless, a wake arriving there can be lost (seen as
    // "already awake") until the next button press -- no board ships
    // headless.
    mutable os::Mutex transition_mutex_;
    // go_to() calls so far, guarded by transition_mutex_: a transition that
    // released the lock can tell another one ran meanwhile.
    uint64_t transitions_ = 0;
    std::atomic<hal::PowerLevel> level_{hal::PowerLevel::Active};
    std::atomic<uint64_t> last_activity_ms_;
    std::atomic<bool> power_key_owned_{false};
    std::atomic<bool> light_sleep_on_{false};

    // Reads the power keys into the cached values below.
    void read_settings();
    // A system.settings event arrived; the next tick() calls read_settings().
    std::atomic<bool> settings_dirty_{false};
    // Ticks since the last read_settings().
    std::atomic<uint32_t> ticks_since_read_{0};
    // Cached so UI-task work never reads Settings.
    std::atomic<int64_t> screen_off_ms_{60000};   // 0: never
    std::atomic<int64_t> deep_after_ms_{300000};  // 0: never
    std::atomic<bool> light_sleep_setting_{true};
    // The deep timer had run out at the last tick-driven evaluation; lock
    // changes, which have no clock of their own, go by this.
    std::atomic<bool> deep_due_{false};
    // The latest `now_ms` tick() saw.
    std::atomic<uint64_t> last_tick_ms_{0};
    // When Standby was last entered from a lit screen (written under
    // transition_mutex_, read by tick() without it).
    std::atomic<uint64_t> standby_since_ms_{0};
    // busy_ is not empty (read by tick() without the lock).
    std::atomic<bool> busy_pending_{false};

    // Guarded by transition_mutex_ from here on.
    // The level devices were last walked to, and the ids that answered
    // Busy (in no particular order).
    hal::PowerLevel device_level_ = hal::PowerLevel::Active;
    std::vector<std::string> busy_;

    // True from just before the panel is put to sleep until just after it
    // wakes; brightness changes in between are only remembered.
    bool panel_asleep_ = false;
    int user_brightness_ = 200;   // what the setting asks for
    int shown_brightness_ = 200;  // what the panel was last told

    std::atomic<bool> tick_eval_queued_{false};
    std::atomic<bool> lock_eval_queued_{false};
    std::atomic<uint64_t> tick_eval_now_{0};

    core::SubscriptionId button_sub_ = 0;
    core::SubscriptionId settings_sub_ = 0;
};

}  // namespace wisp::app
