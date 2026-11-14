#pragma once

// StatusShell (scene-device spec 5.3, 5.5, 5.6): feeds the system layer's
// pull-down panel and banners from the scene runtime and
// the notification services. It only wires services together; what each
// row or banner does is a call into one of them.
//
//   - Counts: scene.state, notification.* and power.state events (on the
//     bus's dispatcher thread) queue one refresh on the UI thread, at most
//     one at a time, which rebuilds the panel's model while it is open. A
//     change of counts is logged "status: background=<n> unread=<m>".
//   - Panel: the model is built when it opens and on every refresh while
//     it is open. Its header is the local time (ui::format_clock() in the
//     Settings time format, "--:--" while the clock is unset), the battery and the Wi-Fi state from
//     device_status; then the background scenes and the unread notifications, newest first. A
//     system notice reads in the UI language: its argument goes in as posted (a scene notice
//     carries the scene's name already), a wake-lock holder through ui::holder_label():
//     "scene.<id>" by the scene's name -- the runtime's while it runs, else its manifest's, read on
//     the storage worker and kept -- and by its id only while that is unknown or the scene is not
//     installed. Opening and closing are logged "panel: open" / "panel: closed"; a top swipe
//     opens it (and fetches the model) as soon as the panel starts following the finger, and one
//     that slides back up closes it again (SystemLayer logs "ui: panel: shown" once it rests
//     fully open). on_open only fetches the model, so nothing is undone when it slides back.
//     The screen going off closes it at once.
//   - Rows: a scene row brings the scene forward (launch(User)), its close
//     button exits it; a notification row is NotificationRouter::tap(), its
//     close button or a swipe dismisses it, its action button is
//     NotificationRouter::act(); "clear all" clears every one.
//   - Banners: NotificationRouter::set_banner() hands each posted
//     notification over; it is shown on the UI thread, and a tap on it is
//     NotificationRouter::tap().
//
// start() runs on any thread. Every SystemLayer call is made on the UI
// thread.

#include <cstdint>
#include <functional>
#include <memory>

#include "notification_router.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/scene/scene_runtime.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

struct StatusShellDeps {
    ui::UiRuntime& ui;
    ui::SystemLayer& layer;
    core::EventBus& bus;
    core::NotificationService& notes;
    NotificationRouter& router;
    // Null: no scenes (none counted, no scene rows).
    scene::SceneRuntime* scenes = nullptr;
    // The same as SceneRuntimeDeps::device_status: {"battery": {"percent",
    // "charging"} or nil, "wifi": WifiState name, "wifi_enabled": bool}.
    // A disabled radio hides the Wi-Fi item. Called on the UI thread,
    // so it must not touch flash. Empty: no battery or Wi-Fi item.
    std::function<Value()> device_status;
    // The time zone, minutes east of UTC. Called on the UI thread, so it
    // returns a cached value. Empty: UTC+8.
    std::function<int()> utc_offset_min;
    // Whether the time shows as a 24-hour clock (sys/time_24h), cached the
    // same way. Empty: 24-hour.
    std::function<bool()> time_24h;
    // Seconds since the epoch; 0 (or empty) while the clock is unset.
    std::function<int64_t()> wall_s;
    // Where the name of a scene that is not running is read (its
    // manifest), and how that read gets off the UI thread: a job on the
    // storage worker. Either null or empty: such a scene reads as its id.
    core::SceneRepository* repo = nullptr;
    std::function<bool(std::function<void()>)> submit_io;
};

class StatusShell {
public:
    explicit StatusShell(StatusShellDeps deps);
    ~StatusShell();  // stop()

    StatusShell(const StatusShell&) = delete;
    StatusShell& operator=(const StatusShell&) = delete;

    // Subscribes to scene.state, notification.* and power.state, installs
    // the panel handlers and the banner, and shows the counts there are
    // now.
    void start();
    // Undoes start(), so the system layer and the router may go next: the
    // panel handlers are removed on the UI thread, and a job posted
    // earlier, a manifest read or a banner call still under way, or a tap
    // on a banner still up, does nothing once stop() has returned. Off the
    // UI thread it also waits for the UI jobs posted before it; on the UI
    // thread it cannot, and those do nothing when they run. Idempotent.
    void stop();

    // What the panel would show now. UI thread.
    ui::PanelModel panel_model() const;

    // The State is shared with the jobs posted to the UI thread and with
    // the banner callback, which may still run once stop() has returned.
    struct State;

private:
    std::shared_ptr<State> state_;
    bool started_ = false;
    // scene.state, notification.*, power.state.
    core::SubscriptionId subs_[4] = {};
};

}  // namespace wisp::app
