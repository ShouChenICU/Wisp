// wisp.notify and wisp.notify_cancel (scene_api.hpp; scene-device spec
// 5.1): thin adapters over core::NotificationService. The service only
// changes memory and arms its own save timer, so calling it on the scene
// task touches no flash.
//
// longjmp rule (vm.hpp): each binding checks its arguments with raising
// Lua calls while its frame holds only trivially destructible locals (the
// field strings stay on the Lua stack, so the pointers into them stay
// valid), then calls a do_* function that builds the C++ objects, calls
// the service and copies what it needs into an Outcome; results are
// pushed only after that function has returned.

#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>

#include "scene_api.hpp"

namespace wisp::scene {

namespace {

using core::NotificationService;

struct Text {
    const char* data = nullptr;
    size_t len = 0;
};

struct Fields {
    Text title, body, id;
    core::NotifySound sound = core::NotifySound::None;
    bool wake = false;
};
static_assert(std::is_trivially_destructible_v<Fields>);

struct Outcome {
    // A code for `nil, code`; static storage.
    const char* code = nullptr;
    // An error to raise (the service refused an argument).
    bool raise = false;
    char msg[128] = {};
    // The id posted; ids are at most NotificationService::kIdMax bytes
    // ("n<seq>" when generated).
    char id[NotificationService::kIdMax + 24] = {};
};
static_assert(std::is_trivially_destructible_v<Outcome>);

int fail(lua_State* L, const char* code) {
    lua_pushnil(L);
    lua_pushstring(L, code);
    return 2;
}

// Pushes t[name] and returns its type (the value stays on the stack).
int field(lua_State* L, const char* name) { return lua_getfield(L, 1, name); }

// t[name] as a string of min..max bytes (`optional`: nil is empty).
Text text_field(lua_State* L, const char* name, size_t min, size_t max, bool optional) {
    const int type = field(L, name);
    if (type == LUA_TNIL && optional) return {};
    if (type != LUA_TSTRING) {
        luaL_argerror(L, 1, lua_pushfstring(L, "%s must be a string", name));
        return {};
    }
    Text t;
    t.data = lua_tolstring(L, -1, &t.len);
    if (t.len < min || t.len > max)
        luaL_argerror(L, 1,
                      lua_pushfstring(L, "%s must be %d-%d bytes", name, static_cast<int>(min),
                                      static_cast<int>(max)));
    return t;
}

void do_post(SceneApi& api, const Fields& f, Outcome& o) {
    core::Notification n;
    n.source = api.scene_id;
    n.source_name = api.scene_name;
    n.id.assign(f.id.data != nullptr ? f.id.data : "", f.id.len);
    n.title.assign(f.title.data, f.title.len);
    n.body.assign(f.body.data != nullptr ? f.body.data : "", f.body.len);
    n.sound = f.sound;
    n.wake = f.wake;
    auto r = api.notifications->post(std::move(n), !api.background);
    if (r) {
        std::snprintf(o.id, sizeof(o.id), "%s", r.value().c_str());
        return;
    }
    if (r.error().code == ErrorCode::RateLimited) {
        o.code = "rate_limited";
    } else if (r.error().code == ErrorCode::InvalidArgument) {
        o.raise = true;
        std::snprintf(o.msg, sizeof(o.msg), "%s", r.error().message.c_str());
    } else {
        o.code = "unavailable";
    }
}

// notify{title, body, sound, wake, id} -> id | nil, code
int notify(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checkstack(L, 8, "notify");
    Fields f;
    f.title = text_field(L, "title", 1, NotificationService::kTitleMax, false);
    f.body = text_field(L, "body", 0, NotificationService::kBodyMax, true);
    Text id = text_field(L, "id", 1, NotificationService::kIdMax, true);
    if (id.data != nullptr && !core::valid_notification_id({id.data, id.len}))
        luaL_argerror(L, 1, "id must be 1-32 characters of a-z, 0-9, _ and -");
    f.id = id;
    const int sound = field(L, "sound");
    if (sound != LUA_TNIL) {
        const char* s = sound == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
        auto parsed = s != nullptr ? core::parse_notify_sound(s) : std::nullopt;
        if (!parsed) luaL_argerror(L, 1, "sound must be \"none\", \"tone\" or \"alarm\"");
        f.sound = *parsed;
    }
    const int wake = field(L, "wake");
    if (wake != LUA_TNIL && wake != LUA_TBOOLEAN) luaL_argerror(L, 1, "wake must be a boolean");
    f.wake = lua_toboolean(L, -1) != 0;

    SceneApi& api = detail::api_of(L);
    if (api.notifications == nullptr) return fail(L, "unavailable");
    Outcome o;
    do_post(api, f, o);
    if (o.raise) return luaL_error(L, "%s", o.msg);
    if (o.code != nullptr) return fail(L, o.code);
    lua_pushstring(L, o.id);
    return 1;
}

bool do_cancel(SceneApi& api, Text id) {
    return api.notifications->cancel(api.scene_id, std::string_view(id.data, id.len));
}

// notify_cancel(id) -> boolean
int notify_cancel(lua_State* L) {
    Text id;
    id.data = luaL_checklstring(L, 1, &id.len);
    SceneApi& api = detail::api_of(L);
    const bool removed = api.notifications != nullptr && do_cancel(api, id);
    lua_pushboolean(L, removed ? 1 : 0);
    return 1;
}

}  // namespace

namespace detail {

void install_notify(lua_State* L, int wisp) {
    wisp = lua_absindex(L, wisp);
    lua_pushcfunction(L, notify);
    push_checked_function(L, -1);
    lua_setfield(L, wisp, "notify");
    lua_pushcfunction(L, notify_cancel);
    push_checked_function(L, -1);
    lua_setfield(L, wisp, "notify_cancel");
    lua_pop(L, 2);
}

}  // namespace detail

}  // namespace wisp::scene
