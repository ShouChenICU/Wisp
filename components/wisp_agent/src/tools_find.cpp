// tools.find and tools.call over the registry's ToolProviders
// (llm-tool-providers spec 4.1): find merges every provider's matches under
// one global limit, call routes a name to the provider that owns it. The
// tools' own names, argument schemas and result shapes are those of
// agent-tools spec 5.3 (llm-tool-providers spec 6).

#include <string>
#include <string_view>

#include "tools_json.hpp"
#include "wisp/agent/tool_index.hpp"
#include "wisp/agent/tool_provider.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/json.hpp"

namespace wisp::agent {

namespace {

constexpr const char* kFindSchema =
    R"({"type":"object","properties":{"query":{"type":"string"},"scene":{"type":"string"}}})";
constexpr const char* kCallSchema =
    R"({"type":"object","properties":{"name":{"type":"string"},"args":{"type":"object"}},"required":["name","args"]})";
constexpr uint32_t kToolCallRegistryTimeoutMs = 125000;

std::string_view view(const os::LargeString& s) { return {s.data(), s.size()}; }

std::string_view arg_string(const Value& args, const char* key) {
    const Value* value = args.find(key);
    return value != nullptr && value->type() == Value::Type::String
               ? std::string_view(value->as_string())
               : std::string_view();
}

void append_entry(os::LargeString& out, const ToolInfo& tool, bool details,
                  const ToolFindFields& fields) {
    out.append("{\"name\":");
    append_json_string(out, view(tool.name));
    out.append(",\"description\":");
    append_json_string(out, view(tool.description));
    if (details) {
        // Schemas were validated when the provider took them in; streaming
        // the text avoids a default-heap Value tree per entry.
        out.append(",\"parameters\":");
        out.append(tool.parameters.data(), tool.parameters.size());
        out.push_back(',');
        append_json_string(out, fields.origin);
        out.push_back(':');
        append_json_string(out, view(tool.origin));
        out.push_back(',');
        append_json_string(out, fields.available);
        out.append(tool.available ? ":true" : ":false");
    }
    out.push_back('}');
}

ToolOutput find(const ToolRegistry& reg, const Value& args, ToolCtx& ctx) {
    const bool details = args.find("query") != nullptr || args.find("scene") != nullptr;
    const std::string_view query = arg_string(args, "query");
    const std::string_view scope = arg_string(args, "scene");
    const size_t max = details ? kFindMax : kListMax;

    ToolOutput out;
    out.result_json = R"({"ok":true,"tools":[)";
    size_t got = 0;
    ToolSearch found;
    for (ToolProvider* provider : reg.providers()) {
        if (got >= max) break;
        found.tools.clear();
        provider->search(query, scope, max - got, details, ctx, found);
        const ToolFindFields fields = provider->find_fields();
        // A provider that overfills is cut to the room it was given, so the
        // merged list never exceeds the limit the model was promised.
        for (size_t i = 0; i < found.tools.size() && got < max; ++i, ++got) {
            if (got != 0) out.result_json.push_back(',');
            append_entry(out.result_json, found.tools[i], details, fields);
        }
    }
    out.result_json.push_back(']');
    if (!details) {
        size_t total = 0;
        for (const ToolProvider* provider : reg.providers()) total += provider->count();
        out.result_json.append(",\"total\":");
        detail::append_number(out.result_json, total);
    }
    // Only present when a source failed, so results from healthy sources
    // keep their shape.
    if (!found.unavailable.empty()) {
        out.result_json.append(",\"unavailable\":[");
        for (size_t i = 0; i < found.unavailable.size(); ++i) {
            if (i != 0) out.result_json.push_back(',');
            out.result_json.append("{\"server\":");
            append_json_string(out.result_json, view(found.unavailable[i].source));
            out.result_json.append(",\"error\":");
            append_json_string(out.result_json, view(found.unavailable[i].error));
            out.result_json.push_back('}');
        }
        out.result_json.push_back(']');
    }
    out.result_json.push_back('}');
    return out;
}

ToolOutput call(const ToolRegistry& reg, const Value& args, ToolCtx& ctx) {
    const std::string_view name = arg_string(args, "name");
    for (ToolProvider* provider : reg.providers()) {
        if (!provider->owns(name)) continue;
        static const Value null;
        const Value* parameters = args.find("args");
        return provider->call(name, parameters != nullptr ? *parameters : null, ctx);
    }
    return tool_error("not_found", "no tool named " + std::string(name));
}

}  // namespace

Result<void> add_find_tools(ToolRegistry& reg) {
    WISP_TRY(reg.add({
        .name = "tools.find",
        .description = "Find or list tools exported by installed scenes and connected MCP "
                       "servers; `scene` narrows the search to one scene id or MCP server id.",
        .schema_json = kFindSchema,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Agent,
        .run = [&reg](const Value& args, ToolCtx& ctx) { return find(reg, args, ctx); },
    }));
    WISP_TRY(reg.add({
        .name = "tools.call",
        .description = "Call a tool exported by an installed scene or a connected MCP server.",
        .schema_json = kCallSchema,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Agent,
        .timeout_ms = kToolCallRegistryTimeoutMs,
        .run = [&reg](const Value& args, ToolCtx& ctx) { return call(reg, args, ctx); },
    }));
    return Result<void>::ok();
}

}  // namespace wisp::agent
