#pragma once

// SceneHost: opening, closing and reloading scenes, and their run state
// (scene-runtime-core spec 2.1, 3.4). The Lua runtime (wisp_scene's
// SceneRuntime) implements it; wisp_app injects it and wisp_admin only
// sees this interface.
//
// Every change of a scene's state is published on the event bus as
// "scene.state" with source "scene" and payload {"id": <scene id>,
// "state": "stopped" | "running" | "background" | "headless" | "error" |
// "deleted"}. "running" is the scene in the foreground; "background" one
// that keeps running without a view (scene-device spec 3.2); "headless"
// one started without a view only to run the agent's calls of its tools
// (agent-tools spec 5.4), which closes once they stop.

#include <span>
#include <string>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/scene_repository.hpp"

namespace wisp::core {

enum class SceneState { Stopped, Running, Background, Headless, Error };

// "stopped", "running", "background", "headless", "error".
const char* to_string(SceneState s);

struct SceneStatus {
    SceneState state = SceneState::Stopped;
    // The last error (ScriptError::text(), "main.lua:12: message") while
    // state is Error; empty otherwise.
    std::string error;
};

class SceneHost {
public:
    virtual ~SceneHost() = default;

    // Opens `id` in the foreground, closing whatever scene is there first.
    // NotFound for a scene that is not installed.
    virtual Result<void> open(const std::string& id, Value args = {}) = 0;
    // "Back" for `id` if it is in the foreground (also from its error
    // panel): a scene that declares "background" goes to the background,
    // any other closes. Ok and nothing changes for a scene that is not in
    // the foreground.
    virtual Result<void> close(const std::string& id) = 0;
    // Closes `id` for good from any state, foreground or background. Ok
    // when it is not running.
    virtual Result<void> exit(const std::string& id) = 0;
    // Closes and opens `id` again with the same arguments; its data stays.
    virtual Result<void> reload(const std::string& id) = 0;
    // Starts `id` in the background, without a view (scene-device spec
    // 3.2, 3.6): what a reload is for a scene that was there. Ok and
    // nothing changes if it is already running; InvalidArgument if its
    // scene.json does not declare "background".
    virtual Result<void> open_background(const std::string& id) = 0;
    // Stopped for a scene this host has not run.
    virtual SceneStatus status(const std::string& id) const = 0;
    // Compiles (never runs) every file the way a scene loads it, on the
    // scene task, waiting at most 10 s. The error reads like
    // "lib/util.lua:3: ...". A SceneRepository::install() checker.
    virtual Result<void> check_sources(std::span<const SourceFile> files) = 0;
    // A new package of `id` was installed: a running scene is reloaded,
    // otherwise its state becomes "stopped" (and is published). Never
    // waits.
    virtual void on_installed(const std::string& id) = 0;
};

}  // namespace wisp::core
