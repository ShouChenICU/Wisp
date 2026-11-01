#pragma once

// Portable flush-area alignment, shared by UiRuntime's LV_EVENT_INVALIDATE_AREA
// handler and its own unit tests. Must never include platform headers (see
// wisp/os/os.hpp for the rule) or LVGL headers -- it only depends on
// wisp::hal::Area.

#include "wisp/hal/display.hpp"

namespace wisp::ui {

// Aligns `in` to a display's required flush granularity, then clamps the
// result to the display's pixel bounds.
//
//   - If `align > 1`: x1/y1 are rounded down to the nearest multiple of
//     `align`; x2/y2 are rounded up to "the next multiple of `align`, minus
//     one" (e.g. x2=10 with align=2 becomes 11: the row pair [10,11]).
//   - If `align <= 1`: no rounding is applied at all (align of 0 or 1 both
//     mean "no alignment requirement").
//   - Either way, the (possibly rounded) result is then clamped to
//     [0, width-1] / [0, height-1] -- this clamp is what keeps an area that
//     already touches the right/bottom edge (e.g. x2 == width-1) from being
//     rounded past the edge of the display.
hal::Area align_area(const hal::Area& in, int align, int width, int height);

}  // namespace wisp::ui
