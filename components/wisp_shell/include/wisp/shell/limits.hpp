#pragma once

// Shell constants (MVP spec 6). Sizes are percentages of the safe area's
// shorter side or pixels, durations milliseconds.

#include <cstddef>
#include <cstdint>

#include "wisp/ui/drag.hpp"

namespace wisp::shell {

// The wisp on the main screen: idle, and during a turn (spec 6.2).
inline constexpr int kWispIdlePercent = 55;
// On a round screen the safe area is the inscribed square, about 70% of
// the diameter, so the same share looked small inside the black ring
// around it; there the idle wisp takes this share of the safe area.
inline constexpr int kWispIdleCirclePercent = 66;
// No larger during a turn: at 32 the 360 px circle's column no longer
// holds kMinColumnPx (home.hpp) below the wisp.
inline constexpr int kWispTurnPercent = 28;
// Idle, the wisp's centre sits this far down the safe area: a little
// above its middle, leaving the hint below it room.
inline constexpr int kWispIdleCentrePercent = 42;
// The <say> text below the wisp, and the heard text above it.
inline constexpr int kSayFontPx = 28;
inline constexpr int kHeardMaxLines = 2;
// The turn's text fades this long after its last interaction.
inline constexpr uint32_t kFadeAfterMs = 15000;
// A swipe (spec 6.3; the talk key's cancel, the wisp's hold): this far,
// and this many times more along its main direction than across it.
inline constexpr int kSwipeMinPx = 40;
inline constexpr int kSwipeDominance = 2;
// A page drag (spec 6.3): it starts once the finger is kDragSlopPx along
// an axis (LVGL's own scroll limit, so the two decide on the same touch
// read). On release it completes and slides like the pull-down panel
// (wisp/ui/drag.hpp, whose constants these are).
inline constexpr int kDragSlopPx = 10;
inline constexpr int kDragCompleteDivisor = ui::kDragCompleteDivisor;
inline constexpr int kFlingPxPerS = ui::kFlingPxPerS;
inline constexpr uint32_t kFlingWindowMs = ui::kFlingWindowMs;
inline constexpr uint32_t kSlideMs = ui::kSlideMs;
// Conversation history pages load this many turns at a time (spec 6.4).
inline constexpr size_t kHistoryBatch = 50;
// The in-scene overlay shows the say text this long (spec 6.5).
inline constexpr uint32_t kOverlaySayMs = 5000;
// The wisp looks upset this long after an error.
inline constexpr uint32_t kErrorMoodMs = 2000;

}  // namespace wisp::shell
