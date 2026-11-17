#include "wisp/net/llm_client.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "secret_scrub.hpp"
#include "wisp/base/base64.hpp"
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

// A sink for append_json_string() that drops the literal's two quotes, so
// that a string can be written in pieces into one literal. The quotes are
// the only bare '"' it writes: one inside the text arrives escaped through
// append().
struct UnquotedSink {
    os::LargeString& out;
    void push_back(char c) {
        if (c != '"') out.push_back(c);
    }
    void append(const char* s, size_t len) { out.append(s, len); }
    void append(const char* s) { out.append(s); }
};

// One text part of a user message's content array (spec 3.4), the text
// wrapped as "<context>\n" + text + "\n</context>" when `context`, written
// in pieces so that the wrapped text is never copied whole.
void append_text_part(os::LargeString& out, std::string_view text, bool context) {
    out += "{\"type\":\"text\",\"text\":";
    if (context) {
        out += "\"<context>\\n";
        UnquotedSink sink{out};
        append_json_string(sink, text);
        out += "\\n</context>\"";
    } else {
        append_json_string(out, text);
    }
    out += '}';
}

// One part of a user message's content array (spec 3.5). The image's
// base64 is encoded straight into the body, so no copy of it exists
// outside the body.
void append_part(os::LargeString& out, const LlmPart& p, bool vision) {
    if (p.kind == LlmPartKind::Text) {
        append_text_part(out, p.text, /*context=*/false);
        return;
    }
    if (!vision) {
        append_text_part(out, kImageOmittedNoVision, /*context=*/false);
        return;
    }
    out += "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:";
    UnquotedSink sink{out};
    append_json_string(sink, p.mime);
    out += ";base64,";
    const size_t old = out.size();
    out.resize(old + base64_encoded_size(p.bytes.size()));
    base64_encode_into(p.bytes, out.data() + old);
    out += "\"}}";
}

// A system, assistant or tool message; a user message goes through
// append_user().
void append_message(os::LargeString& out, const LlmMessage& m, bool echo_reasoning) {
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
    if (echo_reasoning && m.role == LlmRole::Assistant && !m.reasoning.empty()) {
        out += ",\"reasoning_content\":";
        append_json_string(out, m.reasoning);
    }
    out += '}';
}

bool is_movable(const LlmMessage& m) { return m.role == LlmRole::System && m.movable; }

// A user message: the only place that writes one's content, so that every
// kind of part is added here. With no parts and no movable system message
// in `after` or `before` (always so under Anywhere) the content is today's
// plain string. Otherwise it is an array: the movable ones in `after`, then
// those in `before`, as text parts, so that the dynamic info, which follows
// the last user message, is its first part (spec 3.4), then the message's
// own parts (spec 3.5), or its text when it has none.
void append_user(os::LargeString& out, const LlmMessage& m, std::span<const LlmMessage> after,
                 std::span<const LlmMessage> before, bool vision) {
    if (m.parts.empty() && std::none_of(before.begin(), before.end(), is_movable) &&
        std::none_of(after.begin(), after.end(), is_movable)) {
        out += "{\"role\":\"user\",\"content\":";
        append_json_string(out, m.text);
        out += '}';
        return;
    }
    out += "{\"role\":\"user\",\"content\":[";
    for (std::span<const LlmMessage> range : {after, before}) {
        for (const LlmMessage& c : range) {
            if (!is_movable(c)) continue;
            append_text_part(out, c.text, /*context=*/true);
            out += ',';
        }
    }
    if (m.parts.empty()) {
        append_text_part(out, m.text, /*context=*/false);
    } else {
        for (size_t i = 0; i < m.parts.size(); ++i) {
            if (i != 0) out += ',';
            append_part(out, m.parts[i], vision);
        }
    }
    out += "]}";
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
    StreamReader(const LlmCall& call, LlmResult& result, bool keep_reasoning)
        : call_(call), result_(result), keep_reasoning_(keep_reasoning) {}

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
        // Reasoning reaches no listener, so it does not set any_body: a
        // retry after it repeats nothing the caller saw. Past the cap the
        // whole of it is dropped for this reply: a cut block is not echoed.
        if (const std::string* r = string_field(delta, "reasoning_content");
            r != nullptr && keep_reasoning_) {
            if (result_.reasoning.size() + r->size() > LlmClient::kMaxReasoningBytes) {
                os::LargeString().swap(result_.reasoning);
                keep_reasoning_ = false;
            } else {
                result_.reasoning.append(r->data(), r->size());
            }
        }
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
    bool keep_reasoning_;
    // By stream index, like result_.tool_calls (at most kMaxToolCalls).
    std::vector<Progress> progress_;
    bool done_ = false;
    bool bad_ = false;
    bool finished_ = false;
};

// The provider's own words about a failed request, read from at most
// kErrorBodyBytes of its body.
struct ProviderError {
    std::string code;
    bool context_length = false;
};

bool code_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.' || c == '-';
}

bool contains_folded(std::string_view text, std::string_view lower_needle) {
    if (lower_needle.size() > text.size()) return false;
    for (size_t i = 0; i + lower_needle.size() <= text.size(); ++i) {
        size_t j = 0;
        while (j < lower_needle.size() &&
               std::tolower(static_cast<unsigned char>(text[i + j])) == lower_needle[j])
            ++j;
        if (j == lower_needle.size()) return true;
    }
    return false;
}

// {"error":{"code","type","message"}} as OpenAI and most gateways send it.
// The body may quote the key (a 401's often does), whole or a piece of
// it, so only a code that is a plain identifier holding neither the key
// nor a piece of it long enough to matter comes out of here.
ProviderError read_provider_error(std::string_view body, int status, std::string_view key) {
    ProviderError out;
    auto parsed = from_json(body, kMaxEventDepth);
    const Value* error = parsed ? field(parsed.value(), "error") : nullptr;
    if (error == nullptr) return out;
    const std::string* code = string_field(*error, "code");
    if (code == nullptr) code = string_field(*error, "type");
    const bool holds_key = code != nullptr && detail::holds_secret_piece(*code, key);
    if (code != nullptr && !code->empty() && code->size() <= 64 &&
        std::all_of(code->begin(), code->end(), code_char) && !holds_key)
        out.code = *code;
    if (code != nullptr && *code == "context_length_exceeded") {
        out.context_length = true;
    } else if (const std::string* message = string_field(*error, "message");
               message != nullptr && (status == 400 || status == 413)) {
        out.context_length = contains_folded(*message, "context length") ||
                             contains_folded(*message, "maximum context") ||
                             contains_folded(*message, "too many tokens");
    }
    return out;
}

// A Retry-After in delta-seconds; nullopt for an HTTP-date or anything
// else.
std::optional<uint64_t> retry_after_ms(const HttpClientResponse& res) {
    for (const auto& [name, value] : res.headers) {
        if (name != "retry-after") continue;
        uint32_t seconds = 0;
        const char* end = value.data() + value.size();
        // from_chars also refuses a sign, blanks and overflow.
        auto [ptr, ec] = std::from_chars(value.data(), end, seconds);
        if (value.empty() || ec != std::errc() || ptr != end) return std::nullopt;
        return uint64_t{seconds} * 1000;
    }
    return std::nullopt;
}

}  // namespace

os::LargeString build_chat_body(std::string_view model, std::span<const LlmMessage> msgs,
                                std::string_view tools_json, const LlmDialect& dialect,
                                bool vision) {
    const bool echo = dialect.reasoning == core::LlmReasoning::Echo;
    // Under First each movable system message goes into the first user
    // message after it, else the last one before it: so a user message takes
    // those since the previous user message, and the last one also those
    // after it. Without a user message nothing moves.
    constexpr size_t kNone = SIZE_MAX;
    size_t last_user = kNone;
    if (dialect.system_msgs == core::LlmSystemMsgs::First) {
        for (size_t i = msgs.size(); i-- > 0;) {
            if (msgs[i].role == LlmRole::User) {
                last_user = i;
                break;
            }
        }
    }
    const bool merge = last_user != kNone;

    size_t estimate = 128 + model.size() + tools_json.size();
    for (const LlmMessage& m : msgs) {
        estimate += 48 + m.text.size() + m.tool_call_id.size() + m.reasoning.size();
        if (m.movable) estimate += 40;
        if (m.role == LlmRole::User) {
            for (const LlmPart& p : m.parts)
                estimate += p.kind == LlmPartKind::Text
                                ? 48 + p.text.size()
                                : 64 + p.mime.size() + base64_encoded_size(p.bytes.size());
        }
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
    size_t since_user = 0;  // the message after the previous user message
    for (size_t i = 0; i < msgs.size(); ++i) {
        const LlmMessage& m = msgs[i];
        if (merge && is_movable(m)) continue;
        if (!first) out += ',';
        first = false;
        if (m.role == LlmRole::User) {
            std::span<const LlmMessage> before, after;
            if (merge) {
                before = msgs.subspan(since_user, i - since_user);
                if (i == last_user) after = msgs.subspan(i + 1);
                since_user = i + 1;
            }
            append_user(out, m, after, before, vision);
            continue;
        }
        append_message(out, m, echo);
    }
    out += ']';
    if (!tools_json.empty()) {
        out += ",\"tools\":";
        out.append(tools_json.data(), tools_json.size());
        if (dialect.parallel_tools)
            out += *dialect.parallel_tools ? ",\"parallel_tool_calls\":true"
                                           : ",\"parallel_tool_calls\":false";
    }
    if (dialect.out_tokens) {
        out += dialect.out_field == core::LlmOutField::MaxCompletionTokens
                   ? ",\"max_completion_tokens\":"
                   : ",\"max_tokens\":";
        char digits[12];
        // Ten digits always fit a uint32_t.
        const auto res = std::to_chars(digits, digits + sizeof(digits), *dialect.out_tokens);
        out.append(digits, static_cast<size_t>(res.ptr - digits));
    }
    out += '}';
    return out;
}

LlmClient::LlmClient(HttpClient& http, core::ModelProfileService& models,
                     std::function<void(uint32_t)> sleep_ms)
    : http_(http), models_(models), sleep_ms_(std::move(sleep_ms)) {}

std::optional<LlmModelInfo> LlmClient::active_model() const {
    const core::ProfilesState st = models_.get();
    const std::string& id = st.active_for(core::ModelKind::Llm);
    for (const core::ModelProfile& p : st.profiles) {
        if (p.id == id)
            return LlmModelInfo{.profile_id = p.id,
                                .model = p.model,
                                .context_window = core::profile_context_window(p.options),
                                .vision = core::profile_vision(p.options),
                                .dialect = core::resolve_llm_dialect(p.options, p.preset)};
    }
    return std::nullopt;
}

LlmResult LlmClient::complete(const LlmCall& call) {
    LlmResult result;
    const uint64_t start = os::now_ms();
    std::optional<core::ResolvedProfile> profile = models_.active_for_request(core::ModelKind::Llm);
    if (!profile) {
        result.error = LlmError::NoModel;
        WLOG_W(kTag, "no active llm profile");
        return result;
    }
    result.model = profile->model;
    result.profile_id = profile->id;

    const LlmDialect dialect = core::resolve_llm_dialect(profile->options, profile->preset);
    const bool keep_reasoning = dialect.reasoning == core::LlmReasoning::Echo;
    const os::LargeString body = build_chat_body(profile->model, call.messages, call.tools_json,
                                                 dialect, core::profile_vision(profile->options));
    HttpClientRequest req;
    req.method = Method::Post;
    // ModelProfileService strips trailing '/' from base_url.
    req.url = profile->base_url + "/chat/completions";
    req.headers.emplace_back("Content-Type", "application/json");
    req.headers.emplace_back("Accept", "text/event-stream");
    if (!profile->api_key.empty())
        req.headers.emplace_back("Authorization", "Bearer " + profile->api_key);
    // Kept only to scrub error codes; never logged or returned.
    const std::string key = std::move(profile->api_key);
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
            result.error = LlmError::Cancelled;
            result.transport = TransportError::Cancelled;
            break;
        }
        const uint32_t left = left_ms();
        if (left == 0) {
            result.error = LlmError::Transport;
            result.transport = TransportError::Timeout;
            break;
        }

        int status = 0;
        SseParser parser;
        StreamReader reader(call, result, keep_reasoning);
        // A retried attempt streams its reasoning again from the start.
        result.reasoning.clear();
        // A failed request's body may quote the key: it stays in here, is
        // never logged, and only read_provider_error()'s scrubbed code
        // leaves this attempt.
        os::LargeString error_body;
        result.code.clear();
        req.timeout_ms = left;
        req.on_status = [&status](int s) { status = s; };
        req.on_body = [&](std::span<const uint8_t> bytes) {
            if (call.on_bytes) call.on_bytes();
            if (status < 200 || status >= 300) {
                const size_t room = kErrorBodyBytes - error_body.size();
                error_body.append(reinterpret_cast<const char*>(bytes.data()),
                                  std::min(room, bytes.size()));
                return true;
            }
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
            result.error = LlmError::BadStream;
            result.transport = TransportError::None;
            break;
        }
        if (reader.done()) {
            // Reading stopped at [DONE] on purpose.
            result.transport = TransportError::None;
            break;
        }
        // 429, 5xx and transport failures share one budget of retries.
        const bool may_retry = retry < std::size(kRetryDelaysMs);
        uint32_t delay = may_retry ? kRetryDelaysMs[retry] : 0;
        if (res.transport != TransportError::None) {
            if (res.transport == TransportError::Cancelled) {
                result.error = LlmError::Cancelled;
                break;
            }
            result.error = LlmError::Transport;
            // Once part of the reply reached the caller a retry would
            // repeat it; before that, nothing is lost by asking again.
            if (result.any_body) break;
        } else {
            const int s = result.status;
            if (s >= 200 && s < 300) {
                if (!reader.complete()) result.error = LlmError::BadStream;
                break;
            }
            const ProviderError provider =
                read_provider_error({error_body.data(), error_body.size()}, s, key);
            result.code = provider.code;
            if (s == 401) {
                result.error = LlmError::Unauthorized;
                break;
            }
            if (provider.context_length) {
                result.error = LlmError::ContextLength;
                break;
            }
            if (s != 429 && s < 500) {
                result.error = LlmError::Http;
                break;
            }
            result.error = s == 429 ? LlmError::RateLimited : LlmError::Server;
            if (std::optional<uint64_t> after = retry_after_ms(res); after && may_retry) {
                delay = static_cast<uint32_t>(
                    std::min<uint64_t>({*after, kMaxRetryAfterMs, left_ms()}));
            }
        }
        if (!may_retry || delay >= left_ms()) break;
        result.error = LlmError::None;
        result.transport = TransportError::None;
        for (uint32_t wait = delay; wait > 0 && !stopped();) {
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
    if (result.error == LlmError::None) {
        WLOG_I(kTag, "model=%s status=%d ms=%u tokens=%u", result.model.c_str(), result.status, ms,
               tokens);
    } else if (result.code.empty()) {
        WLOG_W(kTag, "model=%s status=%d ms=%u tokens=%u error=%s", result.model.c_str(),
               result.status, ms, tokens, to_string(result.error));
    } else {
        // The code is read_provider_error()'s: scrubbed of the key.
        WLOG_W(kTag, "model=%s status=%d ms=%u tokens=%u error=%s code=%s", result.model.c_str(),
               result.status, ms, tokens, to_string(result.error), result.code.c_str());
    }
    return result;
}

}  // namespace wisp::net
