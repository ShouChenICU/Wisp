#pragma once

// docs.read (agent-core spec 4.4): the scene API by topic, so the system
// prompt need not carry it. "widgets", "widget/<name>", "lua",
// "lua/<module>" and "screen" are built from the widget schema
// (wisp/scene/schema.hpp, whose schema_markdown() docs/scene-api.md is);
// the examples are compiled in from scenes/examples (tools/gen_agent_docs.py).

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wisp::agent {

// widgets, widget/<name>..., lua, lua/<module>..., screen, examples,
// example/<id>...
std::vector<std::string> doc_topics();

// nullopt for a topic not in doc_topics().
std::optional<std::string> read_doc(std::string_view topic);

}  // namespace wisp::agent
