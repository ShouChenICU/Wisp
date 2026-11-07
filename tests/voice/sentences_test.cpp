// voice::SentenceSplitter (wisp/voice/sentences.hpp).

#include "wisp/voice/sentences.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using wisp::voice::SentenceSplitter;

namespace {

size_t code_points(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

}  // namespace

TEST(Sentences, SplitsOnTerminatorsAndNewline) {
    SentenceSplitter split;
    std::vector<std::string> out;
    auto push = [&](std::string s) { out.push_back(std::move(s)); };
    split.feed("你好！今天;晴\n好", push);
    EXPECT_EQ(out, (std::vector<std::string>{"你好！", "今天;", "晴\n"}));
    split.flush(push);
    EXPECT_EQ(out.back(), "好");
    EXPECT_EQ(out.size(), 4u);
    EXPECT_FALSE(split.capped());
}

TEST(Sentences, DeltasMaySplitACodePoint) {
    SentenceSplitter split;
    std::vector<std::string> out;
    auto push = [&](std::string s) { out.push_back(std::move(s)); };
    const std::string whole = "好的。\n\n明天？";
    for (char c : whole) split.feed(std::string(1, c), push);
    split.flush(push);
    EXPECT_EQ(out, (std::vector<std::string>{"好的。", "明天？"}));
}

TEST(Sentences, CapsAt200CodePoints) {
    SentenceSplitter split;
    std::vector<std::string> out;
    auto push = [&](std::string s) { out.push_back(std::move(s)); };
    std::string text;
    for (int i = 0; i < 250; ++i) text += "汉";
    split.feed(text, push);
    split.feed("还有。", push);
    split.flush(push);
    size_t total = 0;
    for (const auto& s : out) total += code_points(s);
    EXPECT_EQ(total, 200u);
    EXPECT_TRUE(split.capped());
}
