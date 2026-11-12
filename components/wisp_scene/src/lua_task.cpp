// Task coroutines (lua_task.hpp; agent-tools spec 6).
//
// Two registry tables hold the live tasks: id -> thread and thread -> id
// (current_task() reads the second). The second has weak keys, and a task
// cancelled on its own (cancel_task()) stays in it with id 0: no longer a
// task, still refused to coroutine.resume and coroutine.close for as long
// as the script holds its thread. A task's slice and result limit live
// in its SceneApi::TaskSlot. A binding that waits registers the task's
// thread in its own callback table where a function would go and yields
// the address of kWaitKey, which no script can produce: a yield that does
// not carry it is a plain coroutine.yield() at the task's own level.
//
// Everything that drives a task (start_task, resume_waiting, cancel_tasks)
// runs from the scene task's C++ code outside any lua_pcall, so it uses
// only non-raising Lua APIs (rawget/rawset of keys that exist or of nil
// values, xmove after checkstack) and leaves anything that can raise to
// Vm::call() (the start entry, a result's arguments), per vm.hpp's longjmp
// rule. Ending a task always comes last, since task_done may do anything
// but destroy the Vm.

#include "wisp/scene/lua_task.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "scene_api.hpp"
#include "wisp/scene/lua_json.hpp"

namespace wisp::scene {

namespace {

char kTaskThreadsKey;  // id -> thread
char kTaskIdsKey;      // thread -> id
char kWaitKey;         // what a waiting binding yields (its address)

// Pushes registry[key] when it is a table; false (nothing pushed) if not.
bool push_table(lua_State* L, const void* key) {
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, key) == LUA_TTABLE) return true;
    lua_pop(L, 1);
    return false;
}

// What the ids table holds for the thread at `idx`: its id, 0 for a task
// cancelled on its own, -1 for anything else. Non-raising; needs two free
// stack slots.
int64_t id_entry(lua_State* L, int idx) {
    idx = lua_absindex(L, idx);
    if (!push_table(L, &kTaskIdsKey)) return -1;
    lua_pushvalue(L, idx);
    lua_rawget(L, -2);
    const int64_t id = lua_isinteger(L, -1) ? static_cast<int64_t>(lua_tointeger(L, -1)) : -1;
    lua_pop(L, 2);
    return id;
}

// The id of the task whose thread is at `idx`, 0 if it is none (or has
// ended, or was cancelled). Non-raising; needs two free stack slots.
uint32_t task_of(lua_State* L, int idx) {
    const int64_t id = id_entry(L, idx);
    return id > 0 ? static_cast<uint32_t>(id) : 0;
}

// Removes task `id` from both registry tables; with `keep_refused` its
// thread stays in the ids table as 0 (cancel_task()). Non-raising: one key
// is set to nil, the other to nil or to 0 where it already exists, and
// neither allocates.
void forget(lua_State* L, uint32_t id, bool keep_refused = false) {
    if (!lua_checkstack(L, 5) || !push_table(L, &kTaskThreadsKey)) return;
    const int threads = lua_gettop(L);
    if (lua_rawgeti(L, threads, id) == LUA_TTHREAD && push_table(L, &kTaskIdsKey)) {
        lua_pushvalue(L, threads + 1);
        if (keep_refused)
            lua_pushinteger(L, 0);
        else
            lua_pushnil(L);
        lua_rawset(L, -3);
    }
    lua_pushnil(L);
    lua_rawseti(L, threads, id);
    lua_settop(L, threads - 1);
}

SceneApi::TaskSlot* slot_of(SceneApi& api, uint32_t id) {
    for (auto& s : api.tasks)
        if (s.id == id) return &s;
    return nullptr;
}

// Ends task `id` with `outcome`. Returns the VM's fault (the caller's own
// result) when it has one. Touches neither the Vm nor the SceneApi after
// task_done, which runs last.
std::optional<ScriptError> finish(Vm& vm, SceneApi& api, uint32_t id, TaskOutcome outcome) {
    forget(vm.L(), id);
    api.tasks.erase(std::remove_if(api.tasks.begin(), api.tasks.end(),
                                   [id](const SceneApi::TaskSlot& s) { return s.id == id; }),
                    api.tasks.end());
    std::optional<ScriptError> ret;
    if (vm.faulted() && outcome.error) ret = outcome.error;
    TaskDone done = api.task_done;
    if (done) done(id, std::move(outcome));
    return ret;
}

TaskOutcome failed(ScriptError e) {
    TaskOutcome o;
    o.error = std::move(e);
    return o;
}

// The JSON of the `n` values on top of co's stack, as scene.eval gives
// them. Returns a lua_json.hpp code on failure. Non-raising.
const char* results_json(lua_State* co, int n, size_t max_json, os::LargeString& out) {
    out.clear();
    if (!lua_checkstack(co, kLuaJsonStackSlots)) return kJsonLimits;
    const int first = lua_gettop(co) - n + 1;
    if (n == 0) {
        out = "null";
        return nullptr;
    }
    if (n == 1) return lua_to_json(co, first, out, max_json);
    out = "[";
    os::LargeString one;
    for (int i = 0; i < n; ++i) {
        if (const char* code = lua_to_json(co, first + i, one, max_json)) return code;
        if (i > 0) out.push_back(',');
        out += one;
        if (out.size() + 1 > max_json) return kJsonTooLarge;
    }
    out.push_back(']');
    return nullptr;
}

// Runs one slice of task `id` (thread `co`, with `nargs` values on top of
// its stack) and ends the task unless it is waiting again.
std::optional<ScriptError> step(Vm& vm, SceneApi& api, uint32_t id, lua_State* co, int nargs) {
    const SceneApi::TaskSlot* slot = slot_of(api, id);
    const uint32_t slice = slot != nullptr ? slot->slice_ms : 0;
    const size_t max_json = slot != nullptr ? slot->max_json : kEvalResultMaxBytes;
    int nres = 0;
    bool yielded = false;
    if (auto err = vm.resume(co, nargs, &nres, &yielded, slice))
        return finish(vm, api, id, failed(std::move(*err)));
    if (yielded) {
        const bool waits = nres == 1 && lua_touserdata(co, -1) == &kWaitKey;
        lua_pop(co, nres);
        if (waits) return std::nullopt;
        // Dropped while suspended: nothing would ever resume it.
        return finish(vm, api, id,
                      failed({ScriptFault::Runtime, "", 0,
                              "attempt to yield from a task (only a wait may suspend it)"}));
    }
    TaskOutcome o;
    const char* code = results_json(co, nres, max_json, o.json);
    lua_pop(co, nres);
    if (code != nullptr) {
        o.json.clear();
        std::string msg =
            code == kJsonTooLarge ? "result too large" : std::string("result ") + code;
        return finish(vm, api, id, failed({ScriptFault::Runtime, "", 0, std::move(msg)}));
    }
    o.ok = true;
    return finish(vm, api, id, std::move(o));
}

// Drops what task `t` waits for, if anything: its entry in the callback
// table (only while it still holds the task's thread) and its wisp.sleep
// timer, so a cancelled wait holds no TimerSet slot. Non-raising.
void drop_wait(lua_State* L, SceneApi& api, const SceneApi::TaskSlot& t) {
    if (t.wait_key == nullptr || !lua_checkstack(L, 4) || !push_table(L, t.wait_key)) return;
    if (lua_rawgeti(L, -1, t.wait_id) == LUA_TTHREAD && task_of(L, -1) == t.id) {
        lua_pushnil(L);
        lua_rawseti(L, -3, t.wait_id);
        if (t.wait_timer) api.timers.cancel(t.wait_id);
    }
    lua_pop(L, 2);
}

struct StartCtx {
    const Value* args;
    uint32_t id;
    lua_State* co;
};

// Inside Vm::call(): 1 the function, 2 the StartCtx. Makes the thread
// with the function and its argument, and registers it.
int start_entry(lua_State* L) {
    auto* c = static_cast<StartCtx*>(lua_touserdata(L, 2));
    luaL_checkstack(L, 8, "task");
    for (const void* key :
         {static_cast<const void*>(&kTaskThreadsKey), static_cast<const void*>(&kTaskIdsKey)}) {
        if (lua_rawgetp(L, LUA_REGISTRYINDEX, key) == LUA_TTABLE) continue;
        lua_pop(L, 1);
        lua_newtable(L);
        if (key == &kTaskIdsKey) {
            // Weak keys: a cancelled thread the script no longer holds goes.
            lua_createtable(L, 0, 1);
            lua_pushliteral(L, "k");
            lua_setfield(L, -2, "__mode");
            lua_setmetatable(L, -2);
        }
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, key);
    }
    // 3: threads, 4: ids
    lua_State* co = lua_newthread(L);  // 5; copies the main thread's hook
    lua_pushvalue(L, 1);
    if (c->args != nullptr)
        detail::push_value(L, *c->args);
    else
        lua_pushnil(L);
    lua_xmove(L, co, 2);
    lua_pushvalue(L, 5);
    lua_rawseti(L, 3, c->id);
    lua_pushvalue(L, 5);
    lua_pushinteger(L, c->id);
    lua_rawset(L, 4);
    c->co = co;
    return 0;
}

}  // namespace

Result<uint32_t> start_task(Vm& vm, const Value* args, uint32_t slice_ms, size_t max_json) {
    lua_State* L = vm.L();
    if (lua_type(L, -1) != LUA_TFUNCTION) {
        lua_pop(L, 1);
        return Result<uint32_t>::err(ErrorCode::InvalidArgument, "not a function");
    }
    SceneApi* api = detail::find_api(L);
    if (api == nullptr) {
        lua_pop(L, 1);
        return Result<uint32_t>::err(ErrorCode::Unsupported, "no scene api");
    }
    if (vm.faulted()) {
        lua_pop(L, 1);
        return Result<uint32_t>::err(ErrorCode::Busy, "vm faulted");
    }
    if (!lua_checkstack(L, 2)) {
        lua_pop(L, 1);
        return Result<uint32_t>::err(ErrorCode::NoMemory, "stack overflow");
    }
    const uint32_t id = api->next_task_id++;
    if (api->next_task_id == 0) api->next_task_id = 1;
    StartCtx ctx{args, id, nullptr};
    lua_pushcfunction(L, start_entry);
    lua_insert(L, -2);
    lua_pushlightuserdata(L, &ctx);
    if (auto err = vm.call(2, 0)) {
        // Only the VM's limits can stop start_entry (it runs no script code).
        const ErrorCode code = err->fault == ScriptFault::Timeout  ? ErrorCode::Timeout
                               : err->fault == ScriptFault::Memory ? ErrorCode::NoMemory
                                                                   : ErrorCode::InvalidArgument;
        return Result<uint32_t>::err(code, err->text());
    }
    api->tasks.push_back({id, slice_ms != 0 ? slice_ms : api->task_slice_ms, max_json});
    step(vm, *api, id, ctx.co, 1);
    return Result<uint32_t>::ok(id);
}

void cancel_tasks(Vm& vm) {
    lua_State* L = vm.L();
    SceneApi* api = detail::find_api(L);
    if (api == nullptr) return;
    std::vector<SceneApi::TaskSlot> tasks;
    tasks.swap(api->tasks);
    for (const auto& t : tasks) {
        drop_wait(L, *api, t);
        forget(L, t.id);
    }
    TaskDone done = api->task_done;
    if (!done) return;
    for (const auto& t : tasks) done(t.id, failed({ScriptFault::Runtime, "", 0, "cancelled"}));
}

void cancel_task(Vm& vm, uint32_t id) {
    lua_State* L = vm.L();
    SceneApi* api = detail::find_api(L);
    if (api == nullptr) return;
    const SceneApi::TaskSlot* slot = slot_of(*api, id);
    if (slot == nullptr) return;
    drop_wait(L, *api, *slot);
    forget(L, id, true);
    api->tasks.erase(std::remove_if(api->tasks.begin(), api->tasks.end(),
                                    [id](const SceneApi::TaskSlot& s) { return s.id == id; }),
                     api->tasks.end());
    TaskDone done = api->task_done;
    if (done) done(id, failed({ScriptFault::Runtime, "", 0, "cancelled"}));
}

uint32_t current_task(lua_State* L) {
    if (!lua_checkstack(L, 3)) return 0;
    lua_pushthread(L);
    const uint32_t id = task_of(L, -1);
    lua_pop(L, 1);
    return id;
}

bool is_task_thread(lua_State* L, int idx) {
    return lua_type(L, idx) == LUA_TTHREAD && lua_checkstack(L, 2) && id_entry(L, idx) >= 0;
}

namespace detail {

void push_waiter(lua_State* L, int cb) {
    if (current_task(L) == 0) {
        luaL_checktype(L, cb, LUA_TFUNCTION);  // raises the usual error
        return;
    }
    if (!lua_isyieldable(L))
        luaL_error(L, "cannot wait here (inside a call from C); pass a callback");
    lua_pushthread(L);
}

bool take_callback(lua_State* L, int cb) {
    if (!lua_isnoneornil(L, cb)) {
        luaL_checktype(L, cb, LUA_TFUNCTION);
        lua_settop(L, cb);
        return false;
    }
    // Checked before the stack is cut, so a missing callback still says
    // "got no value", word for word as before tasks existed.
    if (current_task(L) == 0) luaL_checktype(L, cb, LUA_TFUNCTION);
    lua_settop(L, cb);
    push_waiter(L, cb);
    lua_replace(L, cb);
    return true;
}

int wait(lua_State* L, const void* key, uint32_t id, bool timer) {
    if (SceneApi* api = find_api(L)) {
        if (SceneApi::TaskSlot* slot = slot_of(*api, current_task(L))) {
            slot->wait_key = key;
            slot->wait_id = id;
            slot->wait_timer = timer;
        }
    }
    lua_pushlightuserdata(L, &kWaitKey);
    return lua_yield(L, 1);
}

bool is_waiting(lua_State* L, const void* key, uint32_t id) {
    if (!lua_checkstack(L, 2) || !push_table(L, key)) return false;
    const bool thread = lua_rawgeti(L, -1, id) == LUA_TTHREAD;
    lua_pop(L, 2);
    return thread;
}

std::optional<ScriptError> resume_waiting(Vm& vm, const void* key, uint32_t id, lua_CFunction build,
                                          void* ctx) {
    lua_State* L = vm.L();
    const int base = lua_gettop(L);
    if (!lua_checkstack(L, 6) || !push_table(L, key))
        return ScriptError{ScriptFault::Memory, "", 0, "stack overflow"};
    if (lua_rawgeti(L, base + 1, id) != LUA_TTHREAD) {  // base + 2
        lua_settop(L, base);
        return std::nullopt;
    }
    lua_State* co = lua_tothread(L, base + 2);
    lua_pushnil(L);
    lua_rawseti(L, base + 1, id);
    const uint32_t task = task_of(L, base + 2);
    SceneApi* api = find_api(L);
    // The thread stays referenced by the task tables while it lives.
    lua_settop(L, base);
    // A late answer for a task that has ended (or was cancelled).
    if (task == 0 || api == nullptr) return std::nullopt;
    int nargs = 0;
    if (build != nullptr && !vm.faulted()) {
        lua_pushcfunction(L, build);
        lua_pushlightuserdata(L, ctx);
        if (auto err = vm.call(1, LUA_MULTRET))
            return finish(vm, *api, task, failed(std::move(*err)));
        nargs = lua_gettop(L) - base;
        if (!lua_checkstack(co, nargs)) {
            lua_settop(L, base);
            return finish(vm, *api, task, failed({ScriptFault::Memory, "", 0, "stack overflow"}));
        }
        lua_xmove(L, co, nargs);
    }
    return step(vm, *api, task, co, nargs);
}

}  // namespace detail

}  // namespace wisp::scene
