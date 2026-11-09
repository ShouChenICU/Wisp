#pragma once

// The shell's touch gestures (MVP spec 6.3), without LVGL objects: what a
// press's start and end points make, where a drag leads from each page,
// how a page drag follows the finger, and what a press on the wisp does.
// The pages feed these from their screens' PRESSED / PRESSING / RELEASED
// events (wisp/shell/home.hpp, wisp/shell/shell.hpp).
//
// A swipe (what the talk key's cancel looks for) moved more than
// kSwipeMinPx along its main direction and at least kSwipeDominance times
// as far along it as across it. Screen coordinates: y grows downwards, so
// a finger moving up is Up.

#include <cstdint>
#include <optional>

#include "lvgl.h"
#include "wisp/shell/shell.hpp"
#include "wisp/ui/drag.hpp"

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

// A page drag (spec 6.3), without LVGL objects: where the finger is along
// the drag's direction and whether the release completes the navigation.
// The caller feeds every point of a press; on the move that first takes
// the finger kDragSlopPx from where it went down along either axis,
// moved() reports the direction it went along the axis it moved more on
// (once a press). The caller then start()s the drag or leaves the press
// alone. From start() on the off-axis motion is ignored and offset() is
// the distance along the direction, clamped to [0, extent].
class DragTrack {
public:
    void press(lv_point_t p, uint64_t now);
    // Swipe::None until the slop is passed, and after it.
    Swipe moved(lv_point_t p, uint64_t now);
    void start(Swipe dir, int32_t extent);
    // True from start() to release() or reset().
    bool dragging() const { return dragging_; }
    Swipe dir() const { return dir_; }
    int32_t extent() const { return extent_; }
    int32_t offset() const;
    // Ends the drag: true when it completes (at least extent /
    // kDragCompleteDivisor, or the finger moving on at kFlingPxPerS over
    // the last kFlingWindowMs).
    bool release(lv_point_t p, uint64_t now);
    void reset();

private:
    int32_t along(lv_point_t p) const;
    void record(lv_point_t p, uint64_t now);

    lv_point_t start_{};
    bool pressed_ = false;
    bool decided_ = false;
    bool dragging_ = false;
    Swipe dir_ = Swipe::None;
    int32_t extent_ = 0;
    int32_t along_ = 0;
    ui::FlingTrack fling_;
};

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
