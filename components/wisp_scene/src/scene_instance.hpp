#pragma once

// One running scene's Lua side, on the scene task (scene_runtime.hpp): its
// VM with the prelude, the ui table and the `scene` table of lifecycle
// hooks, and the PatchBatch its messages fill. Every method runs scene
// code under Vm::call() and flushes the reactive state after it, so the
// batch holds the whole message's patches; on an error the caller faults
// the scene (the batch may hold a partial message and must be cleared).

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

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
    // returns into the batch (then the first flush).
    std::optional<ScriptError> start();
    // scene.<name>() if the scene set a function there (on_show, on_hide,
    // on_close), with a fresh time budget.
    std::optional<ScriptError> hook(const char* name);
    // A widget event: a Change first updates what ui.ref():value()
    // reports (__wisp.set_value), then the node's handler (on_click,
    // on_long_press, on_change(value)) runs if it has one. One budget for
    // both.
    std::optional<ScriptError> event(NodeId node, UiEvent ev, const PropValue& value);
    // SceneView::on_list_range: builds the list's rows (build_list_rows()).
    std::optional<ScriptError> list_range(NodeId list, uint32_t first, uint32_t count);
    // __wisp.refresh_all(): every bound value again, into the batch.
    std::optional<ScriptError> refresh();

    PatchBatch& batch() { return batch_; }

private:
    std::optional<ScriptError> flush(std::optional<ScriptError> err);

    std::string id_;
    std::shared_ptr<const core::ScenePackage> package_;
    VmLimits limits_;
    // Declared before vm_: install_prelude() needs it to outlive the VM.
    PatchBatch batch_;
    std::unique_ptr<Vm> vm_;
    NodeId next_id_ = 1;
};

}  // namespace wisp::scene
