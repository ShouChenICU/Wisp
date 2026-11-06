// wisp.http (scene-runtime-core spec 5.3): a thin adapter over the
// net::SceneHttp proxy, which holds every rule (hosts, https, headers,
// sizes, rate).
//
//   wisp.http.get(url, opts, cb)
//   wisp.http.post(url, body, opts, cb)
//       opts (may be left out or nil): {headers = {name = value, ...}};
//       body: a string, sent as it is, or a table, sent as JSON (lua_json.hpp)
//       with "Content-Type: application/json" unless opts.headers names a
//       content type. cb(err, nil) or cb(nil, {status, headers, body,
//       json}) runs later, as a message of its own with a fresh time
//       budget: headers is {lowercase name = value} (repeated headers
//       joined with ", "), body a string, json the decoded body when
//       content-type contains "json" and the body parses, and deferred =
//       true when the request waited for deep standby's network window
//       (scene-device spec 4.4; absent otherwise). The flag rides only on
//       res, as the spec puts it: an error passes no res, so a deferred
//       request that then fails cannot be told from one that did not wait
//       -- except deferred_timeout, which only a waiting request gets.
//   err: permission_denied, not_https, too_large, rate_limited,
//       deferred_queue_full (the scene already has 4 waiting),
//       deferred_timeout (waited an hour without a window that connected),
//       dns, connect, tls, timeout, other (from the proxy), or
//       not_serializable, too_deep (a table body that cannot be encoded)
//       -- always through cb, never at the call.
//   wisp.http.download(url, name, opts, cb) (scene-device spec 7.1): GET
//       url into the scene's file `name` (wisp.files) through
//       SceneApi::files_download; opts as for get. cb(err, nil) or
//       cb(nil, {status, size, deferred}), with the callback kept by
//       bind_files.cpp. A bad name is "bad_name", through cb.
// Malformed arguments (url not a string, body not a string or table, opts
// or opts.headers not a table, a header name or value not a string, cb
// not a function) raise argument errors.
//
// Callbacks wait in a registry table by id until their result arrives;
// results for a closed scene never arrive (the runtime cancels its
// requests and drops stale messages), and the table goes with the VM.
//
// longjmp rule (vm.hpp): send() checks its arguments with raising Lua
// calls while its frame holds only trivially destructible locals, then
// do_send() builds the request with non-raising Lua calls only and hands
// it over; anything left to raise happens after do_send() has returned.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

#include "scene_api.hpp"
#include "wisp/scene/lua_json.hpp"

namespace wisp::scene {

namespace {

// Registry key of the table of pending callbacks, by request id.
char kCallbacksKey;
// How often the header checks look at the VM's limits.
constexpr size_t kCheckEvery = 256;

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

bool mentions_json(std::string_view s) {
    for (size_t i = 0; i + 4 <= s.size(); ++i)
        if (iequals(s.substr(i, 4), "json")) return true;
    return false;
}

std::string_view view_at(lua_State* L, int idx) {
    size_t n = 0;
    const char* s = lua_tolstring(L, idx, &n);
    return {s, n};
}

struct SendArgs {
    bool post = false;
    int body = 0;     // stack index of the body, or 0
    int headers = 0;  // stack index of opts.headers, or 0
    uint32_t id = 0;
};

// Longer than the proxy accepts is too long whatever the rest: one byte
// over the cap keeps that outcome without copying the whole of an
// oversized value.
std::string_view capped(std::string_view s, size_t cap) { return s.substr(0, cap + 1); }

// Fills in the URL (at 1) and opts.headers (at `headers`, or 0) with
// non-raising Lua calls only. True if a content type was among them.
bool fill_request(lua_State* L, net::SceneHttpRequest& req, int headers) {
    const std::string_view url = capped(view_at(L, 1), net::SceneHttp::kMaxUrlBytes);
    req.url.assign(url.data(), url.size());
    bool has_type = false;
    if (headers != 0) {
        lua_pushnil(L);
        while (lua_next(L, headers) != 0) {
            if (req.headers.size() > net::SceneHttp::kMaxHeaders) {
                // Already one over: the proxy refuses the request.
                lua_pop(L, 2);
                break;
            }
            const std::string_view name = capped(view_at(L, -2), net::SceneHttp::kMaxHeaderBytes);
            const std::string_view value = capped(view_at(L, -1), net::SceneHttp::kMaxHeaderBytes);
            has_type = has_type || iequals(name, "content-type");
            req.headers.emplace_back(std::string(name), std::string(value));
            lua_pop(L, 1);
        }
    }
    return has_type;
}

// The C++ part of send(): calls only non-raising Lua APIs. Returns false
// when lua_to_json reached the VM's limits (the caller raises).
bool do_send(lua_State* L, SceneApi& api, const SendArgs& a) {
    net::SceneHttpRequest req;
    req.method = a.post ? net::Method::Post : net::Method::Get;
    const bool has_type = fill_request(L, req, a.headers);
    if (a.post) {
        if (lua_type(L, a.body) == LUA_TSTRING) {
            const std::string_view body =
                capped(view_at(L, a.body), net::SceneHttp::kMaxRequestBody);
            req.body.assign(body.data(), body.size());
        } else {
            const char* code = lua_to_json(L, a.body, req.body, net::SceneHttp::kMaxRequestBody);
            if (code == kJsonLimits) return false;
            if (code != nullptr) {
                api.http_fail(a.id, code == kJsonTooLarge ? "too_large" : code);
                return true;
            }
            if (!has_type) req.headers.emplace_back("Content-Type", "application/json");
        }
    }
    api.http_submit(a.id, std::move(req));
    return true;
}

// Checks opts.headers at `idx`: string names and values only.
void check_headers(lua_State* L, int idx, int opts_arg) {
    Vm* vm = Vm::of(L);
    size_t count = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TSTRING)
            luaL_argerror(L, opts_arg, "header names and values must be strings");
        lua_pop(L, 1);
        if (++count % kCheckEvery == 0) vm->check_limits(L);
    }
}

// opts (at `opts`, may be 0 or nil): checks opts.headers and returns its
// stack index, or 0.
int check_opts(lua_State* L, int opts) {
    if (opts == 0 || !lua_istable(L, opts)) return 0;
    lua_getfield(L, opts, "headers");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    if (!lua_istable(L, -1)) luaL_argerror(L, opts, "opts.headers must be a table");
    const int headers = lua_gettop(L);
    check_headers(L, headers, opts);
    return headers;
}

// get(url, [opts], cb) / post(url, body, [opts], cb)
int send(lua_State* L, bool post) {
    SendArgs a;
    a.post = post;
    a.body = post ? 2 : 0;
    int opts = post ? 3 : 2;
    int cb = opts + 1;
    if (lua_type(L, opts) == LUA_TFUNCTION && lua_isnone(L, cb)) {
        cb = opts;
        opts = 0;
    }
    luaL_checktype(L, 1, LUA_TSTRING);
    if (post && lua_type(L, 2) != LUA_TSTRING && lua_type(L, 2) != LUA_TTABLE)
        luaL_typeerror(L, 2, "string or table");
    if (opts != 0 && !lua_isnoneornil(L, opts)) luaL_checktype(L, opts, LUA_TTABLE);
    luaL_checktype(L, cb, LUA_TFUNCTION);
    lua_settop(L, cb);
    SceneApi& api = detail::api_of(L);
    if (!api.http_submit || !api.http_fail) return luaL_error(L, "wisp.http is not available");
    a.headers = check_opts(L, opts);
    luaL_checkstack(L, kLuaJsonStackSlots, "wisp.http");

    a.id = api.next_http_id++;
    if (api.next_http_id == 0) api.next_http_id = 1;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kCallbacksKey);
    lua_pushvalue(L, cb);
    lua_rawseti(L, -2, a.id);
    lua_pop(L, 1);

    if (!do_send(L, api, a)) {
        Vm::of(L)->check_limits(L);
        return luaL_error(L, "wisp.http: limits reached");
    }
    return 0;
}

// The C++ part of download(): non-raising Lua calls only.
void do_download(lua_State* L, SceneApi& api, uint32_t id, int headers) {
    const std::string_view name = view_at(L, 2);
    if (!core::valid_file_name(name)) return api.files_fail(id, "bad_name");
    net::SceneHttpRequest req;
    req.method = net::Method::Get;
    fill_request(L, req, headers);
    api.files_download(id, std::move(req), std::string(name));
}

// download(url, name, [opts], cb)
int download(lua_State* L) {
    int opts = 3;
    int cb = 4;
    if (lua_type(L, opts) == LUA_TFUNCTION && lua_isnone(L, cb)) {
        cb = opts;
        opts = 0;
    }
    luaL_checktype(L, 1, LUA_TSTRING);
    luaL_checktype(L, 2, LUA_TSTRING);
    if (opts != 0 && !lua_isnoneornil(L, opts)) luaL_checktype(L, opts, LUA_TTABLE);
    luaL_checktype(L, cb, LUA_TFUNCTION);
    lua_settop(L, cb);
    SceneApi& api = detail::api_of(L);
    if (!api.files_download || !api.files_fail)
        return luaL_error(L, "wisp.http.download is not available");
    const int headers = check_opts(L, opts);
    const uint32_t id = detail::register_files_callback(L, api, cb);
    do_download(L, api, id, headers);
    return 0;
}

int get(lua_State* L) { return send(L, false); }
int post(lua_State* L) { return send(L, true); }

struct RunCtx {
    uint32_t id;
    const net::SceneHttpResult* result;
};

// headers[name] = value, or the earlier value joined with ", ".
void set_header(lua_State* L, int table, std::string_view name, std::string_view value) {
    lua_pushlstring(L, name.data(), name.size());
    if (lua_rawget(L, table) == LUA_TSTRING) {
        lua_pushliteral(L, ", ");
        lua_pushlstring(L, value.data(), value.size());
        lua_concat(L, 3);
    } else {
        lua_pop(L, 1);
        lua_pushlstring(L, value.data(), value.size());
    }
    lua_pushlstring(L, name.data(), name.size());
    lua_insert(L, -2);
    lua_rawset(L, table);
}

int run_entry(lua_State* L) {
    const auto* c = static_cast<const RunCtx*>(lua_touserdata(L, 1));
    const net::SceneHttpResult& r = *c->result;
    luaL_checkstack(L, 8, "wisp.http");
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kCallbacksKey);        // 2
    if (lua_rawgeti(L, 2, c->id) != LUA_TFUNCTION) return 0;  // 3
    lua_pushnil(L);
    lua_rawseti(L, 2, c->id);
    if (!r.error.empty()) {
        lua_pushlstring(L, r.error.data(), r.error.size());
        lua_pushnil(L);
        lua_call(L, 2, 0);
        return 0;
    }
    lua_pushnil(L);            // 4
    lua_createtable(L, 0, 5);  // 5
    lua_pushinteger(L, r.status);
    lua_setfield(L, 5, "status");
    lua_createtable(L, 0, static_cast<int>(r.headers.size()));  // 6
    bool json = false;
    for (const auto& [name, value] : r.headers) {
        set_header(L, 6, name, value);
        json = json || (name == "content-type" && mentions_json(value));
    }
    lua_setfield(L, 5, "headers");
    const std::string_view body(reinterpret_cast<const char*>(r.body.data()), r.body.size());
    lua_pushlstring(L, body.data(), body.size());
    lua_setfield(L, 5, "body");
    if (r.deferred) {
        lua_pushboolean(L, 1);
        lua_setfield(L, 5, "deferred");
    }
    if (json) {
        if (push_json(L, body)) {
            lua_setfield(L, 5, "json");
        } else {
            // Not JSON after all: no field. A failure that was the VM's
            // limits raises here.
            Vm::of(L)->check_limits(L);
        }
    }
    lua_call(L, 2, 0);
    return 0;
}

}  // namespace

namespace detail {

void install_http(lua_State* L, int wisp) {
    wisp = lua_absindex(L, wisp);
    lua_newtable(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kCallbacksKey);
    lua_createtable(L, 0, 3);
    lua_pushcfunction(L, get);
    lua_setfield(L, -2, "get");
    lua_pushcfunction(L, download);
    lua_setfield(L, -2, "download");
    lua_pushcfunction(L, post);
    lua_setfield(L, -2, "post");
    check_c_functions(L, -1);
    lua_setfield(L, wisp, "http");
}

}  // namespace detail

std::optional<ScriptError> run_http_callback(Vm& vm, uint32_t id, const net::SceneHttpResult& r) {
    lua_State* L = vm.L();
    if (!lua_checkstack(L, 2)) return ScriptError{ScriptFault::Memory, "", 0, "stack overflow"};
    RunCtx ctx{id, &r};
    lua_pushcfunction(L, run_entry);
    lua_pushlightuserdata(L, &ctx);
    return vm.call(1, 0, Budget::Fresh);
}

}  // namespace wisp::scene
