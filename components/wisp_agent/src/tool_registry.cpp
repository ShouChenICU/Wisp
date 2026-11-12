#include "wisp/agent/tool_registry.hpp"

#include <algorithm>
#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
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
    os::Semaphore done{1, 0};

    // The caller's own cancel check may capture state that dies when call()
    // returns. It only runs under `mutex` with `abandoned` rechecked, and
    // abandon() takes `mutex` after setting the flag, so once abandon()
    // returns no invocation is running and none can start. The flag is
    // also read before locking, so a tool polling in a tight loop cannot
    // keep abandon() from getting the mutex.
    os::Mutex mutex;
    std::atomic<bool> abandoned{false};
    std::function<bool()> outer;

    bool cancelled() {
        if (abandoned.load()) return true;
        std::lock_guard<os::Mutex> lock(mutex);
        return abandoned.load() || (outer && outer());
    }

    void abandon() {
        abandoned.store(true);
        std::lock_guard<os::Mutex> lock(mutex);
    }
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

std::string external_data(std::string_view source, std::string_view content) {
    static constexpr std::string_view kClose = "</data>";
    std::string out = "<data source=\"";
    out.append(source);
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

std::string scene_data(std::string_view scene_id, std::string_view content) {
    return external_data("scene:" + std::string(scene_id), content);
}

ToolRegistry::ToolRegistry(ToolExecutors ex) : ex_(std::move(ex)), json_("[]") {}

Result<void> ToolRegistry::add(ToolDef def) {
    const std::string name(def.name.data(), def.name.size());
    if (!valid_name(def.name)) {
        return Result<void>::err(ErrorCode::InvalidArgument, "bad tool name: " + name);
    }
    if (find(def.name) != nullptr) {
        return Result<void>::err(ErrorCode::InvalidArgument, "duplicate tool: " + name);
    }
    if (!def.run) {
        return Result<void>::err(ErrorCode::InvalidArgument, "no run function: " + name);
    }
    if (entries_.size() >= std::numeric_limits<uint16_t>::max()) {
        return Result<void>::err(ErrorCode::InvalidArgument, "too many tools: " + name);
    }
    // Parsed only to check it and to write it compactly into tools_json();
    // both the tree and the text are freed before anything is kept.
    std::string parameters;
    {
        auto schema = from_json(def.schema_json.empty() ? std::string_view(kEmptySchema)
                                                        : std::string_view(def.schema_json));
        if (!schema) {
            return Result<void>::err(ErrorCode::InvalidArgument,
                                     name + ": schema is not JSON: " + schema.error().message);
        }
        std::string why;
        if (!schema_supported(schema.value(), &why)) {
            return Result<void>::err(ErrorCode::InvalidArgument, name + ": " + why);
        }
        const Value* type = schema.value().find("type");
        if (type->as_string() != "object") {
            return Result<void>::err(ErrorCode::InvalidArgument,
                                     name + ": arguments must be an object schema");
        }
        // Re-serialized, so the text is compact and the same on every boot.
        parameters = to_json(schema.value());
    }

    os::LargeString wire(def.name);
    std::replace(wire.begin(), wire.end(), '.', '_');
    const auto pos = static_cast<uint16_t>(entries_.size());
    entries_.push_back(Entry{std::move(def), std::move(wire)});
    const Entry& e = entries_.back();
    auto name_at = std::lower_bound(
        by_name_.begin(), by_name_.end(), e.def.name,
        [this](uint16_t i, const os::LargeString& k) { return entries_[i].def.name < k; });
    by_name_.insert(name_at, pos);
    auto wire_at = std::lower_bound(
        by_wire_.begin(), by_wire_.end(), e.wire,
        [this](uint16_t i, const os::LargeString& k) { return entries_[i].wire < k; });
    by_wire_.insert(wire_at, pos);
    append_json(e, parameters);
    return Result<void>::ok();
}

const ToolRegistry::Entry* ToolRegistry::find_entry(std::string_view key, bool wire) const {
    const os::LargeVector<uint16_t>& index = wire ? by_wire_ : by_name_;
    auto key_of = [this, wire](uint16_t i) -> std::string_view {
        const Entry& e = entries_[i];
        return wire ? std::string_view(e.wire) : std::string_view(e.def.name);
    };
    auto it = std::lower_bound(index.begin(), index.end(), key,
                               [&](uint16_t i, std::string_view k) { return key_of(i) < k; });
    if (it == index.end() || key_of(*it) != key) return nullptr;
    return &entries_[*it];
}

const ToolDef* ToolRegistry::find(std::string_view dotted) const {
    const Entry* e = find_entry(dotted, /*wire=*/false);
    return e == nullptr ? nullptr : &e->def;
}

const ToolDef* ToolRegistry::find_wire(std::string_view wire) const {
    const Entry* e = find_entry(wire, /*wire=*/true);
    return e == nullptr ? nullptr : &e->def;
}

const os::LargeString& ToolRegistry::tools_json() const { return json_; }

// Appended in place of the closing ']', so adding a tool costs only its own
// text and the array is complete after every add().
void ToolRegistry::append_json(const Entry& e, std::string_view parameters) {
    json_.pop_back();
    if (entries_.size() > 1) json_.push_back(',');
    json_.append(R"({"type":"function","function":{"name":)");
    append_json_string(json_, e.wire);
    json_.append(R"(,"description":)");
    append_json_string(json_, e.def.description);
    json_.append(R"(,"parameters":)");
    json_.append(parameters.data(), parameters.size());
    json_.append("}}]");
}

ToolOutput ToolRegistry::call(std::string_view wire, std::string_view args_json, ToolCtx& ctx) {
    const Entry* found = find_entry(wire, /*wire=*/true);
    if (found == nullptr) {
        return tool_error("unknown_tool", "no tool named " + std::string(wire));
    }
    const Entry& e = *found;

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
    // add() checked it, so it parses.
    auto schema = from_json(e.def.schema_json.empty() ? std::string_view(kEmptySchema)
                                                      : std::string_view(e.def.schema_json));
    if (!schema) {
        return tool_error("internal", schema.error().message);
    }
    std::string why;
    if (auto field = validate_args(schema.value(), args, &why)) {
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
    // Once call() has given up, the tool sees itself cancelled. `raw` is
    // valid whenever the tool can ask: the task owns `state` until the tool
    // returns.
    state->outer = ctx.cancelled;
    CallState* raw = state.get();
    state->ctx.cancelled = [raw] { return raw->cancelled(); };

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
            state->abandon();
            return tool_error("timeout", e.def.name + " did not finish in time");
        }
        const uint32_t slice = static_cast<uint32_t>(std::min<uint64_t>(kPollMs, end - now));
        if (state->done.take(slice)) {
            return std::move(state->out);
        }
        if (ctx.cancelled && ctx.cancelled()) {
            state->abandon();
            return tool_error("cancelled", "the turn was stopped");
        }
    }
}

}  // namespace wisp::agent
