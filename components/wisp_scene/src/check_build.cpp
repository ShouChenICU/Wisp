// SceneRuntime::check_build()'s work on the scene task (scene_runtime.hpp).

#include <optional>
#include <string>

#include "scene_api.hpp"
#include "scene_instance.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/scene_store.hpp"
#include "wisp/os/large.hpp"
#include "wisp/scene/scene_runtime.hpp"

namespace wisp::scene {

Result<void> SceneRuntime::run_check_build(const std::shared_ptr<const core::ScenePackage>& package,
                                           core::SceneStore& store) {
    const core::SceneManifest& m = package->manifest;
    // Not in pool_: a check must neither evict a scene in the background
    // nor fail because the running scenes fill the budget.
    VmLimits limits = config_.vm;
    limits.pool = nullptr;
    auto scene = os::make_large<SceneInstance>(m.id, package, limits);
    if (!scene) return Result<void>::err(ErrorCode::NoMemory, "no memory for the check");
    const Value args;
    SceneApi& api = scene->api();
    api.device = [this] { return device_info(); };
    api.screen = deps_.shape;
    api.args = &args;
    // Whatever the entry writes stays in the batch's overlay, dropped below.
    store.begin_batch();
    api.store = &store;
    api.scene_id = m.id;
    api.scene_name = m.name;
    api.http_submit = [](uint32_t, net::SceneHttpRequest) {};
    api.http_fail = [](uint32_t, const char*) {};
    api.files_run = [](uint32_t, FilesJob) {};
    api.files_fail = [](uint32_t, const char*) {};
    api.files_download = [](uint32_t, net::SceneHttpRequest, std::string) {};
    std::optional<ScriptError> err = scene->start();
    // The VM goes before the store it points at.
    scene.reset();
    store.abort_batch();
    store.close();
    if (err) return Result<void>::err(ErrorCode::InvalidArgument, err->text());
    return Result<void>::ok();
}

}  // namespace wisp::scene
