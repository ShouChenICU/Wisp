#pragma once

// The agent's fixed limits (agent-core spec 3-6). Tests refer
// to these constants instead of repeating the numbers.

#include <cstddef>
#include <cstdint>

namespace wisp::agent {

// A turn: at most this many tool calls, and this long in all.
inline constexpr uint32_t kMaxSteps = 24;
inline constexpr uint32_t kTurnMs = 600000;
// A model stream with no new bytes for this long is aborted.
inline constexpr uint32_t kStallMs = 60000;
// A user message, counted in Unicode code points.
inline constexpr size_t kMaxUserChars = 4000;
// Messages waiting behind the running turn; more are refused as busy.
inline constexpr size_t kMaxQueued = 1;

// Context: every tool result in the history is cut to this many bytes.
inline constexpr size_t kToolResultKeepBytes = 4096;
inline constexpr uint32_t kDefaultContextWindow = 65536;
// Compress past this share of the window, down to the target share,
// never touching the most recent turns.
inline constexpr uint32_t kCompressPercent = 75;
inline constexpr uint32_t kCompressTargetPercent = 40;
inline constexpr uint32_t kKeepRecentTurns = 2;

// An unanswered confirmation dialog counts as a refusal after this long.
inline constexpr uint32_t kConfirmMs = 30000;

// The settle wait and ui.tree's size (kSettleQuietMs, kSettleMaxMs,
// kTreeMaxChars, kTreeKeepEnds) belong to the scene layer that measures
// them: wisp/scene/ui_inspector.hpp.

// WS tool summaries: args and result cut to this many characters.
inline constexpr size_t kSummaryChars = 200;

// Conversation files: archives kept, and the size of current.jsonl past
// which the next user message starts a new conversation.
inline constexpr uint32_t kArchivesKept = 5;
inline constexpr uint64_t kRolloverBytes = 1024 * 1024;

// Tool limits.
inline constexpr size_t kReadMaxBytes = 65536;
inline constexpr size_t kDataReadMaxBytes = 16384;
inline constexpr size_t kLogsMax = 100;
inline constexpr uint32_t kEvalMs = 200;
// The install check reports at most this many W and E log lines each.
inline constexpr size_t kCheckLogsPerLevel = 20;

}  // namespace wisp::agent
