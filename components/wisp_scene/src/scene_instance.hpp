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

#include "scene_api.hpp"
#include "wisp/core/scene_repository.hpp"
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
    std::optional<ScriptError> start();
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
    // on_long_press, on_change(value)) runs if it has one. One budget for
    // both.
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

    // What its VM holds now (0 before start()).
    size_t vm_bytes() const { return vm_ ? vm_->used_bytes() : 0; }

    PatchBatch& batch() { return batch_; }
    // The bindings' state; the runtime fills in its callbacks before
    // start() and drives its timers.
    SceneApi& api() { return api_; }

private:
    std::optional<ScriptError> flush(std::optional<ScriptError> err);

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
