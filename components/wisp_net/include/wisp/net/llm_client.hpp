#pragma once

// LlmClient (agent-core spec 3.6): one streaming OpenAI chat-completions
// request, POST <base_url>/chat/completions with "stream":true, read as
// server-sent events through HttpClient::on_body.
//
// The profile, API key included, comes from ModelProfileService::
// active_for_request(Llm) on every call, and the key goes into the
// Authorization header only: never into the result, a log line or an
// error. Response bodies of failed requests are dropped unread (a 401
// body may quote the key). The only log line per call is metadata under
// the "llm" tag: "model=<m> status=<s> ms=<t> tokens=<u>", plus
// " error=<code>" when it failed.
//
// Retries: a 429 or 5xx is retried at most twice, kRetryDelaysMs apart
// (Retry-After is ignored); waiting ends early when `cancelled` says so.
// A 401 is "unauthorized", any other non-2xx "http", a transport error
// "transport"; none of those is retried. A 2xx is never retried either.
//
// Memory: the request body, the reply text and tool arguments live in
// os::LargeString (PSRAM on the device); each event is parsed on its own.
//
// complete() blocks; one call at a time per client (TLS limits are the
// caller's: see wisp/net/tls_gate.hpp).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/core/model_profiles.hpp"
#include "wisp/net/http.hpp"
#include "wisp/net/request_watch.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::net {

enum class LlmRole : uint8_t { System, User, Assistant, Tool };

struct LlmToolCall {
    std::string id;
    std::string name;
    os::LargeString arguments;  // the JSON text as the model wrote it
};

// Views only: everything must outlive complete().
struct LlmMessage {
    LlmRole role = LlmRole::User;
    std::string_view text;
    std::span<const LlmToolCall> tool_calls;  // Assistant only
    std::string_view tool_call_id;            // Tool only
};

enum class LlmFinish : uint8_t { Stop, ToolCalls, Length, Other };

struct LlmUsage {
    uint32_t prompt = 0, completion = 0, total = 0;
};

struct LlmResult {
    // "" ok; "no_model" "unauthorized" "rate_limited" "server" "http"
    // "transport" "cancelled" "bad_stream"
    std::string error;
    TransportError transport = TransportError::None;
    int status = 0;  // last attempt; 0 without a response
    os::LargeString text;
    std::vector<LlmToolCall> tool_calls;  // by stream index
    LlmFinish finish = LlmFinish::Other;
    std::optional<LlmUsage> usage;
    uint32_t attempts = 0;
    bool any_body = false;  // a delta or tool fragment arrived
    std::string model;      // the profile's model name
};

struct LlmCall {
    std::span<const LlmMessage> messages;
    std::string_view tools_json;  // a JSON array, "" for none
    // For the whole call, retries and their waits included.
    uint32_t timeout_ms = 300000;
    // Armed by complete() for each attempt; the caller aborts it with the
    // ticket on_armed() last reported (RequestWatch::abort).
    RequestWatch* watch = nullptr;
    std::function<void(uint32_t ticket)> on_armed;
    std::function<void(std::string_view)> on_text;
    std::function<void()> on_bytes;   // any body bytes (stall detection)
    std::function<bool()> cancelled;  // also stops the retry waits
};

// The request body: {"model","stream":true,"stream_options":
// {"include_usage":true},"messages":[...],"tools":<tools_json>} (tools left
// out when tools_json is empty). Strings are escaped as to_json() does, so
// the body is valid UTF-8 JSON whatever the input.
os::LargeString build_chat_body(std::string_view model, std::span<const LlmMessage> msgs,
                                std::string_view tools_json);

// Splits a server-sent event stream into events. Lines end with LF, CRLF
// or CR; ':' lines are comments; only the "data" field is kept, one space
// after the colon dropped. An event whose data would pass kMaxEventBytes
// sets overflowed() and the parser ignores everything after it.
class SseParser {
public:
    static constexpr size_t kMaxEventBytes = 256 * 1024;

    // Calls on_data with each event's data (multi-line data joined by '\n').
    void feed(std::span<const uint8_t> bytes, const std::function<void(std::string_view)>& on_data);
    bool overflowed() const { return overflowed_; }

private:
    void end_line(const std::function<void(std::string_view)>& on_data);
    size_t pending_data_bytes() const;

    os::LargeString line_;
    os::LargeString data_;
    bool has_data_ = false;
    bool after_cr_ = false;
    bool overflowed_ = false;
};

class LlmClient {
public:
    static constexpr uint32_t kRetryDelaysMs[2] = {1000, 4000};
    // How long a retry wait sleeps before asking `cancelled` again.
    static constexpr uint32_t kWaitSliceMs = 100;
    // The response cap handed to the HTTP client: a long reply streams
    // far more SSE framing than its text, past the clients' defaults.
    static constexpr size_t kMaxStreamBytes = 16 * 1024 * 1024;
    // Highest tool-call index accepted from the stream.
    static constexpr size_t kMaxToolCalls = 64;

    LlmClient(HttpClient& http, core::ModelProfileService& models,
              std::function<void(uint32_t)> sleep_ms = os::sleep_ms);

    LlmResult complete(const LlmCall& call);

private:
    HttpClient& http_;
    core::ModelProfileService& models_;
    std::function<void(uint32_t)> sleep_ms_;
};

}  // namespace wisp::net
