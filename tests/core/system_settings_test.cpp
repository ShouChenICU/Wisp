#include "wisp/core/system_settings.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <string>
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
    EXPECT_EQ(v.ntp_servers, "pool.ntp.org,ntp.aliyun.com");
}

// The time servers, stored as sys/ntp: one or two host names or IPv4
// addresses separated by a comma, each 1-63 characters of [A-Za-z0-9.-].
TEST(SystemSettings, NtpServersValidateAndAreStored) {
    Fixture fx;
    for (const char* ok : {"time.example.com", "192.168.1.1", "a,b", "ntp-1.example.org,10.0.0.1",
                           "UPPER.Example.COM"}) {
        ASSERT_TRUE(fx.settings.set({{"ntp_servers", ok}})) << ok;
        EXPECT_EQ(fx.settings.get().ntp_servers, ok);
        EXPECT_EQ(Settings(fx.backend, "sys").get_string("ntp", ""), ok);
    }
    EXPECT_EQ(fx.changed.back().ntp_servers, "UPPER.Example.COM");
    const std::string long64(64, 'a');
    const std::string long63(63, 'a');
    ASSERT_TRUE(fx.settings.set({{"ntp_servers", long63 + "," + long63}}));
    for (const wisp::Value& bad :
         {wisp::Value(""), wisp::Value(","), wisp::Value("a,"), wisp::Value(",a"),
          wisp::Value("a,b,c"), wisp::Value("a b"), wisp::Value("a, b"), wisp::Value("a_b"),
          wisp::Value("a:123"), wisp::Value("[::1]"), wisp::Value(long64),
          wisp::Value("a," + long64), wisp::Value(1), wisp::Value(true),
          wisp::Value(wisp::List{})}) {
        auto r = fx.settings.set({{"ntp_servers", bad}, {"volume", 30}});
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    }
    EXPECT_EQ(fx.settings.get().ntp_servers, long63 + "," + long63);
    EXPECT_EQ(fx.settings.get().volume, 60);
    EXPECT_EQ(wisp::core::to_map(fx.settings.get()).at("ntp_servers"),
              wisp::Value(long63 + "," + long63));
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
    ASSERT_TRUE(Settings(fx.backend, "sys").set("ntp", "a b,c"));
    auto v = fx.settings.get();
    EXPECT_EQ(v.ntp_servers, "pool.ntp.org,ntp.aliyun.com");
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
    wisp::Result<std::vector<wisp::core::SettingsEntry>> load_all() override {
        return inner_.load_all();
    }
    wisp::Result<void> write_batch(std::span<const wisp::core::SettingsChange> changes) override {
        return inner_.write_batch(changes);
    }

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

// spec 4.1 and 5.4: power/deep_after_s (0 never, else 60-3600, default
// 300), power/light_sleep (a boolean, default true) and audio/notify_vol
// (0-100, default 60).
TEST(SystemSettings, PowerAndNotifyKeys) {
    Fixture fx;
    auto v = fx.settings.get();
    EXPECT_EQ(v.deep_after_s, 300);
    EXPECT_TRUE(v.light_sleep);
    EXPECT_EQ(v.notify_vol, 60);

    for (int ok : {0, 60, 900, 3600}) {
        ASSERT_TRUE(fx.settings.set({{"deep_after_s", ok}})) << ok;
        EXPECT_EQ(fx.settings.get().deep_after_s, ok);
        EXPECT_EQ(Settings(fx.backend, "power").get_int("deep_after_s", -1), ok);
    }
    for (const wisp::Value& bad : {wisp::Value(59), wisp::Value(3601), wisp::Value(-1),
                                   wisp::Value(1), wisp::Value("300"), wisp::Value(true)}) {
        auto r = fx.settings.set({{"deep_after_s", bad}});
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    }
    EXPECT_EQ(fx.settings.get().deep_after_s, 3600);

    ASSERT_TRUE(fx.settings.set({{"light_sleep", false}}));
    EXPECT_FALSE(fx.settings.get().light_sleep);
    EXPECT_FALSE(Settings(fx.backend, "power").get_bool("light_sleep", true));
    EXPECT_FALSE(fx.changed.back().light_sleep);
    for (const wisp::Value& bad :
         {wisp::Value(1), wisp::Value(0), wisp::Value("true"), wisp::Value(wisp::List{})}) {
        auto r = fx.settings.set({{"light_sleep", bad}});
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    }
    EXPECT_FALSE(fx.settings.get().light_sleep);
    ASSERT_TRUE(fx.settings.set({{"light_sleep", true}}));
    EXPECT_TRUE(fx.settings.get().light_sleep);

    for (int ok : {0, 35, 100}) {
        ASSERT_TRUE(fx.settings.set({{"notify_vol", ok}})) << ok;
        EXPECT_EQ(fx.settings.get().notify_vol, ok);
        EXPECT_EQ(Settings(fx.backend, "audio").get_int("notify_vol", -1), ok);
    }
    for (const wisp::Value& bad :
         {wisp::Value(-1), wisp::Value(101), wisp::Value("50"), wisp::Value(false)}) {
        auto r = fx.settings.set({{"notify_vol", bad}});
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    }
    EXPECT_EQ(fx.settings.get().notify_vol, 100);

    // Stored values set() would refuse read back as the defaults.
    ASSERT_TRUE(Settings(fx.backend, "power").set("deep_after_s", int64_t{30}));
    ASSERT_TRUE(Settings(fx.backend, "power").set("light_sleep", int64_t{1}));
    ASSERT_TRUE(Settings(fx.backend, "audio").set("notify_vol", int64_t{500}));
    v = fx.settings.get();
    EXPECT_EQ(v.deep_after_s, 300);
    EXPECT_TRUE(v.light_sleep);
    EXPECT_EQ(v.notify_vol, 60);

    auto map = wisp::core::to_map(v);
    EXPECT_EQ(map.at("deep_after_s"), wisp::Value(300));
    EXPECT_EQ(map.at("light_sleep"), wisp::Value(true));
    EXPECT_EQ(map.at("notify_vol"), wisp::Value(60));
}

TEST(SystemSettings, TtsMuteStoredUnderVoice) {
    Fixture fx;
    EXPECT_FALSE(fx.settings.get().tts_mute);

    ASSERT_TRUE(fx.settings.set({{"tts_mute", true}}));
    EXPECT_TRUE(fx.settings.get().tts_mute);
    EXPECT_TRUE(Settings(fx.backend, "voice").get_bool("tts_mute", false));
    EXPECT_EQ(wisp::core::to_map(fx.settings.get()).at("tts_mute"), wisp::Value(true));

    ASSERT_TRUE(fx.settings.set({{"tts_mute", false}}));
    EXPECT_FALSE(fx.settings.get().tts_mute);

    for (const wisp::Value& bad :
         {wisp::Value("yes"), wisp::Value(1), wisp::Value(0), wisp::Value(wisp::List{})}) {
        auto r = fx.settings.set({{"tts_mute", bad}});
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    }
    EXPECT_FALSE(fx.settings.get().tts_mute);
}

// The agent's per-turn limits (agent-core spec 3.6): agent/max_steps
// 8-200 tool calls (default 60) and agent/turn_min 2-60 minutes (15).
TEST(SystemSettings, AgentLimitsStoredUnderAgent) {
    Fixture fx;
    auto v = fx.settings.get();
    EXPECT_EQ(v.agent_max_steps, 60);
    EXPECT_EQ(v.agent_turn_min, 15);

    ASSERT_TRUE(fx.settings.set({{"agent_max_steps", 8}, {"agent_turn_min", 60}}));
    v = fx.settings.get();
    EXPECT_EQ(v.agent_max_steps, 8);
    EXPECT_EQ(v.agent_turn_min, 60);
    EXPECT_EQ(Settings(fx.backend, "agent").get_int("max_steps", 0), 8);
    EXPECT_EQ(Settings(fx.backend, "agent").get_int("turn_min", 0), 60);
    auto m = wisp::core::to_map(v);
    EXPECT_EQ(m.at("agent_max_steps"), wisp::Value(8));
    EXPECT_EQ(m.at("agent_turn_min"), wisp::Value(60));

    ASSERT_TRUE(fx.settings.set({{"agent_max_steps", 200}, {"agent_turn_min", 2}}));
    for (const auto& [key, bad] : std::vector<std::pair<std::string, wisp::Value>>{
             {"agent_max_steps", wisp::Value(7)},
             {"agent_max_steps", wisp::Value(201)},
             {"agent_max_steps", wisp::Value("60")},
             {"agent_turn_min", wisp::Value(1)},
             {"agent_turn_min", wisp::Value(61)},
             {"agent_turn_min", wisp::Value(true)},
         }) {
        auto r = fx.settings.set({{key, bad}});
        ASSERT_FALSE(r) << key;
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    }
    v = fx.settings.get();
    EXPECT_EQ(v.agent_max_steps, 200);
    EXPECT_EQ(v.agent_turn_min, 2);
}

TEST(SystemSettings, AgentLimitsOutOfRangeInStoreReadAsDefaults) {
    Fixture fx;
    ASSERT_TRUE(Settings(fx.backend, "agent").set("max_steps", int64_t{5000}));
    ASSERT_TRUE(Settings(fx.backend, "agent").set("turn_min", "ten"));
    auto v = fx.settings.get();
    EXPECT_EQ(v.agent_max_steps, 60);
    EXPECT_EQ(v.agent_turn_min, 15);
}
