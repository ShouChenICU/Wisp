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
// carry instance 0 and name the scene by id.
//
// Scheduling (scene-runtime-core spec 3.1): every step the task runs
// (a message, a result, a timer callback, a slice of the agent's Lua) has
// at most 200 ms, and before each one it picks from three lanes, so that
// the scene in front is answered before background work:
//   1. the queue: one message, in arrival order (the view's events and
//      every control message, whose order must hold);
//   2. the front: one item of the scene in front, its oldest result, else
//      one of its due timers (its wisp.tools call's resumptions are
//      results and timers of the same VM);
//   3. the back: one item of the next source after the last one that ran
//      (by instance, wrapping) that has work: a scene in the background or
//      headless (as 2), or the agent's run.
// Strict order would let a front that never runs dry stop the other
// lanes, so after kQueueBurst messages in a row while a lane item waits,
// one lane item runs, and once kFrontBurst front items have run while a
// back item waited (queue messages in between do not reset that count),
// the next lane item is a back one. With the queue and the front both
// saturated, a waiting back item so runs after at most
// (kQueueBurst + 1) * kFrontBurst + kQueueBurst other items. Within one
// VM a waiting result is taken before a due timer. Due timers are taken
// all at once (TimerSet::due()) and run one per item; one cancelled
// meanwhile runs nothing. Before every item the task does its housekeeping: the
// screen's visibility, the stores' delayed calls, the eviction check
// (scene-device spec 3.5) with a scene in front, the agent run's
// deadline, the minute polls, the tool calls' timeouts and ends, the
// background list and the scenes that asked to close. With no lane item
// waiting it blocks on the queue until the next timer, minute or
// deadline.
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
//           that does not bring its scene up -- refused, not found, a
//           script error -- is a RestoreFailed notice. One of a scene that
//           a user open is already loading rides on that open: if a later
//           open supersedes it, the scene still loads, as a boot launch.
//           open_background() (a reinstall over a scene in the background)
//           sends no such notice, and a scene that left the background
//           by an exit comes back as it was: wisp.scene.launch() as
//           before, the same place in the order of least recently in
//           front.
//   notify  deliver_notification(): a scene running in front stays, one in
//           the background comes forward (resume), any other opens as
//           launch(Notification); then scene.on_notification(note_id),
//           once it is up and in front.
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
//   headless
//           call_tool() for a scene that is not running (agent-tools spec
//           5.4): loaded like a boot launch (the tree is built, no view, no
//           on_show), then scene.on_state("headless") and the state
//           Headless (published "headless"). It need not declare
//           "background", and it is neither counted towards the
//           background's limit nor put on the background list, nor ever
//           evicted; wisp.scene.state() is "headless" and the APIs spec
//           3.4 refuses in the background refuse here too, but its timers
//           run whether the screen is on or off. Its VM draws on the shared
//           budget: its limit is what the budget leaves after what a scene
//           in front is guaranteed (max(vm.memory_bytes, the front's
//           bytes), kept even with none in front) and everything else
//           running, at most vm.memory_bytes. Only when that is under
//           kHeadlessMinMemory are background scenes evicted for it, least
//           recently in front first, until it is not; if it still is, the
//           call is "no_memory" (so is a memory fault while it loads). Once
//           it runs, enforce_limits() charges what it holds against the
//           background's room. At most one scene is headless: a call to another
//           scene's tool closes it first. SceneRuntimeConfig::
//           headless_idle_ms after its last call ended without another
//           one, it closes like exit() (on_close, the final flush;
//           on_state is not called). An open of it (or a notification)
//           brings it forward as a resume does, without reloading, its VM
//           limit raised to vm.memory_bytes (Vm::raise_memory_limit()), and
//           it is an ordinary scene from then on. A boot launch or
//           open_background() of it, if it declares "background", sends it
//           to the background the same way (to_background()), onto the
//           background list. A failure while it loads is
//           the call's ("load_failed", or "no_memory" for a memory fault),
//           and a fault later is logged like one in the background, but
//           sends no notice: the agent's call reports it.
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
// gets a "minute" at once, then on_show. Device events come from state
// cells (SceneRuntimeDeps::supply and ::settings, attach_time_sync()):
// their callbacks, on wisp.bus, only set a bit in pending_device_ and
// push a wake-up, and the scene task takes the bits on every loop step,
// right after its housekeeping (so a screen-off asked for meanwhile is
// already applied), so a change made while the queue is full waits there
// instead of being lost, and several changes between two steps are one
// check.
// "battery" fires when the percentage or charging differ from what the
// scene last saw, checked on each change of the supply's charging or USB
// state and each minute (a percent step alone waits for the minute);
// "lang" when the settings' language differs from the one the scene last
// saw, checked on each change of it and each minute. info()'s settings
// are the settings cell's kSettingScene rows (settings_schema.hpp), each
// under its scene_name (lang, utc_offset, time_24h), and the schema's
// defaults without the cell; info().lang is the cell's language, not
// ui::lang() (ui::set_lang() runs on the UI thread and may lag it). When
// utc_offset changes, "minute" fires at once for every scene whose minute
// ticks, since the local time a scene shows has moved. A finished
// time-sync attempt that synced (net::TimeSyncService) fires it the same
// way, the wall clock itself may have moved, unless the minute the clock
// now reads already fired. A change of time_24h (the Settings time
// format) fires "minute" the same way. The values at start are the ones
// the runtime starts from: no event for them.
//
// Results: every VM (a scene's, the agent's run) has a CompletionBox
// (completion_box.hpp), made with the VM. A finished wisp.http or
// wisp.files call is put there, from the proxy's task, a storage worker
// (wisp.fs or wisp.sd.io) or the scene task itself (a refusal answered inside the call), and the
// queue carries only a Completed wake-up. A VM never has more than
// kMaxCallsInFlight calls waiting (scene_api.hpp), so the box never fills
// and no result is dropped for want of room, however full the queue is.
// Results go to the box of the instance that made the call, so none ever
// reaches a later instance of the same scene. The scene task runs them
// oldest first, each as an item of its VM's lane (Scheduling, above) with
// its own 200 ms budget and the slow message log ("slow message
// http|files"). When a VM goes
// (a close, a fault, the end of a run) its box closes: the results still
// in it are freed with no callback run and counted in stale_dropped(),
// and any that arrive later are freed unseen. A result with no memory to
// keep it still runs its callback, with "other" (wisp.http) or "no_memory"
// (wisp.files). The box's lock is a leaf: producers put without waiting
// for Lua, and take Link::mu only after it, to post the wake-up.
//
// HTTP (scene-runtime-core spec 5.3): opening a scene gives the proxy
// (SceneRuntimeDeps::http) its declared hosts; each wisp.http result
// waits in the scene's box (Results, above) until its callback runs, in
// front or in the background. Closing, reloading or faulting cancels the
// scene's requests (SceneHttp::cancel_scene): queued ones are dropped, the
// one in flight is stopped, and no callback runs for them; results
// already in the box are freed with it and counted stale.
//
// Files (scene-device spec 7.1): each wisp.files call is a job of the
// scene (SceneFiles::submit(), SceneRuntimeDeps::files), run on wisp.sd.io
// once the scene's files are known to be on the card, else on wisp.fs; its
// result waits in the scene's box like an HTTP result, and a refused job is
// answered "busy" there at once. A job already queued when the scene
// closes still runs, and its result is freed unseen. wisp.http.download
// begins the file on that worker, then sends the request through the proxy
// with the body streamed into it (core::FileDownload, fed on the proxy's
// download task) and finishes it on the worker it began on
// (FileDownload::submit()). Closing or faulting cancels the scene's
// downloads before its requests: the transfer stops and the worker removes
// the partial file (or, if that worker cannot take it at once, the next
// count does).
//
// The agent's Lua (agent-tools spec 4.2, 4.3; scene_runtime_lua.cpp):
// run_lua() hands a script to the scene task, which runs it in a VM of its
// own, made for the call and destroyed when it ends. That VM is no scene:
// it has no slot, view, store or `scene` table, only the bindings spec 4.2
// lists (wisp.http as the Agent principal under net::kAgentHttpId,
// wisp.files in core::kAgentFilesId's area, wisp.notify as "Wisp",
// wisp.device, wisp.log, wisp.json and wisp.sleep). It draws on the
// scenes' VmPool, never more than is left there (no scene is evicted for
// it), and runs the script as a task (lua_task.hpp) with
// kLuaRunSliceMs slices, the same 200 ms as a scene's step. Each slice,
// each result the script waits for (from the run's own box) and each of
// its wisp.sleep timers is a step of its own in the back lane
// (Scheduling, above), so the scene in front is answered first and the
// scenes keep running while the script waits. The run's own timeout_ms
// counts from the call, whatever the slices.
// print and wisp.log go to the log (tag "agent.lua") and into the
// result's output. The VM gets a new instance number and a box of its own,
// so a result that arrives after it is gone is freed unseen.
//
// Scene tools (agent-tools spec 5.2, 5.4; scene_runtime_tools.cpp):
// call_tool() hands the call to the scene task, which runs the function
// the scene bound with wisp.tools.register as a task (lua_task.hpp) in the
// scene's own VM, with the scene's own 200 ms slices, whether the scene is
// in front, in the background or headless (above). Each slice, each result
// it waits for (from the scene's own box) and each of its wisp.sleep
// timers is a step of its own, run and flushed as any other of the scene's.
// One call per scene at a time. The call ends with the task;
// at its timeout_ms, or when its caller cancels, the task alone is
// cancelled (cancel_task(), so the script can never resume it) and the
// scene runs on; a fault in a slice (its 200 ms, its memory) faults the
// scene as any message would ("timeout" or "scene_error"); a scene that
// closes or is evicted meanwhile ends it "cancelled".
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

#include "wisp/base/fixed_string.hpp"
#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/device_service.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/power_monitor.hpp"
#include "wisp/core/scene_files.hpp"
#include "wisp/core/scene_host.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/core/scene_store.hpp"
#include "wisp/core/sensor_service.hpp"
#include "wisp/core/state_cell.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/core/time_sync_result.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"
#include "wisp/os/queue.hpp"
#include "wisp/scene/lua_task.hpp"
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
struct SceneQuery;
struct LuaRunJob;
struct ToolCallJob;
struct SceneApi;
class CompletionBox;
class SceneView;
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

// One scene lifecycle edge (spec 3.5 of the state-cells design): what
// scene.state says, for a consumer that must see every one. Trivially
// copyable, so it travels through a core::EdgeInbox.
struct SceneTransition {
    enum class State : uint8_t { Stopped, Running, Background, Error, Headless, Deleted };
    // Every scene's id fits: core::valid_scene_id() allows at most
    // core::kMaxSceneIdBytes.
    FixedString<32> id;
    State state = State::Stopped;
};

// The "state" text of scene.state for `state` ("stopped", "running", ...).
const char* transition_name(SceneTransition::State state);

// What SceneRuntime::overview() holds (spec 3.4 of the state-cells
// design): a summary beside scene.state, which stays as it is.
struct SceneOverview {
    // The scene in front (running or showing its error panel), as
    // foreground() gives it; empty when none. Ids are at most
    // core::kMaxSceneIdBytes.
    FixedString<32> foreground;
    // The entries of background_scenes(), never the scene in front.
    uint16_t background_count = 0;
    // The headless scenes running (0 or 1: a second closes the first).
    // On no background list, so counted apart: a headless scene sent to
    // the background moves both fields, and the cell with them.
    uint8_t headless = 0;
    // Moves on every install, replace and delete of a package (also one
    // that failed after it changed what is stored), on a card coming or
    // going (once the card's scenes are marked) and on a dev-mode change
    // the working directory's watcher reports; wraps. A follower that
    // lists the scenes again on a move sees what a listing shows now.
    uint32_t catalog = 0;
    bool operator==(const SceneOverview&) const = default;
};

// SceneRuntime::eval()'s default budget (agent-core spec 4.2); the most
// JSON its result may take is kEvalResultMaxBytes (lua_task.hpp).
inline constexpr uint32_t kEvalMs = 200;

// run_lua() (agent-tools spec 4.1, 4.2): one slice's limit, the output
// kept, the least memory a run starts with, the source's and the run's
// limits.
inline constexpr uint32_t kLuaRunSliceMs = 200;

// The scene task's lanes (file header, "Scheduling"): after kQueueBurst
// messages in a row, one item of the scenes waits no longer; after
// kFrontBurst items of the scene in front in a row, one background item.
inline constexpr uint32_t kQueueBurst = 8;
inline constexpr uint32_t kFrontBurst = 4;
inline constexpr size_t kLuaRunOutputBytes = 8 * 1024;
inline constexpr size_t kLuaRunMinMemory = 256 * 1024;
inline constexpr size_t kLuaRunMaxMemory = 2 * 1024 * 1024;
inline constexpr size_t kLuaRunMaxSource = 64 * 1024;
inline constexpr uint32_t kLuaRunMinMs = 1000;
inline constexpr uint32_t kLuaRunMaxMs = 300000;

// call_tool() (agent-tools spec 5.1, 5.4): the range of its timeout_ms,
// and how long a headless scene stays without a call (the default of
// SceneRuntimeConfig::headless_idle_ms).
inline constexpr uint32_t kToolCallMinMs = 1000;
inline constexpr uint32_t kToolCallMaxMs = 120000;
inline constexpr uint32_t kHeadlessIdleMs = 60000;
// The least of the VM budget a headless scene starts with; less, after
// evicting the background, is "no_memory" (file header).
inline constexpr size_t kHeadlessMinMemory = 256 * 1024;

struct ToolCallResult {
    bool ok = false;
    // ok: the function's return values as JSON (none -> null, one -> it,
    // several -> an array), at most kEvalResultMaxBytes.
    os::LargeString json;
    // !ok: not_registered | scene_error | timeout | load_failed |
    // no_memory | cancelled | not_found.
    std::string error;
    // ScriptError::text() for the scene's own errors, else a short reason.
    std::string message;
};

struct LuaRunOptions {
    uint32_t timeout_ms = 30000;        // kLuaRunMinMs-kLuaRunMaxMs
    size_t memory_bytes = 1024 * 1024;  // kLuaRunMinMemory-kLuaRunMaxMemory
    // Polled every 20 ms while the run waits; true stops it ("cancelled").
    std::function<bool()> cancelled;
};

struct LuaRunResult {
    bool ok = false;
    // ok: the return values as JSON (none -> null, one -> it, several ->
    // an array), at most kEvalResultMaxBytes.
    os::LargeString json;
    // !ok: compile | runtime | timeout | memory | too_large | no_memory |
    // cancelled.
    std::string error;
    // ScriptError::text() for the script's own errors, else a short reason.
    std::string message;
    // What print and wisp.log wrote, one line each: the last
    // kLuaRunOutputBytes, with "…" in front when earlier output was dropped.
    os::LargeString output;
    uint32_t ms = 0;
    // The VM's peak heap (Vm::peak_bytes()).
    size_t memory_peak = 0;
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
    // wisp.power; null: keep_screen_on() returns nil, "unavailable". Must
    // outlive the runtime.
    core::WakeLockRegistry* locks = nullptr;
    // System notices about the scenes (SceneNotice). Called on the scene
    // task, which must not touch flash: it may only queue the work.
    std::function<void(const SceneNotice&)> on_notice;
    // wisp.notify and wisp.notify_cancel; null: notify() returns nil,
    // "unavailable". Must outlive the runtime.
    core::NotificationService* notifications = nullptr;
    // <System>/scenes/background.json (file header); empty: the list is
    // neither written nor restored.
    std::string background_list_path;
    // wisp.sensor; null: subscribe() and on() return nil, "unavailable".
    // Must outlive the runtime.
    core::SensorService* sensors = nullptr;
    // wisp.files and wisp.http.download (which also needs `http`); null:
    // they raise. Its jobs run on `worker`. Must outlive the runtime and
    // the worker's last job.
    core::SceneFiles* files = nullptr;
    // PowerMonitor::supply(): a change of charging or USB checks "battery"
    // (file header). null: the minute checks only. Followed from start()
    // to stop(); must outlive that.
    const core::StateCell<core::PowerSupply>* supply = nullptr;
    // SystemSettings::cell(): the settings in info() (its kSettingScene
    // rows), and the "minute" and "lang" their changes fire. null: info()
    // reports the schema's defaults and no setting fires an event. Read
    // on the scene task and by device_info()'s other callers (memory
    // only); followed from start() to stop().
    const core::StateCell<core::SystemSettingsValues>* settings = nullptr;
    // wisp.device.list/get/call; null: list() returns {}, get() and call()
    // nil, "unavailable". Must outlive the runtime.
    core::DeviceService* devices = nullptr;
    // Every scene.state the runtime publishes, right after the bus has it,
    // in the same order; empty: none. An id too long for SceneTransition
    // names no scene (an open of it fails to load) and is not reported.
    // Called on the scene task, possibly from deep inside a scene's
    // lifecycle (no runtime lock is held, but the task must not wait):
    // it may only hand the edge on, e.g. push it into a core::EdgeInbox.
    os::LargeFunction<void(const SceneTransition&)> on_lifecycle;
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
    // How long a headless scene stays after its last tool call (file
    // header).
    uint32_t headless_idle_ms = kHeadlessIdleMs;
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
    // TimeSyncService::result_cell(), whose owner is built after the
    // runtime: a synced attempt fires "minute" (file header). Before
    // start(); null (no time sync): nothing. Followed until stop(), which
    // must come before the cell's owner goes.
    void attach_time_sync(const core::StateCell<core::TimeSyncResult>* cell);
    // The screen went on or off. Any thread. The wanted state is kept and
    // the scene task catches up with it on its next pass, so a message
    // dropped on a full queue only delays it.
    void set_visible(bool visible);
    // The id of the scene in the foreground (running or showing its error
    // panel), if any.
    std::optional<std::string> foreground() const;
    // The scenes in the background, least recently in front first. Any
    // thread.
    std::vector<BackgroundScene> background_scenes() const;
    // foreground(), how many scenes run out of sight and the catalog
    // counter, for components that follow them instead of polling or
    // reading scene.state. Set on the scene task with foreground_id_ and
    // the background list, so at rest it answers what those queries do; the
    // catalog moves from note_catalog_changed(). A subscriber releases its
    // subscription before the runtime is destroyed (SceneWiring owns it;
    // wisp/core/state_cell.hpp).
    const core::StateCell<SceneOverview>& overview() const { return overview_; }
    // Moves the overview's catalog counter: install_scene() calls it for an
    // install or a replace, SceneWiring for a delete, a card change and a
    // dev-mode change. Any thread, without the runtime's locks; never
    // waits.
    void note_catalog_changed() override;
    // The manifest name of scene `id` while it is in the foreground or the
    // background; "" otherwise. Any thread.
    std::string running_name(std::string_view id) const;
    // UI thread only: the view of the scene in front, or null while none
    // is up (none in front, or its view not shown yet). Valid until the UI
    // thread returns to its loop.
    const SceneView* ui_current_view() const;
    // Sends a click to the foreground scene's widget with ref = `ref`, as
    // if the user tapped it (the simulator's debug command). NotFound if
    // no running scene has one.
    Result<void> click_ref(std::string_view ref);
    // Sends swipe `dir` ("up", "down", "left" or "right") over the
    // foreground scene's widget with ref = `ref`: the nearest widget at or
    // above it with on_swipe gets it, as if the user swiped there (the
    // simulator's debug command). InvalidArgument for another `dir`;
    // NotFound if no running scene has the ref, or nothing at or above it
    // has on_swipe.
    Result<void> swipe_ref(std::string_view ref, std::string_view dir);

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
    // launch(id, Boot) without the RestoreFailed notice, keeping what the
    // scene had when it last left the background by an exit (file header;
    // SceneHost).
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
    // The user tapped notification `note_id` of scene `scene_id` (scene-
    // device spec 5.1): the scene comes forward (one in the background
    // without reloading; one not running is launched with
    // LaunchReason::Notification), then scene.on_notification(note_id)
    // runs. Never waits. Busy when the runtime is not running, its queue
    // is full, or the scene is being installed or deleted; a scene that
    // turns out not to exist is only logged.
    Result<void> deliver_notification(const std::string& scene_id, const std::string& note_id);
    // Busy when the runtime is not running, or when called on the UI
    // thread or the storage worker (neither may wait); Timeout after 10 s.
    Result<void> check_sources(std::span<const core::SourceFile> files) override;
    void on_installed(const std::string& id) override;
    // The tree check of an install (agent-core spec 4.1): on the scene
    // task, a VM of its own runs the package's entry file and builds its
    // tree, then goes; nothing is shown. The VM has the scenes' own limits
    // (SceneRuntimeConfig::vm) but not the shared budget, so it never
    // evicts or starves a running scene. Its wisp.store is an empty store
    // that is never written to flash; wisp.http and wisp.files take
    // requests and never answer them; timers never fire. The error is
    // ScriptError::text() ("main.lua:3: unknown widget ...") as
    // InvalidArgument. Waits, Busy and Timeout as check_sources().
    Result<void> check_build(std::shared_ptr<const core::ScenePackage> package);

    // The agent's view into a running scene (agent-core spec 4.2), each a
    // message of its own on the scene task, for a scene in the foreground
    // or the background. They wait like open() (10 s, then Timeout), and
    // are Busy when the runtime is not running, its queue stays full, or
    // they are called on the UI thread, the scene task or the storage
    // worker. NotFound when `id` is not running (or shows its error
    // panel).
    //
    // eval(): `lua` (the chunk "eval") runs in the scene's VM with a
    // budget of `budget_ms`; the result is the JSON of its return values:
    // none -> null, one -> it, several -> an array. A Lua error, a timeout
    // or a result JSON cannot hold (or past kEvalResultMaxBytes) is
    // InvalidArgument with ScriptError::text() ("eval:1: x") and does not
    // fault the scene; only a memory fault does, or an error in the flush
    // after it, which the scene's own watches run. UI changes it makes
    // are applied as after any message.
    Result<os::LargeString> eval(const std::string& id, std::string_view lua,
                                 uint32_t budget_ms = kEvalMs);
    // SceneStore::dump_json(max_bytes) of the scene's store.
    Result<os::LargeString> store_dump(const std::string& id, size_t max_bytes);
    // SceneStore::kv_set(key, json) on the scene's store (whose errors it
    // returns), which the scene's wisp.store.get() sees at once.
    Result<void> store_set(const std::string& id, std::string_view key, std::string_view json);

    // Runs `code` (the chunk "run") on a VM of its own (file header) and
    // waits for it to end, for opts.timeout_ms at most (then "timeout"),
    // polling opts.cancelled. Its own failures are in the LuaRunResult. Busy
    // when the runtime is not running, its queue stays full, another
    // run_lua() is under way, or on the UI thread, the scene task or the
    // storage worker; InvalidArgument for options out of range or code past
    // kLuaRunMaxSource.
    Result<LuaRunResult> run_lua(std::string_view code, LuaRunOptions opts);
    // Calls tool `tool` of scene `scene_id` with `args_json` (a JSON
    // object; empty: {}) and waits for it to end, for timeout_ms at most
    // (then "timeout"), polling `cancelled` (may be empty) every 20 ms
    // (then "cancelled"). A scene that is not running is started headless
    // (file header). The call's own failures are in the ToolCallResult.
    // Busy when the runtime is not running, its queue stays full, another
    // call to the same scene is under way, the scene is being installed or
    // deleted, or on the UI thread, the scene task or the storage worker;
    // InvalidArgument for timeout_ms outside kToolCallMinMs-kToolCallMaxMs
    // or args_json that is not a JSON object.
    Result<ToolCallResult> call_tool(const std::string& scene_id, const std::string& tool,
                                     std::string_view args_json, uint32_t timeout_ms,
                                     std::function<bool()> cancelled);

    // What the scenes' VmPool holds right now (and the run_lua() VM with it).
    size_t vm_pool_used() const { return pool_.used(); }

    // Messages dropped because no running scene has their instance (a
    // Completed wake-up among them, on top of the results its box freed when
    // the VM went), and view events for a scene that has gone to the
    // background.
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
    struct FilesGate;
    // run_lua()'s VM and what goes with it (scene task only).
    struct AgentRun;
    // A call_tool() call on the scene task: loading its scene headless, or
    // running its task in the scene.
    struct ToolRun;
    // At most one scene in front, kMaxBackground behind it and one
    // headless, and for a moment one more: a scene sent to the background
    // before the eviction that follows it.
    static constexpr size_t kMaxSlots = kMaxBackground + 3;
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
        // Its place in the background list (Slot::last_front_ms, seq).
        uint64_t last_front_ms = 0;
        uint64_t seq = 0;
        // What wisp.scene.launch() reports: `reason`, or for a reopen what
        // the scene reported before.
        LaunchReason shown = LaunchReason::User;
        // A notification launch: the id scene.on_notification() gets once
        // the scene is up.
        std::string notification;
        // Loaded for a tool call, to run headless (file header).
        bool headless = false;
    };
    // A scene that left the background by an exit: what open_background()
    // gives it back (the last few, scene task only).
    struct Departed {
        std::string id;
        LaunchReason reason = LaunchReason::User;
        uint64_t last_front_ms = 0;
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
    // eval(), store_dump(), store_set(): sends the message and waits for
    // its job's answer.
    Result<os::LargeString> ask(os::LargeUnique<SceneMessage> msg,
                                const std::shared_ptr<SceneQuery>& job);
    // On the scene task: answers a query for scene `id`.
    void answer(const std::string& id, SceneMessage& m);
    bool push(os::LargeUnique<SceneMessage> msg, uint32_t timeout_ms);
    void set_status(const std::string& id, core::SceneState state, std::string error);
    // scene.state, the log line and deps_.on_lifecycle.
    void publish(const std::string& id, SceneTransition::State state);
    // SceneStoreDeps::schedule: runs `fn` on the scene task after `ms`.
    void schedule(uint32_t ms, std::function<void()> fn);

    // --- scene task ---
    // check_build()'s work (check_build.cpp).
    Result<void> run_check_build(const std::shared_ptr<const core::ScenePackage>& package,
                                 core::SceneStore& store);
    void run();
    void handle(SceneMessage& m);
    // The slot `m` is for, or nullptr (a stale message is counted).
    Slot* target(const SceneMessage& m);
    Slot* find(uint32_t instance);
    Slot* find_id(std::string_view id);
    Instances instances() const;
    size_t background_count(const Slot* except) const;
    Result<void> launch_load(const std::string& id, LaunchReason reason, Value args, bool restore,
                             bool reopen = false);
    void begin_open(const std::string& id, Value args, LaunchReason reason, bool restore,
                    std::shared_ptr<SceneWaiter> waiter, bool reopen = false,
                    std::string notification = {});
    // Hands the load of `instance` (already in loading_) to the worker;
    // false if the worker takes no job.
    bool queue_load(const std::string& id, uint32_t instance, LaunchReason reason, Value args,
                    std::shared_ptr<SceneWaiter> waiter);
    // msg::Notify (deliver_notification()).
    void notify(const std::string& id, const std::string& note_id);
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
    // Makes the slot of a loaded scene (VM not started yet), with `limits`
    // (null: config_.vm), headless or not; null without the memory for it.
    Slot* add_slot(SceneMessage& m, LaunchReason reason, const VmLimits* limits = nullptr,
                   bool headless = false);
    // Gives the scene's wisp.http its proxy: the declared hosts, and
    // results that come back into the slot's box.
    void wire_http(Slot& s);
    // wire_http()'s and wire_files()'s work on `api`, for scene `id`'s
    // `instance`, whose results go into `box`. `hosts`: what a download is
    // checked against; null for the Agent principal, whose URLs SceneHttp
    // checks its own way.
    void wire_http_api(SceneApi& api, const std::string& id, uint32_t instance,
                       const std::shared_ptr<CompletionBox>& box);
    void wire_files_api(SceneApi& api, const std::string& id, uint32_t instance,
                        const std::shared_ptr<CompletionBox>& box,
                        const std::shared_ptr<FilesGate>& gate,
                        const std::vector<std::string>* hosts);
    // cancel_files()'s work on a gate.
    void close_gate(const std::shared_ptr<FilesGate>& gate);
    // Gives the scene's wisp.sensor the service, and batches that come
    // back as Sensor messages for this instance.
    void wire_sensors(Slot& s);
    // Gives the scene's wisp.files its jobs on the worker and its
    // downloads, whose results come back into the slot's box.
    void wire_files(Slot& s);
    // The scene is closing or faulted: its downloads stop (each .dl is
    // removed on the worker) and none starts after this.
    void cancel_files(Slot& s);
    // "Back" for the scene in front: to the background if it declares it
    // (and is running), closed otherwise. `except` is not counted towards
    // the background's limit (the scene about to come forward).
    void go_back(Slot& s, std::shared_ptr<SceneWaiter> done, const Slot* except = nullptr);
    void to_background(Slot& s, std::shared_ptr<SceneWaiter> done, const Slot* except);
    void resume(Slot& s, std::shared_ptr<SceneWaiter> done);
    // A boot launch or open_background() of a scene that is headless and
    // declares "background": it goes there as it is (to_background()).
    void headless_to_background(Slot& s, std::shared_ptr<SceneWaiter> done);
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
    // SceneInstance::start() after comparing the data version with the
    // manifest's (spec 7.3): a new store takes the manifest's, an older
    // one is migrated, and a newer one is refused before any VM exists.
    std::optional<ScriptError> start_scene(Slot& s);
    void fault(Slot& s, const ScriptError& err);
    void release_locks(Slot& s);
    void notice(SceneNoticeKind kind, const Slot& s);
    // Rewrites background_ from the slots.
    void update_background_list();
    // With status_mu_ held, on the scene task, in the same hold as every
    // write of foreground_id_ and background_: brings the overview's
    // foreground, background_count and headless to them and the headless
    // slot, leaving its catalog alone.
    void refresh_overview_locked();
    void hide_scenes();
    void show_scene(bool screen_came_on);
    // Hides or shows to match want_visible_ if visible_ differs.
    void follow_visibility();
    void run_delayed();
    // The lanes (file header, "Scheduling"). The per-item steps that do
    // not belong to any lane, in their order.
    void housekeeping();
    // Whether `s` (running) has a result or a timer due.
    bool slot_has_work(Slot& s, uint64_t now);
    bool front_has_work(uint64_t now);
    bool back_has_work(uint64_t now);
    void run_front_item();
    void run_back_item();
    // One result of `s`, else one due timer; false if it had neither.
    bool run_item(Slot& s, uint64_t now);
    // Runs the first of Slot::due_timers (not empty).
    void run_due_timer(Slot& s);
    // Every timer of `front` due now (show_scene(), spec 3.4's order).
    void run_timers_of(Slot& front);
    void drain_queue();
    // How long the task may wait for the scenes' timers and minute tickers.
    uint32_t scene_wait_ms();
    // The agent run's deadline, then the minutes (and the battery checks
    // with them).
    void run_scene_clock();
    // A network time sync set the wall clock (an attempt that synced).
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

    // run_lua() on the scene task (scene_runtime_lua.cpp).
    void lua_start(const std::shared_ptr<LuaRunJob>& job);
    // The caller stopped waiting (`cancelled`) or the runtime stops.
    void lua_stop(const LuaRunJob* job, const char* error, std::string message);
    // After each step of the run: ends it once its task has ended or `err`
    // (what the step returned) stopped it.
    void lua_settle(const std::optional<ScriptError>& err);
    // Ends the run: the result goes to its caller, every request and
    // download of the VM is cancelled and the VM goes.
    void lua_end(LuaRunResult r);
    // Stops the run at its deadline.
    void lua_clock();
    // The run has a result or a wisp.sleep timer due.
    bool agent_has_work(uint64_t now);
    // One result of the run, else one due timer (the back lane).
    void run_agent_item(uint64_t now);
    // How long the task may wait for the run's timers and deadline.
    uint64_t lua_wait_ms(uint64_t now) const;

    // call_tool() on the scene task (scene_runtime_tools.cpp).
    void tool_call(const std::shared_ptr<ToolCallJob>& job);
    // The caller cancelled.
    void tool_stop(const ToolCallJob* job);
    // Starts the call of `job` in the running scene `s`.
    void tool_start(Slot& s, const std::shared_ptr<ToolCallJob>& job);
    // The load of a headless scene arrived (finish_open()).
    void finish_headless(SceneMessage& m, const PendingLoad& load);
    // How much of the VM budget a headless scene may have, after evicting
    // background scenes for it (file header).
    size_t headless_room();
    // SceneApi::task_done of the slot of `instance`: records the outcome.
    void tool_task_done(uint32_t instance, uint32_t task, TaskOutcome outcome);
    // Ends the call of `job` with `r`; a headless scene left without a call
    // starts its idle time.
    void tool_end(const ToolCallJob* job, ToolCallResult r);
    // A call taken off tool_runs_ whose caller is still to hear `result`.
    struct ToolEnding {
        std::shared_ptr<ToolCallJob> job;
        ToolCallResult result;
    };
    // Takes off the call running in the slot of `instance` (it closes or
    // faults): with its task's outcome if the task already ended and
    // `keep_outcome` (a close), else with `error`. Its caller hears it from
    // tool_answer(), once the scene's state says what happened.
    std::optional<ToolEnding> tool_abort(uint32_t instance, const char* error,
                                         const std::string& message, bool keep_outcome);
    void tool_answer(std::optional<ToolEnding> ending);
    // Ends every call still loading a scene whose load left loading_.
    void tool_loads_dropped();
    // Ends the calls whose task has ended (after each pass of the task).
    void tool_settle();
    // Ends the calls whose timeout_ms has passed.
    void tool_clock();
    uint64_t tool_wait_ms(uint64_t now) const;
    // The headless slot of `instance` closes if `gen` is still its idle
    // generation and no call is under way.
    void headless_idle(uint32_t instance, uint64_t gen);

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
    std::string foreground_name_;  // while foreground_id_ is set
    uint32_t foreground_instance_ = 0;
    std::vector<BackgroundScene> background_;
    // id -> instance of every slot, for scene_instance().
    std::map<std::string, uint32_t, std::less<>> slot_instances_;
    // foreground and the two counts are set with status_mu_ held (lock
    // order status_mu_ -> the cell), catalog from any thread; all through
    // update(), so neither undoes the other. Declared after deps_, whose
    // bus its constructor takes.
    core::StateCell<SceneOverview> overview_{deps_.bus, SceneOverview{}};

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
    // Departed entries, newest last, at most kMaxSlots.
    std::vector<Departed> departed_;
    // Something the background list depends on (the slots, the loads)
    // changed since persist_background() last compared it.
    bool list_dirty_ = false;
    // A write could not be queued and that was logged.
    bool persist_warned_ = false;
    // The loads on the worker; one that comes back and is not here was
    // superseded, or cancelled by a close of its scene.
    std::vector<PendingLoad> loading_;
    bool visible_ = true;
    // What set_visible() last asked for; the scene task brings visible_
    // to it after every pass (follow_visibility()).
    std::atomic<bool> want_visible_{true};
    bool quit_ = false;
    // The device checks the cells asked for and the scene task has not
    // run yet (file header): set by the cells' callbacks on wisp.bus,
    // taken whole by take_device_changes().
    static constexpr uint8_t kBattery = 1, kMinute = 2, kLang = 4, kClockSet = 8;
    std::atomic<uint8_t> pending_device_{0};
    // Sets `bits` and wakes the scene task. Any thread; never waits.
    void note_device(uint8_t bits);
    // Scene task: runs the checks pending_device_ holds.
    void take_device_changes();
    const core::StateCell<core::TimeSyncResult>* time_sync_ = nullptr;
    // Made in start(), released in stop() before anything else: the
    // time-sync cell's owner goes before the runtime.
    core::CellSubscription supply_sub_;
    core::CellSubscription settings_sub_;
    core::CellSubscription time_sub_;

    // UI thread only.
    struct UiSide;
    os::LargeUnique<UiSide> ui_side_;

    // post_ui()'s jobs not yet run.
    os::Mutex ui_jobs_mu_;
    os::LargeVector<UiJob> ui_jobs_;

    // A run_lua() call is under way (any thread).
    std::atomic<bool> lua_busy_{false};
    // The scenes with a call_tool() under way (any thread).
    os::Mutex tool_mu_;
    std::vector<std::string> tool_scenes_;
    // Scene task only.
    std::vector<ToolRun> tool_runs_;
    // Scene task only; destroyed before pool_ (declared after it).
    os::LargeUnique<AgentRun> agent_;
    // Scene task only: queue messages and front items run in a row while
    // another lane waited, and the instance the back lane ran last.
    uint32_t queue_run_ = 0, front_run_ = 0, back_cursor_ = 0;
};

}  // namespace wisp::scene
