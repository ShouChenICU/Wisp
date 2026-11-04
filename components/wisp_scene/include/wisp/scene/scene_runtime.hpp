#pragma once

// SceneRuntime: the scene task and the foreground scene's lifecycle
// (scene-runtime-core spec 3.1-3.6). It implements core::SceneHost.
//
// Threads:
//   - The scene task ("wisp.scene") owns the scene: its Vm, its
//     PatchBatch, its SceneStore handle. It takes one message at a time
//     from a queue of SceneMessage pointers (bodies in PSRAM) and runs it
//     in the VM; after each message it flushes the reactive state and
//     submits the batch to the PatchQueue.
//   - The UI thread owns the SceneView. Its frame hook applies the
//     PatchQueue to the view of the instance the scene task runs; its
//     widget events become messages. It never waits for the scene task,
//     and the scene task never waits for it (it only post()s).
//   - The wisp.fs worker reads the package and loads the SceneStore, then
//     posts the loaded scene to the scene task. The scene task touches no
//     flash itself (task_config(): flash_access = false): SceneStore
//     writes go to its in-memory index, and its flushes, like every file
//     operation, are jobs on the worker.
//
// Instances: every open gets a new instance number. Messages from the
// view, the worker and the store carry it, and one whose instance is not
// the foreground scene's is dropped (stale_dropped()), so nothing from a
// closed scene reaches the next one. Requests from the SceneHost methods
// carry instance 0 and name the scene by id.
//
// Lifecycle (spec 3.4):
//   open    the worker loads the package and the store; the scene task
//           then closes the foreground scene (the same scene closes before
//           the load instead, so its final store flush is queued on the
//           worker ahead of it and the reopened store reads what it
//           wrote; a scene that fails to load leaves another one running),
//           makes the VM (prelude, ui, a `scene` table for the hooks),
//           runs the entry file and builds its tree; the UI
//           thread makes the SceneView, gives it the package's assets and
//           loads its screen, remembering the screen it replaced (the
//           self-check screen, hidden, never rebuilt); then
//           scene.on_show() if the screen is on.
//   hide    set_visible(false): scene.on_hide(), and the store flushes.
//   show    set_visible(true): scene.on_show().
//   close   scene.on_hide() (if it was shown), scene.on_close(); each has
//           its own 200 ms and an error or timeout is logged, not shown.
//           Then SceneStore::flush() and close(), the VM goes, and the UI
//           thread loads the remembered screen and deletes the view.
//   reload  close, then open with the same arguments; data stays.
// Timers and device events (spec 3.4, 5.1, 5.2): the task's wait is
// bounded by the scene's next TimerSet deadline and, while the screen is
// on and the scene listens for "minute", "battery" or "lang", its
// MinuteTicker;
// each due timer callback, each event, is a message of its own (own 200
// ms budget, flush, submit). Screen off: timers pause, the ticker is not
// polled. Screen on: timers resume (each missed one fires once), a
// "minute" is forced, then on_show. "battery" fires when the percentage
// or charging differ from what the scene last saw, checked on each
// power.charging.changed and each minute; "lang" when the language of
// the last system.settings event differs from the one the scene last
// saw, checked on each such event and each minute. info().lang
// is the language of the last system.settings event, ui::lang() before
// any (ui::set_lang() runs on the UI thread and may lag the event).
// wisp.scene.close() closes the scene once its current message is done.
//
// HTTP (spec 5.3): opening a scene gives the proxy (SceneRuntimeDeps::
// http) its declared hosts; each wisp.http result is an HttpDone message
// whose callback runs with its own 200 ms budget. Closing, reloading or
// faulting cancels the scene's requests (SceneHttp::cancel_scene): queued
// ones are dropped, the one in flight is stopped, and no callback runs
// for them; a result already queued for the old instance is dropped as
// stale.
//
// A hook is a function the scene stores in its `scene` table
// (scene.on_show = function() ... end).
//
// Errors (spec 3.3, 3.6): an error that reaches the top of a message
// (compile, runtime, timeout, memory, a bad tree, a bad bound value)
// drops the message's patches and everything queued, destroys the VM and
// closes the store; the state becomes Error with ScriptError::text(),
// logged with tag "scene.<id>", and the system layer's error panel comes
// up over the scene's last frame. "重载" reloads it, "返回" (or the back
// gesture) goes back to the remembered screen; the state stays Error
// until the scene is opened again. Nothing is retried automatically.
//
// Each message is timed: one over 50 ms logs "slow message <kind> <ms>
// ms" (tag "scene.<id>"). After a batch is submitted, a refresh the patch
// queue asks for (take_refresh_request()) is answered at once with a
// UiBatch::refresh batch from __wisp.refresh_all().
//
// Lifetime: start() after the UiRuntime and the StorageWorker have
// started; stop() (or the destructor) before either stops, since closing
// the scene needs both.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/scene_host.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"
#include "wisp/os/queue.hpp"
#include "wisp/scene/patch_queue.hpp"
#include "wisp/scene/vm.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::net {
class SceneHttp;
}

namespace wisp::scene {

struct SceneMessage;
struct DeviceInfo;
enum class DeviceEvent : uint8_t;
struct SceneWaiter;
class SceneRuntime;

bool post_scene_message(SceneRuntime& rt, os::LargeUnique<SceneMessage> msg);

struct SceneRuntimeDeps {
    core::EventBus& bus;
    core::StorageManager& storage;
    core::StorageWorker& worker;
    core::SceneRepository& repo;
    ui::UiRuntime& ui;
    ui::SystemLayer& system;
    hal::ScreenShape shape;
    // The proxy wisp.http uses; null: wisp.http raises. Must outlive the
    // runtime (stop() cancels the foreground scene's requests through it).
    net::SceneHttp* http = nullptr;
    // Battery, charging and Wi-Fi for wisp.device.info():
    //   {"battery": null | {"percent": int (-1: unknown), "charging": bool},
    //    "wifi": "off" | "connecting" | "backoff" | "connected"}
    // (missing fields: unknown battery, not charging, "off"). Called on the
    // scene task, which must not touch flash (task_config()): it returns
    // values another thread keeps up to date (PowerMonitor's last sample,
    // a Wi-Fi state snapshot), never reads Settings, NVS or files.
    std::function<Value()> device_status;
    // Wall clock, seconds since the epoch; empty: std::time(). Called on
    // the scene task, same rule.
    std::function<int64_t()> wall_clock;
};

struct SceneRuntimeConfig {
    VmLimits vm;
    size_t queue_capacity = 64;
    // At LUAI_MAXCCALLS (48) C call levels, string.gsub recursion is the
    // deepest use of the C stack (vm.hpp); this is sized for it.
    size_t stack_bytes = 128 * 1024;
    int priority = 3;
};

class SceneRuntime : public core::SceneHost {
public:
    SceneRuntime(SceneRuntimeDeps deps, SceneRuntimeConfig config = {});
    ~SceneRuntime() override;  // stop()

    SceneRuntime(const SceneRuntime&) = delete;
    SceneRuntime& operator=(const SceneRuntime&) = delete;

    // "wisp.scene", unpinned, below the UI task's priority, and
    // flash_access = false (see the file header), so the port may put
    // its stack in PSRAM.
    static os::TaskConfig task_config(const SceneRuntimeConfig& c);

    // Installs the UI frame hook and spawns the scene task. Busy if it
    // was started before, NoMemory if the task cannot be made.
    Result<void> start();
    // Closes the foreground scene (hooks run), stops the task, deletes the
    // view and removes the frame hook. Idempotent. Not from the scene
    // task or the UI thread.
    void stop();

    // The screen went off (false) or on (true): scene.on_hide() and a
    // store flush, or scene.on_show(). Never waits.
    void set_visible(bool visible);
    // The id of the scene in the foreground (running or showing its error
    // panel), if any.
    std::optional<std::string> foreground() const;
    // Sends a click to the foreground scene's widget with ref = `ref`, as
    // if the user tapped it (the simulator's debug command). NotFound if
    // no running scene has one.
    Result<void> click_ref(std::string_view ref);

    // SceneHost. open(), close() and reload() wait until the scene task
    // has done the work (at most 10 s, then Timeout), except on the UI
    // thread, the scene task and the storage worker, where they only queue
    // it and return Ok (failures are logged and published). Busy when the
    // runtime is not running or its queue stays full.
    Result<void> open(const std::string& id, Value args = {}) override;
    Result<void> close(const std::string& id) override;
    Result<void> reload(const std::string& id) override;
    core::SceneStatus status(const std::string& id) const override;
    // Busy when the runtime is not running; Timeout after 10 s.
    Result<void> check_sources(std::span<const core::SourceFile> files) override;
    void on_installed(const std::string& id) override;

    // Messages dropped because their instance was not the foreground's.
    uint64_t stale_dropped() const { return stale_dropped_.load(); }

private:
    friend bool post_scene_message(SceneRuntime& rt, os::LargeUnique<SceneMessage> msg);
    friend size_t pending_patches(const SceneRuntime& rt);
    friend uint32_t foreground_instance(const SceneRuntime& rt);

    struct Link;
    struct Delayed {
        uint64_t due_ms;
        std::function<void()> fn;
    };
    // The scene in the foreground; scene task only.
    struct Foreground;

    // --- any thread ---
    bool must_not_wait() const;
    // Queues a request; waits for it unless must_not_wait().
    Result<void> request(os::LargeUnique<SceneMessage> msg, std::shared_ptr<SceneWaiter> waiter);
    bool push(os::LargeUnique<SceneMessage> msg, uint32_t timeout_ms);
    void set_status(const std::string& id, core::SceneState state, std::string error);
    void publish(const std::string& id, const char* state);
    // SceneStoreDeps::schedule: runs `fn` on the scene task after `ms`.
    void schedule(uint32_t ms, std::function<void()> fn);

    // --- scene task ---
    void run();
    void handle(SceneMessage& m);
    // The foreground scene `m` is for, or nullptr (a stale message is
    // counted).
    Foreground* target(const SceneMessage& m);
    void begin_open(const std::string& id, Value args, std::shared_ptr<SceneWaiter> waiter);
    void finish_open(SceneMessage& m);
    // Gives the scene's wisp.http its proxy: the declared hosts, and
    // results that come back as HttpDone messages for this instance.
    void wire_http(Foreground& f);
    void close_foreground();
    // Runs one step of the running scene; on an error the scene faults.
    // Returns false if it did.
    bool step(const std::optional<ScriptError>& err);
    // Hands the scene's batch to the PatchQueue, then answers a refresh
    // request if the queue made one.
    bool submit();
    void fault(const ScriptError& err);
    void hide_scene();
    void show_scene();
    void run_delayed();
    void drain_queue();
    // How long the task may wait for the scene's timers and minute ticker.
    uint32_t scene_wait_ms();
    // Fires due timers, then the minute (and the battery check with it).
    void run_scene_clock();
    void run_timers();
    void minute();
    // `ev` for the foreground scene; battery and lang only if they changed.
    void device_event(DeviceEvent ev);
    void fire(DeviceEvent ev, const DeviceInfo& info);
    // wisp.scene.close() was called: close now.
    void close_if_asked();
    DeviceInfo device_info() const;
    std::optional<int64_t> wall_now() const;
    bool wants_minute() const;

    // --- UI thread ---
    void ui_frame();
    void ui_show_view(uint32_t instance, std::string id,
                      std::shared_ptr<const core::ScenePackage> package);
    void ui_drop_view();
    // Retries post() while the UI queue is full (bounded).
    void post_ui(std::function<void()> fn);

    SceneRuntimeDeps deps_;
    SceneRuntimeConfig config_;
    os::Queue<SceneMessage*> queue_;
    PatchQueue patches_;
    std::unique_ptr<os::Task> task_;
    std::shared_ptr<Link> link_;
    std::atomic<bool> running_{false};
    bool started_ = false;
    std::atomic<uint64_t> stale_dropped_{0};
    // The instance whose patches the frame hook applies (0: none).
    std::atomic<uint32_t> ui_instance_{0};

    mutable os::Mutex status_mu_;
    std::map<std::string, core::SceneStatus, std::less<>> statuses_;
    std::optional<std::string> foreground_id_;
    uint32_t foreground_instance_ = 0;

    os::Mutex delayed_mu_;
    std::vector<Delayed> delayed_;

    // Scene task only.
    std::unique_ptr<Foreground> fg_;
    uint32_t next_instance_ = 1;
    // The instance the worker is loading for (0: none); a load that comes
    // back for another one was superseded.
    uint32_t loading_ = 0;
    bool visible_ = true;
    bool quit_ = false;
    // The ui::Lang of the last system.settings event, -1 before any. Set
    // by the bus's dispatcher, read by the scene task.
    std::atomic<int> lang_{-1};
    core::SubscriptionId power_sub_ = 0;
    core::SubscriptionId settings_sub_ = 0;

    // UI thread only.
    struct UiSide;
    std::unique_ptr<UiSide> ui_side_;
};

}  // namespace wisp::scene
