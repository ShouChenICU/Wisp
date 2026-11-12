#pragma once

// The wisp.timer, wisp.device, wisp.log, wisp.scene, wisp.store, wisp.http,
// wisp.power, wisp.notify, wisp.sensor and wisp.files bindings (scene-runtime-core spec
// 5.1-5.5, scene-device spec 3.2-3.4, 5.1, 6.2, 7.1). Thin adapters: the state they act on is a
// SceneApi, which the SceneInstance owns and the scene task (SceneRuntime) fills in and drives.
// Everything here runs on the scene task, which touches no flash: SceneApi::device must not either.
//
//   wisp.timer.after(ms, fn) / every(ms, fn)  -> handle, or nil and
//       "interval_too_small" / "too_many_timers" (TimerSet's limits).
//       handle:cancel() -> true if the callback had not run yet (for an
//       every timer: was still set). The callbacks live in a registry
//       table by timer id; the runtime runs the ids TimerSet::due() hands
//       out with run_timer(), each with a fresh time budget.
//   wisp.device.info() -> {width, height, shape, corner_radius, safe_area =
//       {x1, y1, x2, y2}, lang, utc_offset, time, battery, charging, wifi};
//       time and battery are nil when unknown; utc_offset is in minutes
//       east of UTC; corner_radius is the rounded rect's radius, half the
//       smaller side for a circle, 0 for a rect.
//   wisp.device.row_span(y) -> x1, x2: the visible columns of screen row
//       y (hal::row_span() of SceneApi::screen), or nil when the row has
//       none or is off the screen. Scenes own the whole screen and lay
//       themselves out with these (AGENTS.md section 3).
//   wisp.device.on("battery" | "minute" | "lang", fn): fn(info) each time
//       the runtime calls run_device_handlers() for that event; another
//       name is an argument error.
//   wisp.log.info/warn/error(...): log_lua_args() at that level, the same
//   line print makes.
//   wisp.scene.close(): sets close_requested; once the current message is
//       done the runtime goes "back": a scene that declares "background"
//       goes to the background, any other closes.
//   wisp.scene.exit(): sets exit_requested; the runtime closes the scene
//       for good once the current message is done.
//   wisp.scene.state() -> "foreground" | "background" | "headless"
//       (SceneApi::background, SceneApi::headless).
//   wisp.scene.launch() -> "user" | "boot" | "notification"
//       (SceneApi::launch).
//   wisp.power.keep_screen_on(on) (bind_power.cpp): in the foreground takes
//       (true) or drops (false) a "screen" wake lock held by "scene.<id>"
//       and returns true; nil, "not_in_background" in the background; nil,
//       "unavailable" without a WakeLockRegistry.
//   wisp.notify{title, body, sound, wake, id} (bind_notify.cpp) -> id, or
//       nil, "rate_limited" | "unavailable" (no NotificationService). A
//       bad field raises an argument error naming it. Posted with the
//       scene's id and name, as the foreground scene's own post while
//       the scene is in front (shown as a banner, not kept). Works in the
//       background.
//   wisp.notify_cancel(id) -> true if an unread notification of this
//       scene with that id was removed.
//   wisp.sensor (bind_sensor.cpp), over core::SensorService:
//       subscribe(kind, {rate = n}, fn) -> handle: kind "accel" or "gyro",
//       rate 10, 25, 50 or 100; the options may be nil or left out
//       (subscribe(kind, fn)), and rate then defaults to 25;
//       fn(samples) gets { {x =, y =, z =, t =}, ... } every 100 ms or so
//       (m/s², or degrees per second for gyro; t in ms).
//       on("shake" | "flip", fn) -> handle; fn(name) once per gesture.
//       handle:stop() -> true if it was still subscribed.
//       available() -> {accel = true, gyro = true} as the IMU has them,
//       {} without one.
//       subscribe and on: nil, "not_in_background" in the background;
//       nil, "unavailable" without the service or the sensor, or the
//       service's error as its snake_case code ("busy", "no_memory"); a bad kind,
//       rate or gesture raises an argument error naming it.
//       The runtime pauses a scene's subscriptions while it is in the
//       background and resumes them in front (pause_sensors()), and ends
//       them when it closes or faults (release_sensors()).
//   wisp.scene.args(): a fresh table copied from *args each call ({} when
//       there are none).
//   wisp.store (bind_store.cpp): the scene's SceneStore, values converted
//       with lua_json.hpp. Every call works on the store's in-memory
//       index; persisting is the store's own job on the wisp.fs worker.
//   wisp.http (bind_http.cpp): the scene's requests go to the SceneHttp
//       proxy through SceneApi::http_submit; each result comes back to the
//       scene task as a message, and run_http_callback() calls the
//       scene's callback with it.
//   wisp.files (bind_files.cpp; scene-device spec 7.1), over
//       core::SceneFiles, every call asynchronous:
//       write(name, data, cb), append(name, data, cb) -> cb(err, true);
//       read(name, offset, len, cb) -> cb(err, data);
//       list(cb) -> cb(err, { {name =, size =, mtime =}, ... });
//       remove(name, cb) -> cb(err, true);
//       info(cb) -> cb(err, {location = "sd" | "int" | nil, quota =, used =}).
//       wisp.http.download(url, name, [opts], cb) (bind_http.cpp) ->
//       cb(err, {status =, size =, deferred =}); only a 2xx body is kept.
//       Each job goes to the wisp.fs worker through SceneApi::files_run
//       (files_download for a download) and its FilesResult comes back as
//       a message for run_files_callback(), which calls cb with a fresh
//       budget. A bad name is answered "bad_name" through files_fail,
//       also as a message. err is bad_name, too_large, quota_exceeded,
//       unavailable, not_found or another snake_case storage code, and
//       for a download also a wisp.http transport or proxy error. Data,
//       names and arguments that are not strings or integers raise
//       argument errors.
//   wisp.json (bind_json.cpp; agent-tools spec 4.2): encode(v) -> string,
//       or nil and not_serializable | too_deep | too_large; decode(s) ->
//       value, or nil and bad_json | too_deep. The rules are lua_json.hpp's
//       (null decodes to nil); output is capped at kLuaJsonMaxBytes. A
//       decode argument that is not a string raises an argument error.
//   wisp.sleep(ms) (bind_timer.cpp): suspends the running task for ms
//       (0-300000; below TimerSet::kMinIntervalMs it waits that long) with
//       an after timer, and returns nothing. Outside a task, or with ms
//       out of range, an argument error; too many timers raises an error.
//   wisp.tools.register(name, fn) (bind_tools.cpp; agent-tools spec 5.2):
//       binds `fn` to a tool `name` the scene declared in scene.json's
//       "tools" array (SceneApi::tool_names, filled by the runtime from
//       the manifest when it loads the scene); kept in a table in the
//       registry, by name. A `name` the manifest did not declare, one
//       already bound, or a non-function `fn` raise an argument error.
//       Ruling: the spec shows main.lua calling this at its top level
//       (load time), but nothing here requires that -- a call made later
//       only binds a name already declared, which is harmless, so this
//       works any time. SceneRuntime::call_tool() runs `fn` as a task
//       coroutine (SceneInstance::tool(), through detail::push_tool()).
//
// Awaitable calls (agent-tools spec 6, lua_task.hpp): in a task coroutine
// (start_task()), every wisp.http and wisp.files call above may leave out
// its callback; it then suspends the task and returns the callback's
// arguments (err, res; list adds truncated) once the result's message has
// arrived and run_http_callback() / run_files_callback() resumed it. The
// callback table holds the task's thread in place of a function. Outside a
// task, or in a coroutine the script made, a missing callback is the same
// argument error as before. Results must come back as later messages,
// never from inside http_submit / files_run / *_fail themselves.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "wisp/base/value.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/scene_files.hpp"
#include "wisp/core/scene_store.hpp"
#include "wisp/core/sensor_service.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/net/scene_http.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"
#include "wisp/scene/lua_task.hpp"
#include "wisp/scene/timer_set.hpp"
#include "wisp/scene/vm.hpp"

namespace wisp::scene {

enum class DeviceEvent : uint8_t { Battery, Minute, Lang };
inline constexpr size_t kDeviceEventCount = 3;

// "battery", "minute", "lang".
const char* device_event_name(DeviceEvent ev);

// What wisp.device.info() reports. Trivially destructible, so a binding
// can keep one in its frame across calls that may raise.
struct DeviceInfo {
    hal::ScreenShape shape;
    hal::Area safe;
    // The status capsule's box (ui::SystemLayer::status_area()), shown or
    // not.
    hal::Area status_area;
    char lang[4] = "zh";
    int32_t utc_offset_min = 480;
    bool time_24h = true;
    bool has_time = false;
    int64_t time = 0;  // seconds since the epoch
    bool has_battery = false;
    int64_t battery = 0;  // percent
    bool charging = false;
    char wifi[12] = "off";  // off | connecting | backoff | connected
};
static_assert(std::is_trivially_destructible_v<DeviceInfo>);

// What a wisp.http.download callback gets.
struct FilesDownloaded {
    int status = 0;
    uint64_t size = 0;
    bool deferred = false;
};

// A wisp.files job's answer: an error code, or the payload (monostate is
// `true`, a buffer is read()'s data).
using FilesPayload = std::variant<std::monostate, os::LargeBuffer, core::SceneFileList,
                                  core::SceneFilesInfo, FilesDownloaded>;
struct FilesResult {
    std::string error;
    FilesPayload payload;
};

// Runs on the wisp.fs worker with the scene's id.
using FilesJob = std::function<FilesResult(core::SceneFiles& files, const std::string& scene)>;

// One scene's binding state. Must outlive the VM it is installed in.
struct SceneApi {
    TimerSet timers;
    // The timers' clock.
    std::function<uint64_t()> now_ms = os::now_ms;
    // wisp.device.info(); runs on the scene task, so it must not touch
    // flash. Empty: a default DeviceInfo.
    std::function<DeviceInfo()> device;
    // The screen wisp.device.row_span() answers for; pure geometry on the
    // scene task. Zero-sized by default: every row is nil.
    hal::ScreenShape screen;
    // The open arguments; null for none.
    const Value* args = nullptr;
    // The scene's data; wisp.store raises without one. Owned by the
    // runtime, which drops the VM before it closes the store.
    core::SceneStore* store = nullptr;
    // wisp.http: sends `req` (method, url, headers, body filled in) for
    // callback `id`; its result must come back to run_http_callback(id)
    // on the scene task. Empty: wisp.http raises.
    std::function<void(uint32_t id, net::SceneHttpRequest req)> http_submit;
    // Answers callback `id` with `error` the same way, without a request
    // (a table body that cannot be encoded).
    std::function<void(uint32_t id, const char* error)> http_fail;
    uint32_t next_http_id = 1;
    // wisp.files: runs `job` on the worker and brings its result back to
    // run_files_callback(id) on the scene task. Empty: wisp.files raises.
    std::function<void(uint32_t id, FilesJob job)> files_run;
    // Answers callback `id` with `error` without a job (a bad name).
    std::function<void(uint32_t id, const char* error)> files_fail;
    // wisp.http.download: `req` (url, headers) into file `name`; the
    // result comes back like files_run's. Empty: download raises.
    std::function<void(uint32_t id, net::SceneHttpRequest req, std::string name)> files_download;
    uint32_t next_files_id = 1;
    bool close_requested = false;
    bool exit_requested = false;
    // In the background: no view, and the APIs of spec 3.4 refuse with
    // "not_in_background". Set by the runtime.
    bool background = false;
    // Headless (agent-tools spec 5.4): `background` is set as well, so the
    // same APIs refuse; only wisp.scene.state() tells the two apart.
    bool headless = false;
    // What wisp.scene.launch() reports; a string literal.
    const char* launch = "user";
    // wisp.power.keep_screen_on(): the registry (null: "unavailable"), the
    // holder name ("scene.<id>") and the screen lock it holds (0: none).
    // The runtime releases every lock of the holder when the scene leaves
    // the foreground or closes, and then clears screen_lock.
    core::WakeLockRegistry* locks = nullptr;
    std::string holder;
    uint32_t screen_lock = 0;
    // wisp.notify: the service (null: "unavailable"), and the scene's id
    // and display name its notifications carry.
    core::NotificationService* notifications = nullptr;
    std::string scene_id;
    std::string scene_name;
    // wisp.sensor: the service (null: "unavailable") and how a batch for
    // the binding's handle `id` (null: a gesture) reaches the scene task
    // as msg::Sensor. sensor_post runs on the service's "wisp.imu" task,
    // which must not touch flash (core::SensorService), so it only queues
    // the message. Empty: nothing is delivered.
    core::SensorService* sensors = nullptr;
    std::function<void(uint32_t id, const core::SensorBatch* batch)> sensor_post;
    // The binding's handle ids with the service handles behind them.
    struct SensorHandle {
        uint32_t id = 0;
        uint32_t service = 0;
    };
    std::vector<SensorHandle> sensor_handles;
    uint32_t next_sensor_id = 1;
    // wisp.device.on() was called for the event at least once.
    bool has_handlers[kDeviceEventCount] = {};
    // Which principal wisp.http's requests go out as (SceneHttp's rules
    // for it); the agent's own VM sets Agent.
    net::HttpPrincipal http_principal = net::HttpPrincipal::Scene;
    // Task coroutines (lua_task.hpp): told once per task when it ends; the
    // default slice (0: the VM's exec_ms) and result cap of start_task().
    TaskDone task_done;
    uint32_t task_slice_ms = 0;
    size_t task_max_json = kEvalResultMaxBytes;
    // The live tasks, kept by lua_task.cpp.
    struct TaskSlot {
        uint32_t id = 0;
        uint32_t slice_ms = 0;
        size_t max_json = kEvalResultMaxBytes;
        // What it waits for while suspended: callback `wait_id` in the
        // registry table at `wait_key`, a wisp.sleep timer when
        // `wait_timer` (cancel_tasks() drops both).
        const void* wait_key = nullptr;
        uint32_t wait_id = 0;
        bool wait_timer = false;
    };
    std::vector<TaskSlot> tasks;
    uint32_t next_task_id = 1;
    // wisp.tools.register: the full set of names scene.json declared
    // (SceneToolDecl::name), filled by the runtime when it loads the
    // scene. Names bound to a function live in a separate table in the
    // registry (bind_tools.cpp); this is only the declared set
    // wisp.tools.register checks against.
    std::vector<std::string> tool_names;

    bool wants(DeviceEvent ev) const { return has_handlers[static_cast<size_t>(ev)]; }
};

// Adds the binding tables to `wisp`. Call once, before any scene code runs.
std::optional<ScriptError> install_api(Vm& vm, SceneApi& api);

// Runs timer `id`'s callback with a fresh budget, if it still has one.
// `once` (an after timer, or one TimerSet no longer holds): the callback
// is dropped first.
std::optional<ScriptError> run_timer(Vm& vm, uint32_t id, bool once);

// Calls every wisp.device.on(ev) handler, in the order they were added,
// with a table like info()'s, all under one fresh budget.
std::optional<ScriptError> run_device_handlers(Vm& vm, DeviceEvent ev, const DeviceInfo& info);

// Calls wisp.http callback `id` (and forgets it) with cb(err, nil), or
// cb(nil, {status, headers, body, json}), with a fresh budget. Nothing
// happens if the scene has no such callback.
std::optional<ScriptError> run_http_callback(Vm& vm, uint32_t id, const net::SceneHttpResult& r);

// Calls wisp.files (or wisp.http.download) callback `id` (and forgets it)
// with cb(err, nil) or cb(nil, result), with a fresh budget. Nothing
// happens if the scene has no such callback.
std::optional<ScriptError> run_files_callback(Vm& vm, uint32_t id, const FilesResult& r);

// Calls wisp.sensor callback `id` with the samples, or for a gesture with
// its name, with a fresh budget. Nothing happens if the handle was stopped.
std::optional<ScriptError> run_sensor_callback(Vm& vm, uint32_t id, const core::SensorBatch& batch);

// Pauses (true) or resumes every wisp.sensor subscription of the scene.
void pause_sensors(SceneApi& api, bool paused);
// Ends every wisp.sensor subscription of the scene. Idempotent.
void release_sensors(SceneApi& api);

namespace detail {

// For the bind_*.cpp files; all follow the longjmp rule (vm.hpp).
SceneApi& api_of(lua_State* L);
void push_device_info(lua_State* L, const DeviceInfo& info);
// Each sets its table as the field of the table at `wisp`.
void install_timer(lua_State* L, int wisp);
void install_device(lua_State* L, int wisp);
void install_log(lua_State* L, int wisp);
void install_scene(lua_State* L, int wisp);
void install_store(lua_State* L, int wisp);
void install_http(lua_State* L, int wisp);
void install_power(lua_State* L, int wisp);
// Sets wisp.notify and wisp.notify_cancel.
void install_notify(lua_State* L, int wisp);
void install_sensor(lua_State* L, int wisp);
void install_files(lua_State* L, int wisp);
// Sets wisp.json.
void install_json(lua_State* L, int wisp);
// Sets wisp.tools.
void install_tools(lua_State* L, int wisp);
// Pushes the function wisp.tools.register bound to `name` and returns
// true; false (nothing pushed) if no scene code has registered that name
// yet (SceneRuntime::call_tool()'s "not_registered"). Raises only on a
// memory error: call it inside Vm::call().
bool push_tool(lua_State* L, std::string_view name);
// Keeps the function (or waiting task thread) at `cb` as a wisp.files
// callback and returns its id.
uint32_t register_files_callback(lua_State* L, SceneApi& api, int cb);
// Pushes `v` as a Lua value (maps as tables, lists as sequences). Raises
// past 32 levels of nesting.
void push_value(lua_State* L, const Value& v);

// For an awaitable binding whose callback (at `cb`) was left out: outside
// a task (or in a coroutine the script made) raises the argument error a
// missing callback always got; in a task that cannot yield here raises an
// error; otherwise pushes the running thread, which the binding registers
// in place of the callback before it submits its job and returns wait().
void push_waiter(lua_State* L, int cb);
// The callback at `cb`, with the stack cut to it: a function is kept
// (false); nil or none is replaced by push_waiter()'s thread (true, the
// call waits); anything else raises an argument error.
bool take_callback(lua_State* L, int cb);
// Suspends the running task until resume_waiting() resumes it, noting
// that it waits for callback `id` in the registry table at `key` (and,
// with `timer`, for TimerSet timer `id`) so cancel_tasks() can drop the
// wait. A binding returns this, with only trivially destructible locals
// in its frame.
int wait(lua_State* L, const void* key, uint32_t id, bool timer);
// wait() for wisp.files callback `id` (register_files_callback()).
int wait_files(lua_State* L, uint32_t id);
// True when callback `id` in the registry table at `key` is a waiting task
// thread. Non-raising.
bool is_waiting(lua_State* L, const void* key, uint32_t id);
// Forgets callback `id` (a thread, see is_waiting()) and resumes its task
// with the values `build` (run under Vm::call() with `ctx` as its light
// userdata argument; null: none) returns, as a fresh slice. A task that
// has ended or was cancelled is not resumed. Returns the VM's fault, if
// any; the task's own outcome goes to task_done.
std::optional<ScriptError> resume_waiting(Vm& vm, const void* key, uint32_t id, lua_CFunction build,
                                          void* ctx);
// The SceneApi install_api() put in the registry, or null in a VM that
// has none (the ui table alone, in tests).
SceneApi* find_api(lua_State* L);

}  // namespace detail

}  // namespace wisp::scene
