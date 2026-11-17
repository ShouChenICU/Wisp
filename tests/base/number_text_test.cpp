// format_double/parse_double must give what std::to_chars/from_chars give
// for doubles (the host's libstdc++ is the reference here), without linking
// them into the firmware.

#include "wisp/base/number_text.hpp"

#include <gtest/gtest.h>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <system_error>

namespace {

std::string ours(double d) {
    char buf[wisp::kDoubleTextMax];
    return std::string(buf, wisp::format_double(d, buf));
}

std::string reference(double d) {
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof(buf), d);
    return std::string(buf, r.ptr);
}

double from_bits(uint64_t b) {
    double d = 0;
    std::memcpy(&d, &b, sizeof d);
    return d;
}

// What from_chars makes of a whole JSON number: the value, or nothing.
bool reference_parse(const std::string& s, double& out) {
    double v = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return false;
    out = v;
    return true;
}

void expect_parse_like_reference(const std::string& s) {
    double want = 0;
    double have = 0;
    const bool want_ok = reference_parse(s, want);
    ASSERT_EQ(wisp::parse_double(s, have), want_ok) << s;
    if (want_ok) {
        EXPECT_EQ(std::memcmp(&have, &want, sizeof have), 0) << s;
    }
}

}  // namespace

TEST(NumberText, FormatsLikeToChars) {
    for (double d : {0.0,
                     -0.0,
                     0.1,
                     0.1 + 0.2,
                     1.0 / 3,
                     -2.5,
                     1e4,
                     1e5,
                     123456.0,
                     0.001,
                     0.0001,
                     1e-5,
                     1.5e-7,
                     9007199254740992.0,
                     9223372036854775808.0,
                     1.8446744073709552e19,
                     1.2345678901234567e19,
                     1e15,
                     1e16,
                     1e21,
                     1e22,
                     1.23e22,
                     3.141592653589793,
                     std::numeric_limits<double>::max(),
                     std::numeric_limits<double>::min(),
                     std::numeric_limits<double>::denorm_min(),
                     -1e300,
                     4e-320}) {
        EXPECT_EQ(ours(d), reference(d)) << reference(d);
    }
}

// Every power of two: the gap below it is half the gap above, where the
// shortest text is easiest to get wrong.
TEST(NumberText, FormatsPowersOfTwoLikeToChars) {
    for (int e = -1074; e <= 1023; ++e) {
        const double d = std::ldexp(1.0, e);
        ASSERT_EQ(ours(d), reference(d)) << e;
    }
}

// Not finite: nothing is written, rather than reading past the probe text.
TEST(NumberText, WritesNothingForInfinityOrNan) {
    char buf[wisp::kDoubleTextMax];
    EXPECT_EQ(wisp::format_double(std::numeric_limits<double>::infinity(), buf), 0u);
    EXPECT_EQ(wisp::format_double(-std::numeric_limits<double>::infinity(), buf), 0u);
    EXPECT_EQ(wisp::format_double(std::numeric_limits<double>::quiet_NaN(), buf), 0u);
}

TEST(NumberText, FormatsRandomDoublesLikeToChars) {
    std::mt19937_64 rng(12345);
    for (int i = 0; i < 200000; ++i) {
        const double d = from_bits(rng());
        if (!std::isfinite(d)) continue;
        ASSERT_EQ(ours(d), reference(d)) << reference(d);
    }
    // Short decimals, the values scenes mostly store.
    std::uniform_int_distribution<int> digits(0, 999999);
    std::uniform_int_distribution<int> exp(-12, 12);
    for (int i = 0; i < 100000; ++i) {
        const double d = digits(rng) * std::pow(10.0, exp(rng));
        ASSERT_EQ(ours(d), reference(d)) << reference(d);
        ASSERT_EQ(ours(-d), reference(-d)) << reference(-d);
    }
    // Integral values around and past 2^53 and 2^64.
    for (int i = 0; i < 20000; ++i) {
        const double d = std::ldexp(static_cast<double>(rng() >> 11), 10 + i % 20);
        ASSERT_EQ(ours(d), reference(d)) << reference(d);
    }
}

TEST(NumberText, ParsesLikeFromChars) {
    for (const char* s : {"0",
                          "-0",
                          "-0.0",
                          "0.1",
                          "1E2",
                          "1e+2",
                          "1e-2",
                          "2.5e-3",
                          "123456789",
                          "0.1000000000000000055511151231257827",
                          "3.0000000000000000000000000000000000000000000000000000000000000000001",
                          "4e-320",
                          "5e-324",
                          "2e-324",
                          "1.7976931348623157e308",
                          "1.7976931348623158e308",
                          "1.7976931348623159e308",
                          "1e308",
                          "1e309",
                          "1e400",
                          "-1e400",
                          "1e-400",
                          "0e-400",
                          "0e400",
                          "-9223372036854775809",
                          "123456789012345678901234567890",
                          "1e0000000000000000000005"}) {
        expect_parse_like_reference(s);
    }
    std::mt19937_64 rng(777);
    for (int i = 0; i < 50000; ++i) {
        const double d = from_bits(rng());
        if (!std::isfinite(d)) continue;
        expect_parse_like_reference(reference(d));
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.25e", d);
        expect_parse_like_reference(buf);
    }
}

TEST(NumberText, RefusesWhatIsNotAJsonNumber) {
    double v = 42;
    for (const char* s :
         {"",   "+1",  " 1", "1 ",  "0x10", "-0x10", "inf",  "-inf", "nan", "Infinity", "1.",
          ".5", "-.5", "1e", "1e+", "-",    "1.5x",  "0.5e", "01",   "-01", "1,5",      "1e5.5"}) {
        EXPECT_FALSE(wisp::parse_double(s, v)) << s;
    }
    EXPECT_EQ(v, 42);
    // Not NUL-terminated: only the view is read.
    const char text[] = {'1', '.', '5', '7'};
    ASSERT_TRUE(wisp::parse_double(std::string_view(text, 3), v));
    EXPECT_EQ(v, 1.5);
}
