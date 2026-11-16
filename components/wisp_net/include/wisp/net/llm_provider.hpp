#pragma once

// The seam between the agent and a chat model (spec 3.1): the message,
// call and result types of one request, and LlmProvider, which answers
// it. LlmClient (wisp/net/llm_client.hpp) is the OpenAI chat-completions
// implementation.
//
// LlmMessage and LlmModelInfo stay plain aggregates whose every member
// has a default, so that fields added later leave existing designated
// initializers untouched.

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

namespace wisp::net {

// How a profile's requests are shaped (spec 3.2); LlmDialect{} is the
// plain chat-completions body.
using LlmDialect = core::LlmDialect;

// Why a request failed. ContextLength: the provider refused the prompt
// as too long for the model (error.code context_length_exceeded, or a
// 400/413 whose message says so).
enum class LlmError : uint8_t {
    None,
    NoModel,
    Unauthorized,
    RateLimited,
    Server,
    Http,
    Transport,
    Cancelled,
    BadStream,
    ContextLength
};

// The words the "llm" and "agent" log lines print: "" for None,
// "no_model", "unauthorized", "rate_limited", "server", "http",
// "transport", "cancelled", "bad_stream", "context_length".
const char* to_string(LlmError e);

enum class LlmRole : uint8_t { System, User, Assistant, Tool };

struct LlmToolCall {
    std::string id{};
    std::string name{};
    os::LargeString arguments{};  // the JSON text as the model wrote it
};

enum class LlmPartKind : uint8_t { Text, Image };

// One piece of a message's content. Views only, like LlmMessage.
struct LlmPart {
    LlmPartKind kind = LlmPartKind::Text;
    std::string_view text{};           // Text
    std::string_view mime{};           // Image: "image/jpeg" | "image/png"
    std::span<const uint8_t> bytes{};  // Image: the encoded file, not base64
};

// The text part an image becomes when the profile has no vision (spec 3.5).
inline constexpr std::string_view kImageOmittedNoVision =
    "[image omitted: the active model does not take images]";

// Views only: everything must outlive complete().
struct LlmMessage {
    LlmRole role = LlmRole::User;
    std::string_view text{};
    // User only (spec 3.5): when set, the content is these parts and text
    // is unused. Ignored on any other role, so a tool message never
    // carries an image.
    std::span<const LlmPart> parts{};
    std::span<const LlmToolCall> tool_calls{};  // Assistant only
    std::string_view tool_call_id{};            // Tool only
    // Assistant only: the reply's reasoning, sent only under
    // LlmReasoning::Echo.
    std::string_view reasoning{};
    // System only: content the dialect may move into a user message
    // (spec 3.4); the fixed prompt never is.
    bool movable = false;
};

enum class LlmFinish : uint8_t { Stop, ToolCalls, Length, Other };

struct LlmUsage {
    uint32_t prompt = 0, completion = 0, total = 0;
};

struct LlmResult {
    LlmError error = LlmError::None;
    TransportError transport = TransportError::None;
    int status = 0;  // last attempt; 0 without a response
    os::LargeString text{};
    std::vector<LlmToolCall> tool_calls{};  // by stream index
    LlmFinish finish = LlmFinish::Other;
    std::optional<LlmUsage> usage{};
    uint32_t attempts = 0;
    bool any_body = false;  // a delta or tool fragment arrived
    std::string model{};    // the profile's model name
    // The profile that served the call ("" when none), so that what is
    // learned from the reply is kept under the profile that answered,
    // even when the active one changed during the call.
    std::string profile_id{};
    // The provider's error.code (else error.type) from a failed
    // attempt's body: at most 64 bytes of [A-Za-z0-9_.-], "" when absent,
    // malformed or holding the API key or any 8 or more bytes of it.
    std::string code{};
    // The reply's reasoning_content, kept only under LlmReasoning::Echo
    // and only while it stays within LlmClient::kMaxReasoningBytes.
    os::LargeString reasoning{};
};

// A tool call the model is still writing (LlmCall::on_tool).
struct LlmToolProgress {
    size_t index = 0;         // the call's stream index
    std::string_view name{};  // the wire name, as the model wrote it
    size_t arg_bytes = 0;     // the arguments' bytes received so far
};

struct LlmCall {
    std::span<const LlmMessage> messages{};
    std::string_view tools_json{};  // a JSON array, "" for none
    // For the whole call, retries and their waits included.
    uint32_t timeout_ms = 300000;
    // Armed by complete() for each attempt; the caller aborts it with the
    // ticket on_armed() last reported (RequestWatch::abort).
    RequestWatch* watch = nullptr;
    std::function<void(uint32_t ticket)> on_armed{};
    std::function<void(std::string_view)> on_text{};
    std::function<void()> on_bytes{};   // any body bytes (stall detection)
    std::function<bool()> cancelled{};  // also stops the retry waits
    // A tool call while it streams: once as soon as its name has arrived,
    // then as its arguments grow, at most once per kToolProgressMs for
    // each call. Runs on the calling thread, from inside the stream.
    std::function<void(const LlmToolProgress&)> on_tool{};
    // on_tool's clock; empty: os::now_ms.
    std::function<uint64_t()> now_ms{};
};

// The model the next complete() will use.
struct LlmModelInfo {
    std::string profile_id{};
    std::string model{};
    uint32_t context_window = 0;
    bool vision = false;
    LlmDialect dialect{};

    bool operator==(const LlmModelInfo&) const = default;
};

class LlmProvider {
public:
    virtual ~LlmProvider() = default;

    // The active profile as the next complete() will use it; nullopt when
    // there is none. Memory only (no flash, no network); any thread.
    virtual std::optional<LlmModelInfo> active_model() const = 0;

    // One request, retries included. Blocks; one call at a time per
    // provider object; callbacks run on the calling thread from inside
    // the stream. wisp.agent only.
    virtual LlmResult complete(const LlmCall& call) = 0;
};

}  // namespace wisp::net
