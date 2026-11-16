#pragma once

// Context assembly and compression (agent-core spec 3.2-3.3, MVP spec 3.3).
//
// A model request is laid out from the most stable part to the least, so
// the provider's prefix cache keeps hitting:
//   1. system_prompt(): the fixed part, byte for byte the same for the same
//      language and screen shape (no time, battery, memory or source);
//   2. the history: user and assistant messages, tool calls and results,
//      with summaries in place of the turns they cover (a carry, the
//      summary a rollover kept, is one too, at the start); tool results of
//      earlier turns are cut to kToolResultKeepBytes, those of the turn in
//      progress to kLiveToolResultBytes;
//   3. dynamic_info(): one last system message, rebuilt for every request
//      and never stored -- memory notes first (they change least), then the
//      local time and weekday, the message's source, scenes, battery and
//      network.
// The tool definitions travel beside the messages and do not change within
// a conversation.
//
// Every system message but the fixed prompt is movable (LlmMessage::movable,
// spec 3.4): the summaries, the carry and the dynamic info. They are marked by origin,
// not position. Under the dialect's system_msgs=first the body builder
// moves each into the adjacent user message as a <context> text part, and
// system_prompt() words its references to them for that; under anywhere
// the mark changes nothing.
//
// Everything here is pure: what it needs from the device arrives in a
// DeviceSnapshot gathered from cached values (no flash, no other thread).
//
// Tokens are estimated as bytes / 3. Past kCompressPercent of the model's
// context window the oldest whole turns are summarized (by one extra model
// request without tools) until the rest fits in kCompressTargetPercent;
// the last kKeepRecentTurns turns are never touched (the caller may go
// down to the running turn alone when that is not enough). The summary is
// stored as a summary message covering [first_seq, last_seq]; the
// originals stay in the conversation file.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wisp/agent/conversation.hpp"
#include "wisp/agent/limits.hpp"
#include "wisp/core/model_profiles.hpp"
#include "wisp/core/settings_schema.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/net/llm_provider.hpp"
#include "wisp/os/large.hpp"

namespace wisp::agent {

// What the agent knows about the device when it builds a request: the
// values a context source gathers without touching flash (cached state
// only), since it runs on the wisp.agent task.
struct DeviceSnapshot {
    std::string lang{core::setting_default_text(core::SettingId::Language)};  // "zh" | "ja" | "en"
    hal::ScreenShape shape;
    std::string foreground;  // "" none
    std::vector<std::string> background;
    int battery = -1;  // percent; -1 unknown
    bool charging = false;
    std::string wifi;  // "" unknown
    int64_t wall_s = 0;
    int utc_offset_min = core::setting_default_int(core::SettingId::UtcOffsetMin);
    std::vector<std::string> memory;  // "[<id>] <text>"
    int weekday = 0;                  // 1 = Monday; 0 while the clock is unset
    Source source = Source::Web;      // the running turn's
};

// Called on the wisp.agent task once per request, with the running turn's
// source.
using ContextSource = std::function<DeviceSnapshot(Source)>;

// The local weekday of `wall_s` (1 = Monday ... 7 = Sunday), for a context
// source to fill DeviceSnapshot::weekday with once the clock is set.
int weekday_of(int64_t wall_s, int utc_offset_min);

// The fixed part: the same bytes for the same lang, shape and system_msgs.
// Under First it names the <context> block where Anywhere names the last
// system message, and says that <context> blocks come from the device.
std::string system_prompt(const DeviceSnapshot& d,
                          core::LlmSystemMsgs system_msgs = core::LlmSystemMsgs::Anywhere);
// The dynamic part; its first line is the first memory note, if any.
std::string dynamic_info(const DeviceSnapshot& d);

struct BuiltRequest {
    BuiltRequest() = default;
    // `messages` points into the members below: copying would leave the
    // copy's views on the original. A move keeps every heap buffer (and
    // the arena is always on the heap), so the views stay valid.
    BuiltRequest(const BuiltRequest&) = delete;
    BuiltRequest& operator=(const BuiltRequest&) = delete;
    BuiltRequest(BuiltRequest&&) = default;
    BuiltRequest& operator=(BuiltRequest&&) = default;

    os::LargeVector<net::LlmMessage> messages;  // views into `arena`, `calls` and the history
    os::LargeString arena;
    // Each assistant message's calls, with wire names.
    os::LargeVector<os::LargeVector<net::LlmToolCall>> calls;
    size_t est_tokens = 0;    // everything in `messages`, bytes / 3
    size_t fixed_tokens = 0;  // the system prompt and the dynamic info alone
};

// A kept reasoning text for the assistant message `seq` (spec 3.3); a view.
struct ReasoningRef {
    uint64_t seq = 0;
    std::string_view text{};
};

struct BuildOptions {
    // The running turn's tool results are cut to this. A request that
    // would not fit the window otherwise lowers it to kToolResultKeepBytes,
    // and the cut note asks the model to read in smaller pieces.
    size_t live_tool_bytes = kLiveToolResultBytes;
    // The prompt's wording (system_prompt()).
    core::LlmSystemMsgs system_msgs = core::LlmSystemMsgs::Anywhere;
    // Attached to the assistant messages by seq and counted in the
    // estimate. Sorted by seq; must outlive the result.
    std::span<const ReasoningRef> reasoning{};
};

// system_prompt, the history, then dynamic_info last. The history must
// outlive the result.
BuiltRequest build_request(std::span<const Message> history, const DeviceSnapshot& d,
                           BuildOptions opts = {});
// The history part alone, for a service without a context source.
BuiltRequest build_history(std::span<const Message> history, BuildOptions opts = {});

// est_tokens * 100 > window * kCompressPercent.
bool needs_compression(size_t est_tokens, uint32_t window);

// The oldest whole turns to summarize, as (first_seq, last_seq): picked
// from the oldest on until what is left (plus `fixed_tokens`, the part of
// the request that is not history) is at most kCompressTargetPercent of
// the window. Never the last `keep_recent` turns (at least the running
// one is always kept). An earlier summary counts as one turn and may be
// folded into the new one. nullopt when nothing can be picked.
std::optional<std::pair<uint64_t, uint64_t>> pick_compression(
    std::span<const Message> history, uint32_t window, size_t fixed_tokens = 0,
    uint32_t keep_recent = kKeepRecentTurns);

// The summary request for history[first_seq..last_seq]: a fixed system
// prompt and the covered part as one plain-text transcript (no tool
// messages, so it needs no tool definitions). The history must outlive
// the result.
BuiltRequest build_summary_request(std::span<const Message> history, uint64_t first_seq,
                                   uint64_t last_seq);

// The rollover's carry request (spec 3.8): kCarryPrompt, then one
// transcript in build_summary_request()'s line format: every live summary
// unit, then the newest whole turns while the transcript's bytes / 3 stay
// within budget_tokens (it stops at the first turn that does not fit). No
// messages when the transcript is empty. The history must outlive the
// result.
BuiltRequest build_carry_request(std::span<const Message> history, size_t budget_tokens);

}  // namespace wisp::agent
