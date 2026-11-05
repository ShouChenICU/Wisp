#include "wisp/core/system_settings.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "support/temp_dir.hpp"
#include "wisp/base/error.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/port/sim/settings_json_file.hpp"

using wisp::ErrorCode;
using wisp::core::Settings;
using wisp::core::SystemSettings;
using wisp::core::SystemSettingsValues;
using wisp::port::sim::JsonFileSettingsBackend;

namespace {

struct Fixture {
    Fixture()
        : backend((wisp::test::temp_dir() / "settings.json").string()),
          settings(
              backend, [this](uint8_t level) { applied.push_back(level); },
              [this](const SystemSettingsValues& now) { changed.push_back(now); }) {}

    JsonFileSettingsBackend backend;
    std::vector<uint8_t> applied;
    std::vector<SystemSettingsValues> changed;
    SystemSettings settings;
};

}  // namespace

TEST(SystemSettings, Defaults) {
    Fixture fx;
    auto v = fx.settings.get();
    EXPECT_EQ(v.brightness, 200);
    EXPECT_EQ(v.volume, 60);
    EXPECT_EQ(v.screen_off_s, 60);
    EXPECT_EQ(v.language, "zh");
    EXPECT_EQ(v.utc_offset_min, 480);
}

// The time zone as minutes east of UTC, stored as sys/tz_min: UTC-12:00
// to UTC+14:00, half and quarter hours included.
TEST(SystemSettings, UtcOffsetValidatesAndIsStored) {
    Fixture fx;
    for (int ok : {-720, -570, 0, 330, 345, 840}) {
        ASSERT_TRUE(fx.settings.set({{"utc_offset_min", ok}})) << ok;
        EXPECT_EQ(fx.settings.get().utc_offset_min, ok);
        EXPECT_EQ(Settings(fx.backend, "sys").get_int("tz_min", -1), ok);
    }
    EXPECT_EQ(fx.changed.back().utc_offset_min, 840);
    for (const wisp::Value& bad :
         {wisp::Value(-721), wisp::Value(841), wisp::Value("480"), wisp::Value(480.5),
          wisp::Value(true), wisp::Value(wisp::List{})}) {
        auto r = fx.settings.set({{"utc_offset_min", bad}, {"volume", 30}});
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    }
    EXPECT_EQ(fx.settings.get().utc_offset_min, 840);
    EXPECT_EQ(fx.settings.get().volume, 60);
    EXPECT_EQ(wisp::core::to_map(fx.settings.get()).at("utc_offset_min"), wisp::Value(840));
}

// A stored value set() would refuse (a hand-edited or corrupted store)
// reads back as the default, for integers as for the language; an int64
// past int's range is not truncated into a "valid" one either.
TEST(SystemSettings, OutOfRangeStoredValuesReadAsDefaults) {
    Fixture fx;
    ASSERT_TRUE(Settings(fx.backend, "sys").set("tz_min", int64_t{5000}));
    ASSERT_TRUE(Settings(fx.backend, "display").set("brightness", int64_t{4294967496}));
    ASSERT_TRUE(Settings(fx.backend, "audio").set("volume", int64_t{-5}));
    ASSERT_TRUE(Settings(fx.backend, "power").set("screen_off_s", int64_t{5}));
    auto v = fx.settings.get();
    EXPECT_EQ(v.utc_offset_min, 480);
    EXPECT_EQ(v.brightness, 200);
    EXPECT_EQ(v.volume, 60);
    EXPECT_EQ(v.screen_off_s, 60);
}

TEST(SystemSettings, WritesTheExistingSettingsKeys) {
    Fixture fx;
    ASSERT_TRUE(fx.settings.set({{"brightness", 120}, {"volume", 30}, {"screen_off_s", 300}}));
    EXPECT_EQ(Settings(fx.backend, "display").get_int("brightness", -1), 120);
    EXPECT_EQ(Settings(fx.backend, "audio").get_int("volume", -1), 30);
    EXPECT_EQ(Settings(fx.backend, "power").get_int("screen_off_s", -1), 300);
    auto v = fx.settings.get();
    EXPECT_EQ(v.brightness, 120);
    EXPECT_EQ(v.volume, 30);
    EXPECT_EQ(v.screen_off_s, 300);
}

TEST(SystemSettings, ValidatesWholeBatchAtomically) {
    Fixture fx;
    auto r = fx.settings.set({{"brightness", 100}, {"volume", 101}});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(fx.settings.get().brightness, 200);
    EXPECT_TRUE(fx.applied.empty());

    auto unknown = fx.settings.set({{"brightness", 100}, {"contrast", 5}});
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(fx.settings.get().brightness, 200);
    EXPECT_TRUE(fx.applied.empty());
}

TEST(SystemSettings, RangeBoundaries) {
    Fixture fx;
    EXPECT_FALSE(fx.settings.set({{"brightness", 9}}));
    EXPECT_TRUE(fx.settings.set({{"brightness", 10}}));
    EXPECT_TRUE(fx.settings.set({{"brightness", 255}}));
    EXPECT_FALSE(fx.settings.set({{"brightness", 256}}));
    EXPECT_FALSE(fx.settings.set({{"volume", -1}}));
    EXPECT_TRUE(fx.settings.set({{"volume", 0}}));
    EXPECT_TRUE(fx.settings.set({{"volume", 100}}));
    EXPECT_TRUE(fx.settings.set({{"screen_off_s", 3600}}));
    EXPECT_FALSE(fx.settings.set({{"screen_off_s", 3601}}));
    EXPECT_FALSE(fx.settings.set({{"screen_off_s", -1}}));
}

TEST(SystemSettings, LanguageDefaultsToZhAndValidates) {
    Fixture fx;
    EXPECT_EQ(fx.settings.get().language, "zh");

    ASSERT_TRUE(fx.settings.set({{"language", "ja"}}));
    EXPECT_EQ(fx.settings.get().language, "ja");
    EXPECT_EQ(Settings(fx.backend, "sys").get_string("lang", ""), "ja");
    ASSERT_TRUE(fx.settings.set({{"language", "en"}, {"volume", 20}}));
    EXPECT_EQ(fx.settings.get().language, "en");

    for (const wisp::Value& bad : {wisp::Value("fr"), wisp::Value(""), wisp::Value("ZH"),
                                   wisp::Value("zh-CN"), wisp::Value(1), wisp::Value(true)}) {
        auto r = fx.settings.set({{"language", bad}, {"volume", 30}});
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    }
    EXPECT_EQ(fx.settings.get().language, "en");
    EXPECT_EQ(fx.settings.get().volume, 20);

    // A stored value nobody could have written through set() (a hand-edited
    // or corrupted settings store) reads back as the default.
    ASSERT_TRUE(Settings(fx.backend, "sys").set("lang", "fr"));
    EXPECT_EQ(fx.settings.get().language, "zh");
}

TEST(SystemSettings, IntegerKeysRejectOtherTypes) {
    Fixture fx;
    for (const wisp::Value& bad :
         {wisp::Value("30"), wisp::Value(30.5), wisp::Value(true), wisp::Value(wisp::List{})}) {
        auto r = fx.settings.set({{"volume", bad}});
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    }
    EXPECT_EQ(fx.settings.get().volume, 60);
}

TEST(SystemSettings, ReportsEachStoredBatch) {
    Fixture fx;
    ASSERT_TRUE(fx.settings.set({{"language", "ja"}}));
    ASSERT_EQ(fx.changed.size(), 1u);
    EXPECT_EQ(fx.changed[0].language, "ja");
    EXPECT_EQ(fx.changed[0].brightness, 200);

    // Rejected and empty batches change nothing, so report nothing.
    EXPECT_FALSE(fx.settings.set({{"language", "fr"}}));
    EXPECT_TRUE(fx.settings.set({}));
    EXPECT_EQ(fx.changed.size(), 1u);

    ASSERT_TRUE(fx.settings.set({{"brightness", 90}}));
    ASSERT_EQ(fx.changed.size(), 2u);
    EXPECT_EQ(fx.changed[1].brightness, 90);
    EXPECT_EQ(fx.changed[1].language, "ja");
}

// revision() lets a poller notice a change without reading storage: it
// moves on every stored batch and on nothing else.
TEST(SystemSettings, RevisionMovesOnEachStoredBatch) {
    Fixture fx;
    uint32_t r0 = fx.settings.revision();
    ASSERT_TRUE(fx.settings.set({{"language", "en"}}));
    uint32_t r1 = fx.settings.revision();
    EXPECT_NE(r1, r0);

    EXPECT_FALSE(fx.settings.set({{"language", "fr"}}));
    EXPECT_TRUE(fx.settings.set({}));
    EXPECT_EQ(fx.settings.revision(), r1);

    ASSERT_TRUE(fx.settings.set({{"volume", 10}}));
    EXPECT_NE(fx.settings.revision(), r1);
}

TEST(SystemSettings, AppliesBrightnessImmediately) {
    Fixture fx;
    ASSERT_TRUE(fx.settings.set({{"brightness", 100}}));
    ASSERT_EQ(fx.applied.size(), 1u);
    EXPECT_EQ(fx.applied[0], 100);

    // A batch without brightness does not touch the panel.
    ASSERT_TRUE(fx.settings.set({{"volume", 10}}));
    EXPECT_EQ(fx.applied.size(), 1u);
}

TEST(SystemSettings, ScreenOffZeroMeansNever) {
    Fixture fx;
    EXPECT_TRUE(fx.settings.set({{"screen_off_s", 0}}));
    EXPECT_EQ(fx.settings.get().screen_off_s, 0);
    auto r = fx.settings.set({{"screen_off_s", 10}});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(fx.settings.get().screen_off_s, 0);
    EXPECT_TRUE(fx.settings.set({{"screen_off_s", 15}}));
    EXPECT_EQ(fx.settings.get().screen_off_s, 15);
}

namespace {

// Delegates to a real backend but fails the Nth set() call, to check that a
// storage failure partway through a batch leaves no partial write behind.
class FlakyBackend : public wisp::core::SettingsBackend {
public:
    FlakyBackend(wisp::core::SettingsBackend& inner, int fail_on_set)
        : inner_(inner), fail_on_set_(fail_on_set) {}

    wisp::Result<wisp::Value> get(const std::string& ns, const std::string& key) override {
        return inner_.get(ns, key);
    }
    wisp::Result<void> set(const std::string& ns, const std::string& key,
                           const wisp::Value& value) override {
        if (++sets_ == fail_on_set_) {
            return wisp::Result<void>::err(ErrorCode::IoError, "injected");
        }
        return inner_.set(ns, key, value);
    }
    wisp::Result<void> erase(const std::string& ns, const std::string& key) override {
        return inner_.erase(ns, key);
    }
    wisp::Result<void> erase_all() override { return inner_.erase_all(); }

private:
    wisp::core::SettingsBackend& inner_;
    int fail_on_set_;
    int sets_ = 0;
};

}  // namespace

TEST(SystemSettings, StorageFailureMidBatchRollsBack) {
    JsonFileSettingsBackend inner((wisp::test::temp_dir() / "settings.json").string());
    ASSERT_TRUE(Settings(inner, "audio").set("volume", 40));
    FlakyBackend flaky(inner, /*fail_on_set=*/3);
    std::vector<uint8_t> applied;
    SystemSettings settings(flaky, [&](uint8_t level) { applied.push_back(level); });
    uint32_t revision = settings.revision();

    // Written in key order: brightness, language, then screen_off_s fails.
    auto r = settings.set(
        {{"brightness", 100}, {"language", "en"}, {"volume", 10}, {"screen_off_s", 30}});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::IoError);
    auto v = settings.get();
    EXPECT_EQ(v.brightness, 200);
    EXPECT_EQ(v.volume, 40);
    EXPECT_EQ(v.screen_off_s, 60);
    EXPECT_EQ(v.language, "zh");
    EXPECT_TRUE(applied.empty());
    EXPECT_EQ(settings.revision(), revision);
    // Keys that never existed are removed again, not left at a default.
    EXPECT_EQ(inner.get("display", "brightness").error().code, ErrorCode::NotFound);
    EXPECT_EQ(inner.get("sys", "lang").error().code, ErrorCode::NotFound);
}
