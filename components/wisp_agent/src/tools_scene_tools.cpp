// tools.find and tools.call: query installed scene declarations and dispatch
// validated arguments to SceneRuntime without adding scene tools to tools_json().

#include "wisp/agent/tools.hpp"

#include <string>
#include <string_view>

#include "tool_io.hpp"
#include "wisp/base/json.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::agent {

namespace {

using detail::done;
using detail::fail;

constexpr const char* kFindSchema =
    R"({"type":"object","properties":{"query":{"type":"string"},"scene":{"type":"string"}}})";
constexpr const char* kCallSchema =
    R"({"type":"object","properties":{"name":{"type":"string"},"args":{"type":"object"}},"required":["name","args"]})";
constexpr uint32_t kToolCallRegistryTimeoutMs = 125000;

const std::string& arg_string(const Value& args, const char* key) {
    static const std::string empty;
    const Value* value = args.find(key);
    return value != nullptr && value->type() == Value::Type::String ? value->as_string() : empty;
}

Value find_entry(const IndexedTool& tool, bool include_details,
                 const scene::SceneRuntime& runtime) {
    const std::string_view description(tool.description.data(), tool.description.size());
    Map entry{{"name", Value(std::string(tool.full_name.data(), tool.full_name.size()))},
              {"description",
               Value(scene_data(std::string_view(tool.scene_id.data(), tool.scene_id.size()),
                                description))}};
    if (include_details) {
        auto parameters =
            from_json(std::string_view(tool.parameters.data(), tool.parameters.size()));
        if (parameters) entry["parameters"] = std::move(parameters.value());
        entry["scene_name"] = std::string(tool.scene_name.data(), tool.scene_name.size());
        const core::SceneState state =
            runtime.status(std::string(tool.scene_id.data(), tool.scene_id.size())).state;
        entry["running"] = state == core::SceneState::Running ||
                           state == core::SceneState::Background ||
                           state == core::SceneState::Headless;
    }
    return Value(std::move(entry));
}

ui::Lang lang_of(const ToolCtx& ctx) { return ui::parse_lang(ctx.lang).value_or(ui::Lang::Zh); }

}  // namespace

Result<void> add_scene_tool_tools(ToolRegistry& reg, SceneToolDeps deps) {
    WISP_TRY(reg.add({
        .name = "tools.find",
        .description = "Find or list tools exported by installed scenes.",
        .schema_json = kFindSchema,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Agent,
        .run =
            [&index = deps.index, &runtime = deps.runtime](const Value& args, ToolCtx&) {
                const bool include_details =
                    args.find("query") != nullptr || args.find("scene") != nullptr;
                const std::string& query = arg_string(args, "query");
                const std::string& scene_id = arg_string(args, "scene");
                const size_t limit = include_details ? kFindMax : kListMax;
                const os::LargeVector<IndexedTool> matches = index.search(query, scene_id, limit);
                List results;
                results.reserve(matches.size());
                for (const IndexedTool& tool : matches) {
                    results.push_back(find_entry(tool, include_details, runtime));
                }
                Map fields{{"tools", Value(std::move(results))}};
                if (!include_details) fields["total"] = Value(static_cast<int64_t>(index.size()));
                return done(std::move(fields));
            },
    }));

    WISP_TRY(reg.add({
        .name = "tools.call",
        .description = "Call a tool exported by an installed scene.",
        .schema_json = kCallSchema,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Agent,
        .timeout_ms = kToolCallRegistryTimeoutMs,
        .run =
            [&index = deps.index, &runtime = deps.runtime](const Value& args, ToolCtx& ctx) {
                const std::string& name = arg_string(args, "name");
                const std::optional<IndexedTool> tool = index.find(name);
                if (!tool) return tool_error("not_found", "no scene tool named " + name);

                const Value* parameters = args.find("args");
                if (parameters == nullptr || parameters->type() != Value::Type::Map) {
                    return tool_error("bad_args", "arguments must be a JSON object",
                                      Map{{"field", ""}});
                }
                const std::string_view schema_text(tool->parameters.data(),
                                                   tool->parameters.size());
                auto schema = from_json(schema_text);
                if (!schema) return tool_error("internal", "the installed tool schema is invalid");
                std::string why;
                if (const std::optional<std::string> field =
                        validate_args(schema.value(), *parameters, &why)) {
                    return tool_error("bad_args", why, Map{{"field", *field}});
                }

                const std::string scene_id(tool->scene_id.data(), tool->scene_id.size());
                const std::string tool_name = name.substr(name.find('.') + 1);
                auto result = runtime.call_tool(scene_id, tool_name, to_json(*parameters),
                                                tool->timeout_ms, ctx.cancelled);
                if (!result) return fail(result.error());
                if (!result.value().ok) {
                    return tool_error(result.value().error,
                                      scene_data(scene_id, result.value().message));
                }
                ToolOutput out =
                    done({{"result", Value(scene_data(scene_id, result.value().json))}});
                out.note = ui::tr_fill_n(
                    ui::StrId::AgentNoteToolCall, lang_of(ctx),
                    {std::string_view(tool->scene_name.data(), tool->scene_name.size()),
                     tool_name});
                return out;
            },
    }));
    return Result<void>::ok();
}

}  // namespace wisp::agent
