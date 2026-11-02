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
                           std::shared_ptr<hal::Touch> touch, ui::UiRuntime* ui,
                           core::ButtonService* buttons, std::function<void(bool)> set_standby,
                           core::Settings settings)
    : bus_(bus),
      display_(std::move(display)),
      touch_(std::move(touch)),
      ui_(ui),
      buttons_(buttons),
      set_standby_(std::move(set_standby)),
      settings_(std::move(settings)),
      last_activity_ms_(os::now_ms()) {
    if (ui_ != nullptr && touch_ != nullptr) {
        ui_->set_touch_activity_cb([this] { note_activity(); });
    }

    // Any button event -- not just "button.power" -- resets the idle
    // clock, and wakes the device if it's currently in standby (spec
    // 6.7/8's "待机中按任意键都会唤醒"), independently of whatever
    // button-specific handling (e.g. the self-check screen's
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
    button_sub_ = bus_.subscribe("button.", [this](const core::Event&) {
        note_activity();
        if (!standby_.load(std::memory_order_acquire)) {
            return;
        }
        if (ui_ != nullptr) {
            ui_->post([this] { exit_standby(); });
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
            //      the UI task*, not merely be called from here (fix
            //      round 1 -- see task-18b-report.md): indev_read_cb()
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
    std::lock_guard<os::Mutex> lock(transition_mutex_);
    if (standby_.load(std::memory_order_acquire)) {
        return;  // Already in standby -- idempotent, no device calls repeated.
    }

    if (display_) {
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
    if (ui_ != nullptr) {
        ui_->set_paused(true);
    }
    if (buttons_ != nullptr) {
        saved_poll_ms_ = buttons_->poll_interval_ms();
        buttons_->set_poll_interval_ms(kStandbyPollMs);
    }
    if (set_standby_) {
        set_standby_(true);
    }

    standby_.store(true, std::memory_order_release);
    publish_state();
}

void PowerManager::exit_standby() {
    std::lock_guard<os::Mutex> lock(transition_mutex_);
    if (!standby_.load(std::memory_order_acquire)) {
        return;  // Already active -- idempotent.
    }

    if (set_standby_) {
        set_standby_(false);
    }
    if (buttons_ != nullptr) {
        buttons_->set_poll_interval_ms(saved_poll_ms_);
    }
    if (ui_ != nullptr) {
        ui_->set_paused(false);
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
    }

    standby_.store(false, std::memory_order_release);
    publish_state();
    note_activity();
}

bool PowerManager::standby() const { return standby_.load(std::memory_order_acquire); }

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
        enter_standby();
    }
}

void PowerManager::publish_state() {
    core::Event e;
    e.topic = "power/state";
    e.source = "app.power";
    e.payload = wisp::Properties{
        {"state",
         wisp::Value(std::string(standby_.load(std::memory_order_acquire) ? "standby" : "active"))},
    };
    bus_.publish(std::move(e));
}

}  // namespace wisp::app
