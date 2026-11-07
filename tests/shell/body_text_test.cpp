// The home screen's light Markdown (wisp/shell/body_text.hpp, mvp spec 6.2).

#include "wisp/shell/body_text.hpp"

#include <gtest/gtest.h>

#include <string>

using wisp::shell::BodyBlock;
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
    {
        auto b = light_markdown("前\n```lua\nx = **1**\n- y\n```\n后");
        ASSERT_EQ(b.size(), 3u);
        EXPECT_EQ(str(b[0].text), "前");
        EXPECT_FALSE(b[0].code);
        // Code stays as it is.
        EXPECT_EQ(str(b[1].text), "x = **1**\n- y");
        EXPECT_TRUE(b[1].code);
        EXPECT_EQ(str(b[2].text), "后");
        EXPECT_FALSE(b[2].code);
    }
    {
        // Still streaming: the open fence runs to the end.
        auto b = light_markdown("看：\n```\nx");
        ASSERT_EQ(b.size(), 2u);
        EXPECT_EQ(str(b[1].text), "x");
        EXPECT_TRUE(b[1].code);
    }
    EXPECT_TRUE(light_markdown("").empty());
    EXPECT_TRUE(light_markdown("\n \n").empty());
    // Blank lines inside a text block stay; around it they go.
    auto b = light_markdown("\na\n\nb\n\n");
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(str(b[0].text), "a\n\nb");
}
