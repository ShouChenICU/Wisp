#include "wisp/core/settings.hpp"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

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

// "温度湿度设置" is 6 CJK characters, 3 bytes each in UTF-8 = 18 bytes, so
// it must be rejected the same way an 18-byte ASCII name would be (NVS
// counts bytes, not code points, so the simulator must match). "温度湿度设"
// is 5 characters = 15 bytes, exactly at the limit, so it's accepted.
TEST(Settings, MultiByteUtf8NameCountsBytesNotCodePoints) {
    auto path = temp_dir() / "s4.json";
    wisp::port::sim::JsonFileSettingsBackend be(path.string());
    EXPECT_EQ(be.set("display", "温度湿度设置", 1).error().code, wisp::ErrorCode::InvalidArgument);
    EXPECT_TRUE(be.set("display", "温度湿度设", 1));
}

TEST(Settings, SetWriteFailureLeavesStoreUnchanged) {
    // file_path_'s parent directory doesn't exist, so write_through()'s
    // temp file can never be created: set() must fail with IoError and
    // must not have recorded the value in memory either.
    auto path = temp_dir() / "missing_subdir" / "settings.json";
    wisp::port::sim::JsonFileSettingsBackend be(path.string());

    EXPECT_EQ(be.set("display", "brightness", 128).error().code, wisp::ErrorCode::IoError);
    EXPECT_EQ(be.get("display", "brightness").error().code, wisp::ErrorCode::NotFound);
}

TEST(Settings, EraseWriteFailureRestoresPreviousValue) {
    if (geteuid() == 0) {
        GTEST_SKIP() << "running as root: a read-only directory wouldn't block the write";
    }

    auto dir = temp_dir();
    auto path = dir / "settings.json";
    wisp::port::sim::JsonFileSettingsBackend be(path.string());
    ASSERT_TRUE(be.set("display", "brightness", 128));

    // Drop write permission on the directory so write_through() can't
    // create its "<file>.tmp" file; erase() must then fail with IoError
    // and leave the in-memory value as it was.
    ASSERT_EQ(chmod(dir.string().c_str(), 0500), 0);
    wisp::Result<void> result = be.erase("display", "brightness");
    chmod(dir.string().c_str(), 0700);  // restore before temp_dir()'s cleanup runs

    ASSERT_EQ(result.error().code, wisp::ErrorCode::IoError);
    auto got = be.get("display", "brightness");
    ASSERT_TRUE(got);
    EXPECT_EQ(got.value().as_int(), 128);
}
