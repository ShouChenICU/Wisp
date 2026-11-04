// visible_rows(): which rows of a virtualized ui.list are built
// (scene-runtime-core spec 4.3).

#include "list_window.hpp"

#include <gtest/gtest.h>

using wisp::scene::ListWindow;
using wisp::scene::visible_rows;

TEST(ListWindow, ComputesVisibleRangeWithOverscan) {
    // 400 / 56 shows rows 0..7 (the eighth only in part), plus two below.
    EXPECT_EQ(visible_rows(0, 400, 56, 200), (ListWindow{0, 10}));
    // Scrolled to row 50: two above, rows 50..57 shown, two below.
    EXPECT_EQ(visible_rows(50 * 56, 400, 56, 200), (ListWindow{48, 12}));
    EXPECT_EQ(visible_rows(50 * 56, 400, 56, 200, 0), (ListWindow{50, 8}));
    // Fewer rows than the viewport holds: all of them.
    EXPECT_EQ(visible_rows(0, 400, 56, 3), (ListWindow{0, 3}));
    EXPECT_EQ(visible_rows(0, 400, 56, 0), (ListWindow{0, 0}));
}

TEST(ListWindow, ClampsAtEnd) {
    for (int32_t scroll : {200 * 56 - 400, 200 * 56, 1 << 30}) {
        ListWindow w = visible_rows(scroll, 400, 56, 200);
        EXPECT_EQ(w.first + w.count, 200u) << scroll;
        EXPECT_EQ(w.count, 10u) << scroll;
    }
    // A negative scroll (an overscroll bounce) is the top.
    EXPECT_EQ(visible_rows(-500, 400, 56, 200), (ListWindow{0, 10}));
    // Degenerate sizes still give a sane window.
    EXPECT_EQ(visible_rows(0, 0, 0, 5, 0), (ListWindow{0, 1}));
}
