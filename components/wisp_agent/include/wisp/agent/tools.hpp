#pragma once

// The agent's tool sets (agent-core spec 4), each a thin adapter over a
// service: every one registers its tools with the registry and returns the
// first error add() reports.

#include <functional>
#include <string>

#include "wisp/agent/scene_workspace.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/memory_store.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/reminders.hpp"
#include "wisp/core/scene_files.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/net/scene_http.hpp"
#include "wisp/scene/scene_runtime.hpp"
#include "wisp/scene/ui_inspector.hpp"

namespace wisp::agent {

// scene.list, list_files, read_file, write_file, edit_file, delete_file,
// install, discard_draft and rollback over `ws` (spec 4.1).
Result<void> add_scene_authoring_tools(ToolRegistry& reg, SceneWorkspace& ws);

// ui.tree (spec 5.2).
Result<void> add_ui_tools(ToolRegistry& reg, scene::UiInspector& inspector);

struct SceneOpsDeps {
    scene::SceneRuntime& runtime;
    core::SceneRepository& repo;
    // Null: the files tools answer "unavailable".
    core::SceneFiles* files = nullptr;
    core::StorageManager& storage;
    // Runs a job on the storage worker (StorageWorker::submit); false if
    // it could not be queued.
    std::function<bool(std::function<void()>)> submit_io;
    // Deletes a scene without asking (App's SceneWiring::delete_scene):
    // closes it if it is up, removes it on the worker, publishes
    // scene.state "deleted", then calls `done` once, on any thread.
    std::function<void(const std::string& id, bool with_data,
                       std::function<void(Result<void>)> done)>
        delete_scene;
    // Empty: every delete is refused.
    ConfirmFn confirm;
};

// scene.open, close, exit, delete, data_read, data_write, files_list,
// files_read, files_remove, logs and eval (spec 4.2).
Result<void> add_scene_ops_tools(ToolRegistry& reg, SceneOpsDeps deps);

struct DeviceToolDeps {
    // device.status's fields: battery, charging, wifi, storage, power,
    // occupancy, shape, safe_area (a missing one is null). It must not
    // touch flash: it runs on the agent's thread.
    std::function<Value()> status;
    // Read and written only inside submit_io jobs (NVS is flash).
    core::SystemSettings& settings;
    core::NotificationService& notes;
    std::function<bool(std::function<void()>)> submit_io;
};

// device.status, settings.get, settings.set, notify.list and
// notify.dismiss (spec 4.4).
Result<void> add_device_tools(ToolRegistry& reg, DeviceToolDeps deps);

// docs.read (spec 4.4; wisp/agent/docs.hpp).
Result<void> add_docs_tools(ToolRegistry& reg);

struct MemoryToolDeps {
    core::MemoryStore& store;
};

// memory.list, add, update and remove (mvp spec 3.2, 5.1): thin adapters
// over MemoryStore, run on the flash thread (ToolThread::Fs), since every
// one of them reads or writes notes.json.
Result<void> add_memory_tools(ToolRegistry& reg, MemoryToolDeps deps);

struct HttpToolDeps {
    net::SceneHttp& http;
};

// http.fetch (agent-tools spec 3): a thin adapter over SceneHttp's Agent
// principal, on ToolThread::Agent (spec 3.4). Holds the single most
// recent response in a process-wide cache (spec 3.3), keyed by `ref`; its
// own free functions, not a handle on HttpToolDeps, since the cache
// outlives any one HttpToolDeps/ToolRegistry built over it (the only
// alternative would be a second place -- AgentWiring -- that has to agree
// with this one on a lifetime, for no benefit: there is ever one agent
// core per process).
Result<void> add_http_tools(ToolRegistry& reg, HttpToolDeps deps);

// Drops the http.fetch response cache, invalidating any `ref` (spec 3.3):
// AgentService calls this at the end of every turn and from
// new_conversation(), so a `ref` never reaches past the turn, or the
// conversation, that made it.
void http_tools_drop_cache();

struct LuaToolDeps {
    scene::SceneRuntime& runtime;
};

// lua.run (agent-tools spec 4): runs a script on a VM of its own on the
// scene task (SceneRuntime::run_lua()), waiting on ToolThread::Agent.
Result<void> add_lua_tools(ToolRegistry& reg, LuaToolDeps deps);

struct TodoToolDeps {
    core::ReminderScheduler& scheduler;
};

// todo.list, add, update, done and remove (mvp spec 3.2, 5.2): thin
// adapters over ReminderScheduler, run on the flash thread
// (ToolThread::Fs), since the changes write todos.json.
Result<void> add_todo_tools(ToolRegistry& reg, TodoToolDeps deps);

}  // namespace wisp::agent
