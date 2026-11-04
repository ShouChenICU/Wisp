#include "wisp/core/scene_host.hpp"

namespace wisp::core {

const char* to_string(SceneState s) {
    switch (s) {
        case SceneState::Stopped:
            return "stopped";
        case SceneState::Running:
            return "running";
        case SceneState::Error:
            return "error";
    }
    return "stopped";
}

}  // namespace wisp::core
