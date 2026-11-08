#pragma once

// The scenes' half of App's boot (scene-runtime-core spec 2.1, 3, 6, 8;
// app.hpp's file header), kept apart from app.cpp so App::Impl holds one
// object for it.
//
// What it owns, in the order it builds them:
//   - the "wisp.fs" StorageWorker (start_storage(), right after
//     StorageManager::init()): file work for tasks that must not touch
//     flash, the scene task above all;
//   - the SceneRepository over <int>/scenes (or, in dev mode,
//     Platform::scenes_dir for the code and Platform::scenes_data_dir for
//     the data), whose recover() runs on the worker before anything lists
//     or opens a scene. Without an internal storage root there is none;
//   - the SceneFiles (wisp.files, scene-device spec 7.2) over <int>/scenes
//     and the SD card's library, even in dev mode (StorageManager moves
//     files only inside a storage mount). Its scan() follows recover() on
//     the worker; storage.sd.inserted/removed recount the card's scenes
//     there. Deleting a scene with its data also clears its files;
//   - with a screen (build_runtime(), after the system layer): the
//     scenes' HTTP proxy (given Platform::http_client) and the
//     SceneRuntime, built before networking so the admin routes can be
//     handed them, and started (start_runtime()) once the home screen is
//     up -- a scene's view replaces whatever screen is in front and goes
//     back to it.
//     The runtime then follows power.state: standby hides the scene, waking
//     shows it. The system layer's back gesture is the runtime's "back":
//     a scene that declares "background" goes there, any other closes.
//     The runtime's notices become system notifications (scene-device
//     spec 3.2, 3.5, 3.6). Its background list is
//     <System>/scenes/background.json, restored by restore_background()
//     once boot completes (spec 3.6).
//   - in dev mode, once the runtime runs: a DirWatcher over the working
//     directory, polled on the worker every 500 ms (it reads files); when
//     the scene in front (running or showing its error panel) changes on
//     disk, it is reloaded (spec 6.4). Errors reach the error panel and
//     the console (tag "scene.<id>") the way every scene error does. A
//     scene in the background is not reloaded (that would pull it to the
//     front): it keeps its old code until it exits.
//
// Shutdown is split because the pieces sit at different points of
// App::stop(): stop_runtime() before the shell goes (closing the scene in
// front loads the screen it replaced again), stop() after networking
// (whose handlers use the repository and the runtime) and before the
// storage-refresh timer: the proxy stops (it may wait for one request in
// flight), then the worker drains and stops.
//
// Deleting (AdminDeps::request_scene_delete): request_delete() opens the
// confirmation on the device. "Yes" closes the scene if it is up --
// close_async(), whose callback comes once its last store flush is on disk
// -- then removes the package (and the data, if asked) on the worker and
// publishes scene.state {id, "deleted"}; "no" does nothing. The steps
// after "yes" are delete_scene(), which the agent's scene.delete calls
// directly once it has asked the user itself.

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wisp/app/app.hpp"
#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/scene_files.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/net/scene_http.hpp"
#include "wisp/os/large.hpp"
#include "wisp/scene/dir_watcher.hpp"
#include "wisp/scene/scene_runtime.hpp"
#include "wisp/shell/scene_list.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

struct SceneWiringDeps {
    Platform& platform;
    core::EventBus& bus;
    core::StorageManager& storage;
};

// One installed scene as the self-check card lists it.
struct SceneEntry {
    std::string id;
    std::string name;
    std::string version;
    bool operator==(const SceneEntry&) const = default;
};

class SceneWiring {
public:
    explicit SceneWiring(SceneWiringDeps deps);
    // Calls stop().
    ~SceneWiring();

    SceneWiring(const SceneWiring&) = delete;
    SceneWiring& operator=(const SceneWiring&) = delete;

    // Starts the worker and builds the repository (see the file header).
    // Waits for recover() to finish.
    void start_storage();

    // Builds the proxy and the runtime, not started. `device_status` is
    // SceneRuntimeDeps::device_status (runs on the scene task: no flash);
    // `wake` turns a sleeping screen on for the delete confirmation (UI
    // thread). `locks` backs wisp.power; the runtime's notices (a scene
    // that failed in the background, ...) become system notifications in
    // `notices`. Either may be null; both must outlive the runtime. Needs
    // start_storage() to have made a repository; without one, or without
    // the memory for it, there is no runtime.
    void build_runtime(ui::UiRuntime& ui, ui::SystemLayer& system, const hal::ScreenShape& shape,
                       std::function<Value()> device_status, std::function<void()> wake,
                       core::WakeLockRegistry* locks = nullptr,
                       core::NotificationService* notices = nullptr,
                       core::SensorService* sensors = nullptr);
    // Starts the proxy and the runtime and follows power.state;
    // `standby` reads whether the screen is off now.
    void start_runtime(std::function<bool()> standby);
    // SceneRuntime::restore_background() once the runtime runs; App calls
    // it as it publishes system.boot.completed. Never waits.
    void restore_background();

    // Stops the runtime (closing the scene in front) and lets go of the
    // system layer, which App::stop() destroys next. Idempotent.
    void stop_runtime();
    // stop_runtime(), then the proxy, then the worker (drained). Idempotent.
    void stop();

    core::SceneRepository* repository() const { return repo_.get(); }
    // Where the scenes' data lives (SceneRepoConfig::data_root); "" without
    // a repository.
    const std::string& data_root() const { return data_root_; }
    // The scenes' files (wisp.files), built by start_storage(); nullptr
    // without internal storage. Its methods run on worker() or a thread
    // allowed flash.
    core::SceneFiles* files() const { return files_.get(); }
    // Built by build_runtime(); nullptr without a screen.
    scene::SceneRuntime* runtime() const { return runtime_.get(); }
    // The scenes' HTTP proxy, built by build_runtime(); nullptr without a
    // screen or a Platform::http_client. Stopped by stop().
    net::SceneHttp* http() const { return http_.get(); }
    // The "wisp.fs" worker, running from start_storage() until stop().
    core::StorageWorker& worker() { return worker_; }

    // The shell's scene list (ShellDeps::list_scenes): every installed
    // scene with its icon, whether it runs in the background and whether
    // it can run now (its files are not on a card that is out). On
    // worker() only. Empty without a repository or when listing fails
    // (logged).
    //
    // Kept between calls (outside dev mode, whose working directory
    // changes behind the repository's back): only what changed since the
    // last call is read again, so a listing with nothing new does no file
    // work and says `cached`. What it reads again:
    //   - the scenes SceneRepository::changed_since() names (an install,
    //     update, remove or recovered install): their scene.json and icon;
    //   - whether each scene can run, when the card went in or out
    //     (storage.sd.inserted/removed, or SceneFiles::sd_online() reading
    //     otherwise than last time). With the card out it cannot change
    //     otherwise: a scene's files keep the place their first write
    //     picked, and only deleting the scene removes them.
    // Running in the background comes from the runtime each time, never
    // from storage. An icon comes as its bytes when read for this call and
    // as its icon_hash alone after that (shell_icon() reads it again).
    shell::ShellListing shell_scenes();
    // ShellDeps::read_scene_icon: scene `id`'s icon file, or null. On
    // worker() only.
    std::shared_ptr<const os::LargeBuffer> shell_icon(const std::string& id);
    // ShellDeps::scene_has_data: whether deleting `id` with its data would
    // find any (SceneRepository::removal()). Read when asked, never kept:
    // a scene writes its first data whenever it likes. On worker() only.
    bool scene_has_data(const std::string& id);

    // AdminDeps::request_scene_delete. NotFound when there is nothing to
    // delete (SceneRepository::removal()), Unsupported in dev mode, Busy
    // while a dialog is open or once stop_runtime() has run. Not from the
    // UI thread (it waits for it).
    Result<void> request_delete(const std::string& id, bool with_data);
    // request_delete() for the UI thread, without waiting: the lookup runs
    // on the worker and the dialog opens on the UI thread afterwards. What
    // goes wrong after that (nothing to delete, a dialog already open) is
    // only logged. Unsupported as request_delete(); Busy if the worker took
    // no job. Any thread.
    Result<void> ask_delete(const std::string& id, bool with_data);

    // Deletes scene `id` without a dialog, for a caller that has already
    // asked the user (the agent's scene.delete): closes the scene if it is
    // up, removes it on the worker and publishes "deleted". `done` runs
    // once with the outcome, on the worker or the scene task (or here for
    // an error found at once): NotFound when there is nothing to delete,
    // Unsupported in dev mode. Not from the UI thread or the worker.
    void delete_scene(const std::string& id, bool with_data,
                      std::function<void(Result<void>)> done);

    // Lists the installed scenes on the worker and hands them to
    // `on_ui` on the UI thread. False if the worker took no job (or there
    // is no repository or screen); `on_ui` is then never called. A failed
    // listing is logged and not handed over.
    bool list_async(std::function<void(std::vector<SceneEntry>)> on_ui);

    // Reads StorageManager::status() on the worker and hands it to `on_ui`
    // on `ui`'s thread. The read reaches the flash -- on LittleFS it walks
    // every block in use to count them, longer the more is stored -- so it
    // stays off the UI thread. False if the worker took no job; `on_ui` is
    // then never called. An answer that finds the UI queue full is dropped.
    bool storage_status_async(ui::UiRuntime& ui, std::function<void(core::StorageStatus)> on_ui);

private:
    // request_delete()'s dialog, on the UI thread: Busy while another one
    // is open or once stop_runtime() has run.
    Result<void> open_delete_dialog(const std::string& id, const std::string& name, bool with_data);
    // delete_scene() once the name is known: from the dialog's "yes" on
    // the UI thread, so it never waits.
    void delete_named(const std::string& id, const std::string& name, bool with_data,
                      std::function<void(Result<void>)> done);
    // On the worker: removes the scene and publishes "deleted".
    Result<void> remove(const std::string& id, const std::string& name, bool with_data);
    // A delete the user confirmed did not happen: logs `why` and tells the
    // user with a system notice (the admin page already got its 202). The
    // scene stays as it is; nothing is retried behind the user's back.
    void delete_failed(const std::string& id, const std::string& name, const char* why);
    void publish(const std::string& id, const char* state);
    // Dev mode: starts the watch timer (start_runtime()).
    void start_watching();
    // On the worker: one DirWatcher poll, reloading the scene in front if
    // its files changed.
    void poll_scenes();

    // shell_scenes()'s copy of one scene; in Mem::Large with this object.
    struct ListedScene {
        os::LargeString id, name;
        os::LargeString icon;  // the manifest's icon path, or ""
        uint64_t icon_hash = 0;
        bool available = true;
    };
    // shell_scenes() helpers, on the worker: the entry for manifest `m`
    // (its icon read into `bytes`), and whether `id` can run with the card
    // as `online` says.
    ListedScene listed_scene(const core::SceneManifest& m,
                             std::shared_ptr<const os::LargeBuffer>& bytes);
    bool can_run(const os::LargeString& id, bool online);

    SceneWiringDeps deps_;
    core::StorageWorker worker_;
    // The services sit in Mem::Large, like the worker inside this object
    // (App builds it there): long-lived and small, so malloc would put them
    // in internal RAM. The worker's stack and the FreeRTOS objects stay
    // internal (scene-runtime-core spec 2.3).
    os::LargeUnique<core::SceneRepository> repo_;
    std::string data_root_;
    // wisp.files, over <int>/scenes (and the card); built with the
    // repository, counted on the worker after recover().
    os::LargeUnique<core::SceneFiles> files_;
    // storage.sd.inserted/removed, which recount the card's scenes.
    std::vector<core::SubscriptionId> sd_subs_;
    // Bumped by those events (on the bus thread); shell_scenes() then
    // asks again whether each scene can run.
    std::atomic<uint64_t> sd_changes_{0};
    // shell_scenes()'s listing, sorted by id; worker only. `listed_` is
    // false until one has been read (and after a failed read), `listed_gen_`
    // the repository generation it is up to date with, and `listed_online_`
    // / `listed_sd_` the card as its `available` flags saw it.
    os::LargeVector<ListedScene> listing_;
    bool listed_ = false;
    uint64_t listed_gen_ = 0;
    bool listed_online_ = true;
    uint64_t listed_sd_ = 0;
    // Declared before runtime_, so the runtime goes first: it cancels its
    // scene's requests through the proxy while it stops.
    os::LargeUnique<net::SceneHttp> http_;
    os::LargeUnique<scene::SceneRuntime> runtime_;
    ui::UiRuntime* ui_ = nullptr;
    // Cleared on the UI thread by stop_runtime(); read there only after.
    ui::SystemLayer* system_ = nullptr;
    // Cleared with system_: an ask_delete() dialog still on its way to the
    // UI thread then does nothing, even once this object is gone.
    std::shared_ptr<bool> ui_alive_ = std::make_shared<bool>(true);
    std::function<void()> wake_;
    // Set by build_runtime(); may be null.
    core::NotificationService* notices_ = nullptr;
    // Dev mode only. The watcher is used on the worker alone; the timer
    // only queues a poll, at most one at a time.
    os::LargeUnique<scene::DirWatcher> watcher_;
    std::unique_ptr<os::Timer> watch_timer_;
    std::atomic<bool> poll_queued_{false};
    core::SubscriptionId power_sub_ = 0;
    // Read by the delete dialog's answer on the UI thread.
    std::atomic<bool> runtime_started_{false};
};

}  // namespace wisp::app
