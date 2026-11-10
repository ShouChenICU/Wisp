// wisp.files (scene_api.hpp; scene-device spec 7.1): a thin adapter over
// core::SceneFiles. Nothing here touches flash: each call checks its
// arguments, copies what the job needs (the data into PSRAM) and hands a
// job to SceneApi::files_run, which runs it on the wisp.fs worker; the
// result comes back as a message and run_files_callback() calls the
// scene's function with it.
//
// Callbacks wait in a registry table by id until their result arrives;
// results for a closed scene never arrive (the runtime drops stale
// messages), and the table goes with the VM. wisp.http.download keeps its
// callback here too (register_files_callback()). A task that left the
// callback out waits there as its thread instead (lua_task.hpp), and
// run_files_callback() resumes it with the callback's arguments.
//
// longjmp rule (vm.hpp): each binding checks its arguments with raising
// Lua calls while its frame holds only trivially destructible locals, then
// a do_* function builds the job with non-raising Lua calls only.

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "scene_api.hpp"

namespace wisp::scene {

namespace {

// Registry key of the table of pending callbacks, by id.
char kCallbacksKey;

std::string_view view_at(lua_State* L, int idx) {
    size_t n = 0;
    const char* s = lua_tolstring(L, idx, &n);
    return {s, n};
}

FilesResult answer(const Result<void>& r) {
    FilesResult out;
    if (!r) out.error = core::scene_files_code(r.error());
    return out;
}

template <class T>
FilesResult answer(Result<T> r) {
    FilesResult out;
    if (r) {
        out.payload = std::move(r.value());
    } else {
        out.error = core::scene_files_code(r.error());
    }
    return out;
}

SceneApi& checked_api(lua_State* L) {
    SceneApi& api = detail::api_of(L);
    if (!api.files_run || !api.files_fail) luaL_error(L, "wisp.files is not available");
    return api;
}

// Non-raising: the job for write/append of the strings at 1 and 2.
void do_write(lua_State* L, SceneApi& api, uint32_t id, bool append) {
    const std::string_view name = view_at(L, 1);
    const std::string_view data = view_at(L, 2);
    if (!core::valid_file_name(name)) return api.files_fail(id, "bad_name");
    // Refused before its data is copied; SceneFiles checks its own
    // configured limit again.
    if (data.size() > core::kMaxFileWrite) return api.files_fail(id, "too_large");
    auto buf = std::allocate_shared<os::LargeBuffer>(os::LargeAllocator<os::LargeBuffer>(),
                                                     os::LargeBuffer::allocate(data.size()));
    if (!buf || !buf->ok()) return api.files_fail(id, "no_memory");
    if (!data.empty()) std::memcpy(buf->data(), data.data(), data.size());
    api.files_run(
        id, [name = std::string(name), buf, append](core::SceneFiles& f, const std::string& scene) {
            return answer(append ? f.append(scene, name, buf->span())
                                 : f.write(scene, name, buf->span()));
        });
}

int write_like(lua_State* L, bool append) {
    luaL_checktype(L, 1, LUA_TSTRING);
    luaL_checktype(L, 2, LUA_TSTRING);
    const bool waits = detail::take_callback(L, 3);
    SceneApi& api = checked_api(L);
    const uint32_t id = detail::register_files_callback(L, api, 3);
    do_write(L, api, id, append);
    return waits ? detail::wait(L) : 0;
}

int write(lua_State* L) { return write_like(L, false); }
int append(lua_State* L) { return write_like(L, true); }

void do_read(lua_State* L, SceneApi& api, uint32_t id, uint64_t offset, size_t len) {
    const std::string_view name = view_at(L, 1);
    if (!core::valid_file_name(name)) return api.files_fail(id, "bad_name");
    api.files_run(
        id, [name = std::string(name), offset, len](core::SceneFiles& f, const std::string& scene) {
            return answer(f.read(scene, name, offset, len));
        });
}

// read(name, offset, len, cb)
int read(lua_State* L) {
    luaL_checktype(L, 1, LUA_TSTRING);
    const lua_Integer offset = luaL_checkinteger(L, 2);
    const lua_Integer len = luaL_checkinteger(L, 3);
    luaL_argcheck(L, offset >= 0, 2, "offset must not be negative");
    luaL_argcheck(L, len >= 0, 3, "len must not be negative");
    const bool waits = detail::take_callback(L, 4);
    SceneApi& api = checked_api(L);
    const uint32_t id = detail::register_files_callback(L, api, 4);
    // Anything past max_read is "too_large" anyway; this keeps it in size_t.
    const size_t capped = static_cast<size_t>(std::min<lua_Integer>(len, 1 << 30));
    do_read(L, api, id, static_cast<uint64_t>(offset), capped);
    return waits ? detail::wait(L) : 0;
}

void do_remove(lua_State* L, SceneApi& api, uint32_t id) {
    const std::string_view name = view_at(L, 1);
    if (!core::valid_file_name(name)) return api.files_fail(id, "bad_name");
    api.files_run(id, [name = std::string(name)](core::SceneFiles& f, const std::string& scene) {
        return answer(f.remove(scene, name));
    });
}

int remove(lua_State* L) {
    luaL_checktype(L, 1, LUA_TSTRING);
    const bool waits = detail::take_callback(L, 2);
    SceneApi& api = checked_api(L);
    const uint32_t id = detail::register_files_callback(L, api, 2);
    do_remove(L, api, id);
    return waits ? detail::wait(L) : 0;
}

void do_list(SceneApi& api, uint32_t id) {
    api.files_run(
        id, [](core::SceneFiles& f, const std::string& scene) { return answer(f.list(scene)); });
}

void do_info(SceneApi& api, uint32_t id) {
    api.files_run(
        id, [](core::SceneFiles& f, const std::string& scene) { return answer(f.info(scene)); });
}

int list(lua_State* L) {
    const bool waits = detail::take_callback(L, 1);
    SceneApi& api = checked_api(L);
    do_list(api, detail::register_files_callback(L, api, 1));
    return waits ? detail::wait(L) : 0;
}

int info(lua_State* L) {
    const bool waits = detail::take_callback(L, 1);
    SceneApi& api = checked_api(L);
    do_info(api, detail::register_files_callback(L, api, 1));
    return waits ? detail::wait(L) : 0;
}

struct RunCtx {
    uint32_t id;
    const FilesResult* result;
};

void set_int(lua_State* L, int table, const char* key, int64_t v) {
    lua_pushinteger(L, static_cast<lua_Integer>(v));
    lua_setfield(L, table, key);
}

// Pushes the payload as the callback's arguments after `err`; returns how
// many: list() adds `truncated`.
int push_payload(lua_State* L, const FilesPayload& p) {
    if (std::holds_alternative<std::monostate>(p)) {
        lua_pushboolean(L, 1);
    } else if (const auto* data = std::get_if<os::LargeBuffer>(&p)) {
        lua_pushlstring(L, reinterpret_cast<const char*>(data->data()), data->size());
    } else if (const auto* listed = std::get_if<core::SceneFileList>(&p)) {
        lua_createtable(L, static_cast<int>(listed->entries.size()), 0);
        const int list = lua_gettop(L);
        lua_Integer i = 0;
        for (const core::SceneFileEntry& e : listed->entries) {
            lua_createtable(L, 0, 3);
            const int row = lua_gettop(L);
            lua_pushlstring(L, e.name.data(), e.name.size());
            lua_setfield(L, row, "name");
            set_int(L, row, "size", static_cast<int64_t>(e.size));
            set_int(L, row, "mtime", e.mtime);
            lua_rawseti(L, list, ++i);
        }
        lua_pushboolean(L, listed->truncated ? 1 : 0);
        return 2;
    } else if (const auto* info = std::get_if<core::SceneFilesInfo>(&p)) {
        lua_createtable(L, 0, 3);
        const int t = lua_gettop(L);
        if (info->location != core::FilesLocation::None) {
            lua_pushstring(L, core::to_string(info->location));
            lua_setfield(L, t, "location");
        }
        lua_pushstring(L, core::to_string(info->want));
        lua_setfield(L, t, "want");
        set_int(L, t, "quota", static_cast<int64_t>(info->quota));
        set_int(L, t, "used", static_cast<int64_t>(info->used));
    } else if (const auto* dl = std::get_if<FilesDownloaded>(&p)) {
        lua_createtable(L, 0, 3);
        const int t = lua_gettop(L);
        set_int(L, t, "status", dl->status);
        set_int(L, t, "size", static_cast<int64_t>(dl->size));
        lua_pushboolean(L, dl->deferred ? 1 : 0);
        lua_setfield(L, t, "deferred");
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// Pushes cb's arguments: err and the payload (nil and the payload, or the
// error and nil). Returns how many.
int push_args(lua_State* L, const FilesResult& r) {
    if (!r.error.empty()) {
        lua_pushlstring(L, r.error.data(), r.error.size());
        lua_pushnil(L);
        return 2;
    }
    lua_pushnil(L);
    return 1 + push_payload(L, r.payload);
}

int run_entry(lua_State* L) {
    const auto* c = static_cast<const RunCtx*>(lua_touserdata(L, 1));
    luaL_checkstack(L, 8, "wisp.files");
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kCallbacksKey);        // 2
    if (lua_rawgeti(L, 2, c->id) != LUA_TFUNCTION) return 0;  // 3
    lua_pushnil(L);
    lua_rawseti(L, 2, c->id);
    lua_call(L, push_args(L, *c->result), 0);
    return 0;
}

// The waiting task's values (detail::resume_waiting()).
int args_entry(lua_State* L) {
    const auto* c = static_cast<const RunCtx*>(lua_touserdata(L, 1));
    lua_settop(L, 0);
    luaL_checkstack(L, 8, "wisp.files");
    return push_args(L, *c->result);
}

}  // namespace

namespace detail {

uint32_t register_files_callback(lua_State* L, SceneApi& api, int cb) {
    cb = lua_absindex(L, cb);
    const uint32_t id = api.next_files_id++;
    if (api.next_files_id == 0) api.next_files_id = 1;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kCallbacksKey);
    lua_pushvalue(L, cb);
    lua_rawseti(L, -2, id);
    lua_pop(L, 1);
    return id;
}

void install_files(lua_State* L, int wisp) {
    wisp = lua_absindex(L, wisp);
    lua_newtable(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kCallbacksKey);
    lua_createtable(L, 0, 6);
    lua_pushcfunction(L, write);
    lua_setfield(L, -2, "write");
    lua_pushcfunction(L, append);
    lua_setfield(L, -2, "append");
    lua_pushcfunction(L, read);
    lua_setfield(L, -2, "read");
    lua_pushcfunction(L, list);
    lua_setfield(L, -2, "list");
    lua_pushcfunction(L, remove);
    lua_setfield(L, -2, "remove");
    lua_pushcfunction(L, info);
    lua_setfield(L, -2, "info");
    check_c_functions(L, -1);
    lua_setfield(L, wisp, "files");
}

}  // namespace detail

std::optional<ScriptError> run_files_callback(Vm& vm, uint32_t id, const FilesResult& r) {
    lua_State* L = vm.L();
    if (!lua_checkstack(L, 2)) return ScriptError{ScriptFault::Memory, "", 0, "stack overflow"};
    RunCtx ctx{id, &r};
    if (detail::is_waiting(L, &kCallbacksKey, id))
        return detail::resume_waiting(vm, &kCallbacksKey, id, args_entry, &ctx);
    lua_pushcfunction(L, run_entry);
    lua_pushlightuserdata(L, &ctx);
    return vm.call(1, 0, Budget::Fresh);
}

}  // namespace wisp::scene
