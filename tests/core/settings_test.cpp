#include "wisp/core/settings.hpp"

#include <gtest/gtest.h>

#include "support/temp_dir.hpp"
#include "wisp/base/error.hpp"
#include "wisp/port/sim/settings_json_file.hpp"

using wisp::test::temp_dir;

TEST(Settings, DefaultsAndRoundTripPersist) {
    auto path = temp_dir() / "settings.json";
    {
        wisp::port::sim::JsonFileSettingsBackend be(path.string());
        wisp::core::Settings s(be, "display");
        EXPECT_EQ(s.get_int("brightness", 200), 200);
        ASSERT_TRUE(s.set("brightness", 128));
    }
    {
        wisp::port::sim::JsonFileSettingsBackend be(path.string());
        wisp::core::Settings s(be, "display");
        EXPECT_EQ(s.get_int("brightness", 200), 128);
    }
}

TEST(Settings, NameLengthLimit) {
    auto path = temp_dir() / "s2.json";
    wisp::port::sim::JsonFileSettingsBackend be(path.string());
    EXPECT_EQ(be.set("display", "a_key_longer_than_15", 1).error().code,
              wisp::ErrorCode::InvalidArgument);
    EXPECT_EQ(be.set("namespace_too_long", "k", 1).error().code, wisp::ErrorCode::InvalidArgument);
    EXPECT_TRUE(be.set("display", "exactly15chars_", 1));
}

TEST(Settings, TypeMismatchReturnsDefault) {
    auto path = temp_dir() / "s3.json";
    wisp::port::sim::JsonFileSettingsBackend be(path.string());
    wisp::core::Settings s(be, "audio");
    s.set("volume", "loud");
    EXPECT_EQ(s.get_int("volume", 60), 60);
}
