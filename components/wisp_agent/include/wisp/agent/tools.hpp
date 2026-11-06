#pragma once

// The agent's tool sets (agent-core spec 4), each a thin adapter over a
// service: every one registers its tools with the registry and returns the
// first error add() reports.

#include "wisp/agent/scene_workspace.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/base/result.hpp"
#include "wisp/scene/ui_inspector.hpp"

namespace wisp::agent {

// scene.list, list_files, read_file, write_file, edit_file, delete_file,
// install, discard_draft and rollback over `ws` (spec 4.1).
Result<void> add_scene_authoring_tools(ToolRegistry& reg, SceneWorkspace& ws);

// ui.tree (spec 5.2).
Result<void> add_ui_tools(ToolRegistry& reg, scene::UiInspector& inspector);

}  // namespace wisp::agent
