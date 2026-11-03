// Tests for simulator command-line parsing.
// sim_options.hpp/.cpp live under simulator/ (not any wisp_* component --
// see that header's file comment), so this test compiles
// simulator/sim_options.cpp directly (see tests/CMakeLists.txt) rather
// than linking a library.

#include "sim_options.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "wisp/base/error.hpp"

TEST(SimOptions, ParsesAll) {
    const char* argv[] = {"wisp_sim", "--headless", "--screenshot", "o.png",     "--exit-after-ms",
                          "3000",     "--disable",  "imu",          "--disable", "audio_in",
                          "--no-sd",  "--data-dir", "/tmp/d"};
    auto o = parse_sim_options(13, argv).value();
    EXPECT_TRUE(o.headless);
    EXPECT_EQ(o.screenshot, "o.png");
    EXPECT_EQ(o.exit_after_ms, 3000u);
    EXPECT_EQ(o.disabled, (std::vector<std::string>{"imu", "audio_in"}));
    EXPECT_TRUE(o.no_sd);
    EXPECT_EQ(o.data_dir, "/tmp/d");
}

TEST(SimOptions, RejectsUnknownAndMissingValue) {
    const char* a1[] = {"wisp_sim", "--bogus"};
    EXPECT_NE(parse_sim_options(2, a1).error().message.find("--bogus"), std::string::npos);
    const char* a2[] = {"wisp_sim", "--screenshot"};
    EXPECT_EQ(parse_sim_options(2, a2).error().code, wisp::ErrorCode::InvalidArgument);
}

TEST(SimOptions, Shape) {
    const char* a1[] = {"wisp_sim"};
    EXPECT_EQ(parse_sim_options(1, a1).value().shape, "rounded_rect");
    const char* a2[] = {"wisp_sim", "--shape", "circle"};
    EXPECT_EQ(parse_sim_options(3, a2).value().shape, "circle");
    const char* a3[] = {"wisp_sim", "--shape", "hexagon"};
    EXPECT_NE(parse_sim_options(3, a3).error().message.find("hexagon"), std::string::npos);
}

TEST(SimOptions, DefaultsMatchSpec) {
    const char* argv[] = {"wisp_sim"};
    auto o = parse_sim_options(1, argv).value();
    EXPECT_FALSE(o.headless);
    EXPECT_TRUE(o.screenshot.empty());
    EXPECT_EQ(o.exit_after_ms, 0u);
    EXPECT_TRUE(o.disabled.empty());
    EXPECT_FALSE(o.no_sd);
    EXPECT_EQ(o.data_dir, "./sim-data");
}

TEST(SimOptions, DisableIsRepeatable) {
    const char* argv[] = {"wisp_sim", "--disable", "touch.main", "--disable", "storage.sd"};
    auto o = parse_sim_options(5, argv).value();
    EXPECT_EQ(o.disabled, (std::vector<std::string>{"touch.main", "storage.sd"}));
}

// --exit-after-ms must be a plain non-negative
// base-10 integer -- not "-5" (which a bare strtoul() would silently
// wrap around to a huge positive value) and not "123abc" (trailing
// garbage silently ignored).
TEST(SimOptions, ExitAfterMsValidation) {
    const char* ok[] = {"wisp_sim", "--exit-after-ms", "1500"};
    auto parsed_ok = parse_sim_options(3, ok);
    ASSERT_TRUE(parsed_ok);
    EXPECT_EQ(parsed_ok.value().exit_after_ms, 1500u);

    const char* negative[] = {"wisp_sim", "--exit-after-ms", "-5"};
    auto parsed_negative = parse_sim_options(3, negative);
    ASSERT_FALSE(parsed_negative);
    EXPECT_EQ(parsed_negative.error().code, wisp::ErrorCode::InvalidArgument);
    EXPECT_NE(parsed_negative.error().message.find("-5"), std::string::npos);

    const char* trailing_garbage[] = {"wisp_sim", "--exit-after-ms", "123abc"};
    auto parsed_garbage = parse_sim_options(3, trailing_garbage);
    ASSERT_FALSE(parsed_garbage);
    EXPECT_EQ(parsed_garbage.error().code, wisp::ErrorCode::InvalidArgument);
    EXPECT_NE(parsed_garbage.error().message.find("123abc"), std::string::npos);

    const char* empty[] = {"wisp_sim", "--exit-after-ms", ""};
    EXPECT_FALSE(parse_sim_options(3, empty));
}

TEST(SimOptions, ParsesNetFlags) {
    const char* defaults[] = {"wisp_sim"};
    auto d = parse_sim_options(1, defaults).value();
    EXPECT_EQ(d.http_port, 8900);
    EXPECT_TRUE(d.web_dir.empty());
    EXPECT_FALSE(d.wifi_fail);

    const char* argv[] = {"wisp_sim", "--http-port", "0", "--web-dir", "web", "--wifi-fail"};
    auto o = parse_sim_options(6, argv).value();
    EXPECT_EQ(o.http_port, 0);
    EXPECT_EQ(o.web_dir, "web");
    EXPECT_TRUE(o.wifi_fail);

    const char* custom[] = {"wisp_sim", "--http-port", "65535"};
    EXPECT_EQ(parse_sim_options(3, custom).value().http_port, 65535);

    const char* too_big[] = {"wisp_sim", "--http-port", "70000"};
    auto bad = parse_sim_options(3, too_big);
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, wisp::ErrorCode::InvalidArgument);
    EXPECT_NE(bad.error().message.find("70000"), std::string::npos);

    const char* negative[] = {"wisp_sim", "--http-port", "-1"};
    EXPECT_EQ(parse_sim_options(3, negative).error().code, wisp::ErrorCode::InvalidArgument);
    const char* missing[] = {"wisp_sim", "--web-dir"};
    EXPECT_EQ(parse_sim_options(2, missing).error().code, wisp::ErrorCode::InvalidArgument);
}

// Scripted touches drive the UI in headless runs (screenshots, end-to-end
// checks): taps and swipes at a time after boot, in the given order.
TEST(SimOptions, ParsesTouchScript) {
    const char* argv[] = {"wisp_sim", "--tap", "120,340@1500", "--swipe", "240,400,240,100@500",
                          "--tap",    "1,2@0"};
    auto o = parse_sim_options(7, argv).value();
    ASSERT_EQ(o.touches.size(), 3u);
    EXPECT_EQ(o.touches[0].kind, SimTouchStep::Kind::Tap);
    EXPECT_EQ(o.touches[0].at_ms, 1500u);
    EXPECT_EQ(o.touches[0].x1, 120);
    EXPECT_EQ(o.touches[0].y1, 340);
    EXPECT_EQ(o.touches[1].kind, SimTouchStep::Kind::Swipe);
    EXPECT_EQ(o.touches[1].at_ms, 500u);
    EXPECT_EQ(o.touches[1].x1, 240);
    EXPECT_EQ(o.touches[1].y1, 400);
    EXPECT_EQ(o.touches[1].x2, 240);
    EXPECT_EQ(o.touches[1].y2, 100);
    EXPECT_EQ(o.touches[2].at_ms, 0u);
}

TEST(SimOptions, RejectsMalformedTouches) {
    for (const char* bad :
         {"", "1,2", "1,2@", "@5", "1@5", "1,2,3@5", "-1,2@5", "1,2@x", "480,2@5", "1,2@5 "}) {
        const char* argv[] = {"wisp_sim", "--tap", bad};
        auto r = parse_sim_options(3, argv);
        ASSERT_FALSE(r) << bad;
        EXPECT_NE(r.error().message.find("--tap"), std::string::npos) << bad;
    }
    const char* argv[] = {"wisp_sim", "--swipe", "1,2@5"};
    EXPECT_FALSE(parse_sim_options(3, argv));
}

TEST(SimOptions, ParsesStdinTouchCommands) {
    // stdin is always read (sim_stdin.hpp); the old switch is gone.
    const char* argv[] = {"wisp_sim", "--touch-stdin"};
    EXPECT_FALSE(parse_sim_options(2, argv));

    auto tap = parse_touch_command("tap 188,298").value();
    EXPECT_EQ(tap.kind, SimTouchStep::Kind::Tap);
    EXPECT_EQ(tap.at_ms, 0u);
    EXPECT_EQ(tap.x1, 188);
    EXPECT_EQ(tap.y1, 298);
    EXPECT_EQ(tap.x2, 188);
    EXPECT_EQ(tap.y2, 298);
    auto tabbed = parse_touch_command("tap\t 188,298").value();
    EXPECT_EQ(tabbed.kind, SimTouchStep::Kind::Tap);
    EXPECT_EQ(tabbed.x1, 188);
    EXPECT_EQ(tabbed.y1, 298);
    auto swipe = parse_touch_command("swipe 240,400,240,100").value();
    EXPECT_EQ(swipe.kind, SimTouchStep::Kind::Swipe);
    EXPECT_EQ(swipe.x2, 240);
    EXPECT_EQ(swipe.y2, 100);

    for (const char* bad : {"", "tap", "tap ", "tap 1", "tap 1,2,3", "tap 1,2@5", "tap 480,1",
                            "swipe 1,2", "press 1,2", "TAP 1,2", " tap 1,2"}) {
        auto r = parse_touch_command(bad);
        ASSERT_FALSE(r) << bad;
        EXPECT_NE(r.error().message.find("invalid touch command"), std::string::npos) << bad;
    }
}
