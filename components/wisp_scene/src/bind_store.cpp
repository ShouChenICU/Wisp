// wisp.store (scene-runtime-core spec 5.4): a thin adapter over the
// scene's core::SceneStore.
//
//   wisp.store.get(k) -> value or nil
//   wisp.store.set(k, v) -> true, or nil and a code; v == nil deletes
//   wisp.store.batch(fn) -> true, or nil and "batch_aborted"
//   wisp.store.collection(name) -> c
//   c:insert(t) -> id | c:update(id, t) -> true | c:delete(id) -> true,
//       each or nil and a code
//   c:find(filter?, opts?) -> list of records, each with its `id` field
//
// `id` is reserved: it is the record id the store hands out, never a
// stored field. insert and update leave a top-level `id` out of the
// record (so a record from find can be passed back to update), filter.id
// selects the record with that id, and sort = "id" is id order.
//
// Codes: quota_exceeded, record_too_large, too_many_collections,
// not_serializable, too_deep, not_found, ids_exhausted, batch_aborted.
// Malformed arguments (key type or length, collection name, a record that
// is not a table with string keys, filter keys) raise argument errors.
//
// Flash: everything here runs on the scene task, whose stack is in PSRAM
// and which must never touch flash. SceneStore's reads and writes work on
// its in-memory index and pending buffers only; a write arms the store's
// coalescing timer through SceneStoreDeps::schedule (a scene-task timer),
// whose job is queued to the wisp.fs worker, the only place file I/O
// happens.
//
// longjmp rule (vm.hpp): each binding first checks its arguments with
// raising Lua calls while its frame holds only trivially destructible
// locals, then does the C++ work in a separate do_* function that calls
// only non-raising Lua APIs (or protect()) and reports back through an
// Outcome, and raises or pushes results only after that function has
// returned and its C++ objects are gone.

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "scene_api.hpp"
#include "wisp/base/json.hpp"
#include "wisp/scene/lua_json.hpp"

namespace wisp::scene {

namespace {

constexpr const char* kCollType = "wisp.store.collection";
constexpr size_t kMaxCollectionName = 32;
// How often find()'s record loop checks the limits.
constexpr size_t kCheckEvery = 256;

struct CollHandle {
    uint8_t len;
    char name[kMaxCollectionName];
    std::string_view view() const { return {name, len}; }
};

// What a do_* function hands back to its binding.
struct Outcome {
    // A code for `nil, code`; static storage.
    const char* code = nullptr;
    // The VM's limits were reached: Vm::check_limits() raises.
    bool limits = false;
    // An error object protect() left on the stack, to re-raise.
    bool rethrow = false;
    // A message to raise as an error.
    bool raise = false;
    char msg[160] = {};
    int64_t id = 0;
    // find(): nothing can match; the result is an empty list.
    bool empty = false;
};
static_assert(std::is_trivially_destructible_v<Outcome>);

// The codes the API returns; anything else is raised.
const char* known_code(const std::string& message) {
    static constexpr const char* kCodes[] = {
        "quota_exceeded", "record_too_large", "too_many_collections",
        "not_found",      "ids_exhausted",    "batch_aborted",
    };
    for (const char* c : kCodes)
        if (message == c) return c;
    return nullptr;
}

void take_error(const Error& e, Outcome* o) {
    o->code = known_code(e.message);
    if (o->code == nullptr) {
        o->raise = true;
        std::snprintf(o->msg, sizeof(o->msg), "wisp.store: %s", e.message.c_str());
    }
}

void take_json_code(const char* code, Outcome* o) {
    if (code == kJsonLimits) {
        o->limits = true;
    } else {
        o->code = code;
    }
}

// Raises, pushes `nil, code`, or returns -1 so the binding pushes its
// own result.
int settle(lua_State* L, const Outcome& o) {
    if (o.limits) {
        Vm::of(L)->check_limits(L);
        return luaL_error(L, "wisp.store: limits reached");
    }
    if (o.rethrow) return lua_error(L);
    if (o.raise) return luaL_error(L, "%s", o.msg);
    if (o.code != nullptr) {
        lua_pushnil(L);
        lua_pushstring(L, o.code);
        return 2;
    }
    return -1;
}

core::SceneStore& store_of(lua_State* L) {
    core::SceneStore* s = detail::api_of(L).store;
    if (s == nullptr) luaL_error(L, "wisp.store is not available");
    return *s;
}

// The key at `arg`; raises unless it is a string of 1..key_bytes bytes.
std::string_view check_key(lua_State* L, int arg, const core::SceneStore& st) {
    luaL_checktype(L, arg, LUA_TSTRING);
    size_t n = 0;
    const char* k = lua_tolstring(L, arg, &n);
    if (n == 0) luaL_argerror(L, arg, "empty key");
    if (n > st.limits().key_bytes) luaL_argerror(L, arg, "key too long");
    return {k, n};
}

// A record argument: a table whose keys are strings (an object).
// Every key is checked, so a bad one raises the same argument error
// wherever traversal meets it.
void check_record(lua_State* L, int arg) {
    luaL_checktype(L, arg, LUA_TTABLE);
    Vm* vm = Vm::of(L);
    size_t count = 0;
    lua_pushnil(L);
    while (lua_next(L, arg) != 0) {
        lua_pop(L, 1);
        if (lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 1);
            luaL_argerror(L, arg, "record must be a table with string keys");
        }
        if (++count % kCheckEvery == 0) vm->check_limits(L);
    }
}

bool is_id(const char* k, size_t n) { return n == 2 && k[0] == 'i' && k[1] == 'd'; }

CollHandle& check_coll(lua_State* L) {
    return *static_cast<CollHandle*>(luaL_checkudata(L, 1, kCollType));
}

bool valid_collection(const char* s, size_t n) {
    if (n == 0 || n > kMaxCollectionName) return false;
    if (n == 2 && std::memcmp(s, "kv", 2) == 0) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

// --- C++ work (no raising Lua calls) -----------------------------------------------

void do_set(lua_State* L, core::SceneStore& st, std::string_view key, Outcome* o) {
    if (lua_isnil(L, 2)) {
        auto r = st.kv_del(key);
        if (!r) take_error(r.error(), o);
        return;
    }
    os::LargeString json;
    if (const char* code = lua_to_json(L, 2, json, st.limits().record_bytes)) {
        take_json_code(code, o);
        return;
    }
    auto r = st.kv_set(key, json);
    if (!r) take_error(r.error(), o);
}

// 0: no such key; 1: pushed; -1: stored text unreadable or limits reached.
int do_get(lua_State* L, const core::SceneStore& st, std::string_view key) {
    auto v = st.kv_get(key);
    if (!v) return 0;
    return push_json(L, *v) ? 1 : -1;
}

void do_commit(core::SceneStore& st, Outcome* o) {
    auto r = st.commit_batch();
    if (!r) take_error(r.error(), o);
}

void do_insert(lua_State* L, core::SceneStore& st, std::string_view coll, int arg, Outcome* o) {
    os::LargeString json;
    if (const char* code = lua_record_to_json(L, arg, json, st.limits().record_bytes)) {
        take_json_code(code, o);
        return;
    }
    auto r = st.insert(coll, json);
    if (!r) {
        take_error(r.error(), o);
        return;
    }
    o->id = r.value();
}

void do_update(lua_State* L, core::SceneStore& st, std::string_view coll, int64_t id, int arg,
               Outcome* o) {
    os::LargeString json;
    if (const char* code = lua_record_to_json(L, arg, json, st.limits().record_bytes)) {
        take_json_code(code, o);
        return;
    }
    auto r = st.update(coll, id, json);
    if (!r) take_error(r.error(), o);
}

void do_delete(core::SceneStore& st, std::string_view coll, int64_t id, Outcome* o) {
    auto r = st.remove(coll, id);
    if (!r) take_error(r.error(), o);
}

// The filter value at the top of the stack as a Value; false if it cannot
// be one (`o` says why, or sets `empty` for a value too large to equal
// any record's field). `max` is the record size limit.
bool filter_value(lua_State* L, size_t max, Value* out, Outcome* o) {
    switch (lua_type(L, -1)) {
        case LUA_TBOOLEAN:
            *out = Value(lua_toboolean(L, -1) != 0);
            return true;
        case LUA_TNUMBER:
            if (lua_isinteger(L, -1)) {
                *out = Value(static_cast<int64_t>(lua_tointeger(L, -1)));
            } else {
                *out = Value(static_cast<double>(lua_tonumber(L, -1)));
            }
            return true;
        case LUA_TSTRING: {
            size_t n = 0;
            const char* s = lua_tolstring(L, -1, &n);
            if (n > max) {
                o->empty = true;
                return false;
            }
            *out = Value(std::string(s, n));
            return true;
        }
        default: {
            // Tables compare as Values (FindQuery); go through their JSON.
            os::LargeString json;
            if (const char* code = lua_to_json(L, -1, json, max)) {
                if (code == kJsonTooLarge) {
                    o->empty = true;
                } else {
                    take_json_code(code, o);
                }
                return false;
            }
            auto v = from_json(std::string_view(json.data(), json.size()));
            if (!v) {
                o->code = kJsonNotSerializable;
                return false;
            }
            *out = std::move(v.value());
            return true;
        }
    }
}

struct FindArgs {
    int filter = 0;  // stack index of the filter table, or 0
    // filter.id: the record id, which is not a stored field.
    bool has_id = false;
    int64_t id = 0;
    // A sort field; null for id order.
    const char* sort = nullptr;
    size_t sort_len = 0;
    bool desc = false;
    size_t limit = SIZE_MAX;
    size_t offset = 0;
};

struct PushRecords {
    const os::LargeVector<core::StoreRecord>* records;
};

// Inside protect(): the result list.
void push_records(lua_State* L, void* ctx) {
    const auto& records = *static_cast<const PushRecords*>(ctx)->records;
    Vm* vm = Vm::of(L);
    lua_createtable(L, static_cast<int>(std::min<size_t>(records.size(), INT_MAX)), 0);
    lua_Integer i = 0;
    for (const core::StoreRecord& r : records) {
        if (++i % kCheckEvery == 0) vm->check_limits(L);
        decode_json(L, std::string_view(r.json.data(), r.json.size()));
        if (!lua_istable(L, -1))
            luaL_error(L, "wisp.store: record %I is not an object", static_cast<lua_Integer>(r.id));
        lua_pushinteger(L, static_cast<lua_Integer>(r.id));
        lua_setfield(L, -2, "id");
        lua_rawseti(L, -2, i);
    }
}

void do_find(lua_State* L, const core::SceneStore& st, std::string_view coll, const FindArgs& a,
             Outcome* o) {
    core::FindQuery q;
    if (a.has_id) q.id = a.id;
    if (a.sort != nullptr) q.sort.assign(a.sort, a.sort_len);
    q.desc = a.desc;
    q.limit = a.limit;
    q.offset = a.offset;
    if (a.filter != 0) {
        Vm* vm = Vm::of(L);
        const size_t max = st.limits().record_bytes;
        size_t count = 0;
        lua_pushnil(L);
        while (lua_next(L, a.filter) != 0) {
            if (++count % kCheckEvery == 0 && vm->past_limits()) {
                o->limits = true;
                lua_pop(L, 2);
                return;
            }
            size_t n = 0;
            const char* k = lua_tolstring(L, -2, &n);  // checked to be a string
            if (is_id(k, n)) {
                lua_pop(L, 1);
                continue;
            }
            Value v;
            if (!filter_value(L, max, &v, o)) {
                lua_pop(L, 2);
                return;
            }
            q.equals.emplace_back(std::string(k, n), std::move(v));
            lua_pop(L, 1);
        }
    }
    auto found = st.find(coll, q);
    if (!found) {
        take_error(found.error(), o);
        return;
    }
    PushRecords ctx{&found.value()};
    if (protect(L, push_records, &ctx) != LUA_OK) o->rethrow = true;
}

// --- Bindings ------------------------------------------------------------------

int get(lua_State* L) {
    core::SceneStore& st = store_of(L);
    std::string_view key = check_key(L, 1, st);
    lua_settop(L, 1);
    const int r = do_get(L, st, key);
    if (r == 0) lua_pushnil(L);
    if (r < 0) {
        Vm::of(L)->check_limits(L);
        return luaL_error(L, "wisp.store.get: stored value for '%s' is unreadable", key.data());
    }
    return 1;
}

int set(lua_State* L) {
    core::SceneStore& st = store_of(L);
    std::string_view key = check_key(L, 1, st);
    lua_settop(L, 2);  // a missing value is nil: a delete
    luaL_checkstack(L, kLuaJsonStackSlots, "wisp.store");
    Outcome o;
    do_set(L, st, key, &o);
    const int n = settle(L, o);
    if (n >= 0) return n;
    lua_pushboolean(L, 1);
    return 1;
}

int batch(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    core::SceneStore& st = store_of(L);
    lua_settop(L, 1);
    st.begin_batch();
    // Nothing between begin_batch() and here can raise, and the pcall
    // catches everything fn raises, timeouts and memory faults included:
    // every begin is matched by an abort or a commit.
    const int status = lua_pcall(L, 0, 0, 0);
    Vm* vm = Vm::of(L);
    if (status != LUA_OK || vm->past_limits()) {
        st.abort_batch();
        if (status != LUA_OK) return lua_error(L);
        vm->check_limits(L);
    }
    Outcome o;
    do_commit(st, &o);
    const int n = settle(L, o);
    if (n >= 0) return n;
    lua_pushboolean(L, 1);
    return 1;
}

int collection(lua_State* L) {
    luaL_checktype(L, 1, LUA_TSTRING);
    size_t n = 0;
    const char* name = lua_tolstring(L, 1, &n);
    if (!valid_collection(name, n))
        return luaL_argerror(L, 1, "collection names are 1-32 bytes of a-z 0-9 _ - (not 'kv')");
    store_of(L);
    auto* h = static_cast<CollHandle*>(lua_newuserdatauv(L, sizeof(CollHandle), 0));
    h->len = static_cast<uint8_t>(n);
    std::memcpy(h->name, name, n);
    luaL_setmetatable(L, kCollType);
    return 1;
}

int insert(lua_State* L) {
    const CollHandle& c = check_coll(L);
    check_record(L, 2);
    core::SceneStore& st = store_of(L);
    lua_settop(L, 2);
    luaL_checkstack(L, kLuaJsonStackSlots, "wisp.store");
    Outcome o;
    do_insert(L, st, c.view(), 2, &o);
    const int n = settle(L, o);
    if (n >= 0) return n;
    lua_pushinteger(L, static_cast<lua_Integer>(o.id));
    return 1;
}

int update(lua_State* L) {
    const CollHandle& c = check_coll(L);
    const lua_Integer id = luaL_checkinteger(L, 2);
    check_record(L, 3);
    core::SceneStore& st = store_of(L);
    lua_settop(L, 3);
    luaL_checkstack(L, kLuaJsonStackSlots, "wisp.store");
    Outcome o;
    do_update(L, st, c.view(), static_cast<int64_t>(id), 3, &o);
    const int n = settle(L, o);
    if (n >= 0) return n;
    lua_pushboolean(L, 1);
    return 1;
}

int del(lua_State* L) {
    const CollHandle& c = check_coll(L);
    const lua_Integer id = luaL_checkinteger(L, 2);
    core::SceneStore& st = store_of(L);
    Outcome o;
    do_delete(st, c.view(), static_cast<int64_t>(id), &o);
    const int n = settle(L, o);
    if (n >= 0) return n;
    lua_pushboolean(L, 1);
    return 1;
}

lua_Integer opt_count(lua_State* L, int opts, const char* field) {
    lua_getfield(L, opts, field);
    lua_Integer v = -1;
    if (!lua_isnil(L, -1)) {
        int ok = 0;
        v = lua_tointegerx(L, -1, &ok);
        if (!ok || v < 0) {
            lua_pushfstring(L, "opts.%s must be a non-negative integer", field);
            luaL_argerror(L, opts, lua_tostring(L, -1));
        }
    }
    lua_pop(L, 1);
    return v;
}

int find(lua_State* L) {
    const CollHandle& c = check_coll(L);
    core::SceneStore& st = store_of(L);
    lua_settop(L, 3);
    FindArgs a;
    if (!lua_isnil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        Vm* vm = Vm::of(L);
        size_t count = 0;
        lua_pushnil(L);
        while (lua_next(L, 2) != 0) {
            const int kt = lua_type(L, -2);
            const int vt = lua_type(L, -1);
            if (kt == LUA_TSTRING) {
                size_t n = 0;
                const char* k = lua_tolstring(L, -2, &n);
                if (is_id(k, n)) {
                    int ok = 0;
                    const lua_Integer id = vt == LUA_TNUMBER ? lua_tointegerx(L, -1, &ok) : 0;
                    if (!ok) {
                        lua_pop(L, 2);
                        return luaL_argerror(L, 2, "filter.id must be an integer");
                    }
                    a.has_id = true;
                    a.id = static_cast<int64_t>(id);
                }
            }
            lua_pop(L, 1);
            if (++count % kCheckEvery == 0) vm->check_limits(L);
            if (kt != LUA_TSTRING) {
                lua_pop(L, 1);
                return luaL_argerror(L, 2, "filter keys must be strings");
            }
            if (vt != LUA_TBOOLEAN && vt != LUA_TNUMBER && vt != LUA_TSTRING && vt != LUA_TTABLE) {
                lua_pop(L, 1);
                return luaL_argerror(L, 2,
                                     "filter values must be booleans, numbers, strings "
                                     "or tables");
            }
        }
        a.filter = 2;
    }
    if (!lua_isnil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        const int t = lua_getfield(L, 3, "sort");  // stays at 4
        if (t != LUA_TNIL && t != LUA_TSTRING)
            return luaL_argerror(L, 3, "opts.sort must be a string");
        if (t == LUA_TSTRING) a.sort = lua_tolstring(L, 4, &a.sort_len);
        // The store's own order is id order.
        if (a.sort != nullptr && is_id(a.sort, a.sort_len)) a.sort = nullptr;
        lua_getfield(L, 3, "desc");
        a.desc = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        const lua_Integer limit = opt_count(L, 3, "limit");
        const lua_Integer offset = opt_count(L, 3, "offset");
        if (limit >= 0) a.limit = static_cast<size_t>(limit);
        if (offset >= 0) a.offset = static_cast<size_t>(offset);
    }
    luaL_checkstack(L, kLuaJsonStackSlots, "wisp.store");
    Outcome o;
    do_find(L, st, c.view(), a, &o);
    const int n = settle(L, o);
    if (n >= 0) return n;
    if (o.empty) lua_newtable(L);
    return 1;  // protect() left the list on top, or it is the empty one
}

}  // namespace

namespace detail {

void install_store(lua_State* L, int wisp) {
    wisp = lua_absindex(L, wisp);
    new_binding_metatable(L, kCollType);
    lua_createtable(L, 0, 4);
    lua_pushcfunction(L, insert);
    lua_setfield(L, -2, "insert");
    lua_pushcfunction(L, update);
    lua_setfield(L, -2, "update");
    lua_pushcfunction(L, del);
    lua_setfield(L, -2, "delete");
    lua_pushcfunction(L, find);
    lua_setfield(L, -2, "find");
    check_c_functions(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    lua_createtable(L, 0, 4);
    lua_pushcfunction(L, get);
    lua_setfield(L, -2, "get");
    lua_pushcfunction(L, set);
    lua_setfield(L, -2, "set");
    lua_pushcfunction(L, batch);
    lua_setfield(L, -2, "batch");
    lua_pushcfunction(L, collection);
    lua_setfield(L, -2, "collection");
    check_c_functions(L, -1);
    lua_setfield(L, wisp, "store");
}

}  // namespace detail

}  // namespace wisp::scene
