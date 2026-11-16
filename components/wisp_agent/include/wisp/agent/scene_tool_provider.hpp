#pragma once

// SceneToolProvider (llm-tool-providers spec 4.1): the tools installed
// scenes export (agent-tools spec 5.3), reached through tools.find and
// tools.call. Wraps the ToolIndex built from the scene manifests and calls
// into SceneRuntime; the results are the ones the model has always seen
// for scene tools (llm-tool-providers spec 6).

#include <cstddef>
#include <optional>
#include <string_view>

#include "wisp/agent/tool_index.hpp"
#include "wisp/agent/tool_provider.hpp"
#include "wisp/scene/scene_runtime.hpp"

namespace wisp::agent {

class SceneToolProvider final : public ToolProvider {
public:
    // Both must outlive the provider.
    SceneToolProvider(ToolIndex& index, scene::SceneRuntime& runtime);

    std::string_view id() const override;
    ToolFindFields find_fields() const override;
    // Every name without a ':' (the MCP "server:tool" syntax), so a name in
    // neither syntax still gets the scene "no scene tool named" answer.
    bool owns(std::string_view full_name) const override;
    void search(std::string_view query, std::string_view scope, size_t max, bool details,
                ToolCtx& ctx, ToolSearch& out) override;
    size_t count() const override;
    std::optional<ToolInfo> describe(std::string_view full_name, ToolCtx& ctx) override;
    ToolOutput call(std::string_view full_name, const Value& args, ToolCtx& ctx) override;

private:
    ToolIndex& index_;
    scene::SceneRuntime& runtime_;
};

}  // namespace wisp::agent
