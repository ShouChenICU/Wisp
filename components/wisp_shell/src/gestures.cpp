#include "wisp/shell/gestures.hpp"

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
