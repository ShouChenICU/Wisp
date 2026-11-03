#include "wisp/app/power_manager.hpp"

#include <mutex>
#include <utility>

#include "wisp/base/error.hpp"
#include "wisp/core/log.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

namespace {

constexpr const char* kTag = "power_manager";
constexpr uint32_t kStandbyPollMs = 50;

}  // namespace

PowerManager::PowerManager(core::EventBus& bus, std::shared_ptr<hal::Display> display,
                           std::shared_ptr<hal::Touch> touch, std::shared_ptr<hal::Imu> imu,
                           core::StorageManager* storage, core::PowerMonitor* power_monitor,
                           ui::UiRuntime* ui, core::ButtonService* buttons,
                           std::function<void(bool)> set_standby, core::Settings settings)
    : bus_(bus),
      display_(std::move(display)),
      touch_(std::move(touch)),
      imu_(std::move(imu)),
      storage_(storage),
      power_monitor_(power_monitor),
      ui_(ui),
      buttons_(buttons),
      set_standby_(std::move(set_standby)),
      settings_(std::move(settings)),
      last_activity_ms_(os::now_ms()) {
    if (ui_ != nullptr && touch_ != nullptr) {
        ui_->set_touch_activity_cb([this] { note_activity(); });
    }

    // Any button event -- not just "button.power" -- resets the idle
    // clock, and (unless set_power_key_owned(true) says otherwise for
    // "button.power" specifically -- see that method's doc comment)
    // wakes the device if it's currently in
    // standby (spec 6.7/8's "待机中按任意键都会唤醒"), independently of
    // whatever button-specific handling (e.g. the self-check screen's
    // PowerKeyController, which drives its own enter_standby()/
    // exit_standby() calls for "button.power" specifically) also runs
    // off the same events.
    //
    // The actual exit_standby() call is deferred onto the UI task via
    // ui_->post() when one exists, rather than run directly here on the
    // EventBus dispatcher thread: exit_standby() can block for the
    // display panel's wake latency (on real hardware, on the order of
    // 100ms), and EventBus's contract asks handlers to return quickly
    // (see event_bus.hpp) so as not to delay delivery of other events
    // queued behind this one. In headless mode (no UiRuntime) there is
    // no UI task to post onto, so it just runs here directly -- fine,
    // since a headless exit_standby() never touches any lv_* call and
    // has materially less to do (no display either, typically).
    button_sub_ = bus_.subscribe("button.", [this](const core::Event& e) {
        note_activity();
        if (!standby_.load(std::memory_order_acquire)) {
            return;
        }
        if (power_key_owned_.load(std::memory_order_acquire) && e.source == "button.power") {
            return;  // The owner's own logic decides what this does -- see set_power_key_owned().
        }
        if (ui_ != nullptr) {
            // A button-driven wake must not just get silently dropped if
            // the post queue happens to be momentarily full -- log it so
            // a "device didn't wake up" report has something to point
            // at (see event_bus.hpp: the caller still must not block
            // here waiting for a slot, since that would delay every
            // other event queued behind this one).
            if (!ui_->post([this] { exit_standby(); })) {
                WLOG_W(kTag, "post(exit_standby) dropped (post queue full)");
            }
        } else {
            exit_standby();
        }
    });
}

PowerManager::~PowerManager() {
    // Once this returns, the "button." handler above is guaranteed never
    // to run again (EventBus::unsubscribe()'s own contract) -- so no new
    // ui_->post([this] { exit_standby(); }) can be enqueued after this
    // point. One may already be sitting in UiRuntime's post queue,
    // though (enqueued by a dispatch that was in flight, or already
    // finished, before this call) -- see below.
    bus_.unsubscribe(button_sub_);

    if (ui_ != nullptr) {
        // Both remaining hazards below only matter relative to work that
        // runs *on the UI task*, so this is a no-op (and must be, to
        // avoid a self-join-style deadlock) when already called from
        // there -- not expected in practice (PowerManager is torn down
        // from App::stop(), never from inside a ui()->post()'d
        // callback), but guarded the same way UiRuntime::stop() guards
        // against a self-join.
        if (ui_->on_ui_thread()) {
            if (touch_ != nullptr) {
                ui_->set_touch_activity_cb(nullptr);
            }
        } else {
            // Blocks until a job posted *now* has actually run on the UI
            // task. Two hazards this closes, both only safe to close
            // this way (by running the fix itself on the UI task, not
            // by doing it here and merely waiting for *something*):
            //
            //   1. The "button." handler above may have already posted
            //      a `[this] { exit_standby(); }` job before
            //      unsubscribe() (above) returned. UiRuntime's post
            //      queue is FIFO, so that job -- if present -- is
            //      guaranteed to run strictly before this one.
            //   2. set_touch_activity_cb(nullptr) must itself run *on
            //      the UI task*, not merely be called from here:
            //      indev_read_cb()
            //      copies the installed callback under a lock and calls
            //      the copy *after releasing it*, so simply calling
            //      set_touch_activity_cb(nullptr) from this (non-UI)
            //      thread cannot prevent an indev_read_cb() invocation
            //      that already copied the *old* (capturing `this`)
            //      callback from then calling it after `this` is freed.
            //      Doing the clear from inside a job actually running on
            //      the UI task instead sequences it, by thread affinity
            //      alone, against every indev_read_cb() call -- they
            //      can never be concurrent, since both run on that one
            //      task.
            auto done = std::make_shared<os::Semaphore>(1, 0);
            while (!ui_->post([this, done] {
                if (touch_ != nullptr) {
                    ui_->set_touch_activity_cb(nullptr);
                }
                done->give();
            })) {
                os::sleep_ms(1);
            }
            done->take(os::kForever);
        }
    }
}

void PowerManager::enter_standby() {
    std::unique_lock<os::Mutex> lock(transition_mutex_);
    if (standby_.load(std::memory_order_acquire)) {
        return;  // Already in standby -- idempotent, no device calls repeated.
    }

    // Pause the UI *before* sending the panel to
    // sleep -- see the header's doc comment on why ordering only
    // matters for self-documentation purposes in the configuration this
    // actually ships in (ui_ != nullptr), not for correctness.
    if (ui_ != nullptr) {
        ui_->set_paused(true);
    }
    if (display_) {
        panel_asleep_ = true;
        auto r = display_->set_power(false);
        if (!r) {
            WLOG_W(kTag, "display set_power(false) failed: %s", r.error().message.c_str());
        }
    }
    if (touch_) {
        auto r = touch_->set_power(false);
        if (!r && r.error().code != ErrorCode::Unsupported) {
            WLOG_W(kTag, "touch set_power(false) failed: %s", r.error().message.c_str());
        }
    }
    if (imu_) {
        // Software power-down stopgap: same ErrorCode::
        // Unsupported carve-out as touch_ above -- not every IMU driver
        // can power its sensors down independently of the rest of the
        // chip.
        auto r = imu_->set_power(false);
        if (!r && r.error().code != ErrorCode::Unsupported) {
            WLOG_W(kTag, "imu set_power(false) failed: %s", r.error().message.c_str());
        }
    }
    if (storage_ != nullptr) {
        // A pure software hint (see
        // hal::Storage::set_standby()'s doc comment) -- never fails, so
        // there is nothing to log here.
        storage_->set_standby(true);
    }
    if (power_monitor_ != nullptr) {
        // Like buttons_->set_poll_interval_ms() below,
        // PowerMonitor::set_standby() swaps an os::Timer for a new one,
        // which can block waiting for the shared esp_timer task's fence
        // -- release transition_mutex_ for this one call, same reasoning
        // as the button one just below (see transition_mutex_'s own doc
        // comment). `power_monitor_` is never reassigned after
        // construction, so reading it before unlocking is safe without
        // the lock.
        core::PowerMonitor* monitor = power_monitor_;
        lock.unlock();
        monitor->set_standby(true);
        lock.lock();
    }
    if (buttons_ != nullptr) {
        saved_poll_ms_ = buttons_->poll_interval_ms();
        // Release transition_mutex_ for this one call.
        // ButtonService::set_poll_interval_ms() stops/recreates a Timer;
        // off the esp_timer task, that blocks waiting for the esp_timer
        // task to run a fence callback for it -- see transition_mutex_'s
        // own doc comment for why holding this lock across that wait is
        // never safe. `buttons_` itself is never reassigned after
        // construction, so reading it before unlocking and calling
        // through the raw pointer afterwards is safe without the lock.
        core::ButtonService* buttons = buttons_;
        lock.unlock();
        buttons->set_poll_interval_ms(kStandbyPollMs);
        lock.lock();
    }
    if (set_standby_) {
        set_standby_(true);
    }

    standby_.store(true, std::memory_order_release);
    publish_state();
}

void PowerManager::exit_standby() {
    std::unique_lock<os::Mutex> lock(transition_mutex_);
    if (!standby_.load(std::memory_order_acquire)) {
        return;  // Already active -- idempotent.
    }

    if (set_standby_) {
        set_standby_(false);
    }
    if (buttons_ != nullptr) {
        uint32_t restore_ms = saved_poll_ms_;
        core::ButtonService* buttons = buttons_;  // see enter_standby()'s identical unlock/lock.
        lock.unlock();
        buttons->set_poll_interval_ms(restore_ms);
        lock.lock();
    }
    if (power_monitor_ != nullptr) {
        // See enter_standby()'s identical unlock/lock around
        // the matching call.
        core::PowerMonitor* monitor = power_monitor_;
        lock.unlock();
        monitor->set_standby(false);
        lock.lock();
    }
    if (storage_ != nullptr) {
        storage_->set_standby(false);
    }
    if (imu_) {
        auto r = imu_->set_power(true);
        if (!r && r.error().code != ErrorCode::Unsupported) {
            WLOG_W(kTag, "imu set_power(true) failed: %s", r.error().message.c_str());
        }
    }
    if (touch_) {
        auto r = touch_->set_power(true);
        if (!r && r.error().code != ErrorCode::Unsupported) {
            WLOG_W(kTag, "touch set_power(true) failed: %s", r.error().message.c_str());
        }
    }
    if (display_) {
        auto r = display_->set_power(true);
        if (!r) {
            WLOG_W(kTag, "display set_power(true) failed: %s", r.error().message.c_str());
        }
        panel_asleep_ = false;
        if (pending_brightness_ >= 0) {
            auto b = display_->set_brightness(static_cast<uint8_t>(pending_brightness_));
            if (!b) {
                WLOG_W(kTag, "display set_brightness failed: %s", b.error().message.c_str());
            }
            pending_brightness_ = -1;
        }
    }
    // Resume the UI only after the panel is already
    // awake again (the mirror image of enter_standby() pausing it
    // first), so the full redraw set_paused(false) forces lands against
    // an already-woken panel.
    if (ui_ != nullptr) {
        ui_->set_paused(false);
    }

    standby_.store(false, std::memory_order_release);
    publish_state();
    note_activity();
}

bool PowerManager::standby() const { return standby_.load(std::memory_order_acquire); }

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
            // the read then queues a fresh job instead of being lost.
            brightness_job_queued_.store(false, std::memory_order_release);
            apply_brightness(requested_brightness_.load(std::memory_order_acquire));
        });
        if (!posted) {
            brightness_job_queued_.store(false, std::memory_order_release);
            WLOG_W(kTag, "UI queue full; brightness change to %u dropped",
                   static_cast<unsigned>(level));
        }
        return;
    }
    apply_brightness(level);
}

void PowerManager::apply_brightness(uint8_t level) {
    std::lock_guard<os::Mutex> lock(transition_mutex_);
    if (panel_asleep_) {
        pending_brightness_ = level;
        return;
    }
    auto r = display_->set_brightness(level);
    if (!r) {
        WLOG_W(kTag, "display set_brightness failed: %s", r.error().message.c_str());
    }
}

void PowerManager::note_activity() {
    last_activity_ms_.store(os::now_ms(), std::memory_order_relaxed);
}

void PowerManager::tick(uint64_t now_ms) {
    if (standby_.load(std::memory_order_acquire)) {
        return;
    }
    int64_t screen_off_s = settings_.get_int("screen_off_s", 60);
    if (screen_off_s <= 0) {
        return;  // 0 (or negative, treated the same) disables auto screen-off.
    }
    uint64_t last = last_activity_ms_.load(std::memory_order_relaxed);
    uint64_t idle_ms = now_ms > last ? now_ms - last : 0;
    if (idle_ms >= static_cast<uint64_t>(screen_off_s) * 1000) {
        // Never call enter_standby() directly from
        // here -- see tick()'s own doc comment in the header for why.
        if (ui_ != nullptr) {
            // Recheck idle_ms, against the same
            // now_ms, once the posted job actually runs -- see
            // enter_standby_if_still_idle()'s own doc comment.
            ui_->post([this, now_ms] { enter_standby_if_still_idle(now_ms); });
        } else {
            enter_standby();
        }
    }
}

void PowerManager::enter_standby_if_still_idle(uint64_t now_ms) {
    int64_t screen_off_s = settings_.get_int("screen_off_s", 60);
    if (screen_off_s <= 0) {
        return;
    }
    uint64_t last = last_activity_ms_.load(std::memory_order_relaxed);
    uint64_t idle_ms = now_ms > last ? now_ms - last : 0;
    if (idle_ms >= static_cast<uint64_t>(screen_off_s) * 1000) {
        enter_standby();
    }
}

void PowerManager::set_power_key_owned(bool owned) {
    power_key_owned_.store(owned, std::memory_order_release);
}

void PowerManager::publish_state() {
    core::Event e;
    e.topic = "power.state";
    e.source = "app.power";
    e.payload = wisp::Properties{
        {"state",
         wisp::Value(std::string(standby_.load(std::memory_order_acquire) ? "standby" : "active"))},
    };
    bus_.publish(std::move(e));
}

}  // namespace wisp::app
