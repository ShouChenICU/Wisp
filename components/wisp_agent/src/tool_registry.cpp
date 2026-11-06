#include "wisp/agent/tool_registry.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

#include "wisp/base/json.hpp"

namespace wisp::agent {

namespace {

constexpr uint8_t kArgsMaxDepth = 16;
constexpr uint32_t kPollMs = 20;
constexpr const char* kEmptySchema = R"({"type":"object","properties":{}})";

bool lower(char c) { return c >= 'a' && c <= 'z'; }

// ^[a-z]+\.[a-z_]+$
bool valid_name(std::string_view n) {
    const size_t dot = n.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == n.size()) return false;
    for (size_t i = 0; i < dot; ++i) {
        if (!lower(n[i])) return false;
    }
    for (size_t i = dot + 1; i < n.size(); ++i) {
        if (!lower(n[i]) && n[i] != '_') return false;
    }
    return true;
}

bool blank(std::string_view s) {
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
}

// What call() and the tool's thread share. The thread owns a reference
// until the tool returns, so call() can give up and return at any time.
struct CallState {
    ToolFn run;
    Value args;
    ToolCtx ctx;
    ToolOutput out;
    std::atomic<bool> abandoned{false};
    os::Semaphore done{1, 0};
};

}  // namespace

std::string wire_name(std::string_view dotted) {
    std::string out(dotted);
    std::replace(out.begin(), out.end(), '.', '_');
    return out;
}

ToolOutput tool_error(std::string_view code, std::string_view message, Map extra) {
    extra["ok"] = false;
    extra["error"] = std::string(code);
    extra["message"] = std::string(message);
    ToolOutput out;
    out.ok = false;
    out.result = Value(std::move(extra));
    return out;
}

std::string scene_data(std::string_view scene_id, std::string_view content) {
    static constexpr std::string_view kClose = "</data>";
    std::string out = "<data source=\"scene:";
    out.append(scene_id);
    out.append("\">");
    out.reserve(out.size() + content.size() + kClose.size());
    size_t i = 0;
    while (i < content.size()) {
        bool closing = content.size() - i >= kClose.size();
        for (size_t k = 0; closing && k < kClose.size(); ++k) {
            char c = content[i + k];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            closing = c == kClose[k];
        }
        if (closing) {
            out.append("&lt;");
            out.append(content.substr(i + 1, kClose.size() - 1));
            i += kClose.size();
        } else {
            out.push_back(content[i]);
            ++i;
        }
    }
    out.append(kClose);
    return out;
}

ToolRegistry::ToolRegistry(ToolExecutors ex) : ex_(std::move(ex)) { rebuild_json(); }

Result<void> ToolRegistry::add(ToolDef def) {
    if (!valid_name(def.name)) {
        return Result<void>::err(ErrorCode::InvalidArgument, "bad tool name: " + def.name);
    }
    if (by_name_.count(def.name) != 0) {
        return Result<void>::err(ErrorCode::InvalidArgument, "duplicate tool: " + def.name);
    }
    if (!def.run) {
        return Result<void>::err(ErrorCode::InvalidArgument, "no run function: " + def.name);
    }
    auto schema = from_json(def.schema_json.empty() ? kEmptySchema : def.schema_json);
    if (!schema) {
        return Result<void>::err(ErrorCode::InvalidArgument,
                                 def.name + ": schema is not JSON: " + schema.error().message);
    }
    std::string why;
    if (!schema_supported(schema.value(), &why)) {
        return Result<void>::err(ErrorCode::InvalidArgument, def.name + ": " + why);
    }
    const Value* type = schema.value().find("type");
    if (type->as_string() != "object") {
        return Result<void>::err(ErrorCode::InvalidArgument,
                                 def.name + ": arguments must be an object schema");
    }
    std::string wire = wire_name(def.name);
    by_name_.emplace(def.name, entries_.size());
    by_wire_.emplace(wire, entries_.size());
    entries_.push_back(Entry{std::move(def), std::move(wire), std::move(schema.value())});
    rebuild_json();
    return Result<void>::ok();
}

const ToolDef* ToolRegistry::find(std::string_view dotted) const {
    auto it = by_name_.find(dotted);
    return it == by_name_.end() ? nullptr : &entries_[it->second].def;
}

const ToolDef* ToolRegistry::find_wire(std::string_view wire) const {
    auto it = by_wire_.find(wire);
    return it == by_wire_.end() ? nullptr : &entries_[it->second].def;
}

const os::LargeString& ToolRegistry::tools_json() const { return json_; }

void ToolRegistry::rebuild_json() {
    os::LargeString out;
    out.push_back('[');
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        if (i > 0) out.push_back(',');
        out.append(R"({"type":"function","function":{"name":)");
        append_json_string(out, e.wire);
        out.append(R"(,"description":)");
        append_json_string(out, e.def.description);
        out.append(R"(,"parameters":)");
        // Re-serialized, so the text is compact and the same on every boot.
        out.append(to_json(e.schema));
        out.append("}}");
    }
    out.push_back(']');
    json_ = std::move(out);
}

ToolOutput ToolRegistry::call(std::string_view wire, std::string_view args_json, ToolCtx& ctx) {
    auto it = by_wire_.find(wire);
    if (it == by_wire_.end()) {
        return tool_error("unknown_tool", "no tool named " + std::string(wire));
    }
    const Entry& e = entries_[it->second];

    Value args = Value(Map{});
    if (!blank(args_json)) {
        auto parsed = from_json(args_json, kArgsMaxDepth);
        if (!parsed) {
            return tool_error("bad_args", "arguments are not valid JSON: " + parsed.error().message,
                              Map{{"field", ""}});
        }
        args = std::move(parsed.value());
    }
    if (args.type() != Value::Type::Map) {
        return tool_error("bad_args", "arguments must be a JSON object", Map{{"field", ""}});
    }
    std::string why;
    if (auto field = validate_args(e.schema, args, &why)) {
        return tool_error("bad_args", why, Map{{"field", *field}});
    }
    if (ctx.cancelled && ctx.cancelled()) {
        return tool_error("cancelled", "the turn was stopped");
    }

    if (e.def.thread == ToolThread::Agent) {
        return e.def.run(args, ctx);
    }

    const std::function<bool(std::function<void()>)>* post = nullptr;
    switch (e.def.thread) {
        case ToolThread::Ui:
            post = &ex_.ui;
            break;
        case ToolThread::Fs:
            post = &ex_.fs;
            break;
        case ToolThread::Io:
            post = &ex_.io;
            break;
        case ToolThread::Agent:
            break;
    }

    auto state = std::make_shared<CallState>();
    state->run = e.def.run;
    state->args = std::move(args);
    state->ctx = ctx;
    // Once call() has given up, the tool sees itself cancelled; the
    // caller's own check is not consulted after that, since what it
    // captured may be gone by the time a late tool asks.
    CallState* raw = state.get();
    state->ctx.cancelled = [raw, outer = ctx.cancelled] {
        return raw->abandoned.load() || (outer && outer());
    };

    if (post == nullptr || !*post || !(*post)([state] {
            state->out = state->run(state->args, state->ctx);
            state->done.give();
        })) {
        return tool_error("busy", "the " + e.def.name + " thread is busy");
    }

    const uint64_t start = os::now_ms();
    uint64_t end = start + e.def.timeout_ms;
    if (ctx.deadline_ms != 0) end = std::min(end, ctx.deadline_ms);
    for (;;) {
        const uint64_t now = os::now_ms();
        if (now >= end) {
            state->abandoned = true;
            return tool_error("timeout", e.def.name + " did not finish in time");
        }
        const uint32_t slice = static_cast<uint32_t>(std::min<uint64_t>(kPollMs, end - now));
        if (state->done.take(slice)) {
            return std::move(state->out);
        }
        if (ctx.cancelled && ctx.cancelled()) {
            state->abandoned = true;
            return tool_error("cancelled", "the turn was stopped");
        }
    }
}

}  // namespace wisp::agent
