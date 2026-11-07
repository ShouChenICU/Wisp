#include "wisp/net/llm_client.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"

namespace wisp::net {

namespace {

constexpr const char* kTag = "llm";
// Deep enough for a chat-completions chunk; the parser refuses deeper
// input, which bounds its recursion.
constexpr uint8_t kMaxEventDepth = 16;

const char* role_name(LlmRole role) {
    switch (role) {
        case LlmRole::System:
            return "system";
        case LlmRole::User:
            return "user";
        case LlmRole::Assistant:
            return "assistant";
        case LlmRole::Tool:
            return "tool";
    }
    return "user";
}

void append_message(os::LargeString& out, const LlmMessage& m) {
    out += "{\"role\":\"";
    out += role_name(m.role);
    out += '"';
    if (m.role == LlmRole::Assistant && !m.tool_calls.empty()) {
        // OpenAI's own form for a reply that is only tool calls.
        out += ",\"content\":";
        if (m.text.empty()) {
            out += "null";
        } else {
            append_json_string(out, m.text);
        }
        out += ",\"tool_calls\":[";
        bool first = true;
        for (const LlmToolCall& c : m.tool_calls) {
            if (!first) out += ',';
            first = false;
            out += "{\"id\":";
            append_json_string(out, c.id);
            out += ",\"type\":\"function\",\"function\":{\"name\":";
            append_json_string(out, c.name);
            out += ",\"arguments\":";
            append_json_string(out, {c.arguments.data(), c.arguments.size()});
            out += "}}";
        }
        out += ']';
    } else {
        if (m.role == LlmRole::Tool) {
            out += ",\"tool_call_id\":";
            append_json_string(out, m.tool_call_id);
        }
        out += ",\"content\":";
        append_json_string(out, m.text);
    }
    out += '}';
}

const Value* field(const Value& v, std::string_view key) {
    return v.type() == Value::Type::Map ? v.find(key) : nullptr;
}

const std::string* string_field(const Value& v, std::string_view key) {
    const Value* f = field(v, key);
    return f != nullptr && f->type() == Value::Type::String ? &f->as_string() : nullptr;
}

uint32_t count_field(const Value& v, std::string_view key) {
    const Value* f = field(v, key);
    if (f == nullptr || f->type() != Value::Type::Int || f->as_int() < 0) return 0;
    return static_cast<uint32_t>(std::min<int64_t>(f->as_int(), UINT32_MAX));
}

LlmFinish parse_finish(std::string_view s) {
    if (s == "stop") return LlmFinish::Stop;
    if (s == "tool_calls" || s == "function_call") return LlmFinish::ToolCalls;
    if (s == "length") return LlmFinish::Length;
    return LlmFinish::Other;
}

// The state of one streamed reply, fed one event at a time.
class StreamReader {
public:
    StreamReader(const LlmCall& call, LlmResult& result) : call_(call), result_(result) {}

    // False once the stream must not be read any further: [DONE] or a
    // broken event.
    bool on_event(std::string_view data) {
        if (done_ || bad_) return false;
        if (data == "[DONE]") {
            done_ = true;
            return false;
        }
        auto parsed = from_json(data, kMaxEventDepth);
        const Value* error = parsed ? field(parsed.value(), "error") : nullptr;
        if (!parsed || parsed.value().type() != Value::Type::Map ||
            (error != nullptr && error->type() != Value::Type::Null)) {
            bad_ = true;
            return false;
        }
        const Value& ev = parsed.value();
        if (const Value* choices = field(ev, "choices"); choices != nullptr &&
                                                         choices->type() == Value::Type::List &&
                                                         !choices->as_list().empty()) {
            const Value& choice = choices->as_list().front();
            if (const Value* delta = field(choice, "delta"); delta != nullptr) {
                if (!read_delta(*delta)) {
                    bad_ = true;
                    return false;
                }
            }
            if (const std::string* reason = string_field(choice, "finish_reason")) {
                result_.finish = parse_finish(*reason);
                finished_ = true;
            }
        }
        if (const Value* usage = field(ev, "usage");
            usage != nullptr && usage->type() == Value::Type::Map) {
            result_.usage = LlmUsage{count_field(*usage, "prompt_tokens"),
                                     count_field(*usage, "completion_tokens"),
                                     count_field(*usage, "total_tokens")};
        }
        return true;
    }

    void mark_bad() { bad_ = true; }
    bool done() const { return done_; }
    bool bad() const { return bad_; }
    // The stream reached a proper end: [DONE] or a finish_reason.
    bool complete() const { return done_ || finished_; }

private:
    bool read_delta(const Value& delta) {
        if (const std::string* content = string_field(delta, "content");
            content != nullptr && !content->empty()) {
            result_.any_body = true;
            result_.text.append(content->data(), content->size());
            if (call_.on_text) call_.on_text(*content);
        }
        const Value* calls = field(delta, "tool_calls");
        if (calls == nullptr || calls->type() != Value::Type::List) return true;
        for (const Value& c : calls->as_list()) {
            int64_t index = 0;
            if (const Value* i = field(c, "index"); i != nullptr) {
                if (i->type() != Value::Type::Int) return false;
                index = i->as_int();
            }
            if (index < 0 || static_cast<uint64_t>(index) >= LlmClient::kMaxToolCalls) return false;
            auto& list = result_.tool_calls;
            if (list.size() <= static_cast<size_t>(index))
                list.resize(static_cast<size_t>(index) + 1);
            LlmToolCall& out = list[static_cast<size_t>(index)];
            result_.any_body = true;
            if (const std::string* id = string_field(c, "id"); id != nullptr && !id->empty())
                out.id = *id;
            if (const Value* fn = field(c, "function"); fn != nullptr) {
                if (const std::string* name = string_field(*fn, "name");
                    name != nullptr && !name->empty())
                    out.name = *name;
                if (const std::string* args = string_field(*fn, "arguments"))
                    out.arguments.append(args->data(), args->size());
            }
            report(static_cast<size_t>(index), out);
        }
        return true;
    }

    // on_tool for call `index`: at once when its name is first known,
    // then once kToolProgressMs has passed since the last report and the
    // arguments have grown.
    void report(size_t index, const LlmToolCall& c) {
        if (!call_.on_tool || c.name.empty()) return;
        if (progress_.size() <= index) progress_.resize(index + 1);
        Progress& p = progress_[index];
        const uint64_t now = call_.now_ms ? call_.now_ms() : os::now_ms();
        if (p.named) {
            if (c.arguments.size() == p.bytes || now - p.at < LlmClient::kToolProgressMs) return;
        }
        p.named = true;
        p.at = now;
        p.bytes = c.arguments.size();
        call_.on_tool(LlmToolProgress{.index = index, .name = c.name, .arg_bytes = p.bytes});
    }

    struct Progress {
        bool named = false;
        uint64_t at = 0;
        size_t bytes = 0;
    };

    const LlmCall& call_;
    LlmResult& result_;
    // By stream index, like result_.tool_calls (at most kMaxToolCalls).
    std::vector<Progress> progress_;
    bool done_ = false;
    bool bad_ = false;
    bool finished_ = false;
};

}  // namespace

os::LargeString build_chat_body(std::string_view model, std::span<const LlmMessage> msgs,
                                std::string_view tools_json) {
    size_t estimate = 128 + model.size() + tools_json.size();
    for (const LlmMessage& m : msgs) {
        estimate += 48 + m.text.size() + m.tool_call_id.size();
        for (const LlmToolCall& c : m.tool_calls)
            estimate += 80 + c.id.size() + c.name.size() + c.arguments.size();
    }
    os::LargeString out;
    // Escaping can grow the text; this covers the common case in one go.
    out.reserve(estimate + estimate / 8);
    out += "{\"model\":";
    append_json_string(out, model);
    out += ",\"stream\":true,\"stream_options\":{\"include_usage\":true},\"messages\":[";
    bool first = true;
    for (const LlmMessage& m : msgs) {
        if (!first) out += ',';
        first = false;
        append_message(out, m);
    }
    out += ']';
    if (!tools_json.empty()) {
        out += ",\"tools\":";
        out.append(tools_json.data(), tools_json.size());
    }
    out += '}';
    return out;
}

LlmClient::LlmClient(HttpClient& http, core::ModelProfileService& models,
                     std::function<void(uint32_t)> sleep_ms)
    : http_(http), models_(models), sleep_ms_(std::move(sleep_ms)) {}

LlmResult LlmClient::complete(const LlmCall& call) {
    LlmResult result;
    const uint64_t start = os::now_ms();
    std::optional<core::ResolvedProfile> profile = models_.active_for_request(core::ModelKind::Llm);
    if (!profile) {
        result.error = "no_model";
        WLOG_W(kTag, "no active llm profile");
        return result;
    }
    result.model = profile->model;

    const os::LargeString body = build_chat_body(profile->model, call.messages, call.tools_json);
    HttpClientRequest req;
    req.method = Method::Post;
    // ModelProfileService strips trailing '/' from base_url.
    req.url = profile->base_url + "/chat/completions";
    req.headers.emplace_back("Content-Type", "application/json");
    req.headers.emplace_back("Accept", "text/event-stream");
    if (!profile->api_key.empty())
        req.headers.emplace_back("Authorization", "Bearer " + profile->api_key);
    // Only the header copy in `req` is needed from here on.
    profile->api_key.clear();
    req.body_ref = {reinterpret_cast<const uint8_t*>(body.data()), body.size()};
    req.max_response_bytes = kMaxStreamBytes;
    req.cancelled = call.cancelled;

    const auto stopped = [&call] { return call.cancelled && call.cancelled(); };
    const auto left_ms = [&]() -> uint32_t {
        const uint64_t spent = os::now_ms() - start;
        return spent >= call.timeout_ms ? 0 : static_cast<uint32_t>(call.timeout_ms - spent);
    };

    for (uint32_t retry = 0;; ++retry) {
        if (stopped()) {
            result.error = "cancelled";
            result.transport = TransportError::Cancelled;
            break;
        }
        const uint32_t left = left_ms();
        if (left == 0) {
            result.error = "transport";
            result.transport = TransportError::Timeout;
            break;
        }

        int status = 0;
        SseParser parser;
        StreamReader reader(call, result);
        req.timeout_ms = left;
        req.on_status = [&status](int s) { status = s; };
        req.on_body = [&](std::span<const uint8_t> bytes) {
            if (call.on_bytes) call.on_bytes();
            // A failed request's body is dropped unread (it may quote the key).
            if (status < 200 || status >= 300) return true;
            parser.feed(bytes, [&reader](std::string_view data) { reader.on_event(data); });
            if (parser.overflowed()) reader.mark_bad();
            return !reader.done() && !reader.bad();
        };
        if (call.watch != nullptr) {
            const uint32_t ticket = call.watch->arm(left);
            req.watch = call.watch;
            if (call.on_armed) call.on_armed(ticket);
        }
        ++result.attempts;
        HttpClientResponse res = http_.request(req);
        if (call.watch != nullptr) call.watch->disarm();

        result.status = status != 0 ? status : res.status;
        result.transport = res.transport;
        if (reader.bad()) {
            result.error = "bad_stream";
            result.transport = TransportError::None;
            break;
        }
        if (reader.done()) {
            // Reading stopped at [DONE] on purpose.
            result.transport = TransportError::None;
            break;
        }
        if (res.transport != TransportError::None) {
            result.error = res.transport == TransportError::Cancelled ? "cancelled" : "transport";
            break;
        }
        const int s = result.status;
        if (s >= 200 && s < 300) {
            if (!reader.complete()) result.error = "bad_stream";
            break;
        }
        if (s == 401) {
            result.error = "unauthorized";
            break;
        }
        if (s != 429 && s < 500) {
            result.error = "http";
            break;
        }
        result.error = s == 429 ? "rate_limited" : "server";
        if (retry >= std::size(kRetryDelaysMs) || kRetryDelaysMs[retry] >= left_ms()) break;
        result.error.clear();
        for (uint32_t wait = kRetryDelaysMs[retry]; wait > 0 && !stopped();) {
            const uint32_t slice = std::min(wait, kWaitSliceMs);
            sleep_ms_(slice);
            wait -= slice;
        }
    }

    // Slots the stream skipped were never filled: not calls.
    std::erase_if(result.tool_calls, [](const LlmToolCall& c) {
        return c.id.empty() && c.name.empty() && c.arguments.empty();
    });

    const auto ms = static_cast<unsigned>(os::now_ms() - start);
    const unsigned tokens = result.usage ? result.usage->total : 0;
    if (result.error.empty()) {
        WLOG_I(kTag, "model=%s status=%d ms=%u tokens=%u", result.model.c_str(), result.status, ms,
               tokens);
    } else {
        WLOG_W(kTag, "model=%s status=%d ms=%u tokens=%u error=%s", result.model.c_str(),
               result.status, ms, tokens, result.error.c_str());
    }
    return result;
}

}  // namespace wisp::net
