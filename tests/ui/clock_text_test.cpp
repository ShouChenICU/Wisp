// ui::format_clock() and format_local_clock() (wisp/ui/clock_text.hpp,
// mvp spec 6.4): the one place the device writes a wall-clock time.

#include "wisp/ui/clock_text.hpp"

#include <gtest/gtest.h>

using wisp::ui::format_clock;
using wisp::ui::format_local_clock;
using wisp::ui::Lang;

TEST(ClockText, TwentyFourHourIsTheSameInEveryLanguage) {
    for (Lang lang : wisp::ui::kAllLangs) {
        EXPECT_EQ(format_clock(0, 0, true, lang), "00:00");
        EXPECT_EQ(format_clock(0, 5, true, lang), "00:05");
        EXPECT_EQ(format_clock(12, 0, true, lang), "12:00");
        EXPECT_EQ(format_clock(12, 30, true, lang), "12:30");
        EXPECT_EQ(format_clock(23, 59, true, lang), "23:59");
    }
}

TEST(ClockText, TwelveHourChinese) {
    EXPECT_EQ(format_clock(0, 0, false, Lang::Zh), "上午 12:00");
    EXPECT_EQ(format_clock(0, 5, false, Lang::Zh), "上午 12:05");
    EXPECT_EQ(format_clock(12, 0, false, Lang::Zh), "下午 12:00");
    EXPECT_EQ(format_clock(12, 30, false, Lang::Zh), "下午 12:30");
    EXPECT_EQ(format_clock(15, 5, false, Lang::Zh), "下午 3:05");
    EXPECT_EQ(format_clock(23, 59, false, Lang::Zh), "下午 11:59");
}

TEST(ClockText, TwelveHourEnglish) {
    EXPECT_EQ(format_clock(0, 0, false, Lang::En), "12:00 AM");
    EXPECT_EQ(format_clock(0, 5, false, Lang::En), "12:05 AM");
    EXPECT_EQ(format_clock(12, 0, false, Lang::En), "12:00 PM");
    EXPECT_EQ(format_clock(12, 30, false, Lang::En), "12:30 PM");
    EXPECT_EQ(format_clock(15, 5, false, Lang::En), "3:05 PM");
    EXPECT_EQ(format_clock(23, 59, false, Lang::En), "11:59 PM");
}

// Japanese counts the hours of each half from 0, as its clocks do.
TEST(ClockText, TwelveHourJapanese) {
    EXPECT_EQ(format_clock(0, 0, false, Lang::Ja), "午前0:00");
    EXPECT_EQ(format_clock(0, 5, false, Lang::Ja), "午前0:05");
    EXPECT_EQ(format_clock(12, 0, false, Lang::Ja), "午後0:00");
    EXPECT_EQ(format_clock(12, 30, false, Lang::Ja), "午後0:30");
    EXPECT_EQ(format_clock(15, 5, false, Lang::Ja), "午後3:05");
    EXPECT_EQ(format_clock(23, 59, false, Lang::Ja), "午後11:59");
}

TEST(ClockText, OutOfRangeWraps) {
    EXPECT_EQ(format_clock(24, 0, true, Lang::En), "00:00");
    EXPECT_EQ(format_clock(-1, 0, true, Lang::En), "23:00");
    EXPECT_EQ(format_clock(1, 60, true, Lang::En), "01:00");
}

TEST(ClockText, LocalClockAddsTheOffset) {
    // 2026-01-01 00:05:00 UTC.
    const int64_t t = 1767225900;
    EXPECT_EQ(format_local_clock(t, 0, true, Lang::En), "00:05");
    EXPECT_EQ(format_local_clock(t, 480, true, Lang::En), "08:05");
    EXPECT_EQ(format_local_clock(t, -300, false, Lang::En), "7:05 PM");
    EXPECT_EQ(format_local_clock(t, 330, false, Lang::Zh), "上午 5:35");
    EXPECT_EQ(format_local_clock(t, 765, false, Lang::Ja), "午後0:50");
}
