// The home screen's light Markdown (wisp/shell/body_text.hpp, mvp spec 6.2).

#include "wisp/shell/body_text.hpp"

#include <gtest/gtest.h>

#include <string>

#include "wisp/ui/strings.hpp"

using wisp::shell::BodyBlock;
using wisp::shell::code_fold_text;
using wisp::shell::kBodyMaxChars;
using wisp::shell::light_markdown;

namespace {

std::string str(const wisp::os::LargeString& s) { return std::string(s.data(), s.size()); }

}  // namespace

TEST(BodyText, LightMarkdown) {
    {
        auto b = light_markdown("**粗**体");
        ASSERT_EQ(b.size(), 1u);
        EXPECT_EQ(str(b[0].text), "粗体");
        EXPECT_FALSE(b[0].code);
    }
    {
        auto b = light_markdown("- a\n* b\n  - c **d**\n-x");
        ASSERT_EQ(b.size(), 1u);
        EXPECT_EQ(str(b[0].text), "• a\n• b\n  • c d\n-x");
    }
    EXPECT_TRUE(light_markdown("").empty());
    EXPECT_TRUE(light_markdown("\n \n").empty());
    // Blank lines inside a text block stay; around it they go.
    auto b = light_markdown("\na\n\nb\n\n");
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(str(b[0].text), "a\n\nb");
}

// A fenced block is folded: only its line count and a short language tag
// are kept, never its text.
TEST(BodyText, CodeBlockFolds) {
    auto b = light_markdown("前\n```lua\nx = **1**\n\n- y\n```\n后");
    ASSERT_EQ(b.size(), 3u);
    EXPECT_EQ(str(b[0].text), "前");
    EXPECT_FALSE(b[0].code);
    EXPECT_TRUE(b[1].code);
    EXPECT_TRUE(b[1].text.empty());
    EXPECT_EQ(b[1].lines, 3u);  // a blank line inside counts
    EXPECT_EQ(b[1].lang, "lua");
    EXPECT_EQ(str(b[2].text), "后");
    EXPECT_FALSE(b[2].code);
}

TEST(BodyText, OpenFenceCountsLinesSoFar) {
    // Still streaming: the open fence runs to the end; a line just begun
    // counts, an empty one after the last newline does not yet.
    auto b = light_markdown("看：\n```\nx");
    ASSERT_EQ(b.size(), 2u);
    EXPECT_TRUE(b[1].code);
    EXPECT_EQ(b[1].lines, 1u);
    EXPECT_EQ(b[1].lang, "");
    b = light_markdown("看：\n```py\nx\ny\n");
    ASSERT_EQ(b.size(), 2u);
    EXPECT_EQ(b[1].lines, 2u);
    EXPECT_EQ(b[1].lang, "py");
    b = light_markdown("看：\n```lua\n");
    ASSERT_EQ(b.size(), 2u);
    EXPECT_EQ(b[1].lines, 0u);
    EXPECT_EQ(b[1].lang, "lua");
    std::string many = "```lua\n";
    for (int i = 0; i < 300; ++i) many += "local x = " + std::to_string(i) + "\n";
    b = light_markdown(many);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0].lines, 300u);
}

TEST(BodyText, TwoBlocksFoldSeparately) {
    auto b = light_markdown("a\n```\n1\n2\n```\nb\n  ```c++\n3\n```");
    ASSERT_EQ(b.size(), 4u);
    EXPECT_EQ(str(b[0].text), "a");
    EXPECT_EQ(b[1].lines, 2u);
    EXPECT_EQ(b[1].lang, "");
    EXPECT_EQ(str(b[2].text), "b");
    EXPECT_EQ(b[3].lines, 1u);
    EXPECT_EQ(b[3].lang, "c++");
    // Back to back, nothing between them.
    b = light_markdown("```\n1\n```\n```\n2\n3\n```");
    ASSERT_EQ(b.size(), 2u);
    EXPECT_EQ(b[0].lines, 1u);
    EXPECT_EQ(b[1].lines, 2u);
}

TEST(BodyText, LongOrOddLanguageTagIsDropped) {
    auto b = light_markdown("```averyveryverylongtag\nx\n```");
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0].lang, "");
    b = light_markdown("```lua title=\"x\"\nx\n```");
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0].lang, "");
}

TEST(BodyText, FoldedText) {
    wisp::ui::set_lang(wisp::ui::Lang::Zh);
    BodyBlock b;
    b.code = true;
    b.lines = 120;
    EXPECT_EQ(code_fold_text(b), "代码 · 120 行");
    b.lang = "lua";
    EXPECT_EQ(code_fold_text(b), "lua 代码 · 120 行");
    wisp::ui::set_lang(wisp::ui::Lang::En);
    EXPECT_EQ(code_fold_text(b), "lua code · 120 lines");
    wisp::ui::set_lang(wisp::ui::Lang::Zh);
}

// The text beyond kBodyMaxChars code points is left out, the cut marked
// with "…"; folded code does not count toward it.
TEST(BodyText, LongTextIsCapped) {
    std::string body;
    for (size_t i = 0; i < kBodyMaxChars + 500; ++i) body += "字";
    body += "\n尾";
    auto b = light_markdown(body);
    ASSERT_EQ(b.size(), 1u);
    const std::string t = str(b[0].text);
    EXPECT_EQ(t.size(), kBodyMaxChars * 3 + std::string("…").size());
    EXPECT_TRUE(t.ends_with("字…"));

    // Short text around a big code block is all there.
    std::string code = "头\n```\n";
    for (size_t i = 0; i < kBodyMaxChars; ++i) code += "x\n";
    code += "```\n尾";
    b = light_markdown(code);
    ASSERT_EQ(b.size(), 3u);
    EXPECT_EQ(b[1].lines, kBodyMaxChars);
    EXPECT_EQ(str(b[2].text), "尾");
}
