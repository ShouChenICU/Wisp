// net::SseParser (wisp/net/llm_client.hpp).

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/net/llm_client.hpp"

using wisp::net::SseParser;

namespace {

struct Feeder {
    void feed(std::string_view s) {
        parser.feed({reinterpret_cast<const uint8_t*>(s.data()), s.size()},
                    [this](std::string_view d) { events.emplace_back(d); });
    }

    SseParser parser;
    std::vector<std::string> events;
};

TEST(SseParser, EventSplitAcrossChunks) {
    Feeder f;
    f.feed("da");
    f.feed("ta: {\"a\":1}\n");
    EXPECT_TRUE(f.events.empty());
    f.feed("\n");
    EXPECT_EQ(f.events, (std::vector<std::string>{"{\"a\":1}"}));
    EXPECT_FALSE(f.parser.overflowed());
}

TEST(SseParser, IgnoresCommentsAndCrLf) {
    Feeder f;
    f.feed(": ping\r\n\r\n");
    EXPECT_TRUE(f.events.empty());
    f.feed("data: x\r\n\r\n");
    EXPECT_EQ(f.events, (std::vector<std::string>{"x"}));
    // A CR at the end of one chunk and its LF at the start of the next is
    // still one line end.
    f.feed("data: y\r");
    f.feed("\n\r");
    f.feed("\n");
    EXPECT_EQ(f.events, (std::vector<std::string>{"x", "y"}));
    // Other fields and a field without a space after the colon.
    f.feed("event: message\nid: 7\nretry: 10\ndata:z\n\n");
    EXPECT_EQ(f.events, (std::vector<std::string>{"x", "y", "z"}));
}

TEST(SseParser, MultipleDataLinesJoinWithNewline) {
    Feeder f;
    f.feed("data: one\ndata: two\ndata:\n\ndata: three\n\n");
    EXPECT_EQ(f.events, (std::vector<std::string>{"one\ntwo\n", "three"}));
    // A blank line with no data before it dispatches nothing.
    f.feed("\n\n");
    EXPECT_EQ(f.events.size(), 2u);
}

TEST(SseParser, OversizedEventOverflows) {
    Feeder f;
    f.feed("data: ");
    std::string big(SseParser::kMaxEventBytes + 1, 'a');
    f.feed(big);
    EXPECT_TRUE(f.parser.overflowed());
    f.feed("\n\ndata: later\n\n");
    EXPECT_TRUE(f.events.empty());
    EXPECT_TRUE(f.parser.overflowed());
}

TEST(SseParser, EventJustUnderTheLimitPasses) {
    Feeder f;
    std::string payload(SseParser::kMaxEventBytes - 1, 'b');
    f.feed("data: " + payload + "\n\n");
    ASSERT_EQ(f.events.size(), 1u);
    EXPECT_EQ(f.events[0].size(), payload.size());
    EXPECT_FALSE(f.parser.overflowed());
}

}  // namespace
