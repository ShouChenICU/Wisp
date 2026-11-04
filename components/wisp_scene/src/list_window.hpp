#pragma once

// Which rows of a virtualized ui.list exist as widgets (scene-runtime-core
// spec 4.3): the rows the viewport shows plus `overscan` rows on each
// side, so a short scroll finds its next row already built. Pure
// arithmetic, shared by SceneView and its tests.

#include <algorithm>
#include <cstdint>

namespace wisp::scene {

struct ListWindow {
    uint32_t first = 0;
    uint32_t count = 0;
    bool operator==(const ListWindow&) const = default;
};

// `scroll_y` is held to what a list of `total` rows of `row_h` pixels can
// scroll to in a `viewport_h` viewport, so a list that shrank while
// scrolled far down still gets the rows its last screen shows.
inline ListWindow visible_rows(int32_t scroll_y, int32_t viewport_h, int32_t row_h, uint32_t total,
                               uint32_t overscan = 2) {
    if (total == 0) return {};
    const int64_t rh = std::max<int32_t>(row_h, 1);
    const int64_t view = std::max<int32_t>(viewport_h, 0);
    const int64_t content = static_cast<int64_t>(total) * rh;
    const int64_t y = std::clamp<int64_t>(scroll_y, 0, std::max<int64_t>(content - view, 0));
    const int64_t top = y / rh;
    const int64_t bottom = (y + view + rh - 1) / rh;  // one past the last row shown
    const int64_t first = std::max<int64_t>(top - overscan, 0);
    const int64_t last = std::min<int64_t>(std::max(bottom, top + 1) + overscan, total);
    return {static_cast<uint32_t>(first), static_cast<uint32_t>(last - first)};
}

}  // namespace wisp::scene
