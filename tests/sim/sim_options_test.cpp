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
    EXPECT_EQ(d.http_port, 8080);
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
