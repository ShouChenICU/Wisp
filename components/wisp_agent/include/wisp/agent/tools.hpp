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
#include "wisp/core/scene_files.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/system_settings.hpp"
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

}  // namespace wisp::agent
