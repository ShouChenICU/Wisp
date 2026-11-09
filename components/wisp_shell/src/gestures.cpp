#include "wisp/shell/gestures.hpp"

#include <algorithm>
#include <cstdlib>

#include "wisp/shell/limits.hpp"
#include "wisp/voice/limits.hpp"

namespace wisp::shell {

const char* to_string(Swipe s) {
    switch (s) {
        case Swipe::None:
            return "none";
        case Swipe::Left:
            return "left";
        case Swipe::Right:
            return "right";
        case Swipe::Up:
            return "up";
        case Swipe::Down:
            return "down";
    }
    return "none";
}

Swipe classify_swipe(lv_point_t start, lv_point_t end) {
    const int32_t dx = end.x - start.x;
    const int32_t dy = end.y - start.y;
    const int32_t ax = std::abs(dx);
    const int32_t ay = std::abs(dy);
    if (ax > kSwipeMinPx && ax >= kSwipeDominance * ay) return dx > 0 ? Swipe::Right : Swipe::Left;
    if (ay > kSwipeMinPx && ay >= kSwipeDominance * ax) return dy > 0 ? Swipe::Down : Swipe::Up;
    return Swipe::None;
}

std::optional<Page> navigate(Page from, Swipe s, bool from_top_zone, bool at_bottom_end) {
    switch (from) {
        case Page::Home:
            if (s == Swipe::Right) return Page::Settings;
            if (s == Swipe::Left) return Page::Scenes;
            if (s == Swipe::Down && !from_top_zone) return Page::History;
            return std::nullopt;
        case Page::Settings:
            if (s == Swipe::Left) return Page::Home;
            return std::nullopt;
        case Page::Scenes:
            if (s == Swipe::Right) return Page::Home;
            return std::nullopt;
        case Page::History:
            if (s == Swipe::Up && at_bottom_end) return Page::Home;
            return std::nullopt;
    }
    return std::nullopt;
}

void DragTrack::press(lv_point_t p, uint64_t now) {
    reset();
    pressed_ = true;
    start_ = p;
    (void)now;
}

Swipe DragTrack::moved(lv_point_t p, uint64_t now) {
    if (!pressed_) return Swipe::None;
    if (dragging_) {
        record(p, now);
        return Swipe::None;
    }
    if (decided_) return Swipe::None;
    const int32_t dx = p.x - start_.x;
    const int32_t dy = p.y - start_.y;
    if (std::max(std::abs(dx), std::abs(dy)) < kDragSlopPx) return Swipe::None;
    decided_ = true;
    // As LVGL picks a scroll's axis: horizontal only when strictly more so.
    if (std::abs(dx) > std::abs(dy)) return dx > 0 ? Swipe::Right : Swipe::Left;
    return dy > 0 ? Swipe::Down : Swipe::Up;
}

void DragTrack::start(Swipe dir, int32_t extent) {
    if (!pressed_ || dir == Swipe::None) return;
    dragging_ = true;
    dir_ = dir;
    extent_ = std::max<int32_t>(extent, 1);
    fling_.reset();
}

int32_t DragTrack::along(lv_point_t p) const {
    switch (dir_) {
        case Swipe::Right:
            return p.x - start_.x;
        case Swipe::Left:
            return start_.x - p.x;
        case Swipe::Down:
            return p.y - start_.y;
        case Swipe::Up:
            return start_.y - p.y;
        case Swipe::None:
            break;
    }
    return 0;
}

void DragTrack::record(lv_point_t p, uint64_t now) {
    along_ = along(p);
    fling_.add(along_, now);
}

int32_t DragTrack::offset() const {
    return dragging_ ? std::clamp<int32_t>(along_, 0, extent_) : 0;
}

bool DragTrack::release(lv_point_t p, uint64_t now) {
    if (!dragging_) {
        reset();
        return false;
    }
    record(p, now);
    const int32_t dist = offset();
    const bool fling = fling_.fling();
    reset();
    return ui::drag_completes(dist, extent_, fling);
}

void DragTrack::reset() {
    pressed_ = decided_ = dragging_ = false;
    dir_ = Swipe::None;
    along_ = 0;
    fling_.reset();
}

namespace {

bool beyond(lv_point_t a, lv_point_t b) {
    return std::abs(b.x - a.x) > kSwipeMinPx || std::abs(b.y - a.y) > kSwipeMinPx;
}

}  // namespace

WispPress::Action WispPress::pressed(lv_point_t p, uint64_t now) {
    // A press without a release before it (a lost release) ends the
    // last one's hold first.
    const bool was_holding = state_ == State::Holding;
    state_ = State::Pressed;
    start_ = p;
    start_ms_ = now;
    held_ = false;
    return was_holding ? Action::HoldEnd : Action::None;
}

WispPress::Action WispPress::moved(lv_point_t p, uint64_t now) {
    if (state_ == State::Holding) {
        if (start_.y - p.y > kSwipeMinPx) {
            state_ = State::Done;
            return Action::HoldCancel;
        }
        return Action::None;
    }
    if (state_ != State::Pressed) return Action::None;
    if (beyond(start_, p)) {
        state_ = State::Moved;
        return Action::None;
    }
    return tick(now);
}

WispPress::Action WispPress::tick(uint64_t now) {
    if (state_ != State::Pressed || now - start_ms_ < voice::kHoldStartMs) return Action::None;
    state_ = State::Holding;
    held_ = true;
    return Action::HoldBegin;
}

WispPress::Action WispPress::released(lv_point_t p, uint64_t now) {
    const State s = state_;
    state_ = State::Idle;
    switch (s) {
        case State::Holding:
            return Action::HoldEnd;
        case State::Pressed:
            // Held long enough but never ticked: too late for a tap, and
            // no hold began that could end.
            if (beyond(start_, p) || now - start_ms_ >= voice::kHoldStartMs) return Action::None;
            return Action::Tap;
        default:
            return Action::None;
    }
}

WispPress::Action WispPress::lost() {
    const bool was_holding = state_ == State::Holding;
    state_ = State::Idle;
    return was_holding ? Action::HoldCancel : Action::None;
}

}  // namespace wisp::shell
