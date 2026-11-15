#include "wisp/app/power_manager.hpp"

#include <algorithm>
#include <mutex>
#include <type_traits>
#include <utility>

#include "wisp/base/error.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/core/topics.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

static_assert(std::is_trivially_copyable_v<hal::PowerLevel>,
              "the cell copies the level under its lock without allocating");

namespace {

constexpr const char* kTag = "power_manager";
// Dim never goes below the lowest brightness the setting allows.
constexpr int kMinDimBrightness = core::setting_spec(core::SettingId::Brightness).min;

bool screen_lit(hal::PowerLevel level) { return level < hal::PowerLevel::Standby; }

int64_t seconds_to_ms(int64_t s) { return s > 0 ? s * 1000 : 0; }

// The level cell's mirror: the power.state event subscribers had before
// the cell, published under the cell's lock (so in change order).
void power_state_mirror(core::EventBus& bus, const hal::PowerLevel&, const hal::PowerLevel& after) {
    core::Event e;
    e.topic = core::topics::kPowerState;
    e.source = kPowerManagerSource;
    e.payload = wisp::Properties{{"state", wisp::Value(std::string(hal::to_string(after)))}};
    bus.publish(std::move(e));
}

}  // namespace

PowerManager::PowerManager(PowerManagerDeps deps)
    : bus_(deps.bus),
      registry_(deps.registry),
      locks_(deps.locks),
      display_(std::move(deps.display)),
      storage_(deps.storage),
      power_monitor_(deps.power_monitor),
      ui_(deps.ui),
      buttons_(deps.buttons),
      power_key_(std::move(deps.power_key)),
      set_light_sleep_(std::move(deps.set_light_sleep)),
      before_deep_standby_(std::move(deps.before_deep_standby)),
      on_activity_(std::move(deps.on_activity)),
      level_cell_(bus_, hal::PowerLevel::Active, &power_state_mirror),
      last_activity_ms_(os::now_ms()) {
    // Without the cell (tests) the timeouts and brightness keep their
    // defaults.
    if (deps.system_settings != nullptr) {
        // The cell's value now, so the first tick() (possibly before the
        // subscription's first call) already uses it; that first call then
        // delivers the same value again.
        const core::SystemSettingsValues now = deps.system_settings->get();
        apply_settings(now.screen_off_s, now.deep_after_s);
        // App set the panel to this level before the UI started.
        user_brightness_ = now.brightness;
        shown_brightness_ = now.brightness;
        followed_brightness_ = now.brightness;
        settings_sub_ =
            deps.system_settings->subscribe([this](const core::SystemSettingsValues& v) {
                apply_settings(v.screen_off_s, v.deep_after_s);
            });
        // The display is the UI thread's, so brightness follows the cell
        // there only. Without a UI nobody looks at the panel and a wisp.bus
        // callback must not call a device: brightness stays put.
        if (display_ && ui_ != nullptr) {
            brightness_sub_ = deps.system_settings->subscribe(
                [this](const core::SystemSettingsValues& v) { follow_brightness(v.brightness); },
                ui_);
        }
    }

    if (ui_ != nullptr) {
        ui_->set_touch_activity_cb([this] {
            note_activity();
            if (on_activity_) on_activity_();
        });
    }

    // A lock change can move the level without waiting for the next tick:
    // a network lock leaves deep standby, a cpu lock stops light sleep, a
    // screen lock ends dim.
    locks_.set_on_change([this] { schedule_lock_eval(); });

    if (buttons_ != nullptr) {
        // Buttons reach the UI task through a cell rather than a post or
        // the bus, so a press wakes the screen with the post queue full and
        // wisp.bus flooded. The follower's first call, at subscription,
        // finds both flags clear and does nothing. Subscribed before the
        // listener exists, so no edge comes before it.
        button_follower_ = button_activity_.subscribe(
            [this](const uint32_t&) { on_button_activity(); },
            ui_ != nullptr ? static_cast<core::Dispatcher*>(ui_) : nullptr);
        // On wisp.button under ButtonService's lock: atomics and a cell
        // update only. A wake can block for the panel's wake latency, so it
        // waits for the follower.
        button_listener_ = buttons_->add_listener([this](const core::ButtonEdge& e) {
            last_activity_ms_.store(os::now_ms());
            activity_pending_.store(true, std::memory_order_release);
            const bool owned = power_key_owned_.load(std::memory_order_acquire) &&
                               !power_key_.empty() && e.id.view() == power_key_;
            if (!owned) {
                // The owner decides what the power key does -- see
                // set_power_key_owned(). Whether the screen is off is
                // read by the follower, so a press during the transition
                // to standby still wakes it.
                wake_latch_.store(true, std::memory_order_release);
            }
            button_activity_.update([](uint32_t& n) { ++n; });
        });
    }
}

void PowerManager::on_button_activity() {
    if (activity_pending_.exchange(false, std::memory_order_acq_rel)) {
        if (on_activity_) on_activity_();
        undim();
    }
    if (wake_latch_.exchange(false, std::memory_order_acq_rel) && standby()) {
        exit_standby();
    }
}

PowerManager::~PowerManager() {
    // After these return neither the button listener and follower, the
    // settings and brightness followers nor the lock callback runs again
    // (releasing a UI-dispatched follower from this thread waits for its
    // call in progress), so nothing new is posted. The listener goes
    // first, so no edge moves the counter after its follower is gone;
    // ButtonService outlives this object
    // (App::stop()). Jobs already posted are drained below -- except when
    // called on the UI task itself, which cannot wait for its own queue;
    // the header makes that a caller's precondition.
    if (buttons_ != nullptr) {
        buttons_->remove_listener(button_listener_);
    }
    button_follower_.reset();
    settings_sub_.reset();
    brightness_sub_.reset();
    locks_.set_on_change(nullptr);

    if (ui_ == nullptr) {
        return;
    }
    if (ui_->on_ui_thread()) {
        ui_->set_touch_activity_cb(nullptr);
        return;
    }
    // The post queue is FIFO, so this job runs after every job this object
    // posted. Clearing the touch hook must itself happen on the UI task:
    // the touch read copies the callback and calls the copy outside its
    // lock, so only running on that same task orders the clear after
    // every such call.
    auto done = std::make_shared<os::Semaphore>(1, 0);
    while (!ui_->post([this, done] {
        ui_->set_touch_activity_cb(nullptr);
        done->give();
    })) {
        os::sleep_ms(1);
    }
    done->take(os::kForever);
}

void PowerManager::run_on_ui(const char* what, std::function<void()> fn) {
    if (ui_ == nullptr || ui_->on_ui_thread()) {
        fn();
        return;
    }
    // A dropped wake must leave a trace; blocking here would hold up the
    // caller (often the bus dispatcher).
    if (!ui_->post(std::move(fn))) {
        WLOG_W(kTag, "post(%s) dropped (post queue full)", what);
    }
}

hal::PowerLevel PowerManager::level() const { return level_cell_.get(); }

bool PowerManager::standby() const { return !screen_lit(level_cell_.get()); }

bool PowerManager::light_sleep_on() const { return light_sleep_on_.load(); }

bool PowerManager::light_sleep_supported() const { return static_cast<bool>(set_light_sleep_); }

void PowerManager::enter_standby() {
    std::unique_lock<os::Mutex> lock(transition_mutex_);
    if (!screen_lit(level())) {
        return;  // The screen is already off.
    }
    go_to(hal::PowerLevel::Standby, now_for_eval(), lock);
}

void PowerManager::exit_standby() {
    {
        std::unique_lock<os::Mutex> lock(transition_mutex_);
        if (level() == hal::PowerLevel::Active) {
            return;
        }
        go_to(hal::PowerLevel::Active, now_for_eval(), lock);
    }
    note_activity();
}

void PowerManager::note_activity() {
    last_activity_ms_.store(os::now_ms());
    if (level() != hal::PowerLevel::Dim) {
        return;
    }
    run_on_ui("undim", [this] { undim(); });
}

void PowerManager::undim() {
    if (level() != hal::PowerLevel::Dim) {
        return;  // most button edges: no transition lock to take
    }
    std::unique_lock<os::Mutex> lock(transition_mutex_);
    if (level() == hal::PowerLevel::Dim) {
        go_to(hal::PowerLevel::Active, now_for_eval(), lock);
    }
}

void PowerManager::apply_settings(int64_t screen_off_s, int64_t deep_after_s) {
    screen_off_ms_.store(seconds_to_ms(screen_off_s));
    deep_after_ms_.store(seconds_to_ms(deep_after_s));
}

void PowerManager::tick(uint64_t now_ms) {
    last_tick_ms_.store(now_ms);
    // A screen lock counts as continuous activity, so the idle clock starts
    // only once the lock goes.
    if (locks_.held(core::WakeLockKind::Screen)) {
        uint64_t last = last_activity_ms_.load();
        while (last < now_ms && !last_activity_ms_.compare_exchange_weak(last, now_ms)) {
        }
    }

    // deep_due_ is refreshed even while a network lock holds the level,
    // so releasing that lock can go deep without waiting for a tick.
    const hal::PowerLevel current = level();
    const bool work = wanted_level(now_ms) != current || busy_pending_.load() ||
                      wants_light_sleep(current) != light_sleep_on_.load() ||
                      deep_due_at(now_ms) != deep_due_.load();
    if (work) {
        schedule_tick_eval(now_ms);
    }
}

void PowerManager::schedule_tick_eval(uint64_t now_ms) {
    tick_eval_now_.store(now_ms);
    if (ui_ == nullptr) {
        evaluate(now_ms);
        return;
    }
    if (tick_eval_queued_.exchange(true)) {
        return;  // the queued job reads the newest tick_eval_now_
    }
    bool posted = ui_->post([this] {
        // Clear before reading, so a tick landing after the read queues a
        // fresh job instead of being lost.
        tick_eval_queued_.exchange(false);
        evaluate(tick_eval_now_.load());
    });
    if (!posted) {
        tick_eval_queued_.store(false);  // the next tick tries again
    }
}

void PowerManager::schedule_lock_eval() {
    // Called from WakeLockRegistry's change callback, on whichever thread
    // changed a lock, possibly inside its own critical section (even a
    // device's set_power() during one of our transitions). So with a UI
    // task this only queues the work, from the UI task too, and returns.
    // Headless there is no task to queue to; the transition runs here.
    // That makes every thread that takes or drops a wake lock a thread
    // that may run a transition (devices' set_power(), the deep-standby
    // flush, light sleep). None of that needs flash access on this
    // thread: the deep-standby flush (before_deep_standby_) only blocks
    // on Settings::flush() and StorageWorker::drain(), both of which wait
    // for "wisp.fs" to finish its own run_sync() work; wisp.fs remains
    // the only task that declares flash_access (AGENTS.md 5).
    if (ui_ == nullptr) {
        evaluate(std::nullopt);
        return;
    }
    if (lock_eval_queued_.exchange(true)) {
        return;
    }
    bool posted = ui_->post([this] {
        lock_eval_queued_.exchange(false);
        evaluate(std::nullopt);
    });
    if (!posted) {
        lock_eval_queued_.store(false);
        WLOG_W(kTag, "post(lock change) dropped (post queue full); next tick catches up");
    }
}

uint64_t PowerManager::now_for_eval() const {
    return std::max<uint64_t>(os::now_ms(), last_tick_ms_.load());
}

bool PowerManager::deep_due_at(uint64_t now_ms) const {
    if (screen_lit(level())) {
        return false;
    }
    const int64_t deep_ms = deep_after_ms_.load();
    const uint64_t since = standby_since_ms_.load();
    return deep_ms > 0 && now_ms >= since && now_ms - since >= static_cast<uint64_t>(deep_ms);
}

hal::PowerLevel PowerManager::wanted_level(std::optional<uint64_t> now_ms) const {
    const hal::PowerLevel current = level();
    const bool screen_lock = locks_.held(core::WakeLockKind::Screen);
    switch (current) {
        case hal::PowerLevel::Active:
        case hal::PowerLevel::Dim: {
            if (screen_lock) {
                return hal::PowerLevel::Active;
            }
            if (!now_ms) {
                return current;  // idleness is only judged on a tick
            }
            const int64_t off_ms = screen_off_ms_.load();
            if (off_ms <= 0) {
                return hal::PowerLevel::Active;
            }
            const uint64_t last = last_activity_ms_.load();
            const uint64_t idle = *now_ms > last ? *now_ms - last : 0;
            if (idle >= static_cast<uint64_t>(off_ms)) {
                return hal::PowerLevel::Standby;
            }
            if (idle + kDimLeadMs >= static_cast<uint64_t>(off_ms)) {
                return hal::PowerLevel::Dim;
            }
            return hal::PowerLevel::Active;
        }
        case hal::PowerLevel::Standby: {
            const bool due = now_ms ? deep_due_at(*now_ms) : deep_due_.load();
            return due && !locks_.held(core::WakeLockKind::Network) ? hal::PowerLevel::DeepStandby
                                                                    : hal::PowerLevel::Standby;
        }
        case hal::PowerLevel::DeepStandby:
            return locks_.held(core::WakeLockKind::Network) ? hal::PowerLevel::Standby
                                                            : hal::PowerLevel::DeepStandby;
    }
    return current;
}

bool PowerManager::wants_light_sleep(hal::PowerLevel level) const {
    return level == hal::PowerLevel::DeepStandby && WISP_ENABLE_LIGHT_SLEEP && set_light_sleep_ &&
           !locks_.held(core::WakeLockKind::Cpu);
}

void PowerManager::evaluate(std::optional<uint64_t> now_ms) {
    std::unique_lock<os::Mutex> lock(transition_mutex_);
    if (now_ms) {
        deep_due_.store(deep_due_at(*now_ms));
    }
    const hal::PowerLevel target = wanted_level(now_ms);
    if (target != level()) {
        go_to(target, now_ms.value_or(now_for_eval()), lock);
    } else {
        retry_busy();  // a level change has just asked every device anyway
    }
    update_light_sleep();
}

void PowerManager::go_to(hal::PowerLevel target, uint64_t now_ms,
                         std::unique_lock<os::Mutex>& lock) {
    const uint64_t transition = ++transitions_;
    const hal::PowerLevel current = level();
    if (target == current) {
        return;
    }
    if (screen_lit(current) && screen_lit(target)) {
        // Active <-> Dim: brightness only; no device is told.
        show_brightness(target == hal::PowerLevel::Dim ? dim_brightness() : user_brightness_);
        set_level(target);
        return;
    }
    if (screen_lit(target)) {
        screen_on(lock);
        return;
    }
    if (screen_lit(current)) {
        screen_off(now_ms, lock);
        if (target == hal::PowerLevel::Standby) {
            return;
        }
    }
    if (target == hal::PowerLevel::DeepStandby) {
        if (before_deep_standby_) {
            // It waits seconds for the storage worker; see transition_mutex_.
            lock.unlock();
            before_deep_standby_();
            lock.lock();
            // Meanwhile another transition (a wake) may have run, or a
            // network lock may want the radio: either way this one is stale.
            if (transitions_ != transition || locks_.held(core::WakeLockKind::Network)) {
                return;
            }
        }
        walk_devices(hal::PowerLevel::DeepStandby, Dir::Down);
        set_level(hal::PowerLevel::DeepStandby);
        update_light_sleep();
    } else {
        light_sleep_off();
        walk_devices(hal::PowerLevel::Standby, Dir::Up);
        set_level(hal::PowerLevel::Standby);
    }
}

void PowerManager::screen_off(uint64_t now_ms, std::unique_lock<os::Mutex>& lock) {
    // Rendering stops before the panel sleeps; screen_on() mirrors this.
    if (ui_ != nullptr) {
        ui_->set_paused(true);
    }
    panel_asleep_ = true;
    walk_devices(hal::PowerLevel::Standby, Dir::Down);
    if (storage_ != nullptr) {
        storage_->set_standby(true);  // a software hint; never fails
    }
    if (power_monitor_ != nullptr) {
        core::PowerMonitor* monitor = power_monitor_;  // see transition_mutex_
        lock.unlock();
        monitor->set_standby(true);
        lock.lock();
    }
    if (buttons_ != nullptr) {
        core::ButtonService* buttons = buttons_;
        lock.unlock();
        buttons->set_standby(true);
        lock.lock();
    }
    standby_since_ms_.store(now_ms);
    deep_due_.store(false);
    set_level(hal::PowerLevel::Standby);
}

void PowerManager::screen_on(std::unique_lock<os::Mutex>& lock) {
    light_sleep_off();
    if (buttons_ != nullptr) {
        core::ButtonService* buttons = buttons_;  // see transition_mutex_
        lock.unlock();
        buttons->set_standby(false);
        lock.lock();
    }
    if (power_monitor_ != nullptr) {
        core::PowerMonitor* monitor = power_monitor_;
        lock.unlock();
        monitor->set_standby(false);
        lock.lock();
    }
    if (storage_ != nullptr) {
        storage_->set_standby(false);
    }
    walk_devices(hal::PowerLevel::Active, Dir::Up);
    panel_asleep_ = false;
    if (shown_brightness_ != user_brightness_) {
        show_brightness(user_brightness_);
    }
    // Resumed only once the panel is awake, so the full redraw that
    // set_paused(false) forces lands on a lit panel.
    if (ui_ != nullptr) {
        ui_->set_paused(false);
    }
    set_level(hal::PowerLevel::Active);
}

void PowerManager::set_level(hal::PowerLevel level) {
    // Every caller moves to a different level (go_to() returns when the
    // target is the current one), so the cell's skipping of an equal set()
    // drops no power.state. The one exception is headless, where two
    // transitions can overlap while transition_mutex_ is released (see
    // there) and both reach the same level: that change is announced once.
    level_cell_.set(level);
    WLOG_I(kTag, "power: %s", hal::to_string(level));
}

std::vector<std::shared_ptr<hal::Device>> PowerManager::ordered_devices(
    Dir dir, const std::vector<std::string>* only) const {
    std::vector<std::shared_ptr<hal::Device>> others;
    std::vector<std::shared_ptr<hal::Device>> inputs;
    std::vector<std::shared_ptr<hal::Device>> outputs;
    for (auto& device : registry_.all()) {
        const hal::DeviceInfo& info = device->info();
        if (hal::standby_policy(info) == hal::StandbyPolicy::None) {
            continue;
        }
        if (only != nullptr && std::find(only->begin(), only->end(), info.id) == only->end()) {
            continue;
        }
        if (info.cls == hal::DeviceClass::AudioIn) {
            inputs.push_back(std::move(device));
        } else if (info.cls == hal::DeviceClass::AudioOut) {
            outputs.push_back(std::move(device));
        } else {
            others.push_back(std::move(device));
        }
    }
    // Down: everything else, then the audio inputs, then the outputs that
    // clock them. Up is the exact reverse.
    std::vector<std::shared_ptr<hal::Device>> order = std::move(others);
    order.insert(order.end(), inputs.begin(), inputs.end());
    order.insert(order.end(), outputs.begin(), outputs.end());
    if (dir == Dir::Up) {
        std::reverse(order.begin(), order.end());
    }
    return order;
}

bool PowerManager::power_device(hal::Device& device, hal::PowerLevel level, bool first_try) {
    auto r = device.set_power(level);
    if (r || r.error().code == ErrorCode::Unsupported) {
        return false;
    }
    const std::string& id = device.info().id;
    if (r.error().code == ErrorCode::Busy) {
        if (first_try) {
            WLOG_I(kTag, "%s busy (%s); retrying at %s", id.c_str(), r.error().message.c_str(),
                   hal::to_string(level));
        }
        return true;
    }
    WLOG_W(kTag, "%s set_power(%s) failed: %s", id.c_str(), hal::to_string(level),
           r.error().message.c_str());
    return false;
}

void PowerManager::walk_devices(hal::PowerLevel level, Dir dir) {
    device_level_ = level;
    busy_.clear();
    for (const auto& device : ordered_devices(dir, nullptr)) {
        if (power_device(*device, level, /*first_try=*/true)) {
            busy_.push_back(device->info().id);
        }
    }
    busy_pending_.store(!busy_.empty());
}

void PowerManager::retry_busy() {
    if (busy_.empty()) {
        return;
    }
    const std::vector<std::string> ids = std::move(busy_);
    busy_.clear();
    const Dir dir = screen_lit(device_level_) ? Dir::Up : Dir::Down;
    for (const auto& device : ordered_devices(dir, &ids)) {
        if (power_device(*device, device_level_, /*first_try=*/false)) {
            busy_.push_back(device->info().id);
        }
    }
    busy_pending_.store(!busy_.empty());
}

void PowerManager::update_light_sleep() {
    const bool want = wants_light_sleep(level());
    if (want == light_sleep_on_.load()) {
        return;
    }
    if (set_light_sleep_) {
        set_light_sleep_(want);
    }
    light_sleep_on_.store(want);
}

void PowerManager::light_sleep_off() {
    if (light_sleep_on_.load()) {
        if (set_light_sleep_) {
            set_light_sleep_(false);
        }
        light_sleep_on_.store(false);
    }
}

void PowerManager::set_brightness(uint8_t level) {
    if (!display_) {
        return;
    }
    if (ui_ != nullptr && !ui_->on_ui_thread()) {
        if (!off_thread_brightness_logged_.exchange(true)) {
            WLOG_E(kTag, "set_brightness(%u) off the UI thread ignored",
                   static_cast<unsigned>(level));
        }
        return;
    }
    apply_brightness(level);
}

void PowerManager::follow_brightness(int level) {
    // The first call delivers the level the constructor took (the panel's
    // since boot), and a coalesced A -> B -> A delivers A again: only a
    // level the follower has not applied yet is shown.
    if (level == followed_brightness_) {
        return;
    }
    followed_brightness_ = level;
    apply_brightness(level);
}

void PowerManager::apply_brightness(int level) {
    std::lock_guard<os::Mutex> lock(transition_mutex_);
    user_brightness_ = level;
    if (panel_asleep_) {
        return;  // applied on wake
    }
    show_brightness(level_cell_.get() == hal::PowerLevel::Dim ? dim_brightness() : level);
}

void PowerManager::show_brightness(int level) {
    if (!display_) {
        return;
    }
    auto r = display_->set_brightness(static_cast<uint8_t>(level));
    if (!r) {
        WLOG_W(kTag, "display set_brightness failed: %s", r.error().message.c_str());
        return;
    }
    shown_brightness_ = level;
}

int PowerManager::dim_brightness() const {
    return std::max(kMinDimBrightness, user_brightness_ * kDimPercent / 100);
}

void PowerManager::set_power_key_owned(bool owned) {
    power_key_owned_.store(owned, std::memory_order_release);
}

}  // namespace wisp::app
