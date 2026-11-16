#pragma once

// ToolProvider (llm-tool-providers spec 4.1): a source of tools the model
// reaches through tools.find and tools.call instead of the fixed "tools"
// array -- installed scenes today, MCP servers next. Providers are
// registered on ToolRegistry (add_provider()); add_find_tools() searches
// them in registration order and routes a call to the one that owns the
// name. Every method runs on wisp.agent (ToolThread::Agent), so none may
// touch flash.
//
// Everything a provider hands back lives in Mem::Large: a 200-entry
// listing must not land on the default heap (internal RAM on the device).

#include <cstddef>
#include <optional>
#include <string_view>

#include "wisp/agent/tool_registry.hpp"
#include "wisp/base/value.hpp"
#include "wisp/os/large.hpp"

namespace wisp::agent {

struct ToolInfo {
    os::LargeString name{};         // full name, as tools.call takes it
    os::LargeString description{};  // already wrapped as external data
    os::LargeString parameters{};   // JSON Schema text; "" in list mode
    os::LargeString origin{};       // scene name or server name, for the note and details
    bool available = true;          // scene running / server reachable
};

struct ToolSourceIssue {
    os::LargeString source{};  // a server id
    // A category: dns connect tls timeout http unsupported_protocol bad_response ...
    os::LargeString error{};
};

struct ToolSearch {
    os::LargeVector<ToolInfo> tools{};
    os::LargeVector<ToolSourceIssue> unavailable{};
};

// The keys a provider's detailed find entries use for origin and available.
// Scene entries keep the keys they always had ("scene_name", "running"), so
// their bytes do not change.
struct ToolFindFields {
    std::string_view origin{};
    std::string_view available{};
};

class ToolProvider {
public:
    virtual ~ToolProvider() = default;

    // "scene", "mcp".
    virtual std::string_view id() const = 0;
    virtual ToolFindFields find_fields() const = 0;
    // Whether calls to full_name route here. Ownership goes by syntax, not
    // by lookup, so an unknown name still gets its owner's own not_found.
    virtual bool owns(std::string_view full_name) const = 0;
    // Appends at most `max` matches to out.tools (the caller still truncates
    // an overfill). `details` is false in list mode, where parameters may be
    // left empty. `scope` is the tools.find "scene" argument, "" for all.
    virtual void search(std::string_view query, std::string_view scope, size_t max, bool details,
                        ToolCtx& ctx, ToolSearch& out) = 0;
    // How many tools the provider knows, from memory only (no I/O).
    virtual size_t count() const = 0;
    virtual std::optional<ToolInfo> describe(std::string_view full_name, ToolCtx& ctx) = 0;
    // `args` is tools.call's "args" as given, null when absent: the
    // provider looks the name up first and checks args second, so an
    // unknown name is not_found before its args are judged. Builds the
    // whole output, note included.
    virtual ToolOutput call(std::string_view full_name, const Value& args, ToolCtx& ctx) = 0;
};

}  // namespace wisp::agent
