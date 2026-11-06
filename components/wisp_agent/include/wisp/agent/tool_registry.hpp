#pragma once

// ToolRegistry (agent-core spec 4): the agent's tools, their argument
// schemas, and running one on the thread it declares.
//
// Tool names are dotted ("scene.install") everywhere people see them. The
// OpenAI function name allows no dots, so requests carry the wire name with
// '.' replaced by '_' (wire_name()).
//
// Results are {"ok":true,...} or {"ok":false,"error":"<code>","message"}
// (tool_error()). call() itself produces unknown_tool, bad_args (with
// "field"), busy, timeout and cancelled.
//
// A tool on another thread is handed a copy of the ToolCtx and shares only
// a reference-counted state with call(). When call() gives up (timeout,
// cancel) the thread cannot be interrupted: it runs to the end, its
// cancelled() then returns true, and its result is dropped.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::agent {

enum class ToolRisk : uint8_t { Normal, Note, Confirm };
enum class ToolThread : uint8_t { Agent, Ui, Fs, Io };

struct ToolCtx {
    std::function<bool()> cancelled;
    uint64_t deadline_ms = 0;  // os::now_ms() based, 0 for none
    std::string lang;          // "zh" | "ja" | "en", for note text
};

struct ToolOutput {
    bool ok = true;
    Value result;      // {"ok":..., ...}; tool_error() builds failures
    std::string note;  // the system note shown to the user, "" for none
};

using ToolFn = std::function<ToolOutput(const Value& args, ToolCtx& ctx)>;

// Asks the user on the device (agent-core spec 6.1); true for yes.
// AgentService::confirm_fn() provides it.
using ConfirmFn = std::function<bool(const std::string& text, ToolCtx& ctx)>;

struct ToolDef {
    std::string name;  // dotted, ^[a-z]+\.[a-z_]+$
    std::string description;
    std::string schema_json;  // JSON Schema subset (validate_args); "" is an empty object
    ToolRisk risk = ToolRisk::Normal;
    // Where run() runs. The default, Agent, runs it on wisp.agent itself,
    // whose stack may be in PSRAM (flash_access = false): a tool that
    // touches flash in any way -- Settings/NVS, StorageManager files on
    // LittleFS, anything that calls them -- must declare Fs (wisp.fs) or
    // Io (a one-shot task with an internal stack) instead.
    ToolThread thread = ToolThread::Agent;
    uint32_t timeout_ms = 5000;
    ToolFn run;
};

// Each posts `fn` to its thread and returns false if it could not be
// queued. An empty function refuses everything.
struct ToolExecutors {
    std::function<bool(std::function<void()>)> ui, fs, io;
};

class ToolRegistry {
public:
    explicit ToolRegistry(ToolExecutors ex);

    ToolRegistry(const ToolRegistry&) = delete;
    ToolRegistry& operator=(const ToolRegistry&) = delete;

    // InvalidArgument for a duplicate or malformed name, a schema that is
    // not valid JSON or not an object schema, or a missing run function.
    // Not thread-safe: register every tool before the first call().
    Result<void> add(ToolDef def);

    const ToolDef* find(std::string_view dotted) const;
    const ToolDef* find_wire(std::string_view wire) const;

    // The request's "tools" array, in add() order:
    // [{"type":"function","function":{"name":<wire>,"description","parameters"}}].
    const os::LargeString& tools_json() const;

    // Parses args_json ("" means {}), validates it, runs the tool on its
    // thread and waits up to min(timeout_ms, ctx.deadline_ms), checking
    // ctx.cancelled() every 20 ms. An Agent tool runs right here.
    ToolOutput call(std::string_view wire, std::string_view args_json, ToolCtx& ctx);

private:
    struct Entry {
        ToolDef def;
        std::string wire;
        Value schema;
    };

    void rebuild_json();

    ToolExecutors ex_;
    std::vector<Entry> entries_;
    std::map<std::string, size_t, std::less<>> by_name_;
    std::map<std::string, size_t, std::less<>> by_wire_;
    os::LargeString json_;
};

std::string wire_name(std::string_view dotted);

// The JSON Schema subset tools use: type (object, string, integer, number,
// boolean, array; a property may list several, ["integer", "string"],
// and then takes any of them), properties, required, enum, minimum, maximum, maxLength
// (in code points) and items. Properties not in the schema are allowed.
// Returns nullopt when `args` conforms, otherwise the path of the failing
// field ("path", "a.b", "list[2]"; "" for the arguments as a whole), with
// the reason in *why.
std::optional<std::string> validate_args(const Value& schema, const Value& args, std::string* why);

// Whether `schema` uses only the subset above, correctly typed; *why says
// what is wrong.
bool schema_supported(const Value& schema, std::string* why);

// {"ok":false,"error":code,"message":message} plus `extra`.
ToolOutput tool_error(std::string_view code, std::string_view message, Map extra = {});

// Wraps text that came from a scene (agent-core spec 6.3):
// <data source="scene:<id>">content</data>, with every "</data>" inside
// (any letter case) neutralised to "&lt;/data>".
std::string scene_data(std::string_view scene_id, std::string_view content);

}  // namespace wisp::agent
