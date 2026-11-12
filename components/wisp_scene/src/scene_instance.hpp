#pragma once

// One running scene's Lua side, on the scene task (scene_runtime.hpp): its
// VM with the prelude, the ui table and the `scene` table of lifecycle
// hooks, and the PatchBatch its messages fill. Every method runs scene
// code under Vm::call() and flushes the reactive state after it, so the
// batch holds the whole message's patches; on an error the caller faults
// the scene (the batch may hold a partial message and must be cleared).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "scene_api.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/os/large.hpp"
#include "wisp/scene/patch_batch.hpp"
#include "wisp/scene/scene_view.hpp"
#include "wisp/scene/ui_ops.hpp"
#include "wisp/scene/vm.hpp"

namespace wisp::scene {

class SceneInstance {
public:
    SceneInstance(std::string id, std::shared_ptr<const core::ScenePackage> package,
                  VmLimits limits);
    ~SceneInstance();

    SceneInstance(const SceneInstance&) = delete;
    SceneInstance& operator=(const SceneInstance&) = delete;

    // Makes the VM, runs the entry file and puts the Build of the tree it
    // returns into the batch (then the first flush). The root stays in the
    // registry for rebuild().
    //
    // With `migrate_from` (the data version, older than the manifest's;
    // scene-device spec 7.3), scene.migrate(migrate_from) runs between
    // the entry file and the tree, for up to kMigrateMs. The entry file
    // and the migration run inside one store batch: their writes and the
    // manifest's version are committed together
    // (SceneStore::commit_migration()), or on an error or timeout dropped;
    // a migration's error message then starts with "migrate failed: ". A
    // scene without scene.migrate only has its version raised. Building
    // the tree gets a fresh time budget after a migration.
    std::optional<ScriptError> start(std::optional<int> migrate_from = std::nullopt);
    // A Build of the root start() kept, collected again with every bound
    // value as it is now, into the batch (scene-device spec 3.2: a scene
    // coming back from the background gets a new view). Node ids are new;
    // ui.ref handles resolve to them as before. Fresh time budget.
    std::optional<ScriptError> rebuild();
    // scene.<name>(arg) if the scene set a function there (on_show,
    // on_hide, on_close without `arg`; on_state with it), with a fresh
    // time budget.
    std::optional<ScriptError> hook(const char* name, const char* arg = nullptr);
    // A widget event: a Change first updates what ui.ref():value()
    // reports (__wisp.set_value), then the node's handler (on_click,
    // on_long_press, on_change(value), on_swipe(dir)) runs if it has one.
    // One budget for both.
    std::optional<ScriptError> event(NodeId node, UiEvent ev, const PropValue& value);
    // SceneView::on_list_range: builds the list's rows (build_list_rows()).
    std::optional<ScriptError> list_range(NodeId list, uint32_t first, uint32_t count);
    // __wisp.refresh_all(): every bound value again, into the batch.
    std::optional<ScriptError> refresh();
    // A due wisp.timer callback (run_timer()).
    std::optional<ScriptError> timer(uint32_t id, bool once);
    // A wisp.http result for callback `id` (run_http_callback()).
    std::optional<ScriptError> http(uint32_t id, const net::SceneHttpResult& result);
    // A wisp.files result for callback `id` (run_files_callback()).
    std::optional<ScriptError> files(uint32_t id, const FilesResult& result);
    // A wisp.sensor batch or gesture for handle `id` (run_sensor_callback()).
    std::optional<ScriptError> sensor(uint32_t id, const core::SensorBatch& batch);
    // The wisp.device.on(ev) handlers, if the scene added any.
    std::optional<ScriptError> device(DeviceEvent ev, const DeviceInfo& info);

    // The agent's scene.eval (agent-core spec 4.2): compiles `lua` as the
    // chunk "eval" and runs it with a fresh budget of `budget_ms`, then
    // flushes the reactive state as after any message. Its return values
    // go into `out` as JSON (none: null, one: that value, several: an
    // array), at most kEvalResultMaxBytes. A Lua error, a timeout or a
    // result JSON cannot hold is returned and leaves the VM usable
    // (Vm::recover_timeout()); *fatal says the VM cannot go on (a memory
    // fault, or the flush failing) and the caller must fault the scene.
    std::optional<ScriptError> eval(std::string_view lua, uint32_t budget_ms, os::LargeString& out,
                                    bool* fatal);

    // A scene tool call (agent-tools spec 5.2): the function the scene
    // bound to `name` with wisp.tools.register starts as a task coroutine
    // (lua_task.hpp) with `args` as its argument, its slices the VM's
    // exec_ms; the first slice runs now, then the reactive flush. The
    // task's end goes to SceneApi::task_done. *registered is false (and
    // nothing ran) when no function is bound to `name`; *task is the
    // task's id once it started. An error returned is the scene's (a
    // fault, or the flush), as for any message.
    std::optional<ScriptError> tool(std::string_view name, const Value& args, bool* registered,
                                    uint32_t* task);
    // cancel_task() and cancel_tasks() (lua_task.hpp) on the VM; nothing
    // before start().
    void cancel_task(uint32_t id);
    void cancel_tasks();

    // Vm::raise_memory_limit(); nothing before start().
    void raise_memory_limit(size_t bytes) {
        if (vm_) vm_->raise_memory_limit(bytes);
    }

    // What its VM holds now (0 before start()).
    size_t vm_bytes() const { return vm_ ? vm_->used_bytes() : 0; }

    PatchBatch& batch() { return batch_; }
    // The bindings' state; the runtime fills in its callbacks before
    // start() and drives its timers.
    SceneApi& api() { return api_; }

private:
    std::optional<ScriptError> flush(std::optional<ScriptError> err);
    // scene.migrate(from), then commits or aborts the store batch start()
    // opened (see start()).
    std::optional<ScriptError> migrate(int from);

    std::string id_;
    std::shared_ptr<const core::ScenePackage> package_;
    VmLimits limits_;
    // Declared before vm_: install_prelude() and install_api() need them
    // to outlive the VM.
    PatchBatch batch_;
    SceneApi api_;
    std::unique_ptr<Vm> vm_;
    NodeId next_id_ = 1;
};

}  // namespace wisp::scene
