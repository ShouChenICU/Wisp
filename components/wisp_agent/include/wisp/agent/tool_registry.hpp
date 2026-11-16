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
// Vision (llm-tool-providers spec 3.5): tools_json() has two arrays, with
// and without the vision-only tools, and a tool may return one image
// (ToolOutput::image) that the turn shows to a vision model.
//
// A tool on another thread is handed a copy of the ToolCtx and shares only
// a reference-counted state with call(). When call() gives up (timeout,
// cancel) the thread cannot be interrupted: it runs to the end, its
// cancelled() then returns true, and its result is dropped.
//
// Everything the registry keeps for the life of the program lives in
// Mem::Large (PSRAM on the device): the definitions' text and run
// functions (os::LargeString, os::LargeFunction), the entries and their
// indexes, and both tools_json() arrays. An ordinary allocation under 16 KB would
// stay in internal SRAM for good (AGENTS.md section 8), and with dozens
// of tools that is tens of KB. Argument schemas are kept as text and
// parsed per call: a parsed Value is a tree of std::map nodes on the
// default heap, and calls are rare next to the time the registry lives.

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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
enum class ToolThread : uint8_t { Agent, Ui, Fs, Io, FsInteractive };

struct ToolCtx {
    std::function<bool()> cancelled;
    uint64_t deadline_ms = 0;  // os::now_ms() based, 0 for none
    std::string lang;          // "zh" | "ja" | "en", for note text
    // Identifies the turn the call belongs to; unique per boot (not the
    // conversation's turn number, which restarts in a new conversation);
    // 0 outside a turn.
    uint32_t turn = 0;
    // Whether the turn's model takes images (spec 3.5); tools that would produce one
    // answer "unsupported" without it.
    bool vision = false;
};

// One encoded image a tool produced (spec 3.5). Allocated in Mem::Large
// (std::allocate_shared with os::LargeAllocator); never stored.
struct ToolImage {
    const char* mime = "";    // "image/jpeg" | "image/png" (static strings)
    os::LargeBuffer bytes{};  // at most kImageMaxBytes
    uint32_t width = 0;       // 0 when unknown
    uint32_t height = 0;
};

struct ToolOutput {
    bool ok = true;
    Value result;  // Small structured results; tool_error() builds failures.
    // Large results may be pre-serialized into Mem::Large to avoid a default-heap Value tree.
    os::LargeString result_json;
    std::string note;  // the system note shown to the user, "" for none
    // Set only by a tool whose text result already describes it
    // ("image":{"mime","bytes","width","height"}); the turn shows it to a vision model.
    std::shared_ptr<const ToolImage> image;
};

using ToolFn = os::LargeFunction<ToolOutput(const Value& args, ToolCtx& ctx)>;

// Asks the user on the device (agent-core spec 6.1); true for yes.
// AgentService::confirm_fn() provides it.
using ConfirmFn = std::function<bool(const std::string& text, ToolCtx& ctx)>;

struct ToolDef {
    os::LargeString name;  // dotted, ^[a-z]+\.[a-z_]+$
    os::LargeString description;
    os::LargeString schema_json{};  // JSON Schema subset (validate_args); "" is an empty object
    ToolRisk risk = ToolRisk::Normal;
    // Offered only to a model with vision (spec 3.5): left out of tools_json(false), last
    // in tools_json(true), and unknown to call() without ctx.vision.
    bool vision_only = false;
    // Where run() runs. The default, Agent, runs it on wisp.agent itself,
    // whose stack may be in PSRAM (flash_access = false): a tool that
    // touches flash in any way -- Settings/NVS, StorageManager files on
    // LittleFS, anything that calls them -- must declare Fs or
    // FsInteractive (wisp.fs). Io is a one-shot task per call
    // (wisp.tool.io, IoRunner), with its stack in PSRAM on the device. It
    // is for blocking work that cannot watch ctx.cancelled, such as an
    // encoder, and it never touches flash (AGENTS.md section 5;
    // WISP_ASSERT_FLASH_THREAD enforces this). Spec 3.9. FsInteractive
    // is wisp.fs too, on its Interactive lane (flash-thread spec 3.1): for
    // tools whose whole call is a small file write the user waits for (the
    // memory and todo tools), never for long work (scene installs,
    // exports, file tools), which would hold up every other Interactive
    // job. Within a lane jobs keep their order; across lanes they do not,
    // which is safe for a tool since the registry waits for each call.
    ToolThread thread = ToolThread::Agent;
    uint32_t timeout_ms = 5000;
    ToolFn run;
};

// Each posts `fn` to its thread and returns false if it could not be
// queued. An empty function refuses everything, except fs_interactive:
// empty, FsInteractive tools go to fs.
struct ToolExecutors {
    std::function<bool(std::function<void()>)> ui, fs, io;
    std::function<bool(std::function<void()>)> fs_interactive;
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
    // With `vision`, the vision-only tools follow (agent-core spec 3.2 item 2).
    // Without any vision-only tool both are the same array. The vision array
    // is built on its first use, so add() stays O(size of one tool).
    const os::LargeString& tools_json(bool vision = false) const;

    // Parses args_json ("" means {}), validates it, runs the tool on its
    // thread and waits up to min(timeout_ms, ctx.deadline_ms), checking
    // ctx.cancelled() every 20 ms. An Agent tool runs right here. Leaves
    // the registry unchanged, so a const registry (App::agent_tools()) can
    // call its tools. A vision-only tool is unknown_tool without ctx.vision,
    // as any name missing from the array the model got.
    ToolOutput call(std::string_view wire, std::string_view args_json, ToolCtx& ctx) const;

private:
    struct Entry {
        ToolDef def;
        os::LargeString wire;
    };

    const Entry* find_entry(std::string_view key, bool wire) const;
    static void append_entry_json(os::LargeString& out, const Entry& e,
                                  std::string_view parameters);

    ToolExecutors ex_;
    os::LargeVector<Entry> entries_;
    // Positions in entries_, sorted by def.name and by wire, for binary
    // search.
    os::LargeVector<uint16_t> by_name_;
    os::LargeVector<uint16_t> by_wire_;
    os::LargeString json_;
    // The vision-only entries' objects, comma-separated, without brackets.
    os::LargeString vision_tail_;
    mutable os::LargeString vision_json_;
    mutable std::once_flag vision_once_;
};

// A ToolImage in Mem::Large whose bytes hold `size` bytes; null only when the
// byte buffer could not be allocated (allocate_shared over os::LargeAllocator
// aborts on out-of-memory, as every LargeAllocator does).
std::shared_ptr<ToolImage> make_tool_image(const char* mime, size_t size);

// Appends {"mime":…,"bytes":N,"width":W,"height":H} (width/height left out when 0).
void append_image_json(os::LargeString& out, const ToolImage& img);

std::string wire_name(std::string_view dotted);

// The JSON Schema subset tools use: type (object, string, integer, number,
// boolean, array, null; a property may list several, ["integer", "string"]
// or ["string", "null"] for one that may be cleared, and then takes any of
// them), properties, required, enum, minimum, maximum, maxLength
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

// Wraps content from outside the conversation (agent-core spec 6.3,
// agent-tools spec 7.1): <data source="...">content</data>, with every
// "</data>" inside (any letter case) neutralised to "&lt;/data>" so the
// content cannot close the wrapper and speak as the system. `source` is
// used as given: "scene:<id>" (scene_data() below), "http:<host>"
// (http.fetch) or "lua" (lua.run).
std::string external_data(std::string_view source, std::string_view content);

// external_data("scene:" + scene_id, content): text that came from a
// scene.
std::string scene_data(std::string_view scene_id, std::string_view content);

}  // namespace wisp::agent
