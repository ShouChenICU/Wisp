#include "wisp/base/fixed_string.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <type_traits>

using wisp::FixedString;

TEST(FixedString, IsTriviallyCopyable) {
    static_assert(std::is_trivially_copyable_v<FixedString<32>>);
    static_assert(std::is_trivially_copyable_v<FixedString<72>>);
    static_assert(FixedString<32>::capacity() == 32);
}

TEST(FixedString, AssignWithinCapacityKeepsEveryByte) {
    std::string in(32, 'a');
    in[3] = '\0';
    in[31] = '\xff';
    FixedString<32> s;
    EXPECT_TRUE(s.assign(in));
    EXPECT_EQ(s.size(), 32u);
    EXPECT_EQ(s.view(), std::string_view(in));
    EXPECT_EQ(s.str(), in);
}

TEST(FixedString, AssignPastCapacityTruncatesAndSaysSo) {
    FixedString<4> s;
    EXPECT_FALSE(s.assign("abcdef"));
    EXPECT_EQ(s.view(), "abcd");
    EXPECT_TRUE(s.assign("ab"));
    EXPECT_EQ(s.size(), 2u);
    EXPECT_EQ(s.view(), "ab");
}

TEST(FixedString, EqualityIgnoresBytesPastTheSize) {
    FixedString<8> a;
    a.assign("abcdef");
    a.assign("ab");
    FixedString<8> b;
    b.assign("ab");
    EXPECT_TRUE(a == b);
    EXPECT_TRUE(a == std::string_view("ab"));
    EXPECT_FALSE(a == FixedString<8>("abc"));
    EXPECT_TRUE(a != FixedString<8>("abc"));
}

TEST(FixedString, EmptyByDefault) {
    FixedString<16> s;
    EXPECT_EQ(s.size(), 0u);
    EXPECT_TRUE(s.empty());
    EXPECT_TRUE(s.view().empty());
    EXPECT_TRUE(s == "");
}
