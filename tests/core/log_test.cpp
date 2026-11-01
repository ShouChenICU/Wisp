#include "wisp/core/log.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <string>
#include <vector>

namespace {

struct CaptureSink : wisp::LogSink {
    std::vector<std::string> lines;
    void write(wisp::LogLevel, const char* tag, const char* msg) override {
        lines.push_back(std::string(tag) + "|" + msg);
    }
};

}  // namespace

TEST(Log, FiltersByLevelAndFormats) {
    wisp::log::reset_for_test();
    wisp::log::init(1024);
    CaptureSink s;
    wisp::log::add_sink(&s);
    wisp::log::set_level(wisp::LogLevel::Info);

    WLOG_D("t", "hidden");
    WLOG_I("t", "v=%d", 3);

    ASSERT_EQ(s.lines.size(), 1u);
    EXPECT_EQ(s.lines[0], "t|v=3");
    EXPECT_NE(wisp::log::ring_snapshot().find(" I t: v=3\n"), std::string::npos);
}

TEST(Log, RingDropsOldestWholeLines) {
    wisp::log::reset_for_test();
    wisp::log::init(200);

    for (int i = 0; i < 50; ++i) {
        WLOG_I("t", "line-%02d", i);
    }

    auto snap = wisp::log::ring_snapshot();
    EXPECT_LE(snap.size(), 200u);
    EXPECT_NE(snap.find("line-49"), std::string::npos);
    EXPECT_EQ(snap.find("line-00"), std::string::npos);
    EXPECT_EQ(snap.back(), '\n');
    EXPECT_TRUE(isdigit(static_cast<unsigned char>(snap.front())));  // first line intact
}

TEST(Log, TruncatesLongMessages) {
    wisp::log::reset_for_test();
    wisp::log::init(4096);
    CaptureSink s;
    wisp::log::add_sink(&s);

    WLOG_W("t", "%s", std::string(1000, 'x').c_str());

    EXPECT_EQ(s.lines[0].size(), 2u + 255u);
}
