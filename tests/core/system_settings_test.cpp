#include "wisp/core/system_settings.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <ostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "support/bus_flood.hpp"
#include "support/temp_dir.hpp"
#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/topics.hpp"
#include "wisp/port/sim/settings_json_file.hpp"

// Readable gtest failure output for Value and Map comparisons.
namespace wisp {
void PrintTo(const Value& v, std::ostream* os) { *os << to_json(v); }
}  // namespace wisp

using wisp::ErrorCode;
using wisp::Map;
using wisp::Value;
using wisp::core::Event;
using wisp::core::EventBus;
using wisp::core::Settings;
using wisp::core::SystemSettings;
using wisp::core::SystemSettingsValues;
using wisp::port::sim::JsonFileSettingsBackend;

namespace {

// Records every system.settings event on a started bus. The recorder is
// declared before the bus, which is declared before the settings: the
// settings (and their cell) go first, then the bus stops, then the record.
struct Recorder {
    void attach(EventBus& bus) {
        bus.start();
        bus.subscribe(wisp::core::topics::kSystemSettings, [this](const Event& e) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(e);
        });
    }
    // The events so far, once the bus has delivered everything queued.
    std::vector<Event> all(EventBus& bus) {
        EXPECT_TRUE(bus.wait_idle(2000));
        std::lock_guard<std::mutex> lock(mutex);
        return events;
    }

    std::mutex mutex;
    std::vector<Event> events;
};

struct Fixture {
    Fixture()
        : backend((wisp::test::temp_dir() / "settings.json").string()),
          settings(backend, bus, [this](uint8_t level) { applied.push_back(level); }) {
        recorder.attach(bus);
    }

    // The payloads of the system.settings events published so far.
    std::vector<Map> changed() {
        std::vector<Map> out;
        for (const Event& e : recorder.all(bus)) out.push_back(e.payload);
        return out;
    }

    JsonFileSettingsBackend backend;
    std::vector<uint8_t> applied;
    Recorder recorder;
    EventBus bus;
    SystemSettings settings;
};

}  // namespace

TEST(SystemSettings, Defaults) {
    Fixture fx;
    auto v = fx.settings.get();
    EXPECT_EQ(v.brightness, 200);
    EXPECT_EQ(v.volume, 60);
    EXPECT_EQ(v.screen_off_s, 60);
    EXPECT_EQ(v.language, wisp::core::LangCode::Zh);
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
    EXPECT_EQ(fx.changed().back().at("ntp_servers"), Value("UPPER.Example.COM"));
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
    EXPECT_EQ(fx.changed().back().at("utc_offset_min"), Value(840));
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
    EXPECT_EQ(fx.settings.get().language, wisp::core::LangCode::Zh);

    ASSERT_TRUE(fx.settings.set({{"language", "ja"}}));
    EXPECT_EQ(fx.settings.get().language, wisp::core::LangCode::Ja);
    EXPECT_EQ(Settings(fx.backend, "sys").get_string("lang", ""), "ja");
    ASSERT_TRUE(fx.settings.set({{"language", "en"}, {"volume", 20}}));
    EXPECT_EQ(fx.settings.get().language, wisp::core::LangCode::En);

    for (const wisp::Value& bad : {wisp::Value("fr"), wisp::Value(""), wisp::Value("ZH"),
                                   wisp::Value("zh-CN"), wisp::Value(1), wisp::Value(true)}) {
        auto r = fx.settings.set({{"language", bad}, {"volume", 30}});
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    }
    EXPECT_EQ(fx.settings.get().language, wisp::core::LangCode::En);
    EXPECT_EQ(fx.settings.get().volume, 20);

    // A stored value nobody could have written through set() (a hand-edited
    // or corrupted settings store) reads back as the default.
    ASSERT_TRUE(Settings(fx.backend, "sys").set("lang", "fr"));
    EXPECT_EQ(fx.settings.get().language, wisp::core::LangCode::Zh);
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
    auto changed = fx.changed();
    ASSERT_EQ(changed.size(), 1u);
    EXPECT_EQ(changed[0].at("language"), Value("ja"));
    EXPECT_EQ(changed[0].at("brightness"), Value(200));

    // Rejected and empty batches change nothing, so report nothing.
    EXPECT_FALSE(fx.settings.set({{"language", "fr"}}));
    EXPECT_TRUE(fx.settings.set({}));
    EXPECT_EQ(fx.changed().size(), 1u);

    ASSERT_TRUE(fx.settings.set({{"brightness", 90}}));
    changed = fx.changed();
    ASSERT_EQ(changed.size(), 2u);
    EXPECT_EQ(changed[1].at("brightness"), Value(90));
    EXPECT_EQ(changed[1].at("language"), Value("ja"));
}

// Every stored batch that changes a value moves the cell and publishes
// once; a rejected or an empty batch moves nothing and publishes nothing.
TEST(SystemSettings, CellMovesOnEachStoredBatch) {
    Fixture fx;
    uint32_t r0 = fx.settings.cell().revision();
    ASSERT_TRUE(fx.settings.set({{"language", "en"}}));
    uint32_t r1 = fx.settings.cell().revision();
    EXPECT_NE(r1, r0);
    EXPECT_EQ(fx.changed().size(), 1u);

    EXPECT_FALSE(fx.settings.set({{"language", "fr"}}));
    EXPECT_TRUE(fx.settings.set({}));
    EXPECT_EQ(fx.settings.cell().revision(), r1);
    EXPECT_EQ(fx.changed().size(), 1u);

    ASSERT_TRUE(fx.settings.set({{"volume", 10}}));
    EXPECT_NE(fx.settings.cell().revision(), r1);
    EXPECT_EQ(fx.changed().size(), 2u);
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
// With `fail_after`, every set() and erase() from the Nth on fails too, so
// the batch's rollback fails as well.
class FlakyBackend : public wisp::core::SettingsBackend {
public:
    FlakyBackend(wisp::core::SettingsBackend& inner, int fail_on_set, bool fail_after = false)
        : inner_(inner), fail_on_set_(fail_on_set), fail_after_(fail_after) {}

    wisp::Result<wisp::Value> get(const std::string& ns, const std::string& key) override {
        return inner_.get(ns, key);
    }
    wisp::Result<void> set(const std::string& ns, const std::string& key,
                           const wisp::Value& value) override {
        if (++sets_ == fail_on_set_ || failing()) {
            return wisp::Result<void>::err(ErrorCode::IoError, "injected");
        }
        return inner_.set(ns, key, value);
    }
    wisp::Result<void> erase(const std::string& ns, const std::string& key) override {
        if (failing()) return wisp::Result<void>::err(ErrorCode::IoError, "injected");
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
    bool failing() const { return fail_after_ && sets_ >= fail_on_set_; }

    wisp::core::SettingsBackend& inner_;
    int fail_on_set_;
    bool fail_after_;
    int sets_ = 0;
};

}  // namespace

TEST(SystemSettings, StorageFailureMidBatchRollsBack) {
    JsonFileSettingsBackend inner((wisp::test::temp_dir() / "settings.json").string());
    ASSERT_TRUE(Settings(inner, "audio").set("volume", 40));
    FlakyBackend flaky(inner, /*fail_on_set=*/3);
    std::vector<uint8_t> applied;
    Recorder recorder;
    EventBus bus;
    recorder.attach(bus);
    SystemSettings settings(flaky, bus, [&](uint8_t level) { applied.push_back(level); });

    // Written in key order: brightness, language, then screen_off_s fails.
    auto r = settings.set(
        {{"brightness", 100}, {"language", "en"}, {"volume", 10}, {"screen_off_s", 30}});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::IoError);
    auto v = settings.get();
    EXPECT_EQ(v.brightness, 200);
    EXPECT_EQ(v.volume, 40);
    EXPECT_EQ(v.screen_off_s, 60);
    EXPECT_EQ(v.language, wisp::core::LangCode::Zh);
    EXPECT_TRUE(applied.empty());
    // Not a stored batch: nothing published.
    EXPECT_TRUE(recorder.all(bus).empty());
    // Keys that never existed are removed again, not left at a default.
    EXPECT_EQ(inner.get("display", "brightness").error().code, ErrorCode::NotFound);
    EXPECT_EQ(inner.get("sys", "lang").error().code, ErrorCode::NotFound);
}

// The cell holds the whole snapshot as the last stored batch left it, and
// its mirror publishes the event the app used to publish, payload and all.
TEST(SystemSettings, CellFollowsEveryStoredBatch) {
    Fixture fx;
    ASSERT_TRUE(fx.settings.set({{"volume", 30}}));
    EXPECT_EQ(fx.settings.cell().get().volume, 30);
    auto events = fx.recorder.all(fx.bus);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].source, wisp::core::kSystemSettingsSource);
    EXPECT_EQ(events[0].payload.at("volume"), Value(30));
    EXPECT_EQ(events[0].payload, wisp::core::to_map(fx.settings.get()));

    ASSERT_TRUE(fx.settings.set({{"language", "en"}, {"time_24h", false}}));
    const SystemSettingsValues now = fx.settings.cell().get();
    EXPECT_EQ(now.language, wisp::core::LangCode::En);
    EXPECT_FALSE(now.time_24h);
    EXPECT_EQ(now.volume, 30);
    EXPECT_TRUE(now == fx.settings.get());
    events = fx.recorder.all(fx.bus);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1].payload, wisp::core::to_map(now));
}

// A batch that stores what is already there still publishes system.settings
// (one event per stored batch, as before the cell); only the cell stays
// put.
TEST(SystemSettings, UnchangedBatchPublishesButLeavesTheCell) {
    Fixture fx;
    const uint32_t before = fx.settings.cell().revision();
    ASSERT_TRUE(fx.settings.set({{"volume", 30}}));
    const uint32_t cell_revision = fx.settings.cell().revision();
    EXPECT_NE(cell_revision, before);
    ASSERT_TRUE(fx.settings.set({{"volume", 30}}));
    EXPECT_EQ(fx.settings.cell().revision(), cell_revision);
    const auto changed = fx.changed();
    ASSERT_EQ(changed.size(), 2u);
    EXPECT_EQ(changed[0], changed[1]);
    EXPECT_EQ(changed[1].at("volume"), Value(30));
}

// Brightness reaches the panel before the cell moves (and so before the
// event goes out), as it did before the event's publish.
TEST(SystemSettings, BrightnessIsAppliedBeforeTheCellMoves) {
    JsonFileSettingsBackend backend((wisp::test::temp_dir() / "settings.json").string());
    EventBus bus;
    std::vector<int> cell_at_apply;
    std::unique_ptr<SystemSettings> settings;
    settings = std::make_unique<SystemSettings>(
        backend, bus, [&](uint8_t) { cell_at_apply.push_back(settings->cell().get().brightness); });
    ASSERT_TRUE(settings->set({{"brightness", 100}}));
    EXPECT_EQ(cell_at_apply, (std::vector<int>{200}));
    EXPECT_EQ(settings->cell().get().brightness, 100);
}

TEST(SystemSettings, RejectedOrFailedBatchLeavesTheCell) {
    {
        Fixture fx;
        const uint32_t cell_revision = fx.settings.cell().revision();
        EXPECT_FALSE(fx.settings.set({{"volume", 101}}));
        EXPECT_FALSE(fx.settings.set({{"brightness", 100}, {"contrast", 5}}));
        EXPECT_EQ(fx.settings.cell().revision(), cell_revision);
        EXPECT_TRUE(fx.settings.cell().get() == SystemSettingsValues{});
        EXPECT_TRUE(fx.changed().empty());
    }

    JsonFileSettingsBackend inner((wisp::test::temp_dir() / "settings.json").string());
    FlakyBackend flaky(inner, /*fail_on_set=*/2);
    Recorder recorder;
    EventBus bus;
    SystemSettings settings(flaky, bus, nullptr);
    recorder.attach(bus);
    const uint32_t cell_revision = settings.cell().revision();
    // brightness is written, volume fails, brightness is rolled back.
    auto r = settings.set({{"brightness", 100}, {"volume", 10}});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::IoError);
    EXPECT_EQ(settings.cell().revision(), cell_revision);
    EXPECT_TRUE(settings.cell().get() == SystemSettingsValues{});
    EXPECT_TRUE(recorder.all(bus).empty());
}

// A batch whose rollback fails too leaves part of it stored: the cell is
// reseeded from what storage now holds, so its followers do not stay
// behind storage until the next batch, and, as the batch failed, nothing
// is published.
TEST(SystemSettings, FailedRollbackReseedsTheCellFromStorage) {
    JsonFileSettingsBackend inner((wisp::test::temp_dir() / "settings.json").string());
    FlakyBackend flaky(inner, /*fail_on_set=*/2, /*fail_after=*/true);
    Recorder recorder;
    EventBus bus;
    std::vector<uint8_t> applied;
    SystemSettings settings(flaky, bus, [&](uint8_t level) { applied.push_back(level); });
    recorder.attach(bus);
    // A follower on wisp.bus, held while the batch fails: it is handed the
    // reseed once released. Released before the settings (its cell) go.
    std::mutex follower_mutex;
    std::vector<SystemSettingsValues> followed;
    wisp::core::CellSubscription follower = settings.cell().subscribe(
        [&](const SystemSettingsValues& v) {
            std::lock_guard<std::mutex> lock(follower_mutex);
            followed.push_back(v);
        },
        &bus);
    ASSERT_TRUE(bus.wait_idle(2000));
    {
        wisp::test::BusFlood flood(bus);
        flood.hold();
        // brightness is written, volume fails, erasing brightness again
        // fails.
        auto r = settings.set({{"brightness", 100}, {"volume", 10}});
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, ErrorCode::IoError);
        flood.flood(512);
        flood.release();
    }
    EXPECT_EQ(settings.get().brightness, 100);
    EXPECT_EQ(settings.get().volume, SystemSettingsValues{}.volume);
    EXPECT_TRUE(settings.cell().get() == settings.get());
    EXPECT_TRUE(applied.empty());
    EXPECT_TRUE(recorder.all(bus).empty());
    {
        std::lock_guard<std::mutex> lock(follower_mutex);
        ASSERT_FALSE(followed.empty());
        EXPECT_EQ(followed.back().brightness, 100);
        EXPECT_TRUE(followed.back() == settings.get());
    }
    follower.reset();
}

// The seed takes what is stored without announcing it: at boot nothing has
// changed.
TEST(SystemSettings, SeedPublishesNothing) {
    Fixture fx;
    ASSERT_TRUE(Settings(fx.backend, "sys").set("lang", "ja"));
    EXPECT_EQ(fx.settings.cell().get().language, wisp::core::LangCode::Zh);
    fx.settings.seed_cell();
    EXPECT_EQ(fx.settings.cell().get().language, wisp::core::LangCode::Ja);
    EXPECT_TRUE(fx.settings.cell().get() == fx.settings.get());
    EXPECT_TRUE(fx.changed().empty());
}

// The snapshot's default member values are the keys' defaults, so a cell
// that has never been seeded already holds what an empty store reads as.
TEST(SystemSettings, DefaultsMatchAnEmptyBackend) {
    Fixture fx;
    EXPECT_TRUE(SystemSettingsValues{} == fx.settings.get());
    EXPECT_TRUE(fx.settings.cell().get() == SystemSettingsValues{});
    EXPECT_EQ(wisp::core::to_map(SystemSettingsValues{}), wisp::core::to_map(fx.settings.get()));
}

// spec 4.1 and 5.4: power/deep_after_s (0 never, else 60-3600, default
// 300), power/light_sleep (a boolean, default true) and audio/notify_vol
// (0-100, default 60).
TEST(SystemSettings, LightSleepIsNotARuntimeSetting) {
    Fixture fx;
    ASSERT_TRUE(Settings(fx.backend, "power").set("light_sleep", false));
    EXPECT_EQ(wisp::core::to_map(fx.settings.get()).count("light_sleep"), 0u);
    auto result = fx.settings.set({{"light_sleep", true}});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(SystemSettings, PowerAndNotifyKeys) {
    Fixture fx;
    auto v = fx.settings.get();
    EXPECT_EQ(v.deep_after_s, 300);
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
    EXPECT_EQ(v.notify_vol, 60);

    auto map = wisp::core::to_map(v);
    EXPECT_EQ(map.at("deep_after_s"), wisp::Value(300));
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

// The clock format (mvp spec 6.4), stored as sys/time_24h: true (the
// default) for a 24-hour clock, false for 12-hour.
TEST(SystemSettings, Time24hStoredUnderSys) {
    Fixture fx;
    EXPECT_TRUE(fx.settings.get().time_24h);
    EXPECT_EQ(wisp::core::to_map(fx.settings.get()).at("time_24h"), wisp::Value(true));

    ASSERT_TRUE(fx.settings.set({{"time_24h", false}}));
    EXPECT_FALSE(fx.settings.get().time_24h);
    EXPECT_FALSE(Settings(fx.backend, "sys").get_bool("time_24h", true));
    const auto changed = fx.changed();
    ASSERT_FALSE(changed.empty());
    EXPECT_FALSE(changed.back().at("time_24h").as_bool());
    EXPECT_EQ(changed.back().at("time_24h"), wisp::Value(false));

    for (const wisp::Value& bad : {wisp::Value("12"), wisp::Value(12), wisp::Value(0)}) {
        auto r = fx.settings.set({{"time_24h", bad}});
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    }
    EXPECT_FALSE(fx.settings.get().time_24h);

    // A stored value set() would refuse reads back as the default.
    ASSERT_TRUE(fx.backend.set("sys", "time_24h", wisp::Value(int64_t{12})));
    EXPECT_TRUE(fx.settings.get().time_24h);
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

// The bytes GET /settings, PUT's reply and system.settings carry for the
// defaults: keys, value types and values (spec 3.10).
TEST(SettingsGolden, DefaultMapJson) {
    EXPECT_EQ(
        wisp::to_json(Value(wisp::core::to_map(SystemSettingsValues{}))),
        R"({"agent_max_steps":60,"agent_turn_min":15,"brightness":200,"deep_after_s":300,)"
        R"("language":"zh","notify_vol":60,"ntp_servers":"pool.ntp.org,ntp.aliyun.com",)"
        R"("screen_off_s":60,"time_24h":true,"tts_mute":false,"utc_offset_min":480,"volume":60})");
}

// Each value's type, for a non-default set.
TEST(SettingsGolden, NonDefaultMapTypes) {
    SystemSettingsValues v;
    v.brightness = 10;
    v.volume = 0;
    v.screen_off_s = 0;
    v.language = wisp::core::LangCode::Ja;
    v.utc_offset_min = -210;
    v.ntp_servers.assign("10.0.0.1");
    v.deep_after_s = 3600;
    v.notify_vol = 100;
    v.tts_mute = true;
    v.time_24h = false;
    v.agent_max_steps = 200;
    v.agent_turn_min = 2;
    const Map m = wisp::core::to_map(v);
    EXPECT_EQ(m.size(), 12u);
    const std::vector<std::pair<const char*, int64_t>> ints = {
        {"brightness", 10},       {"volume", 0},          {"screen_off_s", 0},
        {"utc_offset_min", -210}, {"deep_after_s", 3600}, {"notify_vol", 100},
        {"agent_max_steps", 200}, {"agent_turn_min", 2}};
    for (const auto& [key, want] : ints) {
        ASSERT_EQ(m.count(key), 1u) << key;
        ASSERT_EQ(m.at(key).type(), Value::Type::Int) << key;
        EXPECT_EQ(m.at(key).as_int(), want) << key;
    }
    for (const auto& [key, want] : std::vector<std::pair<const char*, const char*>>{
             {"language", "ja"}, {"ntp_servers", "10.0.0.1"}}) {
        ASSERT_EQ(m.count(key), 1u) << key;
        ASSERT_EQ(m.at(key).type(), Value::Type::String) << key;
        EXPECT_EQ(m.at(key).as_string(), want) << key;
    }
    for (const auto& [key, want] :
         std::vector<std::pair<const char*, bool>>{{"tts_mute", true}, {"time_24h", false}}) {
        ASSERT_EQ(m.count(key), 1u) << key;
        ASSERT_EQ(m.at(key).type(), Value::Type::Bool) << key;
        EXPECT_EQ(m.at(key).as_bool(), want) << key;
    }
}

// A store written the way the current release writes it (the JSON file
// backend's format, through core::Settings) reads back as these values;
// a device upgraded from that release must read the same settings.
TEST(SystemSettings, StoreWrittenByTheOldCodeReadsTheSame) {
    {
        Fixture fx;
        ASSERT_TRUE(Settings(fx.backend, "display").set("brightness", int64_t{90}));
        ASSERT_TRUE(Settings(fx.backend, "audio").set("volume", int64_t{35}));
        ASSERT_TRUE(Settings(fx.backend, "power").set("screen_off_s", int64_t{120}));
        ASSERT_TRUE(Settings(fx.backend, "sys").set("lang", "en"));
        ASSERT_TRUE(Settings(fx.backend, "sys").set("tz_min", int64_t{330}));
        ASSERT_TRUE(Settings(fx.backend, "sys").set("ntp", "a.example,b.example"));
        ASSERT_TRUE(Settings(fx.backend, "power").set("deep_after_s", int64_t{1800}));
        ASSERT_TRUE(Settings(fx.backend, "audio").set("notify_vol", int64_t{20}));
        ASSERT_TRUE(Settings(fx.backend, "voice").set("tts_mute", true));
        ASSERT_TRUE(Settings(fx.backend, "sys").set("time_24h", false));
        ASSERT_TRUE(Settings(fx.backend, "agent").set("max_steps", int64_t{100}));
        ASSERT_TRUE(Settings(fx.backend, "agent").set("turn_min", int64_t{30}));
        const auto v = fx.settings.get();
        EXPECT_EQ(v.brightness, 90);
        EXPECT_EQ(v.volume, 35);
        EXPECT_EQ(v.screen_off_s, 120);
        EXPECT_EQ(v.language, wisp::core::LangCode::En);
        EXPECT_EQ(v.utc_offset_min, 330);
        EXPECT_EQ(v.ntp_servers, "a.example,b.example");
        EXPECT_EQ(v.deep_after_s, 1800);
        EXPECT_EQ(v.notify_vol, 20);
        EXPECT_TRUE(v.tts_mute);
        EXPECT_FALSE(v.time_24h);
        EXPECT_EQ(v.agent_max_steps, 100);
        EXPECT_EQ(v.agent_turn_min, 30);
        EXPECT_EQ(wisp::core::to_map(v), (Map{{"brightness", Value(90)},
                                              {"volume", Value(35)},
                                              {"screen_off_s", Value(120)},
                                              {"language", Value("en")},
                                              {"utc_offset_min", Value(330)},
                                              {"ntp_servers", Value("a.example,b.example")},
                                              {"deep_after_s", Value(1800)},
                                              {"notify_vol", Value(20)},
                                              {"tts_mute", Value(true)},
                                              {"time_24h", Value(false)},
                                              {"agent_max_steps", Value(100)},
                                              {"agent_turn_min", Value(30)}}));
    }
    {
        Fixture fx;
        ASSERT_TRUE(Settings(fx.backend, "display").set("brightness", int64_t{5}));
        ASSERT_TRUE(Settings(fx.backend, "sys").set("lang", "fr"));
        const auto v = fx.settings.get();
        EXPECT_EQ(v.brightness, 200);
        EXPECT_EQ(v.language, wisp::core::LangCode::Zh);
    }
}

namespace {
struct CaptureSink : wisp::LogSink {
    std::mutex mutex;
    std::vector<std::string> lines;
    void write(const wisp::LogRecord& rec) override {
        std::lock_guard<std::mutex> lock(mutex);
        lines.push_back(rec.msg);
    }
    // The captured lines containing "not valid".
    std::vector<std::string> invalid() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> out;
        for (const std::string& l : lines) {
            if (l.find("not valid") != std::string::npos) out.push_back(l);
        }
        return out;
    }
};
}  // namespace

// A stored value set() would refuse is logged once per seed_cell(), naming
// its store key, and reads as the default; get() alone logs nothing.
TEST(SystemSettings, InvalidStoredValueLogsOnceAtSeed) {
    Fixture fx;
    ASSERT_TRUE(Settings(fx.backend, "display").set("brightness", int64_t{5}));
    ASSERT_TRUE(Settings(fx.backend, "sys").set("lang", "fr"));
    CaptureSink sink;
    wisp::log::add_sink(&sink);

    (void)fx.settings.get();
    EXPECT_TRUE(sink.invalid().empty());

    fx.settings.seed_cell();
    std::vector<std::string> lines = sink.invalid();
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_NE(lines[0].find("display/brightness"), std::string::npos) << lines[0];
    EXPECT_NE(lines[1].find("sys/lang"), std::string::npos) << lines[1];
    EXPECT_EQ(fx.settings.cell().get().brightness, 200);
    EXPECT_EQ(fx.settings.cell().get().language, wisp::core::LangCode::Zh);

    fx.settings.seed_cell();
    EXPECT_EQ(sink.invalid().size(), 4u);
    wisp::log::remove_sink(&sink);
}

// system.settings carries the same map GET /settings returns.
TEST(SettingsGolden, EventPayloadIsTheMapOfTheStoredValues) {
    Fixture fx;
    ASSERT_TRUE(fx.settings.set({{"volume", 30}}));
    const auto changed = fx.changed();
    ASSERT_FALSE(changed.empty());
    EXPECT_EQ(changed.back(), wisp::core::to_map(fx.settings.get()));
}
