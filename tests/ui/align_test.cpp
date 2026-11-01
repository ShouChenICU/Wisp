#include "wisp/ui/align.hpp"

#include <gtest/gtest.h>

TEST(Align, EvenAlignment) {
    auto a = wisp::ui::align_area({3, 5, 10, 12}, 2, 480, 480);
    EXPECT_EQ(a.x1, 2);
    EXPECT_EQ(a.y1, 4);
    EXPECT_EQ(a.x2, 11);
    EXPECT_EQ(a.y2, 13);
}

TEST(Align, ClampsAtRightAndBottomEdges) {
    auto a = wisp::ui::align_area({470, 470, 479, 479}, 2, 480, 480);
    EXPECT_EQ(a.x2, 479);
    EXPECT_EQ(a.y2, 479);

    auto b = wisp::ui::align_area({0, 0, 478, 10}, 4, 479, 480);  // width isn't a multiple of align
    EXPECT_EQ(b.x2, 478);
}

TEST(Align, NoAlignmentOnlyClamps) {
    auto a = wisp::ui::align_area({-5, 3, 500, 9}, 1, 480, 480);
    EXPECT_EQ(a.x1, 0);
    EXPECT_EQ(a.x2, 479);
    EXPECT_EQ(a.y1, 3);
}
