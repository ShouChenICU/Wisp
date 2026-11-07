// Local time at a fixed UTC offset and repeat rules (mvp spec 5.2).

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/reminders.hpp"

using wisp::List;
using wisp::Map;
using wisp::Value;
using wisp::core::format_local_minute;
using wisp::core::local_weekday;
using wisp::core::next_occurrence;
using wisp::core::parse_local_minute;
using wisp::core::parse_repeat;
using wisp::core::Repeat;
using wisp::core::RepeatRule;

namespace {

constexpr int64_t kDay = 86400;

Value weekly(List days) { return Value(Map{{"weekly", Value(std::move(days))}}); }

}  // namespace

TEST(LocalTime, ParseFormatWeekday) {
    EXPECT_EQ(parse_local_minute("2026-11-07T08:00", 480), std::optional<int64_t>(1794009600));
    EXPECT_EQ(format_local_minute(1794009600, 480), "2026-11-07T08:00");
    EXPECT_EQ(format_local_minute(1794009600, 0), "2026-11-07T00:00");
    EXPECT_EQ(parse_local_minute("1970-01-01T00:00", 0), std::optional<int64_t>(0));
    EXPECT_EQ(*parse_local_minute("2028-02-29T23:59", 0) + 60,
              *parse_local_minute("2028-03-01T00:00", 0));
    for (const char* bad :
         {"2026-13-01T00:00", "2026-02-29T08:00", "2026-11-31T08:00", "2026-11-07T24:00",
          "2026-11-07T08:60", "2026-11-07 08:00", "2026-11-7T08:00", "2026-11-07T08:00:00",
          "tomorrow 8am", "", "2026-11-07T0a:00", "+026-11-07T08:00"}) {
        EXPECT_EQ(parse_local_minute(bad, 480), std::nullopt) << bad;
    }
    // 2026-11-06T08:00 local, a Friday.
    EXPECT_EQ(local_weekday(1793923200, 480), 5);
    // The same instant is still Thursday at UTC-10.
    EXPECT_EQ(local_weekday(1793923200, -600), 4);
    EXPECT_EQ(local_weekday(0, 0), 4);  // 1970-01-01, a Thursday
    EXPECT_EQ(local_weekday(-1, 0), 3);
}

TEST(LocalTime, NextOccurrence) {
    const int64_t fri8 = *parse_local_minute("2026-11-06T08:00", 480);
    const int64_t mon8 = *parse_local_minute("2026-11-09T08:00", 480);
    const RepeatRule daily{Repeat::Daily, 0};
    const RepeatRule mwf{Repeat::Weekly, 0b10101};

    EXPECT_EQ(next_occurrence(fri8, daily, fri8, 480), fri8 + kDay);
    EXPECT_EQ(next_occurrence(fri8, daily, fri8 + 3 * kDay + 5, 480), fri8 + 4 * kDay);
    // Already ahead: unchanged.
    EXPECT_EQ(next_occurrence(fri8, daily, fri8 - 1, 480), fri8);
    EXPECT_EQ(next_occurrence(fri8, mwf, fri8, 480), mon8);
    EXPECT_EQ(next_occurrence(mon8, mwf, mon8, 480), mon8 + 2 * kDay);               // Wednesday
    EXPECT_EQ(next_occurrence(fri8, mwf, fri8 + 10 * kDay, 480), fri8 + 12 * kDay);  // Wed
    EXPECT_EQ(next_occurrence(fri8, RepeatRule{}, fri8 + kDay, 480), fri8);
    // Weekdays are local ones: at UTC-5, 08:00 Friday is still Friday.
    const int64_t fri8_ny = *parse_local_minute("2026-11-06T08:00", -300);
    EXPECT_EQ(next_occurrence(fri8_ny, mwf, fri8_ny, -300),
              *parse_local_minute("2026-11-09T08:00", -300));
}

TEST(LocalTime, ParseRepeat) {
    EXPECT_EQ(parse_repeat(Value("none")), RepeatRule{});
    EXPECT_EQ(parse_repeat(Value("daily")), (RepeatRule{Repeat::Daily, 0}));
    auto w = parse_repeat(weekly({Value(int64_t{1}), Value(int64_t{3}), Value(int64_t{5})}));
    ASSERT_TRUE(w);
    EXPECT_EQ(w->kind, Repeat::Weekly);
    EXPECT_EQ(w->days, 0b10101);
    EXPECT_EQ(wisp::to_json(wisp::core::to_value(*w)), R"({"weekly":[1,3,5]})");
    EXPECT_EQ(wisp::to_json(wisp::core::to_value(RepeatRule{})), R"("none")");
    EXPECT_EQ(wisp::to_json(wisp::core::to_value(RepeatRule{Repeat::Daily, 0})), R"("daily")");
    EXPECT_EQ(parse_repeat(weekly({Value(int64_t{7})}))->days, 0b1000000);

    EXPECT_EQ(parse_repeat(weekly({})), std::nullopt);
    EXPECT_EQ(parse_repeat(weekly({Value(int64_t{8})})), std::nullopt);
    EXPECT_EQ(parse_repeat(weekly({Value(int64_t{0})})), std::nullopt);
    EXPECT_EQ(parse_repeat(weekly({Value("1")})), std::nullopt);
    EXPECT_EQ(parse_repeat(Value("weekly")), std::nullopt);
    EXPECT_EQ(parse_repeat(Value("hourly")), std::nullopt);
    EXPECT_EQ(parse_repeat(Value(int64_t{1})), std::nullopt);
    EXPECT_EQ(parse_repeat(Value(Map{{"monthly", Value(List{})}})), std::nullopt);
}
