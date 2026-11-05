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
//   - with a screen (build_runtime(), after the system layer): the
//     scenes' HTTP proxy (given Platform::http_client) and the
//     SceneRuntime, built before networking so the admin routes can be
//     handed them, and started (start_runtime()) once the self-check
//     screen is up -- the screen a scene's view replaces and goes back to.
//     The runtime then follows power.state: standby hides the scene, waking
//     shows it.
//   - in dev mode, once the runtime runs: a DirWatcher over the working
//     directory, polled on the worker every 500 ms (it reads files); when
//     the scene in front (running or showing its error panel) changes on
//     disk, it is reloaded (spec 6.4). Errors reach the error panel and
//     the console (tag "scene.<id>") the way every scene error does.
//
// Shutdown is split because the pieces sit at different points of
// App::stop(): stop_runtime() before the self-check screen goes (closing
// the scene in front loads that screen again), stop() after networking
// (whose handlers use the repository and the runtime) and before the
// storage-refresh timer: the proxy stops (it may wait for one request in
// flight), then the worker drains and stops.
//
// Deleting (AdminDeps::request_scene_delete): request_delete() opens the
// confirmation on the device. "Yes" closes the scene if it is up --
// close_async(), whose callback comes once its last store flush is on disk
// -- then removes the package (and the data, if asked) on the worker and
// publishes scene.state {id, "deleted"}; "no" does nothing.

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
#include "wisp/core/scene_repository.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/net/scene_http.hpp"
#include "wisp/os/large.hpp"
#include "wisp/scene/dir_watcher.hpp"
#include "wisp/scene/scene_runtime.hpp"
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
    // thread). Needs start_storage() to have made a repository; without
    // one, or without the memory for it, there is no runtime.
    void build_runtime(ui::UiRuntime& ui, ui::SystemLayer& system, const hal::ScreenShape& shape,
                       std::function<Value()> device_status, std::function<void()> wake);
    // Starts the proxy and the runtime and follows power.state;
    // `standby` reads whether the screen is off now.
    void start_runtime(std::function<bool()> standby);

    // Stops the runtime (closing the scene in front) and lets go of the
    // system layer, which App::stop() destroys next. Idempotent.
    void stop_runtime();
    // stop_runtime(), then the proxy, then the worker (drained). Idempotent.
    void stop();

    core::SceneRepository* repository() const { return repo_.get(); }
    // Built by build_runtime(); nullptr without a screen.
    scene::SceneRuntime* runtime() const { return runtime_.get(); }

    // AdminDeps::request_scene_delete. NotFound when there is nothing to
    // delete (SceneRepository::removal()), Unsupported in dev mode, Busy
    // while a dialog is open or once stop_runtime() has run. Not from the
    // UI thread (it waits for it).
    Result<void> request_delete(const std::string& id, bool with_data);

    // Lists the installed scenes on the worker and hands them to
    // `on_ui` on the UI thread. False if the worker took no job (or there
    // is no repository or screen); `on_ui` is then never called. A failed
    // listing is logged and not handed over.
    bool list_async(std::function<void(std::vector<SceneEntry>)> on_ui);

private:
    // On the worker: removes the scene and publishes "deleted".
    void remove(const std::string& id, bool with_data);
    void publish(const std::string& id, const char* state);
    // Dev mode: starts the watch timer (start_runtime()).
    void start_watching();
    // On the worker: one DirWatcher poll, reloading the scene in front if
    // its files changed.
    void poll_scenes();

    SceneWiringDeps deps_;
    core::StorageWorker worker_;
    // The services sit in Mem::Large, like the worker inside this object
    // (App builds it there): long-lived and small, so malloc would put them
    // in internal RAM. The worker's stack and the FreeRTOS objects stay
    // internal (scene-runtime-core spec 2.3).
    os::LargeUnique<core::SceneRepository> repo_;
    // Declared before runtime_, so the runtime goes first: it cancels its
    // scene's requests through the proxy while it stops.
    os::LargeUnique<net::SceneHttp> http_;
    os::LargeUnique<scene::SceneRuntime> runtime_;
    ui::UiRuntime* ui_ = nullptr;
    // Cleared on the UI thread by stop_runtime(); read there only after.
    ui::SystemLayer* system_ = nullptr;
    std::function<void()> wake_;
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
