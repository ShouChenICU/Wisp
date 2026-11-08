// voice::SayParser, split_say and first_sentence (wisp/voice/say_parser.hpp).

#include "wisp/voice/say_parser.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

using wisp::voice::first_sentence;
using wisp::voice::SayParser;
using wisp::voice::split_say;

namespace {

struct Sink {
    std::string say, body;
    SayParser parser{
        {[this](std::string_view d) { say += d; }, [this](std::string_view d) { body += d; }}};
};

}  // namespace

TEST(SayParser, SplitsSayAndBody) {
    Sink s;
    s.parser.feed("<say>好的。</say>细节");
    EXPECT_EQ(s.parser.finish(), "");
    EXPECT_EQ(s.say, "好的。");
    EXPECT_EQ(s.body, "细节");
    EXPECT_TRUE(s.parser.saw_say());
}

TEST(SayParser, TagSplitAtEveryPosition) {
    const std::string whole = "前<say>好的。</say>细节<sa>x</sa";

    Sink bytes;
    for (char c : whole) bytes.parser.feed(std::string_view(&c, 1));
    bytes.parser.finish();
    EXPECT_EQ(bytes.say, "好的。");
    EXPECT_EQ(bytes.body, "前细节<sa>x</sa");

    for (size_t cut = 0; cut <= whole.size(); ++cut) {
        Sink two;
        two.parser.feed(std::string_view(whole).substr(0, cut));
        two.parser.feed(std::string_view(whole).substr(cut));
        two.parser.finish();
        EXPECT_EQ(two.say, bytes.say) << "cut at " << cut;
        EXPECT_EQ(two.body, bytes.body) << "cut at " << cut;
    }
}

TEST(SayParser, SeveralSaysAcrossToolSteps) {
    Sink s;
    s.parser.feed("<say>先看");
    s.parser.feed("看。</say>");
    // A tool call happens; the next model request continues the turn.
    s.parser.feed("结果<say>好了。</say>");
    EXPECT_EQ(s.parser.finish(), "");
    EXPECT_EQ(s.say, "先看看。好了。");
    EXPECT_EQ(s.body, "结果");
}

TEST(SayParser, UnclosedSayFlushesAsSay) {
    Sink s;
    s.parser.feed("正文<say>还没说完</sa");
    EXPECT_EQ(s.say, "还没说完");
    EXPECT_EQ(s.parser.finish(), "");
    EXPECT_EQ(s.say, "还没说完</sa");
    EXPECT_EQ(s.body, "正文");
}

TEST(SayParser, NoSayFallsBackToFirstSentence) {
    Sink s;
    s.parser.feed("今天晴。");
    s.parser.feed("明天雨。");
    EXPECT_EQ(s.parser.finish(), "今天晴。");
    EXPECT_EQ(s.body, "今天晴。明天雨。");
    EXPECT_EQ(s.say, "");
    EXPECT_FALSE(s.parser.saw_say());
}

// The fallback never speaks code: fenced blocks (their fence lines too)
// are skipped, even when the deltas cut a fence apart.
TEST(SayParser, FallbackSkipsFencedCode) {
    {
        Sink s;
        s.parser.feed("```lua\nlocal x = 1\n```\n装好了。还有");
        EXPECT_EQ(s.parser.finish(), "装好了。");
        EXPECT_EQ(s.body, "```lua\nlocal x = 1\n```\n装好了。还有");
    }
    {
        Sink s;
        for (std::string_view d : {"  `", "`", "`py\nprint(1)\n", "``", "`\n", "完成"})
            s.parser.feed(d);
        EXPECT_EQ(s.parser.finish(), "完成");
    }
    {
        // A long block before the text: the text is still found.
        Sink s;
        s.parser.feed("```\n");
        for (int i = 0; i < 2000; ++i) s.parser.feed("x = 1。\n");
        s.parser.feed("```\n好了。");
        EXPECT_EQ(s.parser.finish(), "好了。");
    }
    {
        // Only code: nothing to speak. Backticks inside a line are text.
        Sink s;
        s.parser.feed("```\nx\n");
        EXPECT_EQ(s.parser.finish(), "");
        Sink t;
        t.parser.feed("用 ```x``` 吧。");
        EXPECT_EQ(t.parser.finish(), "用 ```x``` 吧。");
    }
}

TEST(SayParser, ResetStartsANewTurn) {
    Sink s;
    s.parser.feed("<say>一");
    s.parser.reset();
    s.say.clear();
    s.parser.feed("二。三。");
    EXPECT_FALSE(s.parser.saw_say());
    EXPECT_EQ(s.parser.finish(), "二。");
    EXPECT_EQ(s.body, "二。三。");
    EXPECT_EQ(s.say, "");
}

TEST(SayParser, StrayCloseTagIsDropped) {
    Sink s;
    s.parser.feed("a</say>b");
    s.parser.finish();
    EXPECT_EQ(s.body, "ab");
    EXPECT_EQ(s.say, "");
}

TEST(SayParser, NestedOpenTagIsDropped) {
    Sink s;
    s.parser.feed("<say>x<say>y</say>");
    s.parser.finish();
    EXPECT_EQ(s.say, "xy");
    EXPECT_EQ(s.body, "");
}

TEST(SayParser, TagsAreCaseSensitive) {
    Sink s;
    s.parser.feed("<SAY>x</SAY>");
    s.parser.finish();
    EXPECT_EQ(s.body, "<SAY>x</SAY>");
    EXPECT_EQ(s.say, "");
    EXPECT_FALSE(s.parser.saw_say());
}

TEST(SayParser, SplitSayMatchesStreaming) {
    const std::string whole = "开头<say>你好，</say>中间</say><say>再<say>见。</say>结尾<say>未完";
    Sink s;
    for (size_t i = 0; i < whole.size(); i += 3) {
        s.parser.feed(std::string_view(whole).substr(i, 3));
    }
    s.parser.finish();
    const auto split = split_say(whole);
    EXPECT_EQ(split.say, s.say);
    EXPECT_EQ(split.body, s.body);
    EXPECT_EQ(split.say, "你好，再见。未完");
    EXPECT_EQ(split.body, "开头中间结尾");
}

TEST(SayParser, FirstSentenceSkipsBlankLinesAndTrims) {
    EXPECT_EQ(first_sentence("\n\n  今天晴！明天雨"), "今天晴！");
    EXPECT_EQ(first_sentence("第一行\n第二行"), "第一行");
    EXPECT_EQ(first_sentence("没有句号"), "没有句号");
    EXPECT_EQ(first_sentence("Hi? there"), "Hi?");
    EXPECT_EQ(first_sentence(" \n "), "");
}
