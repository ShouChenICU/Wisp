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
using wisp::port::sim::JsonFileSettingsBackend;

namespace {

struct Fixture {
    Fixture()
        : backend((wisp::test::temp_dir() / "settings.json").string()),
          settings(backend, [this](uint8_t level) { applied.push_back(level); }) {}

    JsonFileSettingsBackend backend;
    std::vector<uint8_t> applied;
    SystemSettings settings;
};

}  // namespace

TEST(SystemSettings, Defaults) {
    Fixture fx;
    auto v = fx.settings.get();
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

    auto r = settings.set({{"brightness", 100}, {"volume", 10}, {"screen_off_s", 30}});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::IoError);
    auto v = settings.get();
    EXPECT_EQ(v.brightness, 200);
    EXPECT_EQ(v.volume, 40);
    EXPECT_EQ(v.screen_off_s, 60);
    EXPECT_TRUE(applied.empty());
    // Keys that never existed are removed again, not left at a default.
    EXPECT_EQ(inner.get("display", "brightness").error().code, ErrorCode::NotFound);
}
