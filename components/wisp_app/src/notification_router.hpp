#pragma once

// NotificationRouter (scene-device spec 5.1, 5.3, 5.4): what happens when
// a notification is posted, tapped or acted on. It only wires services
// together; each step is a call into one of them.
//
// Posted (the NotificationService listener, on the posting thread -- the
// scene task for wisp.notify, which must not touch flash or wait):
//   - wake, or sound "alarm", and the screen is off: PowerManager::
//     exit_standby() on the UI task (posted; called directly without a
//     UiRuntime). An alarm rings until the user touches the device, so it
//     always wakes it;
//   - a sound: TonePlayer::play() at notify_volume(). Audio is off below
//     Dim (hal/audio.hpp), so with the screen off a sound plays only for a
//     notification that wakes it, right after the wake on the UI task: a
//     "tone" without wake is dropped;
//   - the banner, if one is installed.
// Dismissed: a sound still playing for that notification stops.
//
// tap() and act() come from the system layer (banner, panel) on the UI
// task; neither waits.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "wisp/app/power_manager.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/reminders.hpp"
#include "wisp/core/tone_player.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/os/os.hpp"
#include "wisp/scene/scene_runtime.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

struct NotificationRouterDeps {
    core::NotificationService& notes;
    // Each of these may be null: its step is skipped.
    PowerManager* power = nullptr;
    core::TonePlayer* tones = nullptr;
    scene::SceneRuntime* scenes = nullptr;
    core::WakeLockRegistry* locks = nullptr;
    // audio/notify_vol, 0-100. Called on the posting thread, so it returns
    // a cached value and never reads Settings. Empty: 60.
    std::function<uint8_t()> notify_volume;
    // Where the wake runs (PowerManager's transitions belong to the UI
    // task). Null: on the posting thread.
    ui::UiRuntime* ui = nullptr;
    // A reminder's actions (mvp spec 5.2) go here, on the storage worker
    // through `submit_io` (they write the todo file). Either empty: they
    // only dismiss the notice.
    core::ReminderScheduler* reminders = nullptr;
    std::function<bool(std::function<void()>)> submit_io;
};

class NotificationRouter {
public:
    explicit NotificationRouter(NotificationRouterDeps deps);
    ~NotificationRouter();  // stop()

    NotificationRouter(const NotificationRouter&) = delete;
    NotificationRouter& operator=(const NotificationRouter&) = delete;

    // Becomes the NotificationService's listener.
    void start();
    // Stops listening and waits for the UI jobs it posted (not on the UI
    // task, which cannot wait for its own queue). Idempotent.
    void stop();

    // The user tapped it: dismiss(Tap), silence, and for a scene
    // SceneRuntime::deliver_notification(); a scene that cannot be
    // launched only loses the notification. A reminder opens nothing.
    void tap(const std::string& source, const std::string& id);
    // The notification's action button `index` (0: action, 1: action2):
    // release_lock releases the wake lock it names
    // (WakeLockRegistry::release); a reminder's todo_done and todo_snooze
    // queue ReminderScheduler::done() / snooze() on the worker and silence
    // the alarm. Either way the notice goes.
    void act(const std::string& source, const std::string& id, int index = 0);
    // Shows a posted notification (the banner). Any thread.
    void set_banner(std::function<void(const core::Notification&)> show);

private:
    void on_change(const core::NotificationChange& c);
    void posted(const core::Notification& n);
    void act_reminder(const core::Notification& n, int index);
    uint8_t volume() const;

    NotificationRouterDeps deps_;
    bool started_ = false;

    // Guards banner_ and sounding_*.
    mutable os::Mutex mu_;
    std::shared_ptr<const std::function<void(const core::Notification&)>> banner_;
    // The notification whose sound was played last.
    std::string sounding_source_, sounding_id_;
};

}  // namespace wisp::app
