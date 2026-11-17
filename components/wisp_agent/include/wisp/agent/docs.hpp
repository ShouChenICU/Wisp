#pragma once

// docs.read (agent-core spec 4.4): the scene API by topic, so the system
// prompt need not carry it. "widget/<name>" comes from the widget schema
// (wisp/scene/schema.hpp). "widgets", "lua", "lua/<module>" and "screen"
// are cut from docs/scene-api.md, a copy of the schema's schema_markdown(),
// and the examples come from scenes/examples; both are compiled in
// compressed by tools/gen_agent_docs.py.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wisp::agent {

// widgets, widget/<name>..., lua, lua/<module>..., screen, examples,
// example/<id>...
std::vector<std::string> doc_topics();

// nullopt for a topic not in doc_topics(), or when there is not the
// memory to inflate it.
std::optional<std::string> read_doc(std::string_view topic);

}  // namespace wisp::agent
