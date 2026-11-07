// Local time at a fixed UTC offset and repeat rules (wisp/core/reminders.hpp).
//
// days_from_civil() and civil_from_days() are Howard Hinnant's public-domain
// date algorithms (http://howardhinnant.github.io/date_algorithms.html),
// rewritten here; they are exact for every proleptic Gregorian date.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include "wisp/core/reminders.hpp"

namespace wisp::core {

namespace {

constexpr int64_t kDayS = 86400;

int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const int64_t era = floor_div(y, 400);
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

struct Civil {
    int64_t y;
    unsigned m, d;
};

Civil civil_from_days(int64_t z) {
    z += 719468;
    const int64_t era = floor_div(z, 146097);
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    return {static_cast<int64_t>(yoe) + era * 400 + (m <= 2 ? 1 : 0), m, d};
}

bool leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

unsigned days_in_month(int64_t y, unsigned m) {
    static constexpr unsigned kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap(y) ? 29 : kDays[m - 1];
}

// `n` ASCII digits at s[at...]; -1 if any is not one.
int digits(std::string_view s, size_t at, size_t n) {
    int v = 0;
    for (size_t i = at; i < at + n; ++i) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

int64_t local_s(int64_t utc_s, int utc_offset_min) { return utc_s + int64_t{utc_offset_min} * 60; }

}  // namespace

bool operator==(const RepeatRule& a, const RepeatRule& b) {
    if (a.kind != b.kind) return false;
    return a.kind != Repeat::Weekly || a.days == b.days;
}

std::optional<RepeatRule> parse_repeat(const Value& v) {
    if (v.type() == Value::Type::String) {
        if (v.as_string() == "none") return RepeatRule{};
        if (v.as_string() == "daily") return RepeatRule{Repeat::Daily, 0};
        return std::nullopt;
    }
    if (v.type() != Value::Type::Map || v.as_map().size() != 1) return std::nullopt;
    const Value* days = v.find("weekly");
    if (days == nullptr || days->type() != Value::Type::List || days->as_list().empty()) {
        return std::nullopt;
    }
    uint8_t mask = 0;
    for (const Value& d : days->as_list()) {
        if (d.type() != Value::Type::Int || d.as_int() < 1 || d.as_int() > 7) return std::nullopt;
        mask = static_cast<uint8_t>(mask | (1u << (d.as_int() - 1)));
    }
    return RepeatRule{Repeat::Weekly, mask};
}

Value to_value(const RepeatRule& r) {
    switch (r.kind) {
        case Repeat::None:
            return Value("none");
        case Repeat::Daily:
            return Value("daily");
        case Repeat::Weekly: {
            List days;
            for (int d = 1; d <= 7; ++d) {
                if ((r.days & (1u << (d - 1))) != 0) days.push_back(Value(int64_t{d}));
            }
            return Value(Map{{"weekly", Value(std::move(days))}});
        }
    }
    return Value("none");
}

std::optional<int64_t> parse_local_minute(std::string_view s, int utc_offset_min) {
    // YYYY-MM-DDTHH:MM
    if (s.size() != 16 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':') {
        return std::nullopt;
    }
    const int y = digits(s, 0, 4);
    const int mo = digits(s, 5, 2);
    const int d = digits(s, 8, 2);
    const int h = digits(s, 11, 2);
    const int mi = digits(s, 14, 2);
    if (y < 0 || mo < 1 || mo > 12 || d < 1 || h < 0 || h > 23 || mi < 0 || mi > 59) {
        return std::nullopt;
    }
    if (static_cast<unsigned>(d) > days_in_month(y, static_cast<unsigned>(mo))) {
        return std::nullopt;
    }
    const int64_t days = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
    return days * kDayS + int64_t{h} * 3600 + int64_t{mi} * 60 - int64_t{utc_offset_min} * 60;
}

std::string format_local_minute(int64_t utc_s, int utc_offset_min) {
    const int64_t t = local_s(utc_s, utc_offset_min);
    const int64_t day = floor_div(t, kDayS);
    const int64_t sec = t - day * kDayS;
    const Civil c = civil_from_days(day);
    // Sized for the widest value of every field, so -Wformat-truncation
    // cannot fire on toolchains that check it.
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02uT%02d:%02d", static_cast<long long>(c.y), c.m,
                  c.d, static_cast<int>(sec / 3600), static_cast<int>(sec % 3600 / 60));
    return buf;
}

int64_t local_day(int64_t utc_s, int utc_offset_min) {
    return floor_div(local_s(utc_s, utc_offset_min), kDayS);
}

int local_weekday(int64_t utc_s, int utc_offset_min) {
    // 1970-01-01 was a Thursday (4).
    const int64_t day = local_day(utc_s, utc_offset_min);
    return static_cast<int>(((day + 3) % 7 + 7) % 7) + 1;
}

int64_t next_occurrence(int64_t due, const RepeatRule& r, int64_t after, int utc_offset_min) {
    if (r.kind == Repeat::None || due > after) return due;
    // A fixed offset has no daylight saving, so whole days keep the local
    // time of day.
    int64_t t = due + (floor_div(after - due, kDayS) + 1) * kDayS;
    if (r.kind == Repeat::Daily || r.days == 0) return t;
    for (int i = 0; i < 7; ++i, t += kDayS) {
        if ((r.days & (1u << (local_weekday(t, utc_offset_min) - 1))) != 0) return t;
    }
    return t;
}

}  // namespace wisp::core
