#include "scene_instance.hpp"

#include <cstring>
#include <string_view>
#include <utility>
#include <variant>

#include "wisp/scene/reactive.hpp"
#include "wisp/scene/schema.hpp"

namespace wisp::scene {

namespace {

// Registry key of the `scene` table the hooks are read from. Kept apart
// from the global so that rawset(_G, "scene", x) cannot redirect them.
char kSceneTableKey;

// Every function below runs inside Vm::call() and follows the longjmp
// rule (vm.hpp): their frames hold only trivially destructible locals.

int install_scene_table(lua_State* L) {
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kSceneTableKey);
    lua_setglobal(L, "scene");
    return 0;
}

// call_hook(name)
int call_hook(lua_State* L) {
    const char* name = static_cast<const char*>(lua_touserdata(L, 1));
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &kSceneTableKey) != LUA_TTABLE) return 0;
    lua_pushstring(L, name);
    if (lua_rawget(L, -2) != LUA_TFUNCTION) return 0;
    lua_call(L, 0, 0);
    return 0;
}

void push_value(lua_State* L, const PropValue& v) {
    if (const auto* b = std::get_if<bool>(&v)) {
        lua_pushboolean(L, *b ? 1 : 0);
    } else if (const auto* i = std::get_if<int64_t>(&v)) {
        lua_pushinteger(L, static_cast<lua_Integer>(*i));
    } else if (const auto* d = std::get_if<double>(&v)) {
        lua_pushnumber(L, static_cast<lua_Number>(*d));
    } else if (const auto* s = std::get_if<os::LargeString>(&v)) {
        lua_pushlstring(L, s->data(), s->size());
    } else {
        lua_pushnil(L);
    }
}

struct EventCtx {
    NodeId node;
    UiEvent ev;
    const PropValue* value;
};

// dispatch_event(ctx)
int dispatch_event(lua_State* L) {
    const auto* c = static_cast<const EventCtx*>(lua_touserdata(L, 1));
    Prop handler = Prop::OnClick;
    switch (c->ev) {
        case UiEvent::Click:
            handler = Prop::OnClick;
            break;
        case UiEvent::LongPress:
            handler = Prop::OnLongPress;
            break;
        case UiEvent::Change:
            handler = Prop::OnChange;
            break;
        case UiEvent::Focus:
            return 0;
    }
    luaL_checkstack(L, 4, "event");
    if (c->ev == UiEvent::Change) {
        if (!push_wisp_internal(L)) return luaL_error(L, "the reactive prelude is not installed");
        lua_getfield(L, -1, "set_value");
        lua_pushinteger(L, static_cast<lua_Integer>(c->node));
        push_value(L, *c->value);
        lua_call(L, 2, 0);
        lua_pop(L, 1);
    }
    if (!push_node_prop(L, c->node, handler) || lua_type(L, -1) != LUA_TFUNCTION) return 0;
    int nargs = 0;
    if (c->ev == UiEvent::Change) {
        push_value(L, *c->value);
        nargs = 1;
    }
    lua_call(L, nargs, 0);
    return 0;
}

int refresh_all(lua_State* L) {
    if (!push_wisp_internal(L)) return luaL_error(L, "the reactive prelude is not installed");
    lua_getfield(L, -1, "refresh_all");
    lua_call(L, 0, 0);
    return 0;
}

std::optional<std::string_view> file_of(const core::ScenePackage& pkg, std::string_view path) {
    for (const auto& [name, data] : pkg.files)
        if (name == path)
            return std::string_view(reinterpret_cast<const char*>(data.data()), data.size());
    return std::nullopt;
}

}  // namespace

SceneInstance::SceneInstance(std::string id, std::shared_ptr<const core::ScenePackage> package,
                             VmLimits limits)
    : id_(std::move(id)), package_(std::move(package)), limits_(limits) {}

SceneInstance::~SceneInstance() = default;

std::optional<ScriptError> SceneInstance::flush(std::optional<ScriptError> err) {
    if (err) return err;
    return flush_reactive(*vm_);
}

std::optional<ScriptError> SceneInstance::start() {
    vm_ = Vm::create(id_, limits_);
    if (!vm_) return ScriptError{ScriptFault::Memory, "", 0, "not enough memory for the scene"};
    // The package outlives the VM: this instance holds both.
    const core::ScenePackage* pkg = package_.get();
    vm_->set_sources([pkg](std::string_view path) { return file_of(*pkg, path); });
    if (auto err = install_prelude(*vm_, batch_)) return err;
    if (auto err = install_ui(*vm_)) return err;
    if (auto err = install_api(*vm_, api_)) return err;
    lua_State* L = vm_->L();
    if (!lua_checkstack(L, 2)) return ScriptError{ScriptFault::Memory, "", 0, "stack overflow"};
    lua_pushcfunction(L, install_scene_table);
    if (auto err = vm_->call(0, 0)) return err;

    const std::string& entry = pkg->manifest.entry;
    auto source = file_of(*pkg, entry);
    if (!source) return ScriptError{ScriptFault::Compile, entry, 0, "entry file missing"};
    if (auto err = vm_->load(entry, *source)) return err;
    if (auto err = vm_->call(0, 1)) return err;
    Build tree;
    auto err = collect_tree(*vm_, -1, next_id_, tree);
    lua_pop(L, 1);
    if (err) return err;
    if (!batch_.push(std::move(tree)).is_ok())
        return ScriptError{ScriptFault::Runtime, entry, 0, "ui update too large"};
    return flush_reactive(*vm_);
}

std::optional<ScriptError> SceneInstance::hook(const char* name) {
    lua_State* L = vm_->L();
    if (!lua_checkstack(L, 2)) return ScriptError{ScriptFault::Memory, "", 0, "stack overflow"};
    lua_pushcfunction(L, call_hook);
    lua_pushlightuserdata(L, const_cast<char*>(name));
    return flush(vm_->call(1, 0));
}

std::optional<ScriptError> SceneInstance::event(NodeId node, UiEvent ev, const PropValue& value) {
    lua_State* L = vm_->L();
    if (!lua_checkstack(L, 2)) return ScriptError{ScriptFault::Memory, "", 0, "stack overflow"};
    EventCtx ctx{node, ev, &value};
    lua_pushcfunction(L, dispatch_event);
    lua_pushlightuserdata(L, &ctx);
    return flush(vm_->call(1, 0));
}

std::optional<ScriptError> SceneInstance::list_range(NodeId list, uint32_t first, uint32_t count) {
    return flush(build_list_rows(*vm_, list, first, count, next_id_));
}

std::optional<ScriptError> SceneInstance::timer(uint32_t id, bool once) {
    return flush(run_timer(*vm_, id, once));
}

std::optional<ScriptError> SceneInstance::device(DeviceEvent ev, const DeviceInfo& info) {
    if (!api_.wants(ev)) return std::nullopt;
    return flush(run_device_handlers(*vm_, ev, info));
}

std::optional<ScriptError> SceneInstance::refresh() {
    lua_State* L = vm_->L();
    if (!lua_checkstack(L, 1)) return ScriptError{ScriptFault::Memory, "", 0, "stack overflow"};
    lua_pushcfunction(L, refresh_all);
    return vm_->call(0, 0);
}

}  // namespace wisp::scene
