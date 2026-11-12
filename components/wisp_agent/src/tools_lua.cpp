// lua.run (agent-tools spec 4): a thin adapter over
// scene::SceneRuntime::run_lua(), which owns the VM, its sandbox and its
// limits. What the script returns and prints may carry anything it fetched,
// so both reach the model wrapped as external data from "lua" (spec 7.1).

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "tools_json.hpp"
#include "wisp/agent/limits.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::agent {

namespace {

constexpr const char* kSource = "lua";

int64_t int_arg(const Value& a, const char* key, int64_t fallback) {
    const Value* v = a.find(key);
    if (v == nullptr) return fallback;
    if (v->type() == Value::Type::Int) return v->as_int();
    if (v->type() == Value::Type::Double) return static_cast<int64_t>(v->as_double());
    return fallback;
}

std::string build_schema() {
    return "{\"type\":\"object\",\"properties\":{"
           "\"code\":{\"type\":\"string\",\"maxLength\":" +
           std::to_string(scene::kLuaRunMaxSource) +
           "},"
           "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":" +
           std::to_string(scene::kLuaRunMinMs) +
           ",\"maximum\":" + std::to_string(scene::kLuaRunMaxMs) +
           "},"
           "\"memory_kb\":{\"type\":\"integer\",\"minimum\":" +
           std::to_string(scene::kLuaRunMinMemory / 1024) +
           ",\"maximum\":" + std::to_string(scene::kLuaRunMaxMemory / 1024) +
           "}"
           "},\"required\":[\"code\"]}";
}

std::string_view view(const os::LargeString& s) { return {s.data(), s.size()}; }

ToolOutput run(scene::SceneRuntime& runtime, const Value& args, ToolCtx& ctx) {
    const Value* code = args.find("code");
    if (code == nullptr || code->type() != Value::Type::String)
        return tool_error("bad_args", "'code' is required", Map{{"field", "code"}});
    // maxLength counts characters; the limit is in bytes (spec 4.1).
    if (code->as_string().size() > scene::kLuaRunMaxSource)
        return tool_error("bad_args", "'code' is over 64 KiB", Map{{"field", "code"}});
    scene::LuaRunOptions opts;
    opts.timeout_ms = static_cast<uint32_t>(int_arg(args, "timeout_ms", kLuaRunDefaultTimeoutMs));
    opts.memory_bytes =
        static_cast<size_t>(int_arg(args, "memory_kb", kLuaRunDefaultMemoryKb)) * 1024;
    opts.cancelled = ctx.cancelled;

    Result<scene::LuaRunResult> ran = runtime.run_lua(code->as_string(), std::move(opts));
    if (!ran) {
        if (ran.error().code == ErrorCode::InvalidArgument)
            return tool_error("bad_args", ran.error().message);
        if (ran.error().code == ErrorCode::Busy) return tool_error("busy", ran.error().message);
        return tool_error("timeout", ran.error().message);
    }
    scene::LuaRunResult& r = ran.value();
    ToolOutput out;
    out.ok = r.ok;
    auto& json = out.result_json;
    json = r.ok ? R"({"ok":true,"result":)" : R"({"ok":false,"error":)";
    if (r.ok) {
        detail::append_external(json, kSource, view(r.json));
    } else {
        append_json_string(json, r.error);
        json.append(",\"message\":");
        // A script's own error text can carry anything it fetched (spec
        // 7.1); the runtime's reasons (no_memory, cancelled) are its own.
        const bool from_script = r.error != "no_memory" && r.error != "cancelled";
        if (from_script)
            detail::append_external(json, kSource, r.message);
        else
            append_json_string(json, r.message);
    }
    json.append(",\"output\":");
    detail::append_external(json, kSource, view(r.output));
    json.append(",\"ms\":");
    detail::append_number(json, r.ms);
    json.append(",\"memory_peak_kb\":");
    detail::append_number(json, (r.memory_peak + 1023) / 1024);
    json.push_back('}');
    return out;
}

}  // namespace

Result<void> add_lua_tools(ToolRegistry& reg, LuaToolDeps deps) {
    const std::string schema = build_schema();
    ToolDef def;
    def.name = "lua.run";
    def.description =
        "Runs a Lua 5.5 script on the device in a fresh sandboxed VM and returns what it "
        "returns (as JSON) and what it printed. Without a callback, wisp.http.get/post/"
        "request/download and wisp.files.* wait for their result and return err, res; "
        "wisp.sleep(ms) waits. Also wisp.json, wisp.device, wisp.notify, wisp.log. HTTP goes "
        "out as the agent (any http/https URL); files live in the agent's own area. Each "
        "uninterrupted stretch of computation may run at most 1 s.";
    def.schema_json.assign(schema.data(), schema.size());
    def.risk = ToolRisk::Note;
    def.thread = ToolThread::Agent;
    def.timeout_ms = kLuaRunToolMs;
    scene::SceneRuntime* runtime = &deps.runtime;
    def.run = [runtime](const Value& args, ToolCtx& ctx) {
        ToolOutput out = run(*runtime, args, ctx);
        out.note =
            ui::tr(ui::StrId::AgentNoteLuaRun, ui::parse_lang(ctx.lang).value_or(ui::Lang::Zh));
        return out;
    };
    return reg.add(std::move(def));
}

}  // namespace wisp::agent
