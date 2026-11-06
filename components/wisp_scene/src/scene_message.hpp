#pragma once

// The messages the scene task takes (wisp/scene/scene_runtime.hpp). Each
// lives in PSRAM (os::make_large) and travels through the queue as a
// pointer. Every one names its scene and instance; see the header for how
// instances are matched.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "scene_api.hpp"
#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/core/scene_store.hpp"
#include "wisp/net/scene_http.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"
#include "wisp/scene/scene_runtime.hpp"
#include "wisp/scene/scene_view.hpp"
#include "wisp/scene/ui_ops.hpp"

namespace wisp::scene {

// A caller of open(), close(), reload() waiting for the scene task.
// finish() may be called more than once; the first result counts. `then`
// (close_async()) is called with that result, on the thread that finished
// it, after the waiter has been released.
struct SceneWaiter {
    os::Semaphore sem{1, 0};
    os::Mutex mu;
    bool done = false;
    Result<void> result = Result<void>::ok();
    std::function<void(Result<void>)> then;

    void finish(Result<void> r) {
        std::function<void(Result<void>)> call;
        {
            std::lock_guard<os::Mutex> lock(mu);
            if (done) return;
            done = true;
            result = r;
            call = std::move(then);
        }
        sem.give();
        if (call) call(std::move(r));
    }
};

// check_sources(): the files live in the caller's frame, so the scene task
// reads them only under `mu` and only while the caller has not given up
// (`cancelled`, set under `mu` after a timeout).
struct SourceCheckJob {
    std::span<const core::SourceFile> files;
    os::Semaphore sem{1, 0};
    os::Mutex mu;
    bool cancelled = false;
    bool done = false;
    Result<void> result = Result<void>::ok();

    void finish_locked(Result<void> r) {
        done = true;
        result = std::move(r);
        sem.give();
    }
};

namespace msg {

// launch(): the scene task asks the worker to load the scene (or, for one
// in the background, brings it forward). `done` (may be null) hears how
// the open went. `restore`: a Boot launch whose failure is a RestoreFailed
// notice (launch()). `reopen`: a Boot launch from open_background(), which
// starts the scene as it was before its last exit from the background.
struct Load {
    Value args;
    std::shared_ptr<SceneWaiter> done;
    LaunchReason reason = LaunchReason::User;
    bool restore = false;
    bool reopen = false;
};

// The worker loaded the package and the store (or failed: `error`).
struct Open {
    Value args;
    std::shared_ptr<const core::ScenePackage> package;
    std::shared_ptr<core::SceneStore> store;
    Error error;
    std::shared_ptr<SceneWaiter> done;
    LaunchReason reason = LaunchReason::User;
};

// "Back": from close() by id (instance 0), from the view's back gesture or
// the error panel for one instance. A scene that declares "background"
// goes there, any other closes.
struct Close {
    std::shared_ptr<SceneWaiter> done;
};

// exit(), close_async(): closes the scene for good, whatever its state.
struct Exit {
    std::shared_ptr<SceneWaiter> done;
};

// `if_foreground`: only reload a scene that is in the foreground; any
// other gets its state set to stopped (on_installed()).
struct Reload {
    bool if_foreground = false;
    std::shared_ptr<SceneWaiter> done;
};

struct Show {};
struct Hide {};

// A widget event from the view (never Focus).
struct Event {
    NodeId node = 0;
    UiEvent ev = UiEvent::Click;
    PropValue value;
};

struct ListRange {
    NodeId list = 0;
    uint32_t first = 0;
    uint32_t count = 0;
};

// A wisp.http result for callback `id` (SceneApi::http_submit).
struct HttpDone {
    uint32_t id = 0;
    net::SceneHttpResult result;
};
// A wisp.sensor batch for the binding's handle `handle` (empty for a
// gesture), from the service's task.
struct Sensor {
    uint32_t handle = 0;
    core::SensorBatch samples;
};
// A wisp.files (or wisp.http.download) result for callback `id`, from
// the wisp.fs worker (SceneApi::files_run).
struct FilesDone {
    uint32_t id = 0;
    FilesResult result;
};
// A completion the store bindings will send.
struct StoreDone {};

// A device event for the scenes' wisp.device handlers (sent with
// instance 0 from the event bus). It only says what to look at: the
// task reads the state itself (SceneRuntime::lang_), so a dropped one is
// made up for at the next minute.
struct Device {
    DeviceEvent kind = DeviceEvent::Battery;
};

// check_sources(): compile the files the job holds.
struct CheckSources {
    std::shared_ptr<SourceCheckJob> job;
};

// A network time sync set the wall clock: each scene whose minute ticks
// gets one "minute" with the new time and its minute ticker is re-based,
// so the jump it would also see does not fire a second one.
struct ClockSet {};

// SceneStoreDeps::schedule() added a delayed call: wakes the task so it
// takes the new deadline into account.
struct Wake {};

// restore_background(): the background list as the worker read it from
// disk (empty if there was none); each id is launched as at boot.
struct Restore {
    std::vector<std::string> ids;
};

// deliver_notification(): bring the scene forward (or launch it), then
// scene.on_notification(note_id).
struct Notify {
    std::string note_id;
};

// stop(): close every scene and end the task.
struct Quit {};

using Body =
    std::variant<Load, Open, Close, Exit, Reload, Show, Hide, Event, ListRange, HttpDone, StoreDone,
                 Device, ClockSet, CheckSources, Wake, Restore, Notify, Sensor, FilesDone, Quit>;

}  // namespace msg

struct SceneMessage {
    std::string scene_id;
    uint32_t instance = 0;
    msg::Body body;
};

// For "slow message <kind>": "load", "open", "close", ...
const char* message_kind(const msg::Body& body);

// Gives whoever waits on `m` (a waiter, a source check) `r`: the message
// will not be handled.
void fail_message(SceneMessage& m, const Result<void>& r);

// Queues `msg` without waiting. On a full queue (or a runtime that is not
// running) returns false and frees the message. Every place inside the
// runtime that sends the scene task a message (the UI thread's events,
// the worker's callbacks, later the HTTP proxy's) goes through this.
bool post_scene_message(SceneRuntime& rt, os::LargeUnique<SceneMessage> msg);

// For tests: ops in the patch queue, the foreground's instance (0 if
// none), and the instance of scene `id` in the foreground or the
// background (0 if none).
size_t pending_patches(const SceneRuntime& rt);
uint32_t foreground_instance(const SceneRuntime& rt);
uint32_t scene_instance(const SceneRuntime& rt, const std::string& id);
// For tests: UI jobs handed over and not run yet.
size_t pending_ui_jobs(SceneRuntime& rt);

}  // namespace wisp::scene
