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
//   wisp.http.request(opts, cb) (agent-tools spec 6): opts = {method, url,
//       headers, body, timeout_ms, follow}; method GET (the default),
//       POST, PUT, PATCH, DELETE or HEAD in any case; body as for post (a
//       table also gets the JSON content type); timeout_ms an integer
//       1000-120000 and follow a boolean (follow redirects, up to
//       AgentHttpLimits::kMaxRedirects). Both are checked for every
//       principal but used only for the Agent one: a scene's request
//       keeps the scene rules (no redirects, the proxy's timeout).
//       cb as for get.
// Requests go out as SceneApi::http_principal, with that principal's size
// caps. In a task the callback may be left out: the call then waits and
// returns cb's arguments (scene_api.hpp, lua_task.hpp).
// Malformed arguments (url not a string, body not a string or table, opts
// or opts.headers not a table, a header name or value not a string, cb
// not a function, a bad request() field) raise argument errors.
//
// Callbacks wait in a registry table by id until their result arrives;
// results for a closed scene never arrive (the runtime cancels its
// requests and drops stale messages), and the table goes with the VM. A
// waiting task's thread waits there in place of a function.
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
// request()'s shortest timeout_ms (agent-tools spec 3.1).
constexpr lua_Integer kMinTimeoutMs = 1000;

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

// The size caps the proxy applies to the VM's principal (SceneHttp's for
// a scene, AgentHttpLimits for the agent).
struct Caps {
    size_t url;
    size_t headers;
    size_t header_bytes;
    size_t body;
};

Caps caps_for(net::HttpPrincipal p) {
    if (p == net::HttpPrincipal::Agent)
        return {net::AgentHttpLimits::kMaxUrlBytes, net::AgentHttpLimits::kMaxHeaders,
                net::AgentHttpLimits::kMaxHeaderBytes, net::AgentHttpLimits::kMaxRequestBody};
    return {net::SceneHttp::kMaxUrlBytes, net::SceneHttp::kMaxHeaders,
            net::SceneHttp::kMaxHeaderBytes, net::SceneHttp::kMaxRequestBody};
}

struct SendArgs {
    net::Method method = net::Method::Get;
    int url = 1;      // stack index of the URL
    int body = 0;     // stack index of the body, or 0
    int headers = 0;  // stack index of opts.headers, or 0
    uint32_t timeout_ms = 0;
    bool follow = false;
    uint32_t id = 0;
};

// Longer than the proxy accepts is too long whatever the rest: one byte
// over the cap keeps that outcome without copying the whole of an
// oversized value.
std::string_view capped(std::string_view s, size_t cap) { return s.substr(0, cap + 1); }

// Fills in the URL (at `url`) and opts.headers (at `headers`, or 0) with
// non-raising Lua calls only. True if a content type was among them.
bool fill_request(lua_State* L, net::SceneHttpRequest& req, int url, int headers,
                  const Caps& caps) {
    const std::string_view u = capped(view_at(L, url), caps.url);
    req.url.assign(u.data(), u.size());
    bool has_type = false;
    if (headers != 0) {
        lua_pushnil(L);
        while (lua_next(L, headers) != 0) {
            if (req.headers.size() > caps.headers) {
                // Already one over: the proxy refuses the request.
                lua_pop(L, 2);
                break;
            }
            const std::string_view name = capped(view_at(L, -2), caps.header_bytes);
            const std::string_view value = capped(view_at(L, -1), caps.header_bytes);
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
    const Caps caps = caps_for(api.http_principal);
    net::SceneHttpRequest req;
    req.principal = api.http_principal;
    req.method = a.method;
    // Only the agent may choose its deadline and follow redirects: SceneHttp
    // honours a request's timeout_ms whatever its principal, so a scene's
    // request keeps 0 (the proxy's own deadline) and never follows.
    if (api.http_principal == net::HttpPrincipal::Agent) {
        req.timeout_ms = a.timeout_ms;
        req.max_redirects = a.follow ? net::AgentHttpLimits::kMaxRedirects : 0;
    }
    const bool has_type = fill_request(L, req, a.url, a.headers, caps);
    if (a.body != 0) {
        if (lua_type(L, a.body) == LUA_TSTRING) {
            const std::string_view body = capped(view_at(L, a.body), caps.body);
            req.body.assign(body.data(), body.size());
        } else {
            const char* code = lua_to_json(L, a.body, req.body, caps.body);
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

// Registers the callback (or waiting thread) at `cb` and hands the request
// over; returns what the binding returns.
int submit(lua_State* L, SendArgs& a, int cb, bool waits) {
    SceneApi& api = detail::api_of(L);
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
    return waits ? detail::wait(L, &kCallbacksKey, a.id, false) : 0;
}

void check_available(lua_State* L) {
    SceneApi& api = detail::api_of(L);
    if (!api.http_submit || !api.http_fail) luaL_error(L, "wisp.http is not available");
}

// get(url, [opts], [cb]) / post(url, body, [opts], [cb]); without cb only
// in a task, which waits.
int send(lua_State* L, bool post) {
    SendArgs a;
    a.method = post ? net::Method::Post : net::Method::Get;
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
    const bool waits = detail::take_callback(L, cb);
    check_available(L);
    a.headers = check_opts(L, opts);
    return submit(L, a, cb, waits);
}

bool parse_method(std::string_view m, net::Method* out) {
    static constexpr struct {
        const char* name;
        net::Method method;
    } kMethods[] = {
        {"GET", net::Method::Get},     {"POST", net::Method::Post},     {"PUT", net::Method::Put},
        {"PATCH", net::Method::Patch}, {"DELETE", net::Method::Delete}, {"HEAD", net::Method::Head},
    };
    for (const auto& k : kMethods) {
        if (iequals(m, k.name)) {
            *out = k.method;
            return true;
        }
    }
    return false;
}

// request(opts, [cb]): opts = {method, url, headers, body, timeout_ms,
// follow}; without cb only in a task, which waits.
int request(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const bool waits = detail::take_callback(L, 2);
    lua_getfield(L, 1, "method");      // 3
    lua_getfield(L, 1, "url");         // 4
    lua_getfield(L, 1, "headers");     // 5
    lua_getfield(L, 1, "body");        // 6
    lua_getfield(L, 1, "timeout_ms");  // 7
    lua_getfield(L, 1, "follow");      // 8
    SendArgs a;
    a.url = 4;
    if (!lua_isnil(L, 3) &&
        (lua_type(L, 3) != LUA_TSTRING || !parse_method(view_at(L, 3), &a.method)))
        return luaL_argerror(L, 1, "opts.method must be GET, POST, PUT, PATCH, DELETE or HEAD");
    if (lua_type(L, 4) != LUA_TSTRING) return luaL_argerror(L, 1, "opts.url must be a string");
    if (!lua_isnil(L, 5)) {
        if (!lua_istable(L, 5)) return luaL_argerror(L, 1, "opts.headers must be a table");
        check_headers(L, 5, 1);
        a.headers = 5;
    }
    if (!lua_isnil(L, 6)) {
        if (lua_type(L, 6) != LUA_TSTRING && lua_type(L, 6) != LUA_TTABLE)
            return luaL_argerror(L, 1, "opts.body must be a string or table");
        a.body = 6;
    }
    if (!lua_isnil(L, 7)) {
        const lua_Integer t = lua_isinteger(L, 7) ? lua_tointeger(L, 7) : 0;
        if (t < kMinTimeoutMs || t > static_cast<lua_Integer>(net::AgentHttpLimits::kMaxTimeoutMs))
            return luaL_argerror(L, 1, "opts.timeout_ms must be an integer 1000-120000");
        a.timeout_ms = static_cast<uint32_t>(t);
    }
    if (!lua_isnil(L, 8)) {
        if (!lua_isboolean(L, 8)) return luaL_argerror(L, 1, "opts.follow must be a boolean");
        a.follow = lua_toboolean(L, 8) != 0;
    }
    check_available(L);
    return submit(L, a, 2, waits);
}

// The C++ part of download(): non-raising Lua calls only.
void do_download(lua_State* L, SceneApi& api, uint32_t id, int headers) {
    const std::string_view name = view_at(L, 2);
    if (!core::valid_file_name(name)) return api.files_fail(id, "bad_name");
    net::SceneHttpRequest req;
    req.principal = api.http_principal;
    req.method = net::Method::Get;
    fill_request(L, req, 1, headers, caps_for(api.http_principal));
    api.files_download(id, std::move(req), std::string(name));
}

// download(url, name, [opts], [cb]); without cb only in a task, which
// waits.
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
    const bool waits = detail::take_callback(L, cb);
    SceneApi& api = detail::api_of(L);
    if (!api.files_download || !api.files_fail)
        return luaL_error(L, "wisp.http.download is not available");
    const int headers = check_opts(L, opts);
    const uint32_t id = detail::register_files_callback(L, api, cb);
    do_download(L, api, id, headers);
    return waits ? detail::wait_files(L, id) : 0;
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

// Pushes cb's two arguments: err, nil or nil, res.
void push_args(lua_State* L, const net::SceneHttpResult& r) {
    if (!r.error.empty()) {
        lua_pushlstring(L, r.error.data(), r.error.size());
        lua_pushnil(L);
        return;
    }
    lua_pushnil(L);
    lua_createtable(L, 0, 5);
    const int res = lua_gettop(L);
    lua_pushinteger(L, r.status);
    lua_setfield(L, res, "status");
    lua_createtable(L, 0, static_cast<int>(r.headers.size()));
    const int headers = lua_gettop(L);
    bool json = false;
    for (const auto& [name, value] : r.headers) {
        set_header(L, headers, name, value);
        json = json || (name == "content-type" && mentions_json(value));
    }
    lua_setfield(L, res, "headers");
    const std::string_view body(reinterpret_cast<const char*>(r.body.data()), r.body.size());
    lua_pushlstring(L, body.data(), body.size());
    lua_setfield(L, res, "body");
    if (r.deferred) {
        lua_pushboolean(L, 1);
        lua_setfield(L, res, "deferred");
    }
    if (json) {
        if (push_json(L, body)) {
            lua_setfield(L, res, "json");
        } else {
            // Not JSON after all: no field. A failure that was the VM's
            // limits raises here.
            Vm::of(L)->check_limits(L);
        }
    }
}

int run_entry(lua_State* L) {
    const auto* c = static_cast<const RunCtx*>(lua_touserdata(L, 1));
    luaL_checkstack(L, 8, "wisp.http");
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kCallbacksKey);        // 2
    if (lua_rawgeti(L, 2, c->id) != LUA_TFUNCTION) return 0;  // 3
    lua_pushnil(L);
    lua_rawseti(L, 2, c->id);
    push_args(L, *c->result);
    lua_call(L, 2, 0);
    return 0;
}

// The waiting task's values (detail::resume_waiting()).
int args_entry(lua_State* L) {
    const auto* c = static_cast<const RunCtx*>(lua_touserdata(L, 1));
    lua_settop(L, 0);
    luaL_checkstack(L, 8, "wisp.http");
    push_args(L, *c->result);
    return 2;
}

}  // namespace

namespace detail {

void install_http(lua_State* L, int wisp) {
    wisp = lua_absindex(L, wisp);
    lua_newtable(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kCallbacksKey);
    lua_createtable(L, 0, 4);
    lua_pushcfunction(L, get);
    lua_setfield(L, -2, "get");
    lua_pushcfunction(L, request);
    lua_setfield(L, -2, "request");
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
    if (detail::is_waiting(L, &kCallbacksKey, id))
        return detail::resume_waiting(vm, &kCallbacksKey, id, args_entry, &ctx);
    lua_pushcfunction(L, run_entry);
    lua_pushlightuserdata(L, &ctx);
    return vm.call(1, 0, Budget::Fresh);
}

}  // namespace wisp::scene
