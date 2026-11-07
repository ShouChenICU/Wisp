#pragma once

// The shell's touch gestures (MVP spec 6.3), without LVGL objects: what a
// press's start and end points make, where a swipe leads from each page,
// and what a press on the wisp does. The pages feed these from their
// root objects' PRESSED / PRESSING / RELEASED events (wisp/shell/home.hpp).
//
// A swipe moved more than kSwipeMinPx along its main direction and at
// least kSwipeDominance times as far along it as across it. Screen
// coordinates: y grows downwards, so a finger moving up is Up.

#include <cstdint>
#include <optional>

#include "lvgl.h"
#include "wisp/shell/shell.hpp"

namespace wisp::shell {

enum class Swipe : uint8_t { None, Left, Right, Up, Down };
// "none" "left" "right" "up" "down"
const char* to_string(Swipe s);

Swipe classify_swipe(lv_point_t start, lv_point_t end);

// Where swipe `s` leads from page `from`, if anywhere. On the home screen
// Right opens Settings, Left the scene list and Down the history; a Down
// that started in the top zone (SystemLayer::in_top_zone()) is the
// pull-down panel's and leads nowhere. On a sub-page the swipe opposite
// to the one that opened it goes home; for the history, which scrolls
// vertically, Up does so only once its list is at its bottom end.
std::optional<Page> navigate(Page from, Swipe s, bool from_top_zone, bool at_bottom_end);

// A press on the wisp (spec 6.3): held kHoldStartMs (wisp/voice/limits.hpp)
// without moving more than kSwipeMinPx it is hold-to-talk; let go sooner
// it is a tap. While holding, a move up by more than kSwipeMinPx cancels
// the hold. A press that moved too far before the hold began is a swipe
// and does nothing here. Times are milliseconds on any one clock.
class WispPress {
public:
    enum class Action : uint8_t { None, Tap, HoldBegin, HoldEnd, HoldCancel };

    Action pressed(lv_point_t p, uint64_t now);
    Action moved(lv_point_t p, uint64_t now);
    Action tick(uint64_t now);
    Action released(lv_point_t p, uint64_t now);
    // The press was taken away (LV_EVENT_PRESS_LOST): HoldCancel while
    // holding.
    Action lost();

    // Between HoldBegin and HoldEnd / HoldCancel.
    bool holding() const { return state_ == State::Holding; }
    // The current or last press reached HoldBegin.
    bool held() const { return held_; }

private:
    enum class State : uint8_t { Idle, Pressed, Holding, Moved, Done };

    State state_ = State::Idle;
    lv_point_t start_{};
    uint64_t start_ms_ = 0;
    bool held_ = false;
};

}  // namespace wisp::shell
