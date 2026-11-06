// wisp.sensor (scene_api.hpp; scene-device spec 6.2): a thin adapter over
// core::SensorService. The service calls back on its own task; the
// callbacks given to it only hand the batch to SceneApi::sensor_post,
// which queues a msg::Sensor for the scene task, where
// run_sensor_callback() calls the scene's function.
//
// longjmp rule (vm.hpp): each binding checks its arguments with raising
// Lua calls while its frame holds only trivially destructible locals, and
// does the C++ work (std::function copies, the service, the handle list)
// in helpers whose objects are gone before Lua is touched again. A
// callback's registry slot is set before the service is asked, so a
// memory error there leaves nothing subscribed.

#include <algorithm>
#include <cstdint>
#include <optional>

#include "scene_api.hpp"

namespace wisp::scene {

namespace {

using core::Gesture;
using core::SensorKind;
using core::SensorService;

// Registry keys: the scene's functions by handle id, and for gesture
// handles the gesture's name by handle id.
char kCallbacksKey;
char kGesturesKey;
constexpr const char* kHandleType = "wisp.sensor";
constexpr lua_Integer kDefaultRate = 25;

int fail(lua_State* L, const char* code) {
    lua_pushnil(L);
    lua_pushstring(L, code);
    return 2;
}

// What the service said, as the code a scene sees; null on success.
const char* code_of(ErrorCode code) { return code == ErrorCode::Busy ? "busy" : "unavailable"; }

const char* do_subscribe(SceneApi& api, uint32_t id, SensorKind kind, uint32_t rate) {
    auto post = api.sensor_post;
    auto r = api.sensors->subscribe(kind, rate, api.holder,
                                    [post = std::move(post), id](const core::SensorBatch& b) {
                                        if (post) post(id, &b);
                                    });
    if (!r) return code_of(r.error().code);
    api.sensor_handles.push_back({id, r.value()});
    return nullptr;
}

const char* do_on(SceneApi& api, uint32_t id, Gesture g) {
    auto post = api.sensor_post;
    auto r = api.sensors->on_gesture(g, api.holder, [post = std::move(post), id] {
        if (post) post(id, nullptr);
    });
    if (!r) return code_of(r.error().code);
    api.sensor_handles.push_back({id, r.value()});
    return nullptr;
}

// Ends handle `id`; false if it had ended already.
bool do_stop(SceneApi& api, uint32_t id) {
    auto it = std::find_if(api.sensor_handles.begin(), api.sensor_handles.end(),
                           [&](const SceneApi::SensorHandle& h) { return h.id == id; });
    if (it == api.sensor_handles.end()) return false;
    if (api.sensors != nullptr) api.sensors->unsubscribe(it->service);
    api.sensor_handles.erase(it);
    return true;
}

// Sets (or, with `fn` 0, clears) the registry slots of handle `id`.
void set_slots(lua_State* L, uint32_t id, int fn, const char* gesture) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kCallbacksKey);
    if (fn != 0)
        lua_pushvalue(L, fn);
    else
        lua_pushnil(L);
    lua_rawseti(L, -2, id);
    lua_pop(L, 1);
    if (gesture == nullptr && fn != 0) return;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kGesturesKey);
    if (fn != 0)
        lua_pushstring(L, gesture);
    else
        lua_pushnil(L);
    lua_rawseti(L, -2, id);
    lua_pop(L, 1);
}

enum class Want : uint8_t { Samples, Gesture };

// The part of subscribe()/on() after the arguments were checked; the
// function is at index `fn`.
int add(lua_State* L, int fn, Want want, SensorKind kind, uint32_t rate, Gesture g) {
    SceneApi& api = detail::api_of(L);
    // Spec 3.4: a scene in the background cannot start reading sensors.
    if (api.background) return fail(L, "not_in_background");
    if (api.sensors == nullptr) return fail(L, "unavailable");
    luaL_checkstack(L, 6, "wisp.sensor");
    auto* handle = static_cast<uint32_t*>(lua_newuserdatauv(L, sizeof(uint32_t), 0));
    *handle = 0;
    luaL_setmetatable(L, kHandleType);
    const int ud = lua_gettop(L);
    const uint32_t id = api.next_sensor_id++;
    const char* name = want == Want::Gesture ? core::to_string(g) : nullptr;
    set_slots(L, id, fn, name);
    const char* code =
        want == Want::Gesture ? do_on(api, id, g) : do_subscribe(api, id, kind, rate);
    if (code != nullptr) {
        set_slots(L, id, 0, nullptr);
        return fail(L, code);
    }
    *handle = id;
    lua_pushvalue(L, ud);
    return 1;
}

// subscribe(kind, {rate = n}, fn) -> handle | nil, code
int subscribe(lua_State* L) {
    const char* kind_name = luaL_checkstring(L, 1);
    const std::optional<SensorKind> kind = core::parse_sensor_kind(kind_name);
    if (!kind) luaL_argerror(L, 1, "kind must be \"accel\" or \"gyro\"");
    lua_Integer rate = kDefaultRate;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        if (lua_getfield(L, 2, "rate") != LUA_TNIL) {
            const bool ok = lua_isinteger(L, -1) != 0 &&
                            std::find(SensorService::kRates.begin(), SensorService::kRates.end(),
                                      lua_tointeger(L, -1)) != SensorService::kRates.end();
            if (!ok) luaL_argerror(L, 2, "rate must be 10, 25, 50 or 100");
            rate = lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
    }
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lua_settop(L, 3);
    return add(L, 3, Want::Samples, *kind, static_cast<uint32_t>(rate), Gesture::Shake);
}

// on("shake" | "flip", fn) -> handle | nil, code
int on(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const std::optional<Gesture> g = core::parse_gesture(name);
    if (!g) luaL_argerror(L, 1, "gesture must be \"shake\" or \"flip\"");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_settop(L, 2);
    return add(L, 2, Want::Gesture, SensorKind::Accel, 0, *g);
}

// available() -> {accel = true, gyro = true} | {}
int available(lua_State* L) {
    SceneApi& api = detail::api_of(L);
    const bool accel = api.sensors != nullptr && api.sensors->available(SensorKind::Accel);
    const bool gyro = api.sensors != nullptr && api.sensors->available(SensorKind::Gyro);
    lua_createtable(L, 0, 2);
    if (accel) {
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "accel");
    }
    if (gyro) {
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "gyro");
    }
    return 1;
}

// handle:stop() -> boolean
int stop(lua_State* L) {
    const uint32_t id = *static_cast<uint32_t*>(luaL_checkudata(L, 1, kHandleType));
    const bool was = id != 0 && do_stop(detail::api_of(L), id);
    if (was) set_slots(L, id, 0, nullptr);
    lua_pushboolean(L, was ? 1 : 0);
    return 1;
}

struct RunCtx {
    uint32_t id;
    const core::SensorBatch* batch;
};

int run_entry(lua_State* L) {
    const auto* c = static_cast<const RunCtx*>(lua_touserdata(L, 1));
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kCallbacksKey);        // 2
    if (lua_rawgeti(L, 2, c->id) != LUA_TFUNCTION) return 0;  // 3: stopped
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kGesturesKey);         // 4
    lua_pushvalue(L, 3);                                      // 5
    if (lua_rawgeti(L, 4, c->id) == LUA_TSTRING) {            // 6
        lua_call(L, 1, 0);
        return 0;
    }
    lua_pop(L, 1);
    const core::SensorBatch& b = *c->batch;
    lua_createtable(L, static_cast<int>(b.size()), 0);
    lua_Integer i = 1;
    for (const core::SensorSample& s : b) {
        lua_createtable(L, 0, 4);
        lua_pushnumber(L, s.x);
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, s.y);
        lua_setfield(L, -2, "y");
        lua_pushnumber(L, s.z);
        lua_setfield(L, -2, "z");
        lua_pushinteger(L, static_cast<lua_Integer>(s.t_ms));
        lua_setfield(L, -2, "t");
        lua_rawseti(L, -2, i++);
    }
    lua_call(L, 1, 0);
    return 0;
}

}  // namespace

std::optional<ScriptError> run_sensor_callback(Vm& vm, uint32_t id,
                                               const core::SensorBatch& batch) {
    lua_State* L = vm.L();
    if (!lua_checkstack(L, 2)) return ScriptError{ScriptFault::Memory, "", 0, "stack overflow"};
    RunCtx ctx{id, &batch};
    lua_pushcfunction(L, run_entry);
    lua_pushlightuserdata(L, &ctx);
    return vm.call(1, 0, Budget::Fresh);
}

void pause_sensors(SceneApi& api, bool paused) {
    if (api.sensors == nullptr) return;
    for (const SceneApi::SensorHandle& h : api.sensor_handles)
        api.sensors->set_paused(h.service, paused);
}

void release_sensors(SceneApi& api) {
    if (api.sensors != nullptr)
        for (const SceneApi::SensorHandle& h : api.sensor_handles)
            api.sensors->unsubscribe(h.service);
    api.sensor_handles.clear();
}

namespace detail {

void install_sensor(lua_State* L, int wisp) {
    wisp = lua_absindex(L, wisp);
    lua_newtable(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kCallbacksKey);
    lua_newtable(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kGesturesKey);

    new_binding_metatable(L, kHandleType);
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, stop);
    lua_setfield(L, -2, "stop");
    check_c_functions(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    lua_createtable(L, 0, 3);
    lua_pushcfunction(L, subscribe);
    lua_setfield(L, -2, "subscribe");
    lua_pushcfunction(L, on);
    lua_setfield(L, -2, "on");
    lua_pushcfunction(L, available);
    lua_setfield(L, -2, "available");
    check_c_functions(L, -1);
    lua_setfield(L, wisp, "sensor");
}

}  // namespace detail

}  // namespace wisp::scene
