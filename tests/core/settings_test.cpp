#include "wisp/core/settings.hpp"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <optional>
#include <string>

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

TEST(Settings, EraseAllRemovesEveryNamespace) {
    auto path = temp_dir() / "s5.json";
    {
        wisp::port::sim::JsonFileSettingsBackend be(path.string());
        ASSERT_TRUE(be.set("display", "brightness", 128));
        ASSERT_TRUE(be.set("audio", "volume", 30));
        ASSERT_TRUE(be.set("net", "idle_off_min", 5));
        ASSERT_TRUE(be.erase_all());
        EXPECT_EQ(be.get("display", "brightness").error().code, wisp::ErrorCode::NotFound);
        EXPECT_EQ(be.get("audio", "volume").error().code, wisp::ErrorCode::NotFound);
        EXPECT_EQ(be.get("net", "idle_off_min").error().code, wisp::ErrorCode::NotFound);
        // The backend stays usable afterwards.
        ASSERT_TRUE(be.set("audio", "volume", 70));
        ASSERT_TRUE(be.erase_all());
    }
    {
        wisp::port::sim::JsonFileSettingsBackend be(path.string());
        EXPECT_EQ(be.get("display", "brightness").error().code, wisp::ErrorCode::NotFound);
        EXPECT_EQ(be.get("audio", "volume").error().code, wisp::ErrorCode::NotFound);
    }
}

TEST(Settings, EraseAllWriteFailureKeepsValues) {
    if (geteuid() == 0) {
        GTEST_SKIP() << "running as root: a read-only directory wouldn't block the write";
    }
    auto dir = temp_dir();
    wisp::port::sim::JsonFileSettingsBackend be((dir / "settings.json").string());
    ASSERT_TRUE(be.set("display", "brightness", 128));
    ASSERT_EQ(chmod(dir.string().c_str(), 0500), 0);
    wisp::Result<void> result = be.erase_all();
    chmod(dir.string().c_str(), 0700);
    ASSERT_EQ(result.error().code, wisp::ErrorCode::IoError);
    EXPECT_EQ(be.get("display", "brightness").value().as_int(), 128);
}

TEST(SettingsJsonFile, WriteBatchRewritesTheFileOnce) {
    auto path = temp_dir() / "s6.json";
    {
        wisp::port::sim::JsonFileSettingsBackend be(path.string());
        ASSERT_TRUE(be.set("audio", "volume", 30));
        const size_t before = be.file_writes();
        const wisp::core::SettingsChange changes[] = {
            {"display", "brightness", wisp::Value(128)},
            {"sys", "lang", wisp::Value("en")},
            {"audio", "volume", std::nullopt},
        };
        ASSERT_TRUE(be.write_batch(changes));
        EXPECT_EQ(be.file_writes(), before + 1);
        // A key that fails validation fails the batch before anything changes.
        const wisp::core::SettingsChange bad[] = {
            {"display", "brightness", wisp::Value(1)},
            {"display", "a_key_longer_than_15", wisp::Value(1)},
        };
        EXPECT_EQ(be.write_batch(bad).error().code, wisp::ErrorCode::InvalidArgument);
        EXPECT_EQ(be.file_writes(), before + 1);
    }
    wisp::port::sim::JsonFileSettingsBackend be(path.string());
    auto all = be.load_all();
    ASSERT_TRUE(all);
    std::map<std::string, wisp::Value> got;
    for (const auto& e : all.value()) got[e.ns + "/" + e.key] = e.value;
    EXPECT_EQ(got, (std::map<std::string, wisp::Value>{{"display/brightness", wisp::Value(128)},
                                                       {"sys/lang", wisp::Value("en")}}));
}

TEST(SettingsJsonFile, WriteBatchFailureKeepsValues) {
    if (geteuid() == 0) {
        GTEST_SKIP() << "running as root: a read-only directory wouldn't block the write";
    }
    auto dir = temp_dir();
    wisp::port::sim::JsonFileSettingsBackend be((dir / "settings.json").string());
    ASSERT_TRUE(be.set("display", "brightness", 128));
    ASSERT_EQ(chmod(dir.string().c_str(), 0500), 0);
    const wisp::core::SettingsChange changes[] = {
        {"display", "brightness", std::nullopt},
        {"sys", "lang", wisp::Value("en")},
    };
    wisp::Result<void> result = be.write_batch(changes);
    chmod(dir.string().c_str(), 0700);
    ASSERT_EQ(result.error().code, wisp::ErrorCode::IoError);
    EXPECT_EQ(be.get("display", "brightness").value().as_int(), 128);
    EXPECT_EQ(be.get("sys", "lang").error().code, wisp::ErrorCode::NotFound);
}

TEST(SettingsJsonFile, RefusesValuesOverTheLimit) {
    auto path = temp_dir() / "s7.json";
    wisp::port::sim::JsonFileSettingsBackend be(path.string());
    const std::string too_big(wisp::core::kSettingsMaxValueBytes - 1, 'a');
    EXPECT_EQ(be.set("agent", "prompt", too_big).error().code, wisp::ErrorCode::InvalidArgument);
    const wisp::core::SettingsChange changes[] = {{"agent", "prompt", wisp::Value(too_big)}};
    EXPECT_EQ(be.write_batch(changes).error().code, wisp::ErrorCode::InvalidArgument);
    EXPECT_EQ(be.get("agent", "prompt").error().code, wisp::ErrorCode::NotFound);
}
