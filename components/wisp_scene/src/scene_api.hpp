#pragma once

// The wisp.timer, wisp.device, wisp.log, wisp.scene and wisp.store bindings
// (scene-runtime-core spec 5.1, 5.2, 5.4, 5.5). Thin adapters: the state they
// act on is a SceneApi, which the SceneInstance owns and the scene task
// (SceneRuntime) fills in and drives. Everything here runs on the scene
// task, which touches no flash: SceneApi::device must not either.
//
//   wisp.timer.after(ms, fn) / every(ms, fn)  -> handle, or nil and
//       "interval_too_small" / "too_many_timers" (TimerSet's limits).
//       handle:cancel() -> true if the callback had not run yet (for an
//       every timer: was still set). The callbacks live in a registry
//       table by timer id; the runtime runs the ids TimerSet::due() hands
//       out with run_timer(), each with a fresh time budget.
//   wisp.device.info() -> {width, height, shape, safe_area = {x1, y1, x2,
//       y2}, lang, utc_offset, time, battery, charging, wifi}; time and
//       battery are nil when unknown; utc_offset is in minutes east of UTC.
//   wisp.device.on("battery" | "minute" | "lang", fn): fn(info) each time
//       the runtime calls run_device_handlers() for that event; another
//       name is an argument error.
//   wisp.log.info/warn/error(...): log_lua_args() at that level, the same
//   line print makes.
//   wisp.scene.close(): sets close_requested; the runtime closes the
//       scene once the current message is done.
//   wisp.scene.args(): a fresh table copied from *args each call ({} when
//       there are none).
//   wisp.store (bind_store.cpp): the scene's SceneStore, values converted
//       with lua_json.hpp. Every call works on the store's in-memory
//       index; persisting is the store's own job on the wisp.fs worker.
//   wisp.http (bind_http.cpp): the scene's requests go to the SceneHttp
//       proxy through SceneApi::http_submit; each result comes back to the
//       scene task as a message, and run_http_callback() calls the
//       scene's callback with it.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>

#include "wisp/base/value.hpp"
#include "wisp/core/scene_store.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/net/scene_http.hpp"
#include "wisp/os/os.hpp"
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
    char lang[4] = "zh";
    int32_t utc_offset_min = 480;
    bool has_time = false;
    int64_t time = 0;  // seconds since the epoch
    bool has_battery = false;
    int64_t battery = 0;  // percent
    bool charging = false;
    char wifi[12] = "off";  // off | connecting | backoff | connected
};
static_assert(std::is_trivially_destructible_v<DeviceInfo>);

// One scene's binding state. Must outlive the VM it is installed in.
struct SceneApi {
    TimerSet timers;
    // The timers' clock.
    std::function<uint64_t()> now_ms = os::now_ms;
    // wisp.device.info(); runs on the scene task, so it must not touch
    // flash. Empty: a default DeviceInfo.
    std::function<DeviceInfo()> device;
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
    bool close_requested = false;
    // wisp.device.on() was called for the event at least once.
    bool has_handlers[kDeviceEventCount] = {};

    bool wants(DeviceEvent ev) const { return has_handlers[static_cast<size_t>(ev)]; }
};

// Adds the four tables to `wisp`. Call once, before any scene code runs.
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

}  // namespace detail

}  // namespace wisp::scene
