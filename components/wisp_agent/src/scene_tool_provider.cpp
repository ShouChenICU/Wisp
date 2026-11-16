// SceneToolProvider: installed scene tools behind ToolProvider. The
// results keep the exact bytes, messages and order agent-tools spec 5.3
// gave scene tools before providers existed (llm-tool-providers spec 6).

#include "wisp/agent/scene_tool_provider.hpp"

#include <string>
#include <utility>

#include "tool_io.hpp"
#include "tools_json.hpp"
#include "wisp/base/json.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::agent {

namespace {

using detail::fail;

std::string_view view(const os::LargeString& s) { return {s.data(), s.size()}; }

os::LargeString scene_data_large(std::string_view scene_id, std::string_view content) {
    os::LargeString source = "scene:";
    source.append(scene_id.data(), scene_id.size());
    return detail::external_data_large(view(source), content);
}

ui::Lang lang_of(const ToolCtx& ctx) { return ui::parse_lang(ctx.lang).value_or(ui::Lang::Zh); }

// A scene's tools answer while it runs in any form, foreground or not.
bool scene_running(const scene::SceneRuntime& runtime, const os::LargeString& scene_id) {
    const core::SceneState state =
        runtime.status(std::string(scene_id.data(), scene_id.size())).state;
    return state == core::SceneState::Running || state == core::SceneState::Background ||
           state == core::SceneState::Headless;
}

// Moves the strings out of `tool`, so the argument must be a copy: a
// 200-entry listing then copies no text on its way to tools.find.
ToolInfo to_info(IndexedTool& tool, bool details, const scene::SceneRuntime& runtime) {
    ToolInfo info;
    info.description = scene_data_large(view(tool.scene_id), view(tool.description));
    info.name = std::move(tool.full_name);
    info.origin = std::move(tool.scene_name);
    if (details) {
        info.parameters = std::move(tool.parameters);
        info.available = scene_running(runtime, tool.scene_id);
    }
    return info;
}

}  // namespace

SceneToolProvider::SceneToolProvider(ToolIndex& index, scene::SceneRuntime& runtime)
    : index_(index), runtime_(runtime) {}

std::string_view SceneToolProvider::id() const { return "scene"; }

ToolFindFields SceneToolProvider::find_fields() const {
    return {.origin = "scene_name", .available = "running"};
}

bool SceneToolProvider::owns(std::string_view full_name) const {
    return full_name.find(':') == std::string_view::npos;
}

void SceneToolProvider::search(std::string_view query, std::string_view scope, size_t max,
                               bool details, ToolCtx&, ToolSearch& out) {
    os::LargeVector<IndexedTool> matches = index_.search(query, scope, max);
    out.tools.reserve(out.tools.size() + matches.size());
    for (IndexedTool& tool : matches) out.tools.push_back(to_info(tool, details, runtime_));
}

size_t SceneToolProvider::count() const { return index_.size(); }

std::optional<ToolInfo> SceneToolProvider::describe(std::string_view full_name, ToolCtx&) {
    std::optional<IndexedTool> tool = index_.find(full_name);
    if (!tool) return std::nullopt;
    return to_info(*tool, true, runtime_);
}

ToolOutput SceneToolProvider::call(std::string_view full_name, const Value& args, ToolCtx& ctx) {
    const std::optional<IndexedTool> tool = index_.find(full_name);
    if (!tool) return tool_error("not_found", "no scene tool named " + std::string(full_name));

    if (args.type() != Value::Type::Map) {
        return tool_error("bad_args", "arguments must be a JSON object", Map{{"field", ""}});
    }
    auto schema = from_json(view(tool->parameters));
    if (!schema) return tool_error("internal", "the installed tool schema is invalid");
    std::string why;
    if (const std::optional<std::string> field = validate_args(schema.value(), args, &why)) {
        return tool_error("bad_args", why, Map{{"field", *field}});
    }

    const std::string scene_id(tool->scene_id.data(), tool->scene_id.size());
    const std::string tool_name(full_name.substr(full_name.find('.') + 1));
    auto result =
        runtime_.call_tool(scene_id, tool_name, to_json(args), tool->timeout_ms, ctx.cancelled);
    if (!result) return fail(result.error());
    if (!result.value().ok) {
        return tool_error(result.value().error, scene_data(scene_id, result.value().message));
    }
    ToolOutput out;
    out.result_json = R"({"ok":true,"result":)";
    const os::LargeString wrapped =
        scene_data_large(view(tool->scene_id), view(result.value().json));
    append_json_string(out.result_json, view(wrapped));
    out.result_json.push_back('}');
    out.note = ui::tr_fill_n(ui::StrId::AgentNoteToolCall, lang_of(ctx),
                             {view(tool->scene_name), tool_name});
    return out;
}

}  // namespace wisp::agent
