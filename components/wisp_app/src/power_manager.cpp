#include "wisp/app/power_manager.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

#include "wisp/base/error.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

namespace {

constexpr const char* kTag = "power_manager";
constexpr uint32_t kStandbyPollMs = 50;
constexpr int kMinDimBrightness = 10;

bool screen_lit(hal::PowerLevel level) { return level < hal::PowerLevel::Standby; }

int64_t seconds_to_ms(int64_t s) { return s > 0 ? s * 1000 : 0; }

// A stored value core::SystemSettings::set() would refuse reads as the
// default, as SystemSettings::get() (and so GET /settings) reports it.
int64_t valid_or(int64_t value, bool (*valid)(int64_t), int64_t def) {
    return valid(value) ? value : def;
}

}  // namespace

PowerManager::PowerManager(PowerManagerDeps deps)
    : bus_(deps.bus),
      registry_(deps.registry),
      locks_(deps.locks),
      settings_(std::move(deps.settings)),
      display_(std::move(deps.display)),
      storage_(deps.storage),
      power_monitor_(deps.power_monitor),
      ui_(deps.ui),
      buttons_(deps.buttons),
      set_light_sleep_(std::move(deps.set_light_sleep)),
      last_activity_ms_(os::now_ms()) {
    read_settings();

    // Settings are cached; a stored change (published as system.settings,
    // spec 8.3) makes the next tick() read them again.
    settings_sub_ = bus_.subscribe("system.settings",
                                   [this](const core::Event&) { settings_dirty_.store(true); });

    if (ui_ != nullptr) {
        ui_->set_touch_activity_cb([this] { note_activity(); });
    }

    // A lock change can move the level without waiting for the next tick:
    // a network lock leaves deep standby, a cpu lock stops light sleep, a
    // screen lock ends dim.
    locks_.set_on_change([this] { schedule_lock_eval(); });

    // The wake itself is posted rather than run on the bus dispatcher:
    // waking can block for the panel's wake latency, and handlers must
    // return quickly (event_bus.hpp).
    button_sub_ = bus_.subscribe("button.", [this](const core::Event& e) {
        note_activity();
        if (!standby()) {
            return;
        }
        if (power_key_owned_.load(std::memory_order_acquire) && e.source == "button.power") {
            return;  // The owner decides what this does -- see set_power_key_owned().
        }
        run_on_ui("exit_standby", [this] { exit_standby(); });
    });
}

PowerManager::~PowerManager() {
    // After these two return neither the button handler nor the lock
    // callback runs again, so nothing new is posted. Jobs already posted
    // are drained below.
    bus_.unsubscribe(button_sub_);
    bus_.unsubscribe(settings_sub_);
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

hal::PowerLevel PowerManager::level() const { return level_.load(); }

bool PowerManager::standby() const { return !screen_lit(level_.load()); }

bool PowerManager::light_sleep_on() const { return light_sleep_on_.load(); }

bool PowerManager::light_sleep_supported() const { return static_cast<bool>(set_light_sleep_); }

void PowerManager::enter_standby() {
    std::unique_lock<os::Mutex> lock(transition_mutex_);
    if (!screen_lit(level_.load())) {
        return;  // The screen is already off.
    }
    go_to(hal::PowerLevel::Standby, now_for_eval(), lock);
}

void PowerManager::exit_standby() {
    {
        std::unique_lock<os::Mutex> lock(transition_mutex_);
        if (level_.load() == hal::PowerLevel::Active) {
            return;
        }
        go_to(hal::PowerLevel::Active, now_for_eval(), lock);
    }
    note_activity();
}

void PowerManager::note_activity() {
    last_activity_ms_.store(os::now_ms());
    if (level_.load() != hal::PowerLevel::Dim) {
        return;
    }
    run_on_ui("undim", [this] {
        std::unique_lock<os::Mutex> lock(transition_mutex_);
        if (level_.load() == hal::PowerLevel::Dim) {
            go_to(hal::PowerLevel::Active, now_for_eval(), lock);
        }
    });
}

void PowerManager::read_settings() {
    const int64_t off_s = valid_or(settings_.get_int("screen_off_s", core::kDefaultScreenOffS),
                                   core::valid_screen_off_s, core::kDefaultScreenOffS);
    const int64_t deep_s = valid_or(settings_.get_int("deep_after_s", core::kDefaultDeepAfterS),
                                    core::valid_deep_after_s, core::kDefaultDeepAfterS);
    screen_off_ms_.store(seconds_to_ms(off_s));
    deep_after_ms_.store(seconds_to_ms(deep_s));
    light_sleep_setting_.store(settings_.get_bool("light_sleep", core::kDefaultLightSleep));
}

void PowerManager::tick(uint64_t now_ms) {
    last_tick_ms_.store(now_ms);
    // Read here, on the timer task, which may touch flash; never on the
    // UI task or the bus dispatcher.
    if (settings_dirty_.exchange(false)) {
        read_settings();
    }

    // A screen lock counts as continuous activity, so the idle clock starts
    // only once the lock goes.
    if (locks_.held(core::WakeLockKind::Screen)) {
        uint64_t last = last_activity_ms_.load();
        while (last < now_ms && !last_activity_ms_.compare_exchange_weak(last, now_ms)) {
        }
    }

    // deep_due_ is refreshed even while a network lock holds the level,
    // so releasing that lock can go deep without waiting for a tick.
    const hal::PowerLevel current = level_.load();
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
    if (screen_lit(level_.load())) {
        return false;
    }
    const int64_t deep_ms = deep_after_ms_.load();
    const uint64_t since = standby_since_ms_.load();
    return deep_ms > 0 && now_ms >= since && now_ms - since >= static_cast<uint64_t>(deep_ms);
}

hal::PowerLevel PowerManager::wanted_level(std::optional<uint64_t> now_ms) const {
    const hal::PowerLevel current = level_.load();
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
    return level == hal::PowerLevel::DeepStandby && light_sleep_setting_.load() &&
           set_light_sleep_ && !locks_.held(core::WakeLockKind::Cpu);
}

void PowerManager::evaluate(std::optional<uint64_t> now_ms) {
    std::unique_lock<os::Mutex> lock(transition_mutex_);
    if (now_ms) {
        deep_due_.store(deep_due_at(*now_ms));
    }
    const hal::PowerLevel target = wanted_level(now_ms);
    if (target != level_.load()) {
        go_to(target, now_ms.value_or(now_for_eval()), lock);
    } else {
        retry_busy();  // a level change has just asked every device anyway
    }
    update_light_sleep();
}

void PowerManager::go_to(hal::PowerLevel target, uint64_t now_ms,
                         std::unique_lock<os::Mutex>& lock) {
    const hal::PowerLevel current = level_.load();
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
        saved_poll_ms_ = buttons_->poll_interval_ms();
        core::ButtonService* buttons = buttons_;
        lock.unlock();
        buttons->set_poll_interval_ms(kStandbyPollMs);
        lock.lock();
    }
    standby_since_ms_.store(now_ms);
    deep_due_.store(false);
    set_level(hal::PowerLevel::Standby);
}

void PowerManager::screen_on(std::unique_lock<os::Mutex>& lock) {
    light_sleep_off();
    if (buttons_ != nullptr) {
        uint32_t restore_ms = saved_poll_ms_;
        core::ButtonService* buttons = buttons_;  // see transition_mutex_
        lock.unlock();
        buttons->set_poll_interval_ms(restore_ms);
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
    level_.store(level);
    WLOG_I(kTag, "power: %s", hal::to_string(level));
    publish_state(level);
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
    const bool want = wants_light_sleep(level_.load());
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
        // Never retry or block here: the caller may hold its own lock (e.g.
        // core::SystemSettings::set()) that the UI task could be waiting on.
        requested_brightness_.store(level, std::memory_order_release);
        if (brightness_job_queued_.exchange(true, std::memory_order_acq_rel)) {
            return;  // the queued job will pick up the new level
        }
        bool posted = ui_->post([this] {
            // Clear the flag before reading the level: a call landing after
            // the read then queues a fresh job instead of being lost. An
            // acq_rel exchange (not a release store) keeps the level load
            // below from being reordered ahead of the flag clear.
            brightness_job_queued_.exchange(false, std::memory_order_acq_rel);
            apply_brightness(requested_brightness_.load(std::memory_order_acquire));
        });
        if (!posted) {
            brightness_job_queued_.store(false, std::memory_order_release);
            WLOG_W(kTag, "UI queue full; brightness change to %u dropped",
                   static_cast<unsigned>(level));
        }
        return;
    }
    // Record the level on this path too: a job queued earlier from another
    // thread may still be pending and must not re-apply an older level.
    requested_brightness_.store(level, std::memory_order_release);
    apply_brightness(level);
}

void PowerManager::apply_brightness(uint8_t level) {
    std::lock_guard<os::Mutex> lock(transition_mutex_);
    user_brightness_ = level;
    if (panel_asleep_) {
        return;  // applied on wake
    }
    show_brightness(level_.load() == hal::PowerLevel::Dim ? dim_brightness() : level);
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

void PowerManager::publish_state(hal::PowerLevel level) {
    core::Event e;
    e.topic = "power.state";
    e.source = "app.power";
    e.payload = wisp::Properties{{"state", wisp::Value(std::string(hal::to_string(level)))}};
    bus_.publish(std::move(e));
}

}  // namespace wisp::app
