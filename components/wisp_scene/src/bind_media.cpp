// wisp.media (scene_api.hpp; media spec 7.1): thin adapters over
// media::MediaPlayer and media::MediaLibrary. Nothing here touches a file:
// the player only takes commands on its own task, and every library call
// queues a job on the card's worker whose completion hands its result to
// SceneApi::media_reply, which brings it back to run_files_callback() as
// a message of its own. The callbacks share wisp.files' table and count
// (register_files_callback()), so a result for a scene that has closed
// goes to a closed box and is never seen.
//
// Permissions (spec 7.2) are decided here, before the library is asked:
// playback needs none; reads need permissions.media "read" or "write";
// writes "write". A refusal is answered "permission_denied" through the
// callback, like any other error. A scene names only its own files: a
// {file = name} item becomes "scene:<this scene's id>/<name>", and a
// string that names a scene item is refused.
//
// longjmp rule (vm.hpp): each binding checks its arguments with raising
// Lua calls while its frame holds only trivially destructible locals (the
// strings stay on the Lua stack), then a do_* function builds the C++
// objects with non-raising Lua calls only.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "scene_api.hpp"
#include "wisp/media/errors.hpp"
#include "wisp/media/item_reader.hpp"
#include "wisp/media/path.hpp"

namespace wisp::scene {

namespace {

using core::MediaAccess;

constexpr const char* kUnavailable = "media_unavailable";
constexpr const char* kDenied = "permission_denied";
constexpr const char* kInvalidPath = "invalid_path";

std::string_view view_at(lua_State* L, int idx) {
    size_t n = 0;
    const char* s = lua_tolstring(L, idx, &n);
    return {s, n};
}

SceneApi& checked_api(lua_State* L) {
    SceneApi& api = detail::api_of(L);
    if (!api.media_reply) luaL_error(L, "wisp.media is not available");
    return api;
}

void reply_error(SceneApi& api, uint32_t id, const char* code) {
    FilesResult r;
    r.error = code;
    api.media_reply(id, std::move(r));
}

// The reply for a library completion: runs on the storage worker, so it
// only moves the result into a FilesResult and hands it on.
template <class T>
media::MediaLibrary::Done<T> replier(SceneApi& api, uint32_t id) {
    return [reply = api.media_reply, id](Result<T> r) {
        FilesResult out;
        if (!r) {
            out.error = media::wire_name(r.error());
        } else if constexpr (std::is_void_v<T>) {
            out.payload = std::monostate{};
        } else {
            out.payload = std::move(r.value());
        }
        reply(id, std::move(out));
    };
}

// Whether the scene may do it; otherwise answers why: the permission
// first (decided by scene.json alone, so a scene without it learns that
// whether or not a card is in), then the library.
bool allowed(SceneApi& api, uint32_t id, MediaAccess need) {
    const bool granted = need == MediaAccess::Read ? api.media_access != MediaAccess::None
                                                   : api.media_access == MediaAccess::Write;
    if (!granted) {
        reply_error(api, id, kDenied);
        return false;
    }
    if (api.media_library == nullptr) {
        reply_error(api, id, kUnavailable);
        return false;
    }
    return true;
}

// ---- playback -------------------------------------------------------------

// A string item: a library path, never a scene item.
bool library_item_ok(std::string_view s) {
    return s.substr(0, media::kSceneItemPrefix.size()) != media::kSceneItemPrefix &&
           media::normalize(s).is_ok();
}

// Non-raising: plays the items in the tables at `names` (strings) and
// `files` (true where the name is a {file = name}), `n` of them.
void do_play(lua_State* L, SceneApi& api, uint32_t id, bool has_cb, int names, int files,
             lua_Integer n, bool cut, lua_Integer start, int mode) {
    auto fail = [&](const char* code) {
        if (has_cb) reply_error(api, id, code);
    };
    if (api.media_player == nullptr) return fail(kUnavailable);
    std::vector<std::string> items;
    items.reserve(static_cast<size_t>(n));
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, names, i);
        const std::string_view name = view_at(L, -1);
        lua_pop(L, 1);
        lua_rawgeti(L, files, i);
        const bool file = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        if (file) {
            if (!core::valid_file_name(name)) return fail(kInvalidPath);
            items.push_back(std::string(media::kSceneItemPrefix) + api.scene_id + "/" +
                            std::string(name));
        } else {
            if (!library_item_ok(name)) return fail(kInvalidPath);
            items.emplace_back(name);
        }
    }
    std::optional<media::PlayMode> m;
    if (mode != 0) m = media::play_mode_from(view_at(L, mode));
    media::PlayQueue::SetResult r =
        api.media_player->play(std::move(items), static_cast<size_t>(start - 1), m);
    r.truncated = r.truncated || cut;
    if (!has_cb) return;
    FilesResult out;
    out.payload = r;
    api.media_reply(id, std::move(out));
}

// play(items, [opts], [cb])
int play(lua_State* L) {
    int opts = 2;
    int cb = 3;
    if (lua_type(L, 2) == LUA_TFUNCTION && lua_isnoneornil(L, 3)) {
        cb = 2;
        opts = 0;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    if (opts != 0 && !lua_isnoneornil(L, opts)) luaL_checktype(L, opts, LUA_TTABLE);
    if (!lua_isnoneornil(L, cb)) luaL_checktype(L, cb, LUA_TFUNCTION);
    const bool has_cb = !lua_isnoneornil(L, cb);
    lua_settop(L, 3);
    if (opts == 0) {
        lua_pushvalue(L, 2);
        lua_replace(L, 3);
        lua_pushnil(L);
        lua_replace(L, 2);
        opts = 2;
        cb = 3;
    }
    lua_Integer start = 1;
    int mode = 0;
    if (lua_istable(L, opts)) {
        if (lua_getfield(L, opts, "start") != LUA_TNIL) {  // 4
            if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 1)
                luaL_argerror(L, opts, "opts.start must be an integer from 1");
            start = lua_tointeger(L, -1);
        }
        if (lua_getfield(L, opts, "mode") != LUA_TNIL) {  // 5
            if (lua_type(L, -1) != LUA_TSTRING || !media::play_mode_from(view_at(L, -1)))
                luaL_argerror(L, opts,
                              "opts.mode must be sequence, repeat_all, repeat_one or shuffle");
            mode = lua_gettop(L);
        }
    }
    const lua_Integer total = static_cast<lua_Integer>(lua_rawlen(L, 1));
    const lua_Integer n = std::min<lua_Integer>(total, static_cast<lua_Integer>(media::kMaxQueue));
    lua_createtable(L, static_cast<int>(n), 0);
    const int names = lua_gettop(L);
    lua_createtable(L, 0, 0);
    const int files = lua_gettop(L);
    Vm* vm = Vm::of(L);
    for (lua_Integer i = 1; i <= n; ++i) {
        const int t = lua_rawgeti(L, 1, i);
        if (t == LUA_TTABLE) {
            if (lua_getfield(L, -1, "file") != LUA_TSTRING)
                luaL_argerror(L, 1, "an item must be a string or {file = name}");
            lua_rawseti(L, names, i);
            lua_pushboolean(L, 1);
            lua_rawseti(L, files, i);
        } else if (t == LUA_TSTRING) {
            lua_pushvalue(L, -1);
            lua_rawseti(L, names, i);
        } else {
            luaL_argerror(L, 1, "an item must be a string or {file = name}");
        }
        lua_pop(L, 1);
        if (i % 64 == 0) vm->check_limits(L);
    }
    SceneApi& api = checked_api(L);
    uint32_t id = 0;
    if (has_cb) id = detail::register_files_callback(L, api, cb);
    do_play(L, api, id, has_cb, names, files, n, total > n, start, mode);
    return 0;
}

// For the controls: true, or nil, "media_unavailable".
int sent(lua_State* L, bool ok) {
    if (ok) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, kUnavailable);
    return 2;
}

media::MediaPlayer* player_of(lua_State* L) { return checked_api(L).media_player; }

int pause(lua_State* L) {
    media::MediaPlayer* p = player_of(L);
    if (p != nullptr) p->pause();
    return sent(L, p != nullptr);
}

int resume(lua_State* L) {
    media::MediaPlayer* p = player_of(L);
    if (p != nullptr) p->resume();
    return sent(L, p != nullptr);
}

int next(lua_State* L) {
    media::MediaPlayer* p = player_of(L);
    if (p != nullptr) p->next();
    return sent(L, p != nullptr);
}

int prev(lua_State* L) {
    media::MediaPlayer* p = player_of(L);
    if (p != nullptr) p->prev();
    return sent(L, p != nullptr);
}

int stop(lua_State* L) {
    media::MediaPlayer* p = player_of(L);
    if (p != nullptr) p->stop_playback();
    return sent(L, p != nullptr);
}

int seek(lua_State* L) {
    const lua_Integer s = luaL_checkinteger(L, 1);
    luaL_argcheck(L, s >= 0 && s <= std::numeric_limits<uint32_t>::max(), 1,
                  "seconds must not be negative");
    media::MediaPlayer* p = player_of(L);
    if (p != nullptr) p->seek(static_cast<uint32_t>(s));
    return sent(L, p != nullptr);
}

int set_mode(lua_State* L) {
    luaL_checktype(L, 1, LUA_TSTRING);
    const std::optional<media::PlayMode> m = media::play_mode_from(view_at(L, 1));
    if (!m) return luaL_argerror(L, 1, "mode must be sequence, repeat_all, repeat_one or shuffle");
    media::MediaPlayer* p = player_of(L);
    if (p != nullptr) p->set_mode(*m);
    return sent(L, p != nullptr);
}

int push_state_entry(lua_State* L) {
    detail::push_media_state(L, *static_cast<const media::PlayerState*>(lua_touserdata(L, 1)));
    return 1;
}

int state(lua_State* L) {
    SceneApi& api = checked_api(L);
    lua_settop(L, 0);
    int status = LUA_OK;
    {
        // The copy holds strings: it is pushed under a protected call, so
        // a raise there unwinds to here and the copy is destroyed before
        // the error goes on.
        const media::PlayerState s =
            api.media_player != nullptr ? api.media_player->state().get() : media::PlayerState{};
        lua_pushcfunction(L, push_state_entry);
        lua_pushlightuserdata(L, const_cast<media::PlayerState*>(&s));
        status = lua_pcall(L, 1, 1, 0);
    }
    if (status != LUA_OK) return lua_error(L);
    return 1;
}

// ---- library --------------------------------------------------------------

// Pushes t[name] (nil without a table); raises unless it is nil or a
// string.
void opt_string(lua_State* L, int t, const char* name) {
    if (t == 0) {
        lua_pushnil(L);
        return;
    }
    const int type = lua_getfield(L, t, name);
    if (type != LUA_TNIL && type != LUA_TSTRING)
        luaL_argerror(L, 1, "query fields q, artist and album must be strings");
}

lua_Integer opt_count(lua_State* L, int t, const char* name, lua_Integer dflt) {
    if (t == 0) return dflt;
    const int type = lua_getfield(L, t, name);
    lua_Integer v = dflt;
    if (type != LUA_TNIL) {
        if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 0)
            luaL_argerror(L, 1, "query offset and limit must be integers from 0");
        v = std::min<lua_Integer>(lua_tointeger(L, -1), std::numeric_limits<uint32_t>::max());
    }
    lua_pop(L, 1);
    return v;
}

void do_tracks(lua_State* L, SceneApi& api, uint32_t id, int q, lua_Integer offset,
               lua_Integer limit) {
    if (!allowed(api, id, MediaAccess::Read)) return;
    media::TrackQuery query;
    if (lua_type(L, q) == LUA_TSTRING) query.q = std::string(view_at(L, q));
    if (lua_type(L, q + 1) == LUA_TSTRING) query.artist = std::string(view_at(L, q + 1));
    if (lua_type(L, q + 2) == LUA_TSTRING) query.album = std::string(view_at(L, q + 2));
    query.offset = static_cast<uint32_t>(offset);
    query.limit = static_cast<uint32_t>(limit);
    api.media_library->tracks(std::move(query), replier<media::TrackPage>(api, id));
}

// tracks(query, cb): query {q, artist, album, offset, limit} or nil.
int tracks(lua_State* L) {
    if (!lua_isnoneornil(L, 1)) luaL_checktype(L, 1, LUA_TTABLE);
    const bool waits = detail::take_callback(L, 2);
    const int t = lua_istable(L, 1) ? 1 : 0;
    const lua_Integer offset = opt_count(L, t, "offset", 0);
    const lua_Integer limit = opt_count(L, t, "limit", 50);
    opt_string(L, t, "q");  // 3
    opt_string(L, t, "artist");
    opt_string(L, t, "album");
    SceneApi& api = checked_api(L);
    const uint32_t id = detail::register_files_callback(L, api, 2);
    do_tracks(L, api, id, 3, offset, limit);
    return waits ? detail::wait_files(L, id) : 0;
}

void do_list(lua_State* L, SceneApi& api, uint32_t id) {
    if (!allowed(api, id, MediaAccess::Read)) return;
    std::string dir = lua_type(L, 1) == LUA_TSTRING ? std::string(view_at(L, 1)) : std::string();
    api.media_library->list(std::move(dir), replier<std::vector<media::MediaEntry>>(api, id));
}

// list(dir, cb): dir nil or "" is the root.
int list(lua_State* L) {
    if (!lua_isnoneornil(L, 1)) luaL_checktype(L, 1, LUA_TSTRING);
    const bool waits = detail::take_callback(L, 2);
    SceneApi& api = checked_api(L);
    const uint32_t id = detail::register_files_callback(L, api, 2);
    do_list(L, api, id);
    return waits ? detail::wait_files(L, id) : 0;
}

void do_stat(lua_State* L, SceneApi& api, uint32_t id) {
    if (!allowed(api, id, MediaAccess::Read)) return;
    api.media_library->stat(std::string(view_at(L, 1)), replier<media::MediaEntry>(api, id));
}

int stat(lua_State* L) {
    luaL_checktype(L, 1, LUA_TSTRING);
    const bool waits = detail::take_callback(L, 2);
    SceneApi& api = checked_api(L);
    const uint32_t id = detail::register_files_callback(L, api, 2);
    do_stat(L, api, id);
    return waits ? detail::wait_files(L, id) : 0;
}

void do_read(lua_State* L, SceneApi& api, uint32_t id, uint64_t offset, size_t len) {
    if (!allowed(api, id, MediaAccess::Read)) return;
    api.media_library->read(std::string(view_at(L, 1)), offset, len,
                            replier<os::LargeBuffer>(api, id));
}

// read(path, offset, len, cb)
int read(lua_State* L) {
    luaL_checktype(L, 1, LUA_TSTRING);
    const lua_Integer offset = luaL_checkinteger(L, 2);
    const lua_Integer len = luaL_checkinteger(L, 3);
    luaL_argcheck(L, offset >= 0, 2, "offset must not be negative");
    luaL_argcheck(L, len >= 0, 3, "len must not be negative");
    const bool waits = detail::take_callback(L, 4);
    SceneApi& api = checked_api(L);
    const uint32_t id = detail::register_files_callback(L, api, 4);
    // The library gives at most kMaxReadBytes whatever is asked.
    const size_t capped = static_cast<size_t>(std::min<lua_Integer>(len, media::kMaxReadBytes));
    do_read(L, api, id, static_cast<uint64_t>(offset), capped);
    return waits ? detail::wait_files(L, id) : 0;
}

void do_write(lua_State* L, SceneApi& api, uint32_t id) {
    if (!allowed(api, id, MediaAccess::Write)) return;
    const std::string_view data = view_at(L, 2);
    os::LargeBuffer buf = os::LargeBuffer::allocate(data.size());
    if (!buf.ok()) return reply_error(api, id, "io_error");
    if (!data.empty()) std::memcpy(buf.data(), data.data(), data.size());
    api.media_library->write(std::string(view_at(L, 1)), std::move(buf), replier<void>(api, id));
}

int write(lua_State* L) {
    luaL_checktype(L, 1, LUA_TSTRING);
    luaL_checktype(L, 2, LUA_TSTRING);
    const bool waits = detail::take_callback(L, 3);
    SceneApi& api = checked_api(L);
    const uint32_t id = detail::register_files_callback(L, api, 3);
    do_write(L, api, id);
    return waits ? detail::wait_files(L, id) : 0;
}

void do_remove_like(lua_State* L, SceneApi& api, uint32_t id, bool mkdir) {
    if (!allowed(api, id, MediaAccess::Write)) return;
    std::string path(view_at(L, 1));
    if (mkdir) {
        api.media_library->mkdir(std::move(path), replier<void>(api, id));
    } else {
        api.media_library->remove(std::move(path), replier<void>(api, id));
    }
}

int remove_like(lua_State* L, bool mkdir) {
    luaL_checktype(L, 1, LUA_TSTRING);
    const bool waits = detail::take_callback(L, 2);
    SceneApi& api = checked_api(L);
    const uint32_t id = detail::register_files_callback(L, api, 2);
    do_remove_like(L, api, id, mkdir);
    return waits ? detail::wait_files(L, id) : 0;
}

int remove(lua_State* L) { return remove_like(L, false); }
int mkdir(lua_State* L) { return remove_like(L, true); }

// ---- results --------------------------------------------------------------

void set_int(lua_State* L, int t, const char* key, int64_t v) {
    lua_pushinteger(L, static_cast<lua_Integer>(v));
    lua_setfield(L, t, key);
}

void set_str(lua_State* L, int t, const char* key, std::string_view v) {
    lua_pushlstring(L, v.data(), v.size());
    lua_setfield(L, t, key);
}

void push_entry(lua_State* L, const media::MediaEntry& e) {
    lua_createtable(L, 0, 4);
    const int t = lua_gettop(L);
    set_str(L, t, "name", e.name);
    lua_pushboolean(L, e.dir ? 1 : 0);
    lua_setfield(L, t, "dir");
    set_int(L, t, "size", static_cast<int64_t>(e.size));
    set_int(L, t, "mtime", e.mtime);
}

void push_track(lua_State* L, const media::Track& tr) {
    lua_createtable(L, 0, 7);
    const int t = lua_gettop(L);
    set_str(L, t, "id", tr.id);
    set_str(L, t, "title", tr.title);
    set_str(L, t, "artist", tr.artist);
    set_str(L, t, "album", tr.album);
    set_int(L, t, "duration", tr.duration_s);
    set_int(L, t, "size", static_cast<int64_t>(tr.size));
    set_int(L, t, "mtime", tr.mtime);
}

}  // namespace

namespace detail {

void push_media_state(lua_State* L, const media::PlayerState& s) {
    luaL_checkstack(L, 4, "wisp.media");
    lua_createtable(L, 0, 11);
    const int t = lua_gettop(L);
    set_str(L, t, "status", media::to_string(s.status));
    set_str(L, t, "pause_reason", media::to_string(s.pause_reason));
    set_str(L, t, "id", s.track.id);
    set_str(L, t, "title", s.track.title);
    set_str(L, t, "artist", s.track.artist);
    set_str(L, t, "album", s.track.album);
    set_int(L, t, "duration", s.track.duration_s);
    set_int(L, t, "position", s.position_s);
    // 1-based like every Lua list; 0 with nothing queued.
    set_int(L, t, "index", s.queue_length > 0 ? static_cast<int64_t>(s.index) + 1 : 0);
    set_int(L, t, "length", static_cast<int64_t>(s.queue_length));
    set_str(L, t, "mode", media::to_string(s.mode));
}

int push_media_payload(lua_State* L, const FilesPayload& p) {
    if (const auto* listed = std::get_if<std::vector<media::MediaEntry>>(&p)) {
        lua_createtable(L, static_cast<int>(listed->size()), 0);
        const int list = lua_gettop(L);
        lua_Integer i = 0;
        for (const media::MediaEntry& e : *listed) {
            push_entry(L, e);
            lua_rawseti(L, list, ++i);
        }
        return 1;
    }
    if (const auto* e = std::get_if<media::MediaEntry>(&p)) {
        push_entry(L, *e);
        return 1;
    }
    if (const auto* page = std::get_if<media::TrackPage>(&p)) {
        lua_createtable(L, 0, 2);
        const int t = lua_gettop(L);
        lua_createtable(L, static_cast<int>(page->items.size()), 0);
        const int items = lua_gettop(L);
        lua_Integer i = 0;
        for (const media::Track& tr : page->items) {
            push_track(L, tr);
            lua_rawseti(L, items, ++i);
        }
        lua_setfield(L, t, "items");
        set_int(L, t, "total", page->total);
        return 1;
    }
    if (const auto* played = std::get_if<media::PlayQueue::SetResult>(&p)) {
        lua_createtable(L, 0, 1);
        lua_pushboolean(L, played->truncated ? 1 : 0);
        lua_setfield(L, -2, "truncated");
        return 1;
    }
    return 0;
}

void install_media(lua_State* L, int wisp) {
    wisp = lua_absindex(L, wisp);
    static constexpr luaL_Reg kFns[] = {
        {"play", play},     {"pause", pause},   {"resume", resume},     {"next", next},
        {"prev", prev},     {"seek", seek},     {"set_mode", set_mode}, {"stop", stop},
        {"state", state},   {"tracks", tracks}, {"list", list},         {"stat", stat},
        {"read", read},     {"write", write},   {"remove", remove},     {"mkdir", mkdir},
        {nullptr, nullptr},
    };
    lua_createtable(L, 0, 16);
    luaL_setfuncs(L, kFns, 0);
    check_c_functions(L, -1);
    lua_setfield(L, wisp, "media");
}

}  // namespace detail

}  // namespace wisp::scene
