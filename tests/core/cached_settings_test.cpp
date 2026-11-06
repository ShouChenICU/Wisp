#include "wisp/core/cached_settings.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "wisp/base/error.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/os/flash_guard.hpp"
#include "wisp/os/os.hpp"

using wisp::ErrorCode;
using wisp::Result;
using wisp::Value;
using wisp::core::CachedSettingsBackend;
using wisp::core::Settings;
using wisp::core::SettingsChange;
using wisp::core::SettingsEntry;
using wisp::core::StorageWorker;

namespace {

// A platform backend that keeps its store in memory and records how it is
// used: how often each call happens, what each write_batch() carried, and
// whether every write ran on the flash thread.
class FakePlatformBackend : public wisp::core::SettingsBackend {
public:
    Result<Value> get(const std::string& ns, const std::string& key) override {
        std::lock_guard<std::mutex> lock(m);
        ++gets;
        auto it = store.find({ns, key});
        if (it == store.end()) return Result<Value>::err(ErrorCode::NotFound, ns + "/" + key);
        return Result<Value>::ok(it->second);
    }
    Result<void> set(const std::string& ns, const std::string& key, const Value& value) override {
        std::lock_guard<std::mutex> lock(m);
        store[{ns, key}] = value;
        return Result<void>::ok();
    }
    Result<void> erase(const std::string& ns, const std::string& key) override {
        std::lock_guard<std::mutex> lock(m);
        store.erase({ns, key});
        return Result<void>::ok();
    }
    Result<void> erase_all() override {
        std::lock_guard<std::mutex> lock(m);
        ++erase_alls;
        if (!wisp::os::on_flash_thread()) off_thread = true;
        store.clear();
        return Result<void>::ok();
    }
    Result<std::vector<SettingsEntry>> load_all() override {
        std::lock_guard<std::mutex> lock(m);
        ++loads;
        std::vector<SettingsEntry> out;
        for (const auto& [name, value] : store) out.push_back({name.first, name.second, value});
        return Result<std::vector<SettingsEntry>>::ok(std::move(out));
    }
    Result<void> write_batch(std::span<const SettingsChange> changes) override {
        std::lock_guard<std::mutex> lock(m);
        if (!wisp::os::on_flash_thread()) off_thread = true;
        batches.emplace_back(changes.begin(), changes.end());
        if (fail_writes > 0) {
            --fail_writes;
            return Result<void>::err(ErrorCode::IoError, "injected");
        }
        for (const SettingsChange& c : changes) {
            if (c.value) {
                store[{c.ns, c.key}] = *c.value;
            } else {
                store.erase({c.ns, c.key});
            }
        }
        return Result<void>::ok();
    }

    Value stored(const std::string& ns, const std::string& key) {
        std::lock_guard<std::mutex> lock(m);
        auto it = store.find({ns, key});
        return it == store.end() ? Value() : it->second;
    }
    size_t batch_count() {
        std::lock_guard<std::mutex> lock(m);
        return batches.size();
    }

    std::mutex m;
    std::map<std::pair<std::string, std::string>, Value> store;
    std::vector<std::vector<SettingsChange>> batches;
    int gets = 0;
    int loads = 0;
    int erase_alls = 0;
    int fail_writes = 0;
    bool off_thread = false;
};

// Releases a job blocked on `sem` when the test body exits, including by a
// failed ASSERT, so the worker's destructor can join. Declare it after the
// worker so it runs first.
struct ReleaseOnExit {
    wisp::os::Semaphore& sem;
    ~ReleaseOnExit() { sem.give(); }
};

// Holds the worker inside one job until the returned semaphore is given.
void block_worker(StorageWorker& w, wisp::os::Semaphore& release) {
    wisp::os::Semaphore started(1, 0);
    ASSERT_TRUE(w.submit([&] {
        started.give();
        release.take(wisp::os::kForever);
    }));
    ASSERT_TRUE(started.take(5000));
}

}  // namespace

TEST(CachedSettings, GetNeverTouchesThePlatform) {
    FakePlatformBackend platform;
    platform.store[{"display", "brightness"}] = Value(int64_t{90});
    platform.store[{"sys", "lang"}] = Value("en");
    platform.store[{"ui", "list"}] = Value(wisp::List{Value(1), Value("x")});
    StorageWorker w;
    ASSERT_TRUE(w.start());
    CachedSettingsBackend cache(platform, w, {});
    ASSERT_TRUE(cache.load());
    EXPECT_EQ(platform.loads, 1);

    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(Settings(cache, "display").get_int("brightness", 0), 90);
        EXPECT_EQ(Settings(cache, "sys").get_string("lang", ""), "en");
    }
    EXPECT_EQ(cache.get("ui", "list").value(), Value(wisp::List{Value(1), Value("x")}));
    EXPECT_EQ(cache.get("sys", "missing").error().code, ErrorCode::NotFound);
    EXPECT_EQ(cache.get("sys", "a_key_longer_than_15").error().code, ErrorCode::InvalidArgument);
    ASSERT_TRUE(cache.set("sys", "lang", "ja"));
    EXPECT_EQ(Settings(cache, "sys").get_string("lang", ""), "ja");
    EXPECT_EQ(platform.gets, 0);
    EXPECT_TRUE(cache.flush(1000));
}

TEST(CachedSettings, RepeatedSetsOfOneKeyPersistOnce) {
    FakePlatformBackend platform;
    wisp::os::Semaphore release(1, 0);
    StorageWorker w;
    ReleaseOnExit guard{release};
    ASSERT_TRUE(w.start());
    CachedSettingsBackend cache(platform, w, {});
    ASSERT_TRUE(cache.load());
    block_worker(w, release);

    for (int i = 1; i <= 100; ++i) ASSERT_TRUE(cache.set("audio", "volume", i));
    EXPECT_EQ(platform.batch_count(), 0u);
    release.give();
    ASSERT_TRUE(w.drain(5000));

    ASSERT_EQ(platform.batches.size(), 1u);
    ASSERT_EQ(platform.batches[0].size(), 1u);
    EXPECT_EQ(platform.batches[0][0].ns, "audio");
    EXPECT_EQ(platform.batches[0][0].key, "volume");
    EXPECT_EQ(platform.batches[0][0].value, std::optional<Value>(Value(100)));
    EXPECT_EQ(platform.stored("audio", "volume"), Value(100));
}

TEST(CachedSettings, PersistsOnTheFlashThread) {
    FakePlatformBackend platform;
    StorageWorker w;
    ASSERT_TRUE(w.start());
    CachedSettingsBackend cache(platform, w, {});
    ASSERT_TRUE(cache.load());

    ASSERT_TRUE(cache.set("power", "screen_off_s", 30));
    ASSERT_TRUE(cache.erase("power", "screen_off_s"));
    ASSERT_TRUE(cache.set("power", "deep_after_s", 600));
    ASSERT_TRUE(cache.erase_all());
    ASSERT_TRUE(cache.set("sys", "lang", "en"));
    ASSERT_TRUE(cache.flush(1000));

    EXPECT_GE(platform.batch_count(), 1u);
    EXPECT_EQ(platform.erase_alls, 1);
    EXPECT_FALSE(platform.off_thread);
    EXPECT_EQ(platform.stored("sys", "lang"), Value("en"));
}

TEST(CachedSettings, FailedPersistKeepsDirtyAndRetries) {
    FakePlatformBackend platform;
    platform.fail_writes = 1;
    StorageWorker w;
    ASSERT_TRUE(w.start());
    CachedSettingsBackend cache(platform, w, {});
    ASSERT_TRUE(cache.load());

    ASSERT_TRUE(cache.set("sys", "lang", "en"));
    ASSERT_TRUE(w.drain(5000));
    ASSERT_EQ(platform.batch_count(), 1u);
    // The new value stays in memory though it is not stored yet.
    EXPECT_EQ(Settings(cache, "sys").get_string("lang", ""), "en");
    EXPECT_EQ(platform.stored("sys", "lang"), Value());

    ASSERT_TRUE(cache.set("sys", "tz_min", 60));
    ASSERT_TRUE(w.drain(5000));
    ASSERT_EQ(platform.batches.size(), 2u);
    EXPECT_EQ(platform.batches[1].size(), 2u);
    EXPECT_EQ(platform.stored("sys", "lang"), Value("en"));
    EXPECT_EQ(platform.stored("sys", "tz_min"), Value(60));
}

TEST(CachedSettings, ThreeFailuresNotifyOnce) {
    FakePlatformBackend platform;
    platform.fail_writes = 5;
    std::atomic<int> notices{0};
    StorageWorker w;
    ASSERT_TRUE(w.start());
    CachedSettingsBackend cache(platform, w, [&] { ++notices; });
    ASSERT_TRUE(cache.load());

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(cache.set("audio", "volume", i));
        ASSERT_TRUE(w.drain(5000));
        EXPECT_EQ(notices.load(), i >= 2 ? 1 : 0) << "after failure " << i + 1;
    }
    // A success resets the count: three more failures notify again.
    ASSERT_TRUE(cache.set("audio", "volume", 50));
    ASSERT_TRUE(w.drain(5000));
    EXPECT_EQ(platform.stored("audio", "volume"), Value(50));
    platform.fail_writes = 3;
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(cache.set("audio", "volume", i));
        ASSERT_TRUE(w.drain(5000));
    }
    EXPECT_EQ(notices.load(), 2);
}

TEST(CachedSettings, FlushWritesDirtyKeys) {
    FakePlatformBackend platform;
    wisp::os::Semaphore release(1, 0);
    StorageWorker w;
    ReleaseOnExit guard{release};
    ASSERT_TRUE(w.start());
    CachedSettingsBackend cache(platform, w, {});
    ASSERT_TRUE(cache.load());

    // Nothing to write: at once, even with the worker held up.
    block_worker(w, release);
    EXPECT_TRUE(cache.flush(0));
    ASSERT_TRUE(cache.set("display", "brightness", 42));
    EXPECT_EQ(cache.flush(50).error().code, ErrorCode::Timeout);
    release.give();
    ASSERT_TRUE(cache.flush(1000));
    EXPECT_EQ(platform.stored("display", "brightness"), Value(42));

    // The write set() queued fails, and so does the one flush() runs.
    platform.fail_writes = 2;
    ASSERT_TRUE(cache.set("display", "brightness", 43));
    EXPECT_EQ(cache.flush(1000).error().code, ErrorCode::IoError);
    ASSERT_TRUE(cache.flush(1000));
    EXPECT_EQ(platform.stored("display", "brightness"), Value(43));
}

TEST(CachedSettings, EraseAllClearsCacheAndPlatform) {
    FakePlatformBackend platform;
    platform.store[{"sys", "lang"}] = Value("en");
    StorageWorker w;
    ASSERT_TRUE(w.start());
    CachedSettingsBackend cache(platform, w, {});
    ASSERT_TRUE(cache.load());
    ASSERT_TRUE(cache.set("power", "screen_off_s", 30));

    ASSERT_TRUE(cache.erase_all());
    EXPECT_EQ(cache.get("sys", "lang").error().code, ErrorCode::NotFound);
    EXPECT_EQ(cache.get("power", "screen_off_s").error().code, ErrorCode::NotFound);
    ASSERT_TRUE(cache.flush(1000));
    EXPECT_EQ(platform.erase_alls, 1);
    EXPECT_TRUE(platform.store.empty());
}

TEST(CachedSettings, WorksInlineBeforeStartAndAfterStop) {
    FakePlatformBackend platform;
    platform.store[{"sys", "lang"}] = Value("en");
    StorageWorker w;
    CachedSettingsBackend cache(platform, w, {});
    ASSERT_TRUE(cache.load());
    EXPECT_EQ(Settings(cache, "sys").get_string("lang", ""), "en");
    ASSERT_TRUE(cache.set("sys", "lang", "zh"));
    EXPECT_EQ(platform.stored("sys", "lang"), Value("zh"));

    ASSERT_TRUE(w.start());
    w.stop();
    ASSERT_TRUE(cache.set("sys", "lang", "ja"));
    EXPECT_EQ(platform.stored("sys", "lang"), Value("ja"));
    EXPECT_TRUE(cache.flush(0));
}

TEST(CachedSettings, LoadAllAndWriteBatchServeTheCache) {
    FakePlatformBackend platform;
    StorageWorker w;
    CachedSettingsBackend cache(platform, w, {});
    ASSERT_TRUE(cache.load());
    const SettingsChange changes[] = {{"a", "x", Value(1)}, {"b", "y", Value("s")}};
    ASSERT_TRUE(cache.write_batch(changes));
    const SettingsChange erase[] = {{"a", "x", std::nullopt}};
    ASSERT_TRUE(cache.write_batch(erase));

    auto all = cache.load_all();
    ASSERT_TRUE(all);
    ASSERT_EQ(all.value().size(), 1u);
    EXPECT_EQ(all.value()[0].ns, "b");
    EXPECT_EQ(all.value()[0].key, "y");
    EXPECT_EQ(all.value()[0].value, Value("s"));
    EXPECT_EQ(platform.stored("b", "y"), Value("s"));
    EXPECT_EQ(platform.stored("a", "x"), Value());
}
