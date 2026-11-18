#pragma once

// StatusShell (scene-device spec 5.3, 5.5, 5.6): feeds the system layer's
// pull-down panel and banners from the scene runtime and
// the notification services. It only wires services together; what each
// row or banner does is a call into one of them.
//
//   - Counts: the shell follows four state cells on the UI thread (the
//     scenes overview, the notice summary, the power level and the Wi-Fi
//     station). Each call refreshes: it re-reads the services, so one call
//     that stands for several changes still shows the last, and rebuilds
//     the panel's model while it is open. A change of counts is logged
//     "status: background=<n> unread=<m>". The power level at Standby or
//     DeepStandby closes the panel instead, also when the shell starts in
//     standby (a cell's first call delivers its current value).
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
//     NotificationRouter::tap(). A listed notice's banner follows the
//     notice summary cell like the panel: each refresh takes down the
//     banners (shown or waiting) whose notice has gone, and one whose
//     notice went before it reached the UI thread never shows. A banner-
//     only post (a foreground scene's own) is not in the list and only
//     goes on its own. The pull-down panel coming down takes every banner
//     away (SystemLayer::show_banner()).
//
// start() runs on any thread. Every SystemLayer call is made on the UI
// thread.

#include <cstdint>
#include <functional>
#include <memory>

#include "notification_router.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/core/state_cell.hpp"
#include "wisp/hal/device.hpp"
#include "wisp/net/wifi_snapshot.hpp"
#include "wisp/scene/scene_runtime.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

struct StatusShellDeps {
    ui::UiRuntime& ui;
    ui::SystemLayer& layer;
    // Its summary() is followed.
    core::NotificationService& notes;
    NotificationRouter& router;
    // Null: no scenes (none counted, no scene rows). Its overview() is
    // followed.
    scene::SceneRuntime* scenes = nullptr;
    // The same as SceneRuntimeDeps::device_status: {"battery": {"percent",
    // "charging"} or nil, "wifi": WifiState name, "wifi_enabled": bool}.
    // A disabled radio hides the Wi-Fi item. Called on the UI thread,
    // so it must not touch flash. Empty: no battery or Wi-Fi item.
    std::function<Value()> device_status;
    // The time zone, minutes east of UTC. Called on the UI thread, so it
    // reads memory, never Settings. Empty: the setting's default.
    std::function<int()> utc_offset_min;
    // Whether the time shows as a 24-hour clock (sys/time_24h), read the
    // same way. Empty: the setting's default.
    std::function<bool()> time_24h;
    // Seconds since the epoch; 0 (or empty) while the clock is unset.
    std::function<int64_t()> wall_s;
    // Where the name of a scene that is not running is read (its
    // manifest), and how that read gets off the UI thread: a job on the
    // storage worker. Either null or empty: such a scene reads as its id.
    core::SceneRepository* repo = nullptr;
    std::function<bool(std::function<void()>)> submit_io;
    // The power level (PowerManager::level_cell()) and the Wi-Fi station
    // (WifiService::wifi_cell()), followed; each must outlive the shell's
    // stop(). Null: not followed (no standby closing; no Wi-Fi refresh).
    const core::StateCell<hal::PowerLevel>* power = nullptr;
    const core::StateCell<net::WifiSnapshot>* wifi = nullptr;
};

class StatusShell {
public:
    explicit StatusShell(StatusShellDeps deps);
    ~StatusShell();  // stop()

    StatusShell(const StatusShell&) = delete;
    StatusShell& operator=(const StatusShell&) = delete;

    // Subscribes to its cells on the UI thread, installs the panel handlers
    // and the banner, and shows the counts there are now.
    void start();
    // Undoes start(), so the system layer, the router and the cells' owners
    // may go next: no cell callback runs once it has returned (off the UI
    // thread, releasing waits for one under way), the panel handlers are
    // removed on the UI thread, and a job posted
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
    // The scenes overview, the notice summary, the power level, the Wi-Fi
    // station.
    core::CellSubscription subs_[4];
};

}  // namespace wisp::app
