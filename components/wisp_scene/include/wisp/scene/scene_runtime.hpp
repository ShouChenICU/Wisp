#pragma once

// SceneRuntime: the scene task and the lifecycle of every running scene
// (scene-runtime-core spec 3.1-3.6, scene-device spec 3.1-3.4). It
// implements core::SceneHost.
//
// Threads:
//   - The scene task ("wisp.scene") owns every running scene: its Vm, its
//     PatchBatch, its SceneStore handle. It takes one message at a time
//     from a queue of SceneMessage pointers (bodies in PSRAM) and runs it
//     in the VM of the scene it is for; after each message it flushes
//     that scene's reactive state and submits the batch to the
//     PatchQueue (the scene in front) or drops it (a scene in the
//     background).
//   - The UI thread owns the SceneView, at most one, of the scene in
//     front. Its frame hook applies the PatchQueue to that view; its
//     widget events become messages. It never waits for the scene task,
//     and the scene task never waits for it (it only post()s).
//   - The wisp.fs worker reads the package and loads the SceneStore, then
//     posts the loaded scene to the scene task. The scene task touches no
//     flash itself (task_config(): flash_access = false): SceneStore
//     writes go to its in-memory index, and its flushes, like every file
//     operation, are jobs on the worker.
//
// Scenes (spec 3.1): one task runs them all, in slots: at most one in the
// foreground (with a view) and up to three in the background (without
// one). Each slot has its own instance, VM, timers, minute ticker and
// store. Every open gets a new instance number; a scene keeps it while it
// moves between foreground and background. Messages from the view, the
// worker, the store and the HTTP proxy carry it, and one whose instance
// no slot has is dropped (stale_dropped()), so nothing from a closed
// scene reaches the next one; neither do the view's events for a scene
// that has gone to the background. Requests from the SceneHost methods
// carry instance 0 and name the scene by id. Each message still has its
// own 200 ms, so a scene in the background cannot hold up the one in
// front.
//
// Lifecycle (scene-runtime-core spec 3.4, scene-device spec 3.2):
//   open    launch(User or Notification): a scene in the background comes
//           forward (resume, below). Otherwise the worker loads the
//           package and the store; the scene task then sends the scene in
//           front back (below; the same scene closes before the load
//           instead, so its final store flush is queued on the worker
//           ahead of it and the reopened store reads what it wrote; a
//           scene that fails to load leaves another one running), makes
//           the VM (prelude, ui, a `scene` table for the hooks), runs the
//           entry file and builds its tree; the UI thread makes the
//           SceneView, gives it the package's assets and loads its
//           screen, remembering the screen it replaced (the self-check
//           screen, hidden, never rebuilt); then scene.on_show() if the
//           screen is on, and scene.on_state("foreground").
//   boot    launch(Boot), open_background(): loaded the same way, into
//           the background: the tree is built but there is no view and no
//           on_show; scene.on_state("background"). Only for a scene whose
//           scene.json declares "background" (InvalidArgument otherwise),
//           and not past the background's count (Busy). A boot launch
//           (not open_background(), which a reinstall uses) that does not
//           bring its scene up -- refused, not found, a script error --
//           is a RestoreFailed notice. One of a scene that a user open
//           is already loading rides on that open: if a later open
//           supersedes it, the scene still loads, as a boot launch.
//   back    the back gesture, the error panel's "返回", close() and
//           wisp.scene.close(): a scene whose scene.json declares
//           "background" goes to the background, any other closes.
//   to the background
//           spec 3.2's order: scene.on_hide() (if it was shown) while the
//           scene is still in front (wisp.scene.state() is "foreground"
//           there, keep_screen_on() still works); every wake lock the
//           scene holds is released; the view goes (the remembered screen
//           comes back); then scene.on_state("background"), and the state
//           becomes Background. An error in on_hide still sends the view
//           away: the scene faults as one in the background (a notice, no
//           panel), as the user asked to leave it. The VM, the store, the
//           timers and the tree stay; the scene keeps running, its UI
//           changes only update the tree.
//   resume  SceneInstance::rebuild() makes a Build of the kept tree with
//           every bound value as it is now, the UI thread makes a new
//           view from it, scene.on_show() (screen on),
//           scene.on_state("foreground"), Running.
//   hide    set_visible(false): scene.on_hide() for the scene in front;
//           every store flushes.
//   show    set_visible(true): scene.on_show() for the scene in front.
//   exit    exit(), close_async(), wisp.scene.exit(), from any state:
//           scene.on_hide() (if it was shown), scene.on_close(); each has
//           its own 200 ms and an error or timeout is logged, not shown.
//           Then the wake locks go, SceneStore::flush() and close(), the
//           VM goes, and for the scene in front the UI thread loads the
//           remembered screen and deletes the view.
//   reload  exit, then open in front with the same arguments; data stays.
// Every move between foreground and background calls scene.on_state with
// the new state ("foreground" or "background"; also on the first open,
// never on a close) and publishes it ("running" or "background").
//
// Budget and eviction (scene-device spec 3.5): every scene's VM draws on
// one VmPool of SceneRuntimeConfig::vm_budget, besides its own limit
// (SceneRuntimeConfig::vm.memory_bytes, also what the scene in front is
// guaranteed); an allocation past either fails as a memory error. When a
// scene opens, comes forward or goes to the background, and before each
// message while a scene is in front, pick_evictions() names the
// background scenes to close, least recently in front first: those past
// max_background, then, with a scene in front or opening, those that
// keep the background above vm_budget - vm.memory_bytes. Each closes
// like exit() (on_close, the final flush), is logged ("<id>: evicted
// (...)") and becomes an EvictedCount or EvictedMemory notice.
//
// Background list (scene-device spec 3.6): with
// SceneRuntimeDeps::background_list_path, the ids of the scenes in the
// background, least recently in front first, together with the boot
// launches still loading, are written there ({"ids": [...]},
// StorageManager::write_atomic on the worker) whenever they change, but
// only once restore_background() has read the file (so nothing running
// before the restore overwrites the last boot's list), and never by
// stop(): the scenes it closes are the ones to restore next time. A boot
// launch that fails leaves the list, which is then written again.
//
// Timers and device events (scene-runtime-core spec 3.4, 5.1, 5.2;
// scene-device spec 3.3): the task's wait is bounded by every scene's
// next TimerSet deadline and the MinuteTicker of each scene that listens
// for "minute", "battery" or "lang" and whose minute ticks: a scene that
// declares "background" (in front or not) always, any other while the
// screen is on. Each due timer callback, each event, is a message of its
// own (own 200 ms budget, flush, submit). Screen off: the timers of a
// scene in front that does not declare "background" pause and its ticker
// is not polled; a scene that declares it keeps both going. Screen on:
// paused timers resume (each missed one fires once), the scene in front
// gets a "minute" at once, then on_show. "battery" fires when the
// percentage or charging differ from what the scene last saw, checked on
// each power.charging.changed and each minute; "lang" when the language
// of the last system.settings event differs from the one the scene last
// saw, checked on each such event and each minute. info().lang is the
// language of the last system.settings event, ui::lang() before any
// (ui::set_lang() runs on the UI thread and may lag the event).
// info().utc_offset is that event's utc_offset_min, else the
// seed_utc_offset() value, else SceneRuntimeDeps::utc_offset_min; when an
// event changes it, "minute" fires at once for every scene whose minute
// ticks, since the local time a scene shows has moved. A system.time
// event of state "synced" (net::TimeSyncService) fires it the same way,
// the wall clock itself may have moved, unless the minute the clock now
// reads already fired.
//
// HTTP (scene-runtime-core spec 5.3): opening a scene gives the proxy
// (SceneRuntimeDeps::http) its declared hosts; each wisp.http result is an
// HttpDone message whose callback runs with its own 200 ms budget, in
// front or in the background. Closing, reloading or faulting cancels the
// scene's requests (SceneHttp::cancel_scene): queued ones are dropped, the
// one in flight is stopped, and no callback runs for them; a result
// already queued for the old instance is dropped as stale.
//
// A hook is a function the scene stores in its `scene` table
// (scene.on_show = function() ... end).
//
// Errors (scene-runtime-core spec 3.3, 3.6): an error that reaches the
// top of a message (compile, runtime, timeout, memory, a bad tree, a bad
// bound value) drops the message's patches, destroys the VM, cancels the
// scene's requests, releases its wake locks and closes the store; the
// state becomes Error with ScriptError::text(), logged with tag
// "scene.<id>". For the scene in front the system layer's error panel
// comes up over its last frame: "重载" reloads it, "返回" (or the back
// gesture) goes back to the remembered screen; the state stays Error
// until the scene is opened again. A scene in the background just stops,
// with no panel, and SceneRuntimeDeps::on_notice hears of it (Faulted;
// scene-device spec 3.2). Nothing is retried automatically.
//
// Each message is timed: one over 50 ms logs "slow message <kind> <ms>
// ms" (tag "scene.<id>"). After a batch is submitted, a refresh the patch
// queue asks for (take_refresh_request()) is answered at once with a
// UiBatch::refresh batch from __wisp.refresh_all().
//
// Lifetime: start() after the UiRuntime and the StorageWorker have
// started; stop() (or the destructor) before either stops, since closing
// the scenes needs both.

#include <array>
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
#include "wisp/core/wakelock.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"
#include "wisp/os/queue.hpp"
#include "wisp/scene/patch_queue.hpp"
#include "wisp/scene/scene_schedule.hpp"
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

// Why a scene was opened; wisp.scene.launch() reports it.
enum class LaunchReason : uint8_t { User, Boot, Notification };
// "user", "boot", "notification".
const char* to_string(LaunchReason r);

// What SceneRuntimeDeps::on_notice is told: a scene closed to make room
// (memory or count), one that failed in the background, one that could
// not be restored at boot. `name` is the scene's display name.
enum class SceneNoticeKind : uint8_t { EvictedMemory, EvictedCount, Faulted, RestoreFailed };
struct SceneNotice {
    SceneNoticeKind kind;
    std::string id;
    std::string name;
};

// A scene running in the background; `since_ms` is os::now_ms() when it
// went there.
struct BackgroundScene {
    std::string id;
    std::string name;
    uint64_t since_ms = 0;
    bool operator==(const BackgroundScene&) const = default;
};

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
    // The time zone wisp.device.info().utc_offset reports (minutes east of
    // UTC) until seed_utc_offset() or a system.settings event says
    // otherwise.
    int utc_offset_min = 480;
    // wisp.power; null: keep_screen_on() returns nil, "unavailable". Must
    // outlive the runtime.
    core::WakeLockRegistry* locks = nullptr;
    // System notices about the scenes (SceneNotice). Called on the scene
    // task, which must not touch flash: it may only queue the work.
    std::function<void(const SceneNotice&)> on_notice;
    // <System>/scenes/background.json (file header); empty: the list is
    // neither written nor restored.
    std::string background_list_path;
};

struct SceneRuntimeConfig {
    // Each scene's own limits; vm.memory_bytes is also what the scene in
    // front is guaranteed of vm_budget. vm.pool is ignored: the runtime
    // gives every scene its own pool of vm_budget.
    VmLimits vm;
    size_t vm_budget = kVmBudgetBytes;
    // At most kMaxBackground (larger values are taken as that).
    size_t max_background = kMaxBackground;
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
    // Closes every scene (hooks run), stops the task, deletes the view and
    // removes the frame hook. Idempotent. Not from the scene task or the
    // UI thread.
    void stop();

    // The screen went off (false) or on (true): scene.on_hide() for the
    // scene in front and a flush of every store, or scene.on_show(). Never
    // waits.
    // The time zone setting, read by App (which may touch flash) after
    // start(): from then on a change also arrives as a system.settings
    // event, so a seed loses to any event already seen, whose value is at
    // least as new. Values outside -720..840 are ignored. Any thread.
    void seed_utc_offset(int minutes);
    void set_visible(bool visible);
    // The id of the scene in the foreground (running or showing its error
    // panel), if any.
    std::optional<std::string> foreground() const;
    // The scenes in the background, least recently in front first. Any
    // thread.
    std::vector<BackgroundScene> background_scenes() const;
    // Sends a click to the foreground scene's widget with ref = `ref`, as
    // if the user tapped it (the simulator's debug command). NotFound if
    // no running scene has one.
    Result<void> click_ref(std::string_view ref);

    // SceneHost. open(), close(), exit() and reload() wait until the scene
    // task has done the work (at most 10 s, then Timeout) -- for exit(),
    // and for a close() that closes, until every store flush the scene
    // queued has run, its final one and, for a scene that faulted or is
    // not running, any still queued from before; by then status() is
    // Stopped and "stopped" has been published (a faulted scene stays
    // Error) -- except on the UI thread, the scene task and the storage
    // worker, where they only queue it and return Ok (failures are logged
    // and published). Busy when the runtime is not running or its queue
    // stays full. open() is also Busy, at once, while
    // SceneRepository::installing(id) (scene-runtime-core spec 6.3: the
    // package is being replaced) or removing(id) (it is being deleted); an
    // open that was already on its way when the install or delete began
    // is dropped the same way when its load arrives. close(id) or exit(id)
    // while a load of that id is on its way cancels the open. close() is
    // "back" (file header); exit() closes from any state.
    Result<void> open(const std::string& id, Value args = {}) override;
    Result<void> close(const std::string& id) override;
    Result<void> exit(const std::string& id) override;
    Result<void> reload(const std::string& id) override;
    core::SceneStatus status(const std::string& id) const override;
    // User and Notification open `id` in the foreground (one in the
    // background comes forward without reloading; `args` are then
    // ignored). Boot starts it in the background without a view (Ok and
    // nothing changes if it is already running or loading; see "boot" in
    // the file header for what is refused, and the RestoreFailed notice).
    // Waits like open().
    Result<void> launch(const std::string& id, LaunchReason reason, Value args = {});
    // launch(id, Boot) without the RestoreFailed notice (SceneHost).
    Result<void> open_background(const std::string& id) override;
    // Reads the background list on the worker and launches each scene in
    // it as at boot, in order (file header). Never waits; does nothing
    // without SceneRuntimeDeps::background_list_path or a running
    // runtime. Once per boot, after start().
    void restore_background();
    // exit() without waiting, from any thread: `done` runs once exit()
    // would have returned (on the storage worker or the scene task), or at
    // once with Busy if the request could not be queued. It must be quick
    // and must not wait for the scene task.
    void close_async(const std::string& id, std::function<void(Result<void>)> done);
    // Busy when the runtime is not running, or when called on the UI
    // thread or the storage worker (neither may wait); Timeout after 10 s.
    Result<void> check_sources(std::span<const core::SourceFile> files) override;
    void on_installed(const std::string& id) override;

    // Messages dropped because no running scene has their instance, and
    // view events for a scene that has gone to the background.
    uint64_t stale_dropped() const { return stale_dropped_.load(); }

private:
    friend bool post_scene_message(SceneRuntime& rt, os::LargeUnique<SceneMessage> msg);
    friend size_t pending_patches(const SceneRuntime& rt);
    friend uint32_t foreground_instance(const SceneRuntime& rt);
    friend uint32_t scene_instance(const SceneRuntime& rt, const std::string& id);
    friend size_t pending_ui_jobs(SceneRuntime& rt);

    struct Link;
    struct Delayed {
        uint64_t due_ms;
        std::function<void()> fn;
    };
    // One running scene, in front or in the background; scene task only.
    struct Slot;
    // At most one scene in front and kMaxBackground behind it, and for a
    // moment one more: a scene sent to the background before the eviction
    // that follows it.
    static constexpr size_t kMaxSlots = kMaxBackground + 2;
    // The instances of every slot, copied for loops whose steps may close
    // slots; fixed size, as these loops run on every timer tick.
    struct Instances {
        std::array<uint32_t, kMaxSlots> ids{};
        size_t n = 0;
        const uint32_t* begin() const { return ids.data(); }
        const uint32_t* end() const { return ids.data() + n; }
    };
    // A load on the worker (scene task only). A user or notification open
    // supersedes the other ones still loading (the latest one wins) and a
    // boot launch of the same scene; a boot launch of a scene already
    // running or loading does nothing. So a list of boot launches queued
    // at once all come up, and a boot launch never cancels a user open.
    // Nor is one lost to it: a boot launch of a scene a user open is
    // loading (or one that open replaced) sets `then_boot` on it, and a
    // user open that supersedes such a load turns it into a boot launch
    // instead of dropping it.
    struct PendingLoad {
        uint32_t instance = 0;
        std::string id;
        LaunchReason reason = LaunchReason::User;
        // A failure is a RestoreFailed notice (a boot launch, or a user
        // open a boot launch rides on).
        bool restore = false;
        bool then_boot = false;
        // Its place in the background list (Slot::seq).
        uint64_t seq = 0;
    };
    // A job for the UI thread (post_ui()). Showing and dropping a view name
    // their instance, so that a drop can cancel a show still waiting.
    struct UiJob {
        enum class Kind : uint8_t { Show, Drop, Other };
        Kind kind = Kind::Other;
        uint32_t instance = 0;
        std::function<void()> fn;
    };

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
    // The slot `m` is for, or nullptr (a stale message is counted).
    Slot* target(const SceneMessage& m);
    Slot* find(uint32_t instance);
    Slot* find_id(std::string_view id);
    Instances instances() const;
    size_t background_count(const Slot* except) const;
    Result<void> launch_load(const std::string& id, LaunchReason reason, Value args, bool restore);
    void begin_open(const std::string& id, Value args, LaunchReason reason, bool restore,
                    std::shared_ptr<SceneWaiter> waiter);
    void finish_open(SceneMessage& m);
    // Forgets the loads of `id` still on the worker (closed or exited by
    // id: whoever did may go on to delete or replace its files).
    void cancel_loads(std::string_view id);
    // What a superseded load of a scene hands on to the user open of the
    // same scene that superseded it: a boot launch to carry on.
    struct Carried {
        bool then_boot = false;
        bool restore = false;
        uint64_t seq = 0;
    };
    // A user open supersedes the user and notification loads still on the
    // worker (but turns those with then_boot into boot launches) and, if
    // `id` is not empty, every other load of `id`.
    Carried supersede_loads(std::string_view id);
    // The scene could not be restored: logged, and a RestoreFailed notice.
    void restore_failed(const std::string& id, const std::string& name, const char* why);
    // msg::Restore.
    void restore(std::vector<std::string> ids);
    // Closes what pick_evictions() names (file header); `coming` is a
    // scene in the background about to come forward, counted as in front.
    void enforce_limits(bool opening, const Slot* coming = nullptr);
    void evict(Slot& s, bool memory);
    // The slots in the background, least recently in front first.
    std::vector<const Slot*> background_order() const;
    // The background list as it should be on disk (file header).
    std::vector<std::string> wanted_background() const;
    // Writes it if it differs from what was last written.
    void persist_background();
    // Shows the view of `s` (now in front) on the UI thread.
    void show_view(Slot& s);
    // Makes the slot of a loaded scene (VM not started yet); null without
    // the memory for it.
    Slot* add_slot(SceneMessage& m, LaunchReason reason);
    // Gives the scene's wisp.http its proxy: the declared hosts, and
    // results that come back as HttpDone messages for this instance.
    void wire_http(Slot& s);
    // "Back" for the scene in front: to the background if it declares it
    // (and is running), closed otherwise. `except` is not counted towards
    // the background's limit (the scene about to come forward).
    void go_back(Slot& s, std::shared_ptr<SceneWaiter> done, const Slot* except = nullptr);
    void to_background(Slot& s, std::shared_ptr<SceneWaiter> done, const Slot* except);
    void resume(Slot& s, std::shared_ptr<SceneWaiter> done);
    // No scene is in front any more (foreground() says so).
    void forget_front();
    // The view of `s` goes: its patches stop, the UI thread drops it and
    // loads the remembered screen (and logs "<id>: closed" if `closing`).
    void drop_view(const Slot& s, bool closing);
    // Exit: hooks, requests, locks, store, view; the slot goes.
    void close_slot(Slot& s, std::shared_ptr<SceneWaiter> done = nullptr);
    // Removes the slot (after a close, or a fault in the background).
    void erase_slot(Slot& s);
    // Finishes `done` from a worker job queued now, behind every flush
    // queued before it (Busy if the worker takes no job).
    void finish_after_queued_io(std::shared_ptr<SceneWaiter> done);
    // Runs one step of a running scene; on an error the scene faults.
    // Returns false if it did (a scene in the background is gone then).
    bool step(Slot& s, const std::optional<ScriptError>& err);
    // Hands the batch of the scene in front to the PatchQueue, then
    // answers a refresh request if the queue made one; drops the batch of
    // one in the background.
    bool submit(Slot& s);
    // step(), then submit(); false if the scene faulted.
    bool run_step(Slot& s, const std::optional<ScriptError>& err);
    void fault(Slot& s, const ScriptError& err);
    void release_locks(Slot& s);
    void notice(SceneNoticeKind kind, const Slot& s);
    // Rewrites background_ from the slots.
    void update_background_list();
    void hide_scenes();
    void show_scene(bool screen_came_on);
    void run_delayed();
    void drain_queue();
    // How long the task may wait for the scenes' timers and minute tickers.
    uint32_t scene_wait_ms();
    // Fires due timers, then the minutes (and the battery checks with them).
    void run_scene_clock();
    void run_timers();
    // A network time sync set the wall clock (system.time "synced").
    void clock_set();
    void minute(uint32_t instance);
    // `ev` for every scene (battery and lang only if they changed for it).
    void device_event(DeviceEvent ev);
    void device_event(uint32_t instance, DeviceEvent ev, const DeviceInfo& info);
    void fire(uint32_t instance, DeviceEvent ev, const DeviceInfo& info);
    // wisp.scene.close() or exit() was called: act on it now.
    void close_if_asked();
    DeviceInfo device_info() const;
    std::optional<int64_t> wall_now() const;
    bool wants_minute(const Slot& s) const;
    // Whether the scene's minute ticks now (file header).
    bool minute_ticks(const Slot& s) const;

    // --- UI thread ---
    void ui_frame();
    void ui_show_view(uint32_t instance, uint64_t token, std::string id,
                      std::shared_ptr<const core::ScenePackage> package);
    // Drops the view if it is `instance`'s (any view for 0).
    void ui_drop_view(uint32_t instance);
    // Hands `fn` (showing or dropping a view, the error panel) to the UI
    // thread, in order, and never drops it: it waits in ui_jobs_ until
    // the UI thread runs it, from a post()ed nudge or, when the UI queue
    // was full, from the next frame hook (or stop()). Any thread; never
    // waits.
    void post_ui(UiJob job);
    // A Drop of `instance` while its Show still waits: both go (with any
    // later job of that instance, its error panel), so a view that would
    // only come up to go again holds no package meanwhile. True if so.
    bool cancel_show_locked(uint32_t instance);
    // UI thread: runs the jobs post_ui() queued, oldest first.
    void run_ui_jobs();

    SceneRuntimeDeps deps_;
    SceneRuntimeConfig config_;
    // Every scene's VM draws on it (config_.vm.pool points here); it
    // outlives the slots.
    VmPool pool_;
    os::Queue<SceneMessage*> queue_;
    PatchQueue patches_;
    std::unique_ptr<os::Task> task_;
    std::shared_ptr<Link> link_;
    std::atomic<bool> running_{false};
    bool started_ = false;
    std::atomic<uint64_t> stale_dropped_{0};
    // The view the frame hook applies patches to and click_ref() clicks
    // in: a token each show_view() takes, 0 while no view should take
    // any. A scene keeps its instance across the background, so the
    // instance alone cannot tell its new view from the old one still up.
    std::atomic<uint64_t> ui_view_{0};

    mutable os::Mutex status_mu_;
    std::map<std::string, core::SceneStatus, std::less<>> statuses_;
    std::optional<std::string> foreground_id_;
    uint32_t foreground_instance_ = 0;
    std::vector<BackgroundScene> background_;
    // id -> instance of every slot, for scene_instance().
    std::map<std::string, uint32_t, std::less<>> slot_instances_;

    os::Mutex delayed_mu_;
    std::vector<Delayed> delayed_;

    // Scene task only.
    std::vector<os::LargeUnique<Slot>> slots_;
    // The slot in the foreground (running or showing its error panel), or
    // null.
    Slot* front_ = nullptr;
    uint32_t next_instance_ = 1;
    uint64_t next_view_ = 1;
    // Slot::seq and PendingLoad::seq.
    uint64_t next_seq_ = 1;
    // What the background list on disk holds, as last read or written;
    // nullopt until restore() has read it (nothing is written before).
    std::optional<std::vector<std::string>> persisted_;
    // The loads on the worker; one that comes back and is not here was
    // superseded, or cancelled by a close of its scene.
    std::vector<PendingLoad> loading_;
    bool visible_ = true;
    bool quit_ = false;
    // The ui::Lang of the last system.settings event, -1 before any. Set
    // by the bus's dispatcher, read by the scene task.
    std::atomic<int> lang_{-1};
    // kUnsetOffset until seeded or set by an event.
    static constexpr int kUnsetOffset = INT32_MIN;
    std::atomic<int> utc_offset_min_{kUnsetOffset};
    // utc_offset_min_, or SceneRuntimeDeps::utc_offset_min while unset.
    int utc_offset_min() const;
    core::SubscriptionId power_sub_ = 0;
    core::SubscriptionId settings_sub_ = 0;
    core::SubscriptionId time_sub_ = 0;

    // UI thread only.
    struct UiSide;
    os::LargeUnique<UiSide> ui_side_;

    // post_ui()'s jobs not yet run.
    os::Mutex ui_jobs_mu_;
    os::LargeVector<UiJob> ui_jobs_;
};

}  // namespace wisp::scene
