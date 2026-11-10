// html_to_text(): the "as":"text" conversion for http.fetch (agent-tools
// spec 3.1).

#include "wisp/agent/html_text.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "wisp/os/large.hpp"

namespace {

std::string text(std::string_view html) {
    const wisp::os::LargeString out = wisp::agent::html_to_text(html);
    return std::string(out.data(), out.size());
}

}  // namespace

TEST(HtmlText, StripsScriptStyleComments) {
    EXPECT_EQ(text("<p>Hello</p><script>var x=1;</script>"
                   "<style>.a{color:red}</style><!-- comment -->World"),
              "Hello\nWorld");
}

TEST(HtmlText, BlockTagsBecomeNewlines) {
    EXPECT_EQ(text("<div>A</div><div>B</div><p>C</p><br>D<li>E</li>"
                   "<h1>F</h1><h2>G</h2><tr>H</tr>"),
              "A\nB\nC\nD\nE\nF\nG\nH");
}

TEST(HtmlText, DecodesEntities) {
    EXPECT_EQ(text("&amp;&lt;&gt;&quot;&#39;&nbsp;&#x4F60;&#20320;"),
              "&<>\"' \xE4\xBD\xA0\xE4\xBD\xA0");
}

TEST(HtmlText, CollapsesWhitespace) { EXPECT_EQ(text("a   b\n\n\nc\t\td   \n  e"), "a b\nc d\ne"); }

TEST(HtmlText, Malformed) {
    // An unclosed raw-text element goes to the end of input (a half-
    // received page must not hang the conversion).
    EXPECT_EQ(text("before<script>var x = 1 < 2;"), "before");
    // The opening tag itself never closes.
    EXPECT_EQ(text("x<script"), "x");
    // An ordinary tag that never closes drops the rest of the input.
    EXPECT_EQ(text("y<div"), "y");
    // A numeric entity past the Unicode range becomes U+FFFD.
    EXPECT_EQ(text("a&#99999999;b"),
              "a\xEF\xBF\xBD"
              "b");
    // A stray "&" with nothing that completes an entity is kept literally.
    EXPECT_EQ(text("a&"), "a&");
    // Bytes that are not valid UTF-8 become one U+FFFD each, so the
    // result stays valid UTF-8.
    EXPECT_EQ(text("a\xff\xfe"
                   "b"),
              "a\xEF\xBF\xBD\xEF\xBF\xBD"
              "b");
}
