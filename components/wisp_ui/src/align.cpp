#include "wisp/ui/align.hpp"

namespace wisp::ui {

namespace {

int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

// Rounds `v` down to the nearest multiple of `align` (align > 1), using a
// true mathematical floor (not C++'s truncate-toward-zero `%`/`/`), so this
// stays correct for negative `v` too.
int32_t floor_align(int32_t v, int32_t align) {
    int32_t rem = v % align;
    if (rem < 0) {
        rem += align;
    }
    return v - rem;
}

}  // namespace

hal::Area align_area(const hal::Area& in, int align, int width, int height) {
    hal::Area out = in;

    if (align > 1) {
        out.x1 = floor_align(in.x1, align);
        out.y1 = floor_align(in.y1, align);
        // "Next multiple of align, minus one": floor_align(v) is already
        // the multiple at or below v, so the inclusive upper edge of that
        // same align-sized block is floor_align(v) + align - 1.
        out.x2 = floor_align(in.x2, align) + align - 1;
        out.y2 = floor_align(in.y2, align) + align - 1;
    }

    // Clamp last so an area already touching the right/bottom edge (e.g.
    // x2 == width - 1) is never rounded past it.
    out.x1 = clamp_i32(out.x1, 0, width - 1);
    out.x2 = clamp_i32(out.x2, 0, width - 1);
    out.y1 = clamp_i32(out.y1, 0, height - 1);
    out.y2 = clamp_i32(out.y2, 0, height - 1);

    return out;
}

}  // namespace wisp::ui
