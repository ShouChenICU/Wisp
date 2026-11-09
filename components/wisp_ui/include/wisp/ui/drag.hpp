#pragma once

// How a drag that follows the finger settles on release (MVP spec 6.3,
// scene-device spec 5.6): the shell's page drags and the system layer's
// pull-down panel decide and slide the same way, so they feel alike.
//
// On release a drag completes from 1 / kDragCompleteDivisor of its extent
// on, or when the finger was moving on at kFlingPxPerS or faster over the
// last kFlingWindowMs. The rest of the way, either way, slides in at most
// kSlideMs (slide_ms()), eased out. No LVGL objects here.

#include <array>
#include <cstddef>
#include <cstdint>

#include "wisp/hal/display_geometry.hpp"

namespace wisp::ui {

// A press becomes a drag (a shell page drag, or LVGL's own scroll) once
// the finger is this share of the screen's shorter side from where it
// went down (10 px on a 480 px screen). UiRuntime sets the touch input's
// LVGL scroll limit to drag_slop_px(), so the shell and LVGL decide on
// the same touch read.
inline constexpr int kDragSlopPercent = 2;
constexpr int32_t drag_slop_px(const hal::ScreenShape& s) {
    return hal::share_of_screen(s, kDragSlopPercent);
}

inline constexpr int kDragCompleteDivisor = 3;
inline constexpr int kFlingPxPerS = 400;
inline constexpr uint32_t kFlingWindowMs = 50;
inline constexpr uint32_t kSlideMs = 220;

// The finger's latest distances along a drag's direction, for the fling
// test. Times are milliseconds on any one clock.
class FlingTrack {
public:
    void reset() { count_ = next_ = 0; }
    // `along` grows as the finger moves the drag's way.
    void add(int32_t along, uint64_t now);
    // Whether the finger was moving on (`along` growing) at kFlingPxPerS
    // or faster from the oldest point inside the last kFlingWindowMs to
    // the latest; false with no points.
    bool fling() const;

private:
    struct Sample {
        uint64_t ms = 0;
        int32_t along = 0;
    };
    // The latest points, oldest first once full; next_ is where the next
    // one goes.
    std::array<Sample, 8> samples_{};
    size_t count_ = 0, next_ = 0;
};

// Whether a drag released `dist` along its `extent` completes.
constexpr bool drag_completes(int32_t dist, int32_t extent, bool fling) {
    return fling || int64_t{dist} * kDragCompleteDivisor >= extent;
}

// How long the rest of a drag, `left` of `extent`, slides: kSlideMs for
// all of it, in proportion for less, and at least a millisecond, so the
// end always runs on the animation timer and never inside the release's
// own event.
uint32_t slide_ms(int32_t left, int32_t extent);

}  // namespace wisp::ui
