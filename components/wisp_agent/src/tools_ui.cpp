// ui.tree (agent-core spec 5.2): a thin adapter over scene::UiInspector.
// It runs on the agent's thread: the inspector posts its one job to the UI
// thread and waits for it there.

#include <string>
#include <string_view>

#include "wisp/agent/tools.hpp"

namespace wisp::agent {

Result<void> add_ui_tools(ToolRegistry& reg, scene::UiInspector& inspector) {
    return reg.add({
        .name = "ui.tree",
        .description = "The widgets on screen, one per line: type#ref \"text\" [x,y w×h] and "
                       "state. With `ref`, only that widget's subtree.",
        .schema_json = R"({"type":"object","properties":{"ref":{"type":"string","maxLength":64}}})",
        .thread = ToolThread::Agent,
        .run =
            [&inspector](const Value& a, ToolCtx&) {
                const Value* ref = a.find("ref");
                auto r = inspector.tree(ref != nullptr && ref->type() == Value::Type::String
                                            ? std::string_view(ref->as_string())
                                            : std::string_view());
                // The message starts with the code: "no_scene", "scene_error:
                // ..." (the scene's text already wrapped), "not_found: ...".
                if (!r) return tool_error(error_code(r.error()), r.error().message);
                ToolOutput out;
                out.result = Value(Map{{"ok", Value(true)}, {"tree", Value(r.value())}});
                return out;
            },
    });
}

}  // namespace wisp::agent
