#pragma once

// LlmClient (agent-core spec 3.6): one streaming OpenAI chat-completions
// request, POST <base_url>/chat/completions with "stream":true, read as
// server-sent events through HttpClient::on_body.
//
// The profile, API key included, comes from ModelProfileService::
// active_for_request(Llm) on every call, and the key goes into the
// Authorization header only: never into the result, a log line or an
// error. The body of a failed request (a 401's may quote the key) is read
// up to kErrorBodyBytes for its error code, never logged or kept: only
// LlmResult::code, an identifier with the key scrubbed, comes out of it.
// The only log line per call is metadata under the "llm" tag:
// "model=<m> status=<s> ms=<t> tokens=<u>", plus " error=<error>" (the
// LlmError's to_string() word) when it failed and then " code=<code>" when the provider sent one
// that passed the scrub (LlmResult::code: dropped when it holds the key or any 8 or more bytes of
// it). Reasoning text is never logged either.
//
// Dialect (spec 3.2): each call resolves the profile's options against its
// preset's defaults (core::resolve_llm_dialect()) and builds the body with
// that dialect; a profile without options on a preset without defaults
// sends exactly the plain body. reasoning_content deltas are kept in
// LlmResult::reasoning only under LlmReasoning::Echo; they never count as
// part of the reply (any_body), so a retry loses nothing, and each attempt
// starts them afresh.
//
// Retries: a 429, a 5xx and a transport error that came before any of
// the reply (any_body false) share one budget of two retries,
// kRetryDelaysMs apart; a 429/5xx's Retry-After in whole seconds
// replaces the delay (capped at kMaxRetryAfterMs). Waiting ends early
// when `cancelled` says so, and no retry starts that the call's timeout
// could not wait for. A 401 is LlmError::Unauthorized, a refusal of the
// prompt's size ContextLength, any other non-2xx Http; none of those is
// retried, nor is a 2xx, a cancellation or a transport error once part
// of the reply has arrived.
//
// Memory: the request body, the reply text and tool arguments live in
// os::LargeString (PSRAM on the device); each event is parsed on its own.
//
// active_model() reads ModelProfileService::get(), as the agent's window
// and calibration need it, without the key.
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
#include "wisp/net/llm_provider.hpp"
#include "wisp/net/request_watch.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::net {

// The request body: {"model","stream":true,"stream_options":
// {"include_usage":true},"messages":[...],"tools":<tools_json>} (tools left
// out when tools_json is empty). Strings are escaped as to_json() does, so
// the body is valid UTF-8 JSON whatever the input.
//
// The dialect adds, after "tools" so that LlmDialect{} gives the plain
// body: "parallel_tool_calls" (only with tools), then "max_tokens" or
// "max_completion_tokens". Under Echo an assistant message with reasoning
// ends with "reasoning_content". Under system_msgs First each movable
// system message goes into the first user message after it, else the last
// one before it (none: it stays a system message); a user message that
// receives any becomes a content array of text parts: the movable messages
// after it, then those before it, each as "<context>\n...\n</context>",
// then its own text (spec 3.4).
//
// A user message with parts is a content array of them, in order (spec
// 3.5): a text part as {"type":"text","text":...}, an image as
// {"type":"image_url","image_url":{"url":"data:<mime>;base64,..."}}, the
// base64 written straight into the body. Without `vision` each image is
// the text part kImageOmittedNoVision instead. Under First the
// "<context>" parts come first, then the message's parts. A message
// without parts, and parts on any other role, are written as before.
os::LargeString build_chat_body(std::string_view model, std::span<const LlmMessage> msgs,
                                std::string_view tools_json, const LlmDialect& dialect = {},
                                bool vision = false);

// Splits a server-sent event stream into events. Lines end with LF, CRLF
// or CR; ':' lines are comments; only the "data" field is kept, one space
// after the colon dropped. An event whose data would pass the parser's
// limit (kMaxEventBytes unless given) sets overflowed() and the parser
// ignores everything after it. Its buffers grow in PSRAM (os::LargeString).
class SseParser {
public:
    static constexpr size_t kMaxEventBytes = 256 * 1024;

    explicit SseParser(size_t max_event_bytes = kMaxEventBytes)
        : max_event_bytes_(max_event_bytes) {}

    // Calls on_data with each event's data (multi-line data joined by '\n').
    void feed(std::span<const uint8_t> bytes, const std::function<void(std::string_view)>& on_data);
    bool overflowed() const { return overflowed_; }

private:
    void end_line(const std::function<void(std::string_view)>& on_data);
    size_t pending_data_bytes() const;

    size_t max_event_bytes_;
    os::LargeString line_;
    os::LargeString data_;
    bool has_data_ = false;
    bool after_cr_ = false;
    bool overflowed_ = false;
};

class LlmClient final : public LlmProvider {
public:
    static constexpr uint32_t kRetryDelaysMs[2] = {1000, 4000};
    // The longest Retry-After wait honoured.
    static constexpr uint32_t kMaxRetryAfterMs = 30000;
    // How much of a failed request's body is read for its error code.
    static constexpr size_t kErrorBodyBytes = 2048;
    // How long a retry wait sleeps before asking `cancelled` again.
    static constexpr uint32_t kWaitSliceMs = 100;
    // The response cap handed to the HTTP client: a long reply streams
    // far more SSE framing than its text, past the clients' defaults.
    static constexpr size_t kMaxStreamBytes = 16 * 1024 * 1024;
    // Highest tool-call index accepted from the stream.
    static constexpr size_t kMaxToolCalls = 64;
    // LlmCall::on_tool reports a call's growing arguments at most this
    // often.
    static constexpr uint32_t kToolProgressMs = 500;
    // One reply's reasoning past this is dropped whole: a cut reasoning
    // block is not echoed.
    static constexpr size_t kMaxReasoningBytes = 64 * 1024;

    LlmClient(HttpClient& http, core::ModelProfileService& models,
              std::function<void(uint32_t)> sleep_ms = os::sleep_ms);

    std::optional<LlmModelInfo> active_model() const override;
    LlmResult complete(const LlmCall& call) override;

private:
    HttpClient& http_;
    core::ModelProfileService& models_;
    std::function<void(uint32_t)> sleep_ms_;
};

}  // namespace wisp::net
