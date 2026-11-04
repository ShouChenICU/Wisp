#pragma once

// The wisp.timer, wisp.device, wisp.log and wisp.scene bindings
// (scene-runtime-core spec 5.1, 5.2, 5.5). Thin adapters: the state they
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
//       y2}, lang, time, battery, charging, wifi}; time and battery are
//       nil when unknown.
//   wisp.device.on("battery" | "minute" | "lang", fn): fn(info) each time
//       the runtime calls run_device_handlers() for that event; another
//       name is an argument error.
//   wisp.log.info/warn/error(...): log_lua_args() at that level, the same
//   line print makes.
//   wisp.scene.close(): sets close_requested; the runtime closes the
//       scene once the current message is done.
//   wisp.scene.args(): a fresh table copied from *args each call ({} when
//       there are none).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>

#include "wisp/base/value.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/display_geometry.hpp"
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

namespace detail {

// For the bind_*.cpp files; all follow the longjmp rule (vm.hpp).
SceneApi& api_of(lua_State* L);
void push_device_info(lua_State* L, const DeviceInfo& info);
// Each sets its table as the field of the table at `wisp`.
void install_timer(lua_State* L, int wisp);
void install_device(lua_State* L, int wisp);
void install_log(lua_State* L, int wisp);
void install_scene(lua_State* L, int wisp);

}  // namespace detail

}  // namespace wisp::scene
