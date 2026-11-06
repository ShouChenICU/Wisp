#pragma once

// Text that came from a scene, wrapped for the agent (agent-core spec 6.3):
// <data source="scene:<id>">content</data>, with every "</data>" inside
// (any letter case) neutralised to "&lt;/data>", so a scene cannot close
// the wrapper and speak as the system. Here rather than in wisp_agent
// because the scene layer's own inspectors (ui_inspector.hpp) wrap the
// scene's part of what they report.

#include <string>
#include <string_view>

namespace wisp::scene {

std::string scene_data(std::string_view scene_id, std::string_view content);

}  // namespace wisp::scene
