#include "wisp/base/fixed_string.hpp"

#include <gtest/gtest.h>

#include <cstring>
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

TEST(FixedString, AssignFromItsOwnTailIsSafe) {
    FixedString<8> s("abcdef");
    // The source overlaps the destination (ASan reports memcpy here).
    EXPECT_TRUE(s.assign(s.view().substr(1)));
    EXPECT_EQ(s.view(), "bcdef");
    EXPECT_TRUE(s.assign(s.view().substr(2, 2)));
    EXPECT_EQ(s.view(), "de");
}

namespace {

// Two FixedStrings with the same contents are equal byte for byte: the
// tail past the size is zero whatever it held before.
template <size_t N>
bool same_bytes(const FixedString<N>& a, std::string_view expected) {
    FixedString<N> fresh;
    fresh.assign(expected);
    return std::memcmp(&a, &fresh, sizeof(a)) == 0;
}

}  // namespace

TEST(FixedString, AssignUtf8KeepsAsciiAndTruncatesAtTheLimit) {
    FixedString<4> s;
    EXPECT_FALSE(s.assign_utf8("abcdef"));
    EXPECT_EQ(s.view(), "abcd");
    EXPECT_TRUE(s.assign_utf8("ab"));
    EXPECT_EQ(s.view(), "ab");
    EXPECT_TRUE(same_bytes(s, "ab"));
}

TEST(FixedString, AssignUtf8NeverSplitsASequenceAtTheLimit) {
    FixedString<4> two;
    EXPECT_FALSE(two.assign_utf8("abc\xc3\xa9"));  // "abcé": é straddles byte 4
    EXPECT_EQ(two.view(), "abc");
    EXPECT_TRUE(same_bytes(two, "abc"));

    FixedString<4> three;
    EXPECT_FALSE(three.assign_utf8("ab\xe4\xbd\xa0"));  // "ab你"
    EXPECT_EQ(three.view(), "ab");
    FixedString<3> three_short;
    EXPECT_FALSE(three_short.assign_utf8("ab\xe4\xbd\xa0"));
    EXPECT_EQ(three_short.view(), "ab");

    FixedString<4> four;
    EXPECT_FALSE(four.assign_utf8("a\xf0\x9f\x98\x80"));  // "a😀"
    EXPECT_EQ(four.view(), "a");
    EXPECT_TRUE(same_bytes(four, "a"));
}

TEST(FixedString, AssignUtf8KeepsAnExactFit) {
    FixedString<5> s;
    EXPECT_TRUE(s.assign_utf8("ab\xe4\xbd\xa0"));
    EXPECT_EQ(s.view(), "ab\xe4\xbd\xa0");
    FixedString<4> t;
    EXPECT_FALSE(t.assign_utf8("\xe4\xbd\xa0\xe5\xa5\xbd"));  // "你好": the second does not fit
    EXPECT_EQ(t.view(), "\xe4\xbd\xa0");
}

TEST(FixedString, AssignUtf8OfEmptyClearsIt) {
    FixedString<4> s("abcd");
    EXPECT_TRUE(s.assign_utf8(std::string_view()));
    EXPECT_TRUE(s.empty());
    EXPECT_TRUE(same_bytes(s, ""));
}

TEST(FixedString, AssignUtf8CutsBareContinuationBytesAtTheLimit) {
    // No lead byte to protect: the cut stays at N, as assign() does.
    FixedString<4> s;
    EXPECT_FALSE(s.assign_utf8("\x80\x80\x80\x80\x80\x80"));
    EXPECT_EQ(s.view(), "\x80\x80\x80\x80");
}
