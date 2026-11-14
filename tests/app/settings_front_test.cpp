#include "settings_front.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <utility>

#include "support/fake_settings_backend.hpp"
#include "support/wait.hpp"
#include "wisp/core/cached_settings.hpp"
#include "wisp/core/storage_worker.hpp"

using wisp::Value;
using wisp::app::SettingsFront;
using wisp::core::CachedSettingsBackend;
using wisp::core::Settings;
using wisp::core::StorageWorker;
using wisp::test::FakePlatformBackend;

TEST(SettingsFront, FailedLoadLeavesReadsOnThePlatform) {
    auto owned = std::make_unique<FakePlatformBackend>();
    FakePlatformBackend& platform = *owned;
    platform.store[{"display", "brightness"}] = Value(90);
    platform.fail_load = true;
    SettingsFront front(std::move(owned));
    StorageWorker w;
    ASSERT_TRUE(w.start());
    CachedSettingsBackend cache(platform, w, {});

    EXPECT_FALSE(front.load_and_attach(cache));
    // Not the empty cache's default: the stored value.
    EXPECT_EQ(Settings(front, "display").get_int("brightness", 0), 90);
    EXPECT_EQ(platform.gets, 1);
}

// Without a cache while the worker runs (the cache could not be loaded or
// allocated), the platform backend is still only touched on the flash
// thread: each direct call is run on the worker.
TEST(SettingsFront, DirectCallsRunOnTheWorkerWhileItRuns) {
    auto owned = std::make_unique<FakePlatformBackend>();
    FakePlatformBackend& platform = *owned;
    platform.store[{"display", "brightness"}] = Value(90);
    platform.fail_load = true;
    SettingsFront front(std::move(owned));
    StorageWorker w;
    ASSERT_TRUE(w.start());
    front.set_worker(&w);
    CachedSettingsBackend cache(platform, w, {});
    EXPECT_FALSE(front.load_and_attach(cache));

    EXPECT_EQ(Settings(front, "display").get_int("brightness", 0), 90);
    ASSERT_TRUE(Settings(front, "display").set("brightness", 40));
    EXPECT_EQ(platform.stored("display", "brightness"), Value(40));
    EXPECT_EQ(platform.gets, 1);
    EXPECT_EQ(platform.gets_off_thread, 0);
    EXPECT_EQ(platform.sets_off_thread, 0);

    front.set_worker(nullptr);
    w.stop();
}

TEST(SettingsFront, ReadsComeFromTheCacheOnceAttached) {
    auto owned = std::make_unique<FakePlatformBackend>();
    FakePlatformBackend& platform = *owned;
    platform.store[{"display", "brightness"}] = Value(90);
    SettingsFront front(std::move(owned));
    StorageWorker w;
    ASSERT_TRUE(w.start());
    CachedSettingsBackend cache(platform, w, {});

    ASSERT_TRUE(front.load_and_attach(cache));
    EXPECT_EQ(Settings(front, "display").get_int("brightness", 0), 90);
    EXPECT_EQ(platform.gets, 0);
    ASSERT_TRUE(front.detach_and_flush());
}

// What App::stop() relies on: the cache's last dirty value is written
// before the front lets a direct platform write through, so the newer
// direct write is the one that stays.
TEST(SettingsFront, DetachWritesTheCacheBeforeDirectWrites) {
    auto owned = std::make_unique<FakePlatformBackend>();
    FakePlatformBackend& platform = *owned;
    SettingsFront front(std::move(owned));
    StorageWorker w;
    ASSERT_TRUE(w.start());
    CachedSettingsBackend cache(platform, w, {});
    ASSERT_TRUE(front.load_and_attach(cache));

    // Left dirty: the save fails once.
    platform.fail_writes = 1;
    ASSERT_TRUE(Settings(front, "audio").set("volume", 1));
    ASSERT_TRUE(w.drain(5000));
    ASSERT_EQ(platform.stored("audio", "volume"), Value());
    w.stop();

    ASSERT_TRUE(front.detach_and_flush());
    EXPECT_EQ(platform.stored("audio", "volume"), Value(1));
    ASSERT_TRUE(Settings(front, "audio").set("volume", 2));
    EXPECT_EQ(platform.stored("audio", "volume"), Value(2));
    // Detached: a second call has nothing to do.
    EXPECT_TRUE(front.detach_and_flush());
    EXPECT_EQ(platform.stored("audio", "volume"), Value(2));
}

// A direct call is a settings save too: it goes on the Interactive lane,
// ahead of Normal work queued before it.
TEST(SettingsFront, DirectCallsOvertakeANormalBacklog) {
    auto owned = std::make_unique<FakePlatformBackend>();
    FakePlatformBackend& platform = *owned;
    SettingsFront front(std::move(owned));
    StorageWorker w;
    ASSERT_TRUE(w.start());
    front.set_worker(&w);

    wisp::os::Semaphore started(1, 0);
    wisp::os::Semaphore release(1, 0);
    ASSERT_TRUE(w.submit([&] {
        started.give();
        release.take(wisp::os::kForever);
    }));
    ASSERT_TRUE(started.take(5000));
    // N1 records whether the platform write had already happened.
    Value seen_by_n1;
    ASSERT_TRUE(w.submit([&] { seen_by_n1 = platform.stored("display", "brightness"); },
                         wisp::core::IoLane::Normal));
    wisp::Result<void> set_result = wisp::Result<void>::ok();
    std::thread setter([&] { set_result = Settings(front, "display").set("brightness", 40); });
    // The blocker, N1 and the set's job.
    const bool queued = wisp::test::wait_until([&] { return w.pending() == 3; }, 5000);
    release.give();
    setter.join();
    ASSERT_TRUE(queued);
    ASSERT_TRUE(w.drain(5000));

    EXPECT_TRUE(set_result);
    EXPECT_EQ(seen_by_n1, Value(40));
    front.set_worker(nullptr);
    w.stop();
}
