#pragma once

// Installing a scene package over the running system (scene-runtime-core
// spec 6.3): the one place that combines SceneRepository::install() with
// SceneHost, so every front end (REST, tools) installs the same way.

#include <cstdint>
#include <span>

#include "wisp/base/result.hpp"
#include "wisp/core/scene_host.hpp"
#include "wisp/core/scene_repository.hpp"

namespace wisp::core {

// Compiles the package's sources through host.check_sources(), then
// replaces the package keeping its data. Before the replace the scene is
// closed whatever its state (host.close() returns once its last store
// flush is on disk); one that was running is opened again after it
// (whether or not the replace succeeded; a failed reopen is logged under
// "scene.<id>" and leaves the new package installed), otherwise
// host.on_installed() is told. A failed close (Timeout, Busy) stops the
// install before anything is replaced, with that code and a message
// saying so (after a Timeout the close is still queued and the scene
// closes later, on its old package). While this runs, SceneRepository::
// installing() is true for the id, and the host refuses to open it.
// Blocks for as long as the check and the host's calls take, so it must not
// run on the UI thread, the scene task or the storage worker.
Result<SceneInfo> install_scene(SceneRepository& repo, SceneHost& host,
                                std::span<const uint8_t> zip);

}  // namespace wisp::core
